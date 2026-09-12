#!/usr/bin/env python3
"""
tests/mock_daemon.py — Mock Headless Daemon for Offline E2E Testing
Simulates bose-700-daemon:
- Listens on UNIX domain socket ($XDG_RUNTIME_DIR/bose-700.sock)
- Manages headphone state
- Atomically writes state updates to $XDG_STATE_HOME/bose-700/status.json
- Handles wire protocol commands (JSON {"ok": ...} responses):
    status, cnc, eq, sidetone, prompts, multipoint, reconnect
- Signal handling:
    SIGTERM / SIGINT: clean shutdown (unlinks socket and status.json)
    SIGUSR1: simulate disconnect (connected: false)
    SIGUSR2: simulate reconnect (connected: true)
"""

import os
import sys
import json
import socket
import select
import signal
import argparse
from datetime import datetime, timezone

SIDETONE_LEVELS = ["off", "low", "medium", "high"]


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class MockDaemon:
    def __init__(self, state_dir=None, runtime_dir=None):
        self.state_dir = state_dir or os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state"))
        self.runtime_dir = runtime_dir or os.environ.get("XDG_RUNTIME_DIR", f"/tmp/run-{os.getuid()}")

        self.bose_state_dir = os.path.join(self.state_dir, "bose-700")
        self.status_file = os.path.join(self.bose_state_dir, "status.json")
        self.socket_path = os.path.join(self.runtime_dir, "bose-700.sock")

        self.running = True
        self.server_sock = None

        self.state = {
            "schema": 1,
            "connected": True,
            "device": {
                "name": "Bose NC 700",
                "model": "Bose NC Headphones 700",
                "firmware": "1.8.2-11524+e0f7590",
                "address": "4C:87:5D:A3:D1:4F",
            },
            "battery": {"level": 85},
            "cnc": {"level": 10, "max": 10},
            "eq": {"bass": 0, "mid": 0, "treble": 0, "min": -10, "max": 10},
            "sidetone": "medium",
            "voice_prompts": {"enabled": True, "language": "English"},
            "multipoint": True,
        }

    def setup_directories(self):
        os.makedirs(self.bose_state_dir, mode=0o700, exist_ok=True)
        os.makedirs(self.runtime_dir, mode=0o700, exist_ok=True)

    def payload(self):
        if self.state.get("connected", True):
            out = dict(self.state)
        else:
            out = {"schema": 1, "connected": False}
        out["updated_at"] = now_iso()
        return out

    def write_status(self):
        tmp_file = f"{self.status_file}.tmp.{os.getpid()}"
        with open(tmp_file, "w") as f:
            json.dump(self.payload(), f, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp_file, 0o600)
        os.replace(tmp_file, self.status_file)

    @staticmethod
    def ok(**extra):
        return json.dumps({"ok": True, **extra}) + "\n"

    @staticmethod
    def err(message):
        return json.dumps({"ok": False, "error": message}) + "\n"

    def handle_command(self, cmd_line):
        line = cmd_line.strip()
        if not line:
            return self.err("empty command")
        parts = line.split()
        verb = parts[0].lower()

        if verb == "status":
            return json.dumps({"ok": True, **self.payload()}) + "\n"

        if verb == "cnc":
            max_level = self.state["cnc"]["max"]
            if len(parts) < 2:
                return self.err(f"missing level (0-{max_level})")
            try:
                level = int(parts[1])
            except ValueError:
                return self.err(f"invalid level '{parts[1]}'")
            if not (0 <= level <= max_level):
                return self.err(f"cnc level out of range [0-{max_level}]")
            self.state["cnc"]["level"] = level
            self.write_status()
            return self.ok()

        if verb == "eq":
            eq = self.state["eq"]
            if len(parts) < 4:
                return self.err("eq requires bass, mid and treble "
                                f"({eq['min']}..{eq['max']} each)")
            try:
                bands = [int(p) for p in parts[1:4]]
            except ValueError:
                return self.err("invalid eq arguments")
            for b in bands:
                if not (eq["min"] <= b <= eq["max"]):
                    return self.err(f"eq band out of range [{eq['min']}, {eq['max']}]")
            eq["bass"], eq["mid"], eq["treble"] = bands
            self.write_status()
            return self.ok()

        if verb == "sidetone":
            if len(parts) < 2:
                return self.err("expected " + "|".join(SIDETONE_LEVELS))
            level = parts[1].lower()
            if level not in SIDETONE_LEVELS:
                return self.err(f"unknown sidetone value '{parts[1]}'")
            self.state["sidetone"] = level
            self.write_status()
            return self.ok()

        if verb in ("prompts", "multipoint"):
            if len(parts) < 2 or parts[1].lower() not in ("on", "off"):
                return self.err("expected on|off")
            if verb == "prompts":
                self.state["voice_prompts"]["enabled"] = parts[1].lower() == "on"
            else:
                self.state["multipoint"] = parts[1].lower() == "on"
            self.write_status()
            return self.ok()

        if verb == "reconnect":
            # The real daemon drops and re-opens the RFCOMM link; the mock
            # simply confirms.
            return self.ok()

        # Internal test helper verbs
        if verb == "_set_battery":
            if len(parts) >= 2:
                self.state["battery"]["level"] = int(parts[1])
            self.write_status()
            return self.ok()

        if verb == "_disconnect":
            self.state["connected"] = False
            self.write_status()
            return self.ok()

        if verb == "_reconnect":
            self.state["connected"] = True
            self.write_status()
            return self.ok()

        return self.err(f"unknown command '{verb}'")

    def run(self):
        self.setup_directories()

        if os.path.exists(self.socket_path):
            try:
                os.unlink(self.socket_path)
            except OSError:
                pass

        self.server_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server_sock.bind(self.socket_path)
        os.chmod(self.socket_path, 0o700)
        self.server_sock.listen(10)
        self.server_sock.setblocking(False)

        # Initial status write
        self.write_status()

        def sig_term_handler(_signum, _frame):
            self.running = False

        def sig_usr1_handler(_signum, _frame):
            # Disconnect simulation
            self.state["connected"] = False
            self.write_status()

        def sig_usr2_handler(_signum, _frame):
            # Reconnect simulation
            self.state["connected"] = True
            self.write_status()

        signal.signal(signal.SIGTERM, sig_term_handler)
        signal.signal(signal.SIGINT, sig_term_handler)
        signal.signal(signal.SIGUSR1, sig_usr1_handler)
        signal.signal(signal.SIGUSR2, sig_usr2_handler)

        sys.stdout.write(f"[MOCK_DAEMON] Ready PID={os.getpid()}\n")
        sys.stdout.flush()

        inputs = [self.server_sock]
        clients = {}

        while self.running:
            try:
                readable, _, _ = select.select(inputs, [], [], 0.5)
            except (select.error, InterruptedError):
                continue

            for s in readable:
                if s is self.server_sock:
                    try:
                        conn, _ = self.server_sock.accept()
                        conn.setblocking(False)
                        inputs.append(conn)
                        clients[conn] = b""
                    except OSError:
                        pass
                else:
                    try:
                        data = s.recv(1024)
                        if data:
                            clients[s] += data
                            if b"\n" in clients[s]:
                                line, _, rest = clients[s].partition(b"\n")
                                clients[s] = rest
                                cmd_str = line.decode("utf-8", errors="replace")
                                response = self.handle_command(cmd_str)
                                s.sendall(response.encode("utf-8"))
                        else:
                            inputs.remove(s)
                            if s in clients:
                                del clients[s]
                            s.close()
                    except OSError:
                        inputs.remove(s)
                        if s in clients:
                            del clients[s]
                        s.close()

        # Shutdown cleanup
        try:
            if self.server_sock:
                self.server_sock.close()
            if os.path.exists(self.socket_path):
                os.unlink(self.socket_path)
            if os.path.exists(self.status_file):
                os.unlink(self.status_file)
        except OSError:
            pass
        sys.stdout.write("[MOCK_DAEMON] Shutdown cleanly\n")
        sys.stdout.flush()

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--state-dir", default=None)
    parser.add_argument("--runtime-dir", default=None)
    args = parser.parse_args()
    daemon = MockDaemon(state_dir=args.state_dir, runtime_dir=args.runtime_dir)
    daemon.run()
