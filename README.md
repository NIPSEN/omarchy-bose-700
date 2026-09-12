# Omarchy Bose 700

An Omarchy bar-widget plugin and headless C++20 daemon for managing **Bose
Noise Cancelling Headphones 700** on Linux.

This is a port of [kevincardwell/omarchy-sony-xm3](https://github.com/kevincardwell/omarchy-sony-xm3)
(itself a port of [andROYdified/omarchy-sony](https://github.com/andROYdified/omarchy-sony)
to the XM3) to the Bose NC700 and its BMAP protocol. The architecture is the
same; the protocol layer and the feature set are not.

---

## Requirements

**The headphones must be paired to this machine's own Bluetooth adapter.**

Control runs over an RFCOMM socket that BlueZ opens to the headset. A USB
Bluetooth *audio transmitter* dongle (Avantree DG60, TaoTronics, and similar)
pairs with the headphones itself and shows up on Linux as a USB sound card — the
host Bluetooth stack never sees the headphones, so there is nothing for the
daemon to connect to.

If audio currently reaches your headphones through a transmitter dongle, you
will need to pair them directly to this machine to use any of this. `./setup`
checks for an adapter and a paired headset and warns you if either is missing.

### Pairing the NC700

1. On the headphones, slide the **Power/Bluetooth button** to the Bluetooth
   symbol and **hold it** until you hear *"Ready to connect"* (the LED blinks
   blue).
2. Then, on this machine:

```bash
bluetoothctl
  scan on
  pair    <MAC>
  trust   <MAC>
  connect <MAC>
```

The headset advertises as `Bose NC 700` (or `LE-Bose NC 700`).

---

## Features

- 🔋 **Live battery** — percentage in the bar and in the panel header.
- 🎧 **CNC noise-cancelling slider** — the NC700's 0–10 axis: 0 is full
  transparency, 10 is maximum noise cancelling.
- 🎛️ **3-band equalizer** — bass, mid and treble, −10…+10 each.
- 🎙️ **Sidetone** — off / low / medium / high (how much of your own voice you
  hear during calls).
- 🗣️ **Voice prompts** — on or off; the prompt language is shown read-only.
- 🔗 **Multipoint** — stay connected to two devices at once.
- 🔄 **Reconnect** — drop and re-open the link to the headphones.
- ℹ️ **Device name and firmware version**, read from the headset.
- 🗂️ **Two tabs** — *Sound* and *Device*, so the panel stays short.
- ⌨️ **Keyboard navigation** — vim-style (`h`/`j`/`k`/`l`, `Enter`, `Esc`) in the panel.
- 💻 **CLI (`bose-700-ctl`)** — everything the panel does, scriptable.
- ⚡ **No polling** — native BlueZ RFCOMM plus a file-watched state file.

Right-clicking the bar widget cycles noise cancelling: max ANC → transparency →
halfway → max ANC, like the headset's own noise-control button.

---

## Architecture

```
┌────────────────────────────────────────────────────────┐
│                   Omarchy Shell (QML)                  │
│   ┌───────────────┐ ┌─────────────┐ ┌──────────────┐   │
│   │   BoseIcon    │ │  Service    │ │    Panel     │   │
│   └───────▲───────┘ └──────▲──────┘ └──────▲───────┘   │
│           │                │               │           │
│           └────────────────┼───────────────┘           │
│                            │ watches (FileView)        │
│                 ~/.local/state/bose-700/               │
│                        status.json                     │
│                            ▲                           │
└────────────────────────────┼───────────────────────────┘
                             │ writes (atomic, 0600)
┌────────────────────────────┼───────────────────────────┐
│  Headless Daemon           │      Companion CLI        │
│  (bose-700-daemon)         │      (bose-700-ctl)       │
│                            │            │              │
│   UNIX Domain Socket ◄─────┴────────────┘              │
│   (/run/user/$UID/bose-700.sock)                       │
│                 │                                      │
│                 ▼                                      │
│   Bluetooth RFCOMM  (BMAP protocol, channel 8)         │
│                 ▼                                      │
│       Bose Noise Cancelling Headphones 700             │
└────────────────────────────────────────────────────────┘
```

- **Plugin** (`Panel.qml`, `Service.qml`, `Model.js`, `BoseIcon.qml`) — Quickshell/QML, Omarchy manifest schema 1.
- **`daemon/`** — C++20 daemon owning the RFCOMM link and the UNIX socket.
- **`cli/`** — `bose-700-ctl`, a thin client for the same socket.
- **`installer/`** — `bose-700-deploy`, the helper `setup` uses to place and remove files without following symlinks (built, never installed).

---

## Prerequisites

Arch / Omarchy:
```bash
sudo pacman -S --needed base-devel cmake ninja bluez bluez-libs bluez-utils jq
```

---

## Install

```bash
git clone https://github.com/nipsen/omarchy-bose-700.git
cd omarchy-bose-700
./setup
```

`./setup` verifies build dependencies, warns about missing Bluetooth
prerequisites, builds with CMake + Ninja, installs `bose-700-daemon` and
`bose-700-ctl` to `~/.local/bin/`, registers the `bose-700.service` user unit,
and deploys the plugin to `~/.config/omarchy/plugins/`.

**How setup protects itself.** Setup re-runs itself with an empty environment
and a fixed system `PATH` (`/usr/bin:/usr/sbin:/bin:/sbin`). It keeps only the
session variables that `systemctl --user` and the Omarchy shell need, and runs
bash with `-p`, so `BASH_ENV` and exported shell functions are ignored. Every
tool it runs (`sudo`, `pacman`, `omarchy`, `cmake`, `ninja`, the compiler,
`systemctl`) is called by an absolute path and must be a root-owned file that
only root can write. It builds in its own `build-setup/` directory with the
compiler and generator pinned. Every file in your home directory is written or
removed by `bose-700-deploy`. It opens each directory below your home with
`O_NOFOLLOW`, keeps it open while it works, and refuses symlinks and
directories other users own or can write to. It writes each file to a
temporary name and renames it into place. It deletes only the files it is
named, never a directory tree.

**Installed from the Omarchy plugin marketplace** (or with `omarchy plugin
add`)? That adds the bar widget only. The widget needs the daemon, so build and
install it from the plugin's own directory:

```bash
~/.config/omarchy/plugins/io.github.nipsen.omarchybose700/setup
```

Setup notices it is running from the installed plugin and uses those files in
place.

Pair the headset first if you have not already — see
[Pairing the NC700](#pairing-the-nc700).

---

## Uninstall

```bash
./setup --uninstall
```

This stops and removes `bose-700.service`, deletes the two binaries from
`~/.local/bin/` and the state in `~/.local/state/bose-700/`, and runs
`omarchy plugin remove`, which takes the widget off the bar and deletes the
plugin directory. Your Bluetooth pairing is left alone. If you installed from
the marketplace, run it as
`~/.config/omarchy/plugins/io.github.nipsen.omarchybose700/setup --uninstall`.

---

## CLI

```bash
bose-700-ctl status                    # full state as JSON

# Noise cancelling (0 = full transparency, 10 = maximum ANC)
bose-700-ctl cnc 10
bose-700-ctl cnc 0

# Equalizer: bass, mid and treble are set together, each -10..10
bose-700-ctl eq 0 2 -1

# Everything else
bose-700-ctl sidetone medium           # off|low|medium|high
bose-700-ctl prompts on                # voice prompts on|off
bose-700-ctl multipoint off            # on|off
bose-700-ctl reconnect                 # drop and re-open the headset link
```

Exit codes: `0` success, `1` bad arguments or a daemon error, `2` daemon
unreachable.

---

## Service

```bash
systemctl --user status bose-700.service
journalctl --user -u bose-700.service -f
systemctl --user restart bose-700.service
```

---

## Tests

```bash
# Plugin model tests (Deno or Node)
deno run --allow-read tests/model.test.js
node tests/model.test.js

# Mock daemon for offline end-to-end testing
python3 tests/mock_daemon.py --state-dir /tmp/bose700 --runtime-dir /run/user/$UID
```

---

## Protocol

The NC700 speaks Bose's **BMAP** (Bose Management and Control Protocol) over a
plain RFCOMM link on **channel 8** — no SDP lookup needed, no pairing handshake
beyond Bluetooth itself. Each message is a framed packet addressed to a
two-byte `[block.function]` address, with GET, SET and SETGET operations.

Verified on firmware 1.8.2:

| Feature | Address | Notes |
|---|---|---|
| Firmware version | `[0.5]` | GET |
| Device name | `[1.2]` | GET |
| Voice prompts | `[1.3]` | GET (on/off + language) |
| Noise cancelling (CNC) | `[1.5]` | 0–10, SET with SETGET replies with the new level |
| Equalizer | `[1.7]` | 4-byte groups `[min, max, current, band]` per band |
| Button configuration | `[1.9]` | GET |
| Multipoint | `[1.10]` | GET/SET |
| Sidetone | `[1.11]` | GET/SET (off/low/medium/high) |
| Battery level | `[2.2]` | GET (percent) |
| Active source | `[5.1]` | GET |
| Block 31 | — | not supported on the NC700 |

Byte-level details and the reverse-engineering notes behind them:
[docs/protocol-bmap.md](docs/protocol-bmap.md). The protocol knowledge comes
from [aaronsb/bosectl](https://github.com/aaronsb/bosectl).

---

## Not included, and why

- **Firmware updates and voice-prompt language changes.** Both stream a pack
  into the headset; a failed transfer can leave it unusable. Use the Bose app.
  The language is shown in the panel but not changed here.
- **Spotify Tap / assistant button remapping** beyond what `[1.9]` reports.
- **Wearing detection.** Not exposed by this build.

---

## Acknowledgments

1. **[kevincardwell/omarchy-sony-xm3](https://github.com/kevincardwell/omarchy-sony-xm3)** — the project this is ported from: architecture, daemon/state/IPC design, and the QML panel.
2. **[andROYdified/omarchy-sony](https://github.com/andROYdified/omarchy-sony)** — the XM5 project the XM3 port came from.
3. **[thisisgm/omarchy-pods](https://github.com/thisisgm/omarchy-pods)** — the bar-widget + headless-daemon + atomic-state-file pattern underneath all of these.
4. **[aaronsb/bosectl](https://github.com/aaronsb/bosectl)** — the Bose BMAP protocol reverse engineering this daemon is built on.

---

## Disclaimer

Unofficial community project for Linux desktop integration. Not affiliated
with, authorized, maintained, sponsored, or endorsed by Bose Corporation.
"Bose" and "Noise Cancelling Headphones 700" are trademarks of Bose
Corporation.

---

## License

MIT. See [LICENSE](LICENSE). The MIT attribution chain (bosectl © aaronsb,
omarchy-sony-xm3 © Kevin Cardwell, omarchy-sony © Roy Kevin De Jesus) is kept
in [LICENSE](LICENSE).
