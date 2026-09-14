// bose-700-daemon: headless manager for the Bose NC Headphones 700 on Linux.
// Event-loop/daemon scaffolding adapted from omarchy-sony-xm3 (Copyright (c)
// Kevin Cardwell, MIT license); BMAP layer adapted from bosectl (Copyright (c)
// aaronsb, MIT license).

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <optional>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/signalfd.h>

#include "BmapProtocol.hpp"
#include "BluetoothManager.hpp"
#include "BluezWatcher.hpp"
#include "BmapLink.hpp"
#include "StateEngine.hpp"
#include "IpcServer.hpp"

namespace {

using namespace omarchy::bose;

constexpr const char* DAEMON_VERSION = "0.1.0";
constexpr const char* DEFAULT_DEVICE_NAME = "Bose NC Headphones 700";

// Bose has no documented event subscription on this firmware, so battery and
// CNC level are refreshed with light polling.
constexpr auto kStatusPollInterval = std::chrono::seconds(60);

struct DaemonOptions {
    bool showHelp{false};
    bool showVersion{false};
    bool mockMode{false};
    std::string preferredMac;
    std::string stateDir;
    std::string runtimeDir;
    int channel{-1};
};

void printHelp(const char* progName) {
    std::cout << "Usage: " << progName << " [OPTIONS]\n\n"
              << "Headless background daemon managing Bose NC Headphones 700 on Linux.\n\n"
              << "Options:\n"
              << "  -h, --help               Display this help message and exit\n"
              << "  -v, --version            Display version information and exit\n"
              << "  -m, --mock               Run in mock simulation mode (completely offline)\n"
              << "      --mac <MAC>          Target specific Bluetooth MAC address\n"
              << "  -d, --device <MAC>       Alias for --mac\n"
              << "      --channel <N>        Override the RFCOMM channel (default: 8, SDP-resolved)\n"
              << "      --state-dir <DIR>    Override directory for status.json\n"
              << "      --runtime-dir <DIR>  Override directory for IPC socket\n\n";
}

void printVersion() {
    std::cout << "bose-700-daemon " << DAEMON_VERSION << "\n";
}

std::optional<DaemonOptions> parseCommandLine(int argc, char* argv[]) {
    DaemonOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            opts.showHelp = true;
            return opts;
        } else if (arg == "-v" || arg == "--version") {
            opts.showVersion = true;
            return opts;
        } else if (arg == "-m" || arg == "--mock") {
            opts.mockMode = true;
        } else if (arg == "--mac" || arg == "-d" || arg == "--device") {
            if (i + 1 < argc) {
                opts.preferredMac = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires a MAC address argument\n";
                return std::nullopt;
            }
        } else if (arg == "--channel") {
            if (i + 1 < argc) {
                opts.channel = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: --channel requires a channel number\n";
                return std::nullopt;
            }
        } else if (arg == "--state-dir") {
            if (i + 1 < argc) {
                opts.stateDir = argv[++i];
            } else {
                std::cerr << "Error: --state-dir requires a directory path\n";
                return std::nullopt;
            }
        } else if (arg == "--runtime-dir") {
            if (i + 1 < argc) {
                opts.runtimeDir = argv[++i];
            } else {
                std::cerr << "Error: --runtime-dir requires a directory path\n";
                return std::nullopt;
            }
        } else {
            std::cerr << "Error: Unrecognized option '" << arg << "'\n";
            return std::nullopt;
        }
    }
    return opts;
}

std::string resolveStateDir(const std::string& overrideDir) {
    if (!overrideDir.empty()) {
        return overrideDir;
    }
    const char* xdgState = std::getenv("XDG_STATE_HOME");
    if (xdgState && xdgState[0] != '\0') {
        return std::string(xdgState);
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.local/state";
    }
    return "/tmp";
}

std::string resolveRuntimeDir(const std::string& overrideDir) {
    if (!overrideDir.empty()) {
        return overrideDir;
    }
    const char* xdgRun = std::getenv("XDG_RUNTIME_DIR");
    if (xdgRun && xdgRun[0] != '\0') {
        return std::string(xdgRun);
    }
    return "/tmp/run-" + std::to_string(::getuid());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Main Daemon Entry Point
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Restrict default permissions on all newly created files and directories
    ::umask(0077);

    auto optsOpt = parseCommandLine(argc, argv);
    if (!optsOpt) {
        return 1;
    }
    const auto& opts = *optsOpt;

    if (opts.showHelp) {
        printHelp(argv[0]);
        return 0;
    }
    if (opts.showVersion) {
        printVersion();
        return 0;
    }

    // 1. Resolve paths
    std::string stateDir = resolveStateDir(opts.stateDir);
    std::string runtimeDir = resolveRuntimeDir(opts.runtimeDir);

    // 2. Setup Linux signalfd (ignoring SIGPIPE, blocking SIGINT/TERM/USR1/USR2)
    ::signal(SIGPIPE, SIG_IGN);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);

    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        std::cerr << "Fatal: Failed to mask signals: " << std::strerror(errno) << "\n";
        return 1;
    }

    int sigFd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigFd < 0) {
        std::cerr << "Fatal: Failed to create signalfd: " << std::strerror(errno) << "\n";
        return 1;
    }

    // 3. Initialize StateEngine
    StateEngine stateEngine(stateDir);
    if (!stateEngine.initialize(opts.mockMode)) {
        std::cerr << "Fatal: Failed to initialize StateEngine at " << stateDir << "\n";
        ::close(sigFd);
        return 1;
    }

    // In mock mode, ensure initial standard state
    if (opts.mockMode) {
        stateEngine.setConnected(true);
        stateEngine.setDeviceInfo("Panthère", "1.8.2-11524+e0f7590", "4C:87:5D:A3:D1:4F");
        stateEngine.setBatteryLevel(87);
        stateEngine.setCnc(0, 10);
        stateEngine.setEq(0, 0, 0, -10, 10);
        stateEngine.setSidetone("medium");
        stateEngine.setVoicePrompts(false, "French");
        stateEngine.setMultipoint(true);
    }

    // 4. Initialize BluetoothManager
    BluetoothConfig btConfig;
    btConfig.preferredMac = opts.preferredMac;
    btConfig.autoReconnect = true;
    btConfig.mockMode = opts.mockMode;
    if (opts.channel > 0 && opts.channel <= 30) {
        btConfig.defaultChannel = static_cast<uint8_t>(opts.channel);
    }

    int mockPeerFd = -1;
    std::unique_ptr<BluetoothManager> btManager;
    if (opts.mockMode) {
        btManager = BluetoothManager::createMock(btConfig, &mockPeerFd);
    } else {
        btManager = BluetoothManager::createLinux(btConfig);
    }

    if (!btManager) {
        std::cerr << "Fatal: Failed to create BluetoothManager\n";
        stateEngine.cleanup();
        ::close(sigFd);
        return 1;
    }

    // BOSE_700_DEBUG=1 logs every raw packet received, before dispatch.
    const char* debugEnv = std::getenv("BOSE_700_DEBUG");
    const bool verboseBmap = debugEnv && debugEnv[0] == '1';

    BmapLink link;

    auto connected = [&]() {
        return btManager && btManager->getState() == ConnectionState::CONNECTED;
    };

    // Blocking BMAP round-trip helpers. On a dead link they schedule a
    // reconnect and fail the caller's command.
    auto transact = [&](uint8_t fblock, uint8_t func, bmap::Operator op,
                        const std::vector<uint8_t>& payload,
                        bmap::BmapResponse& out, std::string& err) -> bool {
        bool linkDead = false;
        bool ok = link.transact(*btManager, fblock, func, op, payload, out, err, linkDead);
        if (linkDead) {
            btManager->dropLink(err);
        }
        return ok;
    };

    auto bmapGet = [&](uint8_t fblock, uint8_t func, std::vector<uint8_t>& payload, std::string& err) -> bool {
        bmap::BmapResponse resp;
        if (!transact(fblock, func, bmap::Operator::Get, {}, resp, err)) {
            return false;
        }
        payload = std::move(resp.payload);
        return true;
    };

    // Applies one device payload to the persisted state.
    auto applyPayload = [&](uint8_t fblock, uint8_t func, const std::vector<uint8_t>& p) {
        namespace g = bmap::goodyear;
        if (fblock == g::kBatteryFblock && func == g::kBatteryFunc && !p.empty()) {
            stateEngine.setBatteryLevel(bmap::parse_battery(p));
        } else if (fblock == g::kCncFblock && func == g::kCncFunc && p.size() >= 3) {
            auto [cur, max] = bmap::parse_cnc(p);
            stateEngine.setCnc(cur, max);
        } else if (fblock == g::kEqFblock && func == g::kEqFunc) {
            auto bands = bmap::parse_eq(p);
            if (!bands.empty()) {
                int bass = 0, mid = 0, treble = 0, mn = -10, mx = 10;
                for (const auto& b : bands) {
                    if (b.band_id == 0) { bass = b.current; mn = b.min_val; mx = b.max_val; }
                    else if (b.band_id == 1) mid = b.current;
                    else if (b.band_id == 2) treble = b.current;
                }
                stateEngine.setEq(bass, mid, treble, mn, mx);
            }
        } else if (fblock == g::kSidetoneFblock && func == g::kSidetoneFunc && p.size() >= 2) {
            stateEngine.setSidetone(bmap::parse_sidetone(p));
        } else if (fblock == g::kPromptsFblock && func == g::kPromptsFunc && !p.empty()) {
            auto [enabled, lang] = bmap::parse_voice_prompts(p);
            stateEngine.setVoicePrompts(enabled, lang);
        } else if (fblock == g::kMultipointFblock && func == g::kMultipointFunc && !p.empty()) {
            stateEngine.setMultipoint(bmap::parse_multipoint(p));
        } else if (fblock == g::kNameFblock && func == g::kNameFunc && p.size() > 1) {
            stateEngine.setDeviceName(bmap::parse_product_name(p));
        } else if (fblock == g::kFirmwareFblock && func == g::kFirmwareFunc && !p.empty()) {
            stateEngine.setFirmware(bmap::parse_firmware(p));
        }
    };

    // Unsolicited packets (headset-side changes, late replies) update state.
    link.setUnsolicitedHandler([&](const bmap::BmapResponse& resp) {
        if (verboseBmap) {
            fprintf(stderr, "[BMAP] RX unsolicited %s\n", resp.fmt().c_str());
            fflush(stderr);
        }
        if (resp.op == bmap::Operator::Status) {
            applyPayload(resp.fblock, resp.func, resp.payload);
        }
    });

    // Refreshes one state field from the headset; failures only get logged.
    auto refreshField = [&](uint8_t fblock, uint8_t func, const char* what) {
        std::vector<uint8_t> payload;
        std::string err;
        if (bmapGet(fblock, func, payload, err)) {
            applyPayload(fblock, func, payload);
            return true;
        }
        fprintf(stderr, "[BMAP] GET %s failed: %s\n", what, err.c_str());
        fflush(stderr);
        return false;
    };

    // SETGET with a short answer window: on this firmware (1.8.2) the Settings
    // block applies every SETGET, but [1.5] CNC and [1.3] voice-prompts never
    // send a reply, while [1.7] EQ, [1.10] multipoint and [1.11] sidetone
    // answer STATUS. A response timeout is therefore NOT a failure here; the
    // subsequent read-back GET is what confirms the write.
    auto bmapSetGet = [&](uint8_t fblock, uint8_t func, const std::vector<uint8_t>& payload,
                          std::string& err) -> bool {
        bmap::BmapResponse resp;
        bool linkDead = false;
        bool ok = link.transact(*btManager, fblock, func, bmap::Operator::SetGet,
                                payload, resp, err, linkDead, 600);
        if (linkDead) {
            btManager->dropLink(err);
            return false;
        }
        if (ok) {
            applyPayload(fblock, func, resp.payload);
            return true;
        }
        if (err == "response timeout") {
            err.clear();
            return true; // silent-write firmware quirk; read-back verifies
        }
        return false;
    };

    // Event-driven ACL link tracking (replaces the periodic bluetoothctl
    // state scrape while the headset is paired but not connected). Optional:
    // without a working system bus we fall back to timer-based polling.
    BluezWatcher bluezWatcher;
    if (!opts.mockMode) {
        if (bluezWatcher.start()) {
            bluezWatcher.setTargetMac(opts.preferredMac); // empty until first discovery
            bluezWatcher.setCallback([&](bool aclConnected) {
                btManager->onBluezConnectedChange(aclConnected);
            });
        } else {
            fprintf(stderr, "[BT] BlueZ D-Bus watcher unavailable (%s); "
                            "falling back to %u ms polling\n",
                    bluezWatcher.getLastError().c_str(), btConfig.discoveryRetryMs);
            fflush(stderr);
        }
    }

    BluetoothCallbacks callbacks;
    callbacks.onConnected = [&]() {
        link.reset();
        const auto& dev = btManager->getCurrentDevice();
        bluezWatcher.setTargetMac(dev.macAddress);
        const std::string name = dev.name.empty() ? DEFAULT_DEVICE_NAME : dev.name;
        stateEngine.setConnected(true);
        stateEngine.setDeviceInfo(name, "", dev.macAddress);
        stateEngine.save();
        fprintf(stderr, "[DAEMON] Headset connected (%s, %s)\n", name.c_str(), dev.macAddress.c_str());
        fflush(stderr);

        if (!opts.mockMode) {
            // Initial sync: read everything the panel displays. Failures of
            // individual fields only leave them omitted from status.json.
            namespace g = bmap::goodyear;
            refreshField(g::kNameFblock, g::kNameFunc, "name");
            refreshField(g::kFirmwareFblock, g::kFirmwareFunc, "firmware");
            refreshField(g::kBatteryFblock, g::kBatteryFunc, "battery");
            refreshField(g::kCncFblock, g::kCncFunc, "cnc");
            refreshField(g::kEqFblock, g::kEqFunc, "eq");
            refreshField(g::kPromptsFblock, g::kPromptsFunc, "voice prompts");
            refreshField(g::kMultipointFblock, g::kMultipointFunc, "multipoint");
            refreshField(g::kSidetoneFblock, g::kSidetoneFunc, "sidetone");
        }
    };

    callbacks.onDisconnected = [&](const std::string& reason) {
        link.reset();
        stateEngine.setConnected(false);
        stateEngine.save();
        fprintf(stderr, "[DAEMON] Headset disconnected: %s\n", reason.c_str());
        fflush(stderr);
    };

    callbacks.onDataReceived = [&](const uint8_t* data, size_t length) {
        if (verboseBmap) {
            fprintf(stderr, "[BMAP] RX raw %zu bytes:", length);
            for (size_t i = 0; i < length; ++i) fprintf(stderr, " %02x", data[i]);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        link.feed(data, length);
    };

    btManager->setCallbacks(std::move(callbacks));
    btManager->start();

    // 5. Initialize UNIX Domain Socket IPC Server
    //
    // With no explicit --runtime-dir the server resolves its own path, which
    // also lets it adopt the listening socket systemd holds for the session
    // (bose-700.socket) rather than binding the path itself. An explicit
    // override always binds, so tests and manual runs stay predictable.
    std::string socketPath = runtimeDir + "/bose-700.sock";
    IpcServer ipcServer(opts.runtimeDir.empty() ? std::string() : socketPath);

    // Shared guard for write commands: an idle daemon is not connected.
    auto requireConnected = [&](std::string& err) -> bool {
        if (opts.mockMode) return true;
        if (!connected()) {
            err = "headset not connected";
            return false;
        }
        return true;
    };

    IpcCallbacks ipcCb;
    ipcCb.getStatusJson = [&]() {
        return stateEngine.getStatusJson();
    };
    ipcCb.setCnc = [&](int level, std::string& err) {
        if (!requireConnected(err)) return false;
        fprintf(stderr, "[DAEMON] CNC level: %d\n", level);
        fflush(stderr);
        if (opts.mockMode) {
            stateEngine.setCnc(level, 10);
            return true;
        }
        namespace g = bmap::goodyear;
        if (!bmapSetGet(g::kCncFblock, g::kCncFunc,
                        bmap::build_cnc(static_cast<uint8_t>(level)), err)) {
            return false;
        }
        // Read back what the headset actually applied.
        if (!refreshField(g::kCncFblock, g::kCncFunc, "cnc")) {
            err = "cnc write sent but read-back failed";
            return false;
        }
        const int applied = stateEngine.getState().cncLevel;
        if (applied != level) {
            err = "headset did not apply cnc " + std::to_string(level) +
                  " (still " + std::to_string(applied) + ")";
            return false;
        }
        return true;
    };
    ipcCb.setEq = [&](int bass, int mid, int treble, std::string& err) {
        if (!requireConnected(err)) return false;
        fprintf(stderr, "[DAEMON] EQ: bass=%d mid=%d treble=%d\n", bass, mid, treble);
        fflush(stderr);
        if (opts.mockMode) {
            stateEngine.setEq(bass, mid, treble, -10, 10);
            return true;
        }
        namespace g = bmap::goodyear;
        // One SETGET per band: {signed value, band id}; band 0/1/2 = bass/mid/treble.
        const std::array<std::pair<uint8_t, int>, 3> bands{{
            {0, bass}, {1, mid}, {2, treble}
        }};
        for (const auto& [bandId, value] : bands) {
            if (!bmapSetGet(g::kEqFblock, g::kEqFunc,
                            bmap::build_eq_band(static_cast<int8_t>(value), bandId), err)) {
                refreshField(g::kEqFblock, g::kEqFunc, "eq");
                return false;
            }
        }
        if (!refreshField(g::kEqFblock, g::kEqFunc, "eq")) {
            err = "eq write sent but read-back failed";
            return false;
        }
        const auto s = stateEngine.getState();
        if (s.eqBass != bass || s.eqMid != mid || s.eqTreble != treble) {
            err = "headset did not fully apply the eq";
            return false;
        }
        return true;
    };
    ipcCb.setSidetone = [&](const std::string& level, std::string& err) {
        if (!requireConnected(err)) return false;
        auto id = bmap::sidetone_id_from_name(level);
        if (!id) {
            err = "invalid sidetone level '" + level + "'";
            return false;
        }
        fprintf(stderr, "[DAEMON] Sidetone: %s\n", level.c_str());
        fflush(stderr);
        if (opts.mockMode) {
            stateEngine.setSidetone(level);
            return true;
        }
        namespace g = bmap::goodyear;
        if (!bmapSetGet(g::kSidetoneFblock, g::kSidetoneFunc,
                        bmap::build_sidetone(*id), err)) {
            return false;
        }
        if (!refreshField(g::kSidetoneFblock, g::kSidetoneFunc, "sidetone")) {
            err = "sidetone write sent but read-back failed";
            return false;
        }
        if (stateEngine.getState().sidetone != level) {
            err = "headset did not apply sidetone '" + level + "'";
            return false;
        }
        return true;
    };
    ipcCb.setPrompts = [&](bool enabled, std::string& err) {
        if (!requireConnected(err)) return false;
        fprintf(stderr, "[DAEMON] Voice prompts: %s\n", enabled ? "on" : "off");
        fflush(stderr);
        if (opts.mockMode) {
            const auto s = stateEngine.getState();
            stateEngine.setVoicePrompts(enabled, s.promptsLanguage);
            return true;
        }
        namespace g = bmap::goodyear;
        // Read-modify-write: byte0 holds the language in its low 5 bits.
        std::vector<uint8_t> current;
        if (!bmapGet(g::kPromptsFblock, g::kPromptsFunc, current, err)) {
            return false;
        }
        const uint8_t byte0 = current.empty() ? 0 : current[0];
        if (!bmapSetGet(g::kPromptsFblock, g::kPromptsFunc,
                        bmap::build_voice_prompts(enabled, byte0), err)) {
            return false;
        }
        if (!refreshField(g::kPromptsFblock, g::kPromptsFunc, "voice prompts")) {
            err = "voice prompts write sent but read-back failed";
            return false;
        }
        if (stateEngine.getState().promptsEnabled != enabled) {
            err = "headset did not apply voice prompts change";
            return false;
        }
        return true;
    };
    ipcCb.setMultipoint = [&](bool enabled, std::string& err) {
        if (!requireConnected(err)) return false;
        fprintf(stderr, "[DAEMON] Multipoint: %s\n", enabled ? "on" : "off");
        fflush(stderr);
        if (opts.mockMode) {
            stateEngine.setMultipoint(enabled);
            return true;
        }
        namespace g = bmap::goodyear;
        if (!bmapSetGet(g::kMultipointFblock, g::kMultipointFunc,
                        bmap::build_multipoint(enabled), err)) {
            return false;
        }
        if (!refreshField(g::kMultipointFblock, g::kMultipointFunc, "multipoint")) {
            err = "multipoint write sent but read-back failed";
            return false;
        }
        if ((stateEngine.getState().multipoint == 1) != enabled) {
            err = "headset did not apply multipoint change";
            return false;
        }
        return true;
    };
    ipcCb.reconnect = [&](std::string& err) {
        (void)err;
        fprintf(stderr, "[DAEMON] Reconnect requested via IPC\n");
        fflush(stderr);
        link.reset();
        stateEngine.setConnected(false);
        stateEngine.save();
        if (!opts.mockMode) {
            btManager->disconnect();
            btManager->start();
        } else {
            stateEngine.setConnected(true);
            stateEngine.save();
        }
        return true;
    };

    ipcServer.setCallbacks(std::move(ipcCb));

    if (!ipcServer.start()) {
        std::cerr << "Fatal: Failed to start IPC server at " << socketPath << "\n";
        btManager->stop();
        stateEngine.cleanup();
        ::close(sigFd);
        return 1;
    }

    // 6. Signal daemon readiness
    if (opts.mockMode) {
        std::cout << "[MOCK_DAEMON] Ready PID=" << ::getpid() << std::endl;
    } else {
        std::cout << "[DAEMON] Ready PID=" << ::getpid() << std::endl;
    }

    // 7. Unified Event Loop
    bool running = true;
    auto lastStatusPoll = std::chrono::steady_clock::now();
    std::string lastPublishedStatus;

    while (running) {
        std::vector<struct pollfd> pfds;

        // Entry 0: Signal descriptor
        pfds.push_back({sigFd, POLLIN, 0});

        // Entry 1 (Optional): Bluetooth transport descriptor
        int btFd = btManager->getPollFd();
        short btEvents = btManager->getPollEvents();
        int btIndex = -1;
        if (btFd >= 0 && btEvents != 0) {
            btIndex = static_cast<int>(pfds.size());
            pfds.push_back({btFd, btEvents, 0});
        }

        // Entry 2 (Optional): sd-bus fd of the BlueZ link-state watcher
        int busIndex = -1;
        int busFd = bluezWatcher.getPollFd();
        short busEvents = bluezWatcher.getPollEvents();
        if (busFd >= 0 && busEvents != 0) {
            busIndex = static_cast<int>(pfds.size());
            pfds.push_back({busFd, busEvents, 0});
        }

        // Entries 3+: IPC listen socket and connected client sockets
        size_t ipcStartIndex = pfds.size();
        ipcServer.appendPollFds(pfds);

        // Poll with 100ms timeout for periodic BluetoothManager tick, lowered
        // to whatever the sd-bus connection asks for while the watcher lives.
        int pollTimeoutMs = 100;
        if (bluezWatcher.isActive()) {
            const int busTimeout = bluezWatcher.getPollTimeoutMs();
            if (busTimeout >= 0 && busTimeout < pollTimeoutMs) {
                pollTimeoutMs = busTimeout;
            }
        }
        int pollRc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), pollTimeoutMs);
        if (pollRc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Event loop poll error: " << std::strerror(errno) << "\n";
            break;
        }

        // Check Signal Descriptor
        if (pfds[0].revents & POLLIN) {
            struct signalfd_siginfo fdsi{};
            ssize_t s = ::read(sigFd, &fdsi, sizeof(fdsi));
            if (s == sizeof(fdsi)) {
                if (fdsi.ssi_signo == SIGINT || fdsi.ssi_signo == SIGTERM) {
                    running = false;
                    break;
                } else if (fdsi.ssi_signo == SIGUSR1) {
                    // Simulated disconnect
                    stateEngine.setConnected(false);
                    stateEngine.save();
                    if (!opts.mockMode) {
                        btManager->disconnect();
                        btManager->start();
                    }
                } else if (fdsi.ssi_signo == SIGUSR2) {
                    // Simulated reconnect
                    stateEngine.setConnected(true);
                    stateEngine.save();
                    if (!opts.mockMode) {
                        btManager->start();
                    }
                }
            }
        }

        // Check Bluetooth Transport Descriptor
        if (btIndex >= 0 && (pfds[btIndex].revents != 0)) {
            btManager->handleSocketEvent(pfds[btIndex].revents);
        }

        // Check the BlueZ watcher's sd-bus descriptor (also drains its
        // internal timers on timeout; a bus error degrades to polling).
        if (busIndex >= 0 && (pfds[busIndex].revents != 0 || bluezWatcher.getPollTimeoutMs() == 0)) {
            bluezWatcher.process();
        }

        // Check IPC Descriptors
        for (size_t i = ipcStartIndex; i < pfds.size(); ++i) {
            if (pfds[i].revents != 0) {
                ipcServer.handleSocketEvent(pfds[i].fd, pfds[i].revents);
            }
        }

        // Subsystem periodic tick (reconnect backoff). While the BlueZ watcher
        // is tracking a paired-but-ACL-down headset there is no timer polling
        // at all: the Connected event drives the reconnect, and the only
        // scheduled work left is the throttled `bluetoothctl connect` nudge
        // that wakes a paired but idle headset.
        if (bluezWatcher.isActive() && btManager->waitingForAclLink()) {
            // Keep the watcher aimed at the headset even if it was parked
            // before ever connecting (target MAC is otherwise only learned
            // in onConnected).
            bluezWatcher.setTargetMac(btManager->getCurrentDevice().macAddress);
            // Throttled internally to connectRequestIntervalMs.
            btManager->requestConnectWakeUp();
        } else {
            btManager->tick();
        }

        // Light polling: battery + CNC level (the two values that drift on
        // their own) every kStatusPollInterval.
        const auto now = std::chrono::steady_clock::now();
        if (!opts.mockMode && connected() && now - lastStatusPoll >= kStatusPollInterval) {
            lastStatusPoll = now;
            namespace g = bmap::goodyear;
            refreshField(g::kBatteryFblock, g::kBatteryFunc, "battery");
            if (connected()) {
                refreshField(g::kCncFblock, g::kCncFunc, "cnc");
            }
        }

        // Push the state to subscribed clients whenever it changes, so the bar
        // widget never has to read the state file. The comparison runs at most
        // once per 100 ms poll cycle and only sends when something differs.
        if (ipcServer.getSubscriberCount() > 0) {
            std::string statusJson = stateEngine.getStatusJson();
            if (statusJson != lastPublishedStatus) {
                lastPublishedStatus = statusJson;
                ipcServer.broadcastStatus(statusJson);
            }
        }
    }

    // 8. Graceful Shutdown & Resource Cleanup
    if (opts.mockMode) {
        std::cout << "[MOCK_DAEMON] Shutdown cleanly\n";
    } else {
        std::cout << "[DAEMON] Shutdown cleanly\n";
    }
    std::cout.flush();

    btManager->stop();
    ipcServer.stop();
    stateEngine.cleanup();

    if (mockPeerFd >= 0) {
        ::close(mockPeerFd);
        mockPeerFd = -1;
    }
    if (sigFd >= 0) {
        ::close(sigFd);
        sigFd = -1;
    }

    return 0;
}
