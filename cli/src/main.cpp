// bose-700-ctl: thin command-line client for bose-700-daemon's UNIX socket.
// Client scaffolding adapted from omarchy-sony-xm3 (Copyright (c) Kevin
// Cardwell, MIT license); the daemon answers with one JSON object per line.
//
// Exit codes: 0 = ok, 1 = bad arguments or daemon-reported error,
//             2 = daemon unreachable.

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <limits>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

std::string to_lower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool parse_int(const std::string& str, int& out) {
    if (str.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long val = std::strtol(str.c_str(), &end, 10);
    if (errno != 0 || end == str.c_str() || *end != '\0') {
        return false;
    }
    if (val < std::numeric_limits<int>::min() || val > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(val);
    return true;
}

bool is_negative_number(const std::string& str) {
    if (str.size() < 2 || str[0] != '-') {
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(str[1])) != 0;
}

void print_usage(std::ostream& os) {
    os << "Usage: bose-700-ctl [-s <socket>] <subcommand> [args...]\n\n"
       << "Subcommands:\n"
       << "  status                       Print the full headset state as JSON\n"
       << "  cnc <0-10>                   Noise cancelling level (0 = passthrough, 10 = max ANC)\n"
       << "  eq <bass> <mid> <treble>     EQ bands, each -10..10\n"
       << "  sidetone <off|low|medium|high>  Hear your own voice during calls\n"
       << "  prompts <on|off>             Spoken voice prompts\n"
       << "  multipoint <on|off>          Connect to two devices at once\n"
       << "  reconnect                    Drop and re-establish the RFCOMM link\n\n"
       << "Options:\n"
       << "  -s, --socket <path>  Override socket path\n"
       << "  -h, --help           Show help\n"
       << "  -v, --version        Show version\n";
}

std::string get_default_socket_path() {
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime != nullptr && *xdg_runtime != '\0') {
        return std::string(xdg_runtime) + "/bose-700.sock";
    }
    return "/tmp/run-" + std::to_string(::getuid()) + "/bose-700.sock";
}

int send_command(const std::string& socket_path, const std::string& command_str, bool is_status) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Error: Cannot create socket (" << std::strerror(errno) << ")\n";
        return 1;
    }

    // Writes go through a BMAP round-trip on the headset, which can take a
    // moment; allow well over the daemon's own response timeout.
    struct timeval tv{};
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "Error: Socket path too long: " << socket_path << "\n";
        ::close(fd);
        return 1;
    }
    std::memcpy(addr.sun_path, socket_path.data(), socket_path.size());
    addr.sun_path[socket_path.size()] = '\0';

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Error: Cannot connect to daemon socket (" << std::strerror(errno) << ")\n";
        ::close(fd);
        return 2;
    }

    std::string payload = command_str;
    if (payload.empty() || payload.back() != '\n') {
        payload.push_back('\n');
    }

    size_t total_sent = 0;
    while (total_sent < payload.size()) {
        ssize_t sent = ::send(fd, payload.data() + total_sent, payload.size() - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: Socket communication failed (" << std::strerror(errno) << ")\n";
            ::close(fd);
            return 1;
        }
        if (sent == 0) {
            std::cerr << "Error: Socket communication failed (connection closed)\n";
            ::close(fd);
            return 1;
        }
        total_sent += static_cast<size_t>(sent);
    }

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            response.append(buf, static_cast<size_t>(n));
            if (response.find('\n') != std::string::npos) {
                break;
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: Socket communication failed (" << std::strerror(errno) << ")\n";
            ::close(fd);
            return 1;
        }
    }
    ::close(fd);

    size_t newline_pos = response.find('\n');
    if (newline_pos != std::string::npos) {
        response = response.substr(0, newline_pos);
    }
    while (!response.empty() && (response.back() == '\r' || response.back() == ' ' || response.back() == '\t')) {
        response.pop_back();
    }

    if (response.empty()) {
        std::cerr << "Error: Empty response from daemon\n";
        return 1;
    }

    // The daemon answers {"ok":true,...} or {"ok":false,"error":"..."}.
    if (response.find("\"ok\":true") != std::string::npos) {
        std::cout << response << "\n";
        return 0;
    }

    std::cerr << "Error from daemon: " << response << "\n";
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string socket_path;
    bool show_help = false;
    bool show_version = false;
    std::vector<std::string> remaining;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            show_help = true;
        } else if (arg == "-v" || arg == "--version") {
            show_version = true;
        } else if (arg == "-s" || arg == "--socket") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a socket path argument\n";
                return 1;
            }
            socket_path = argv[++i];
        } else if (arg.rfind("--socket=", 0) == 0) {
            socket_path = arg.substr(9);
        } else if (arg == "--") {
            for (++i; i < argc; ++i) {
                remaining.emplace_back(argv[i]);
            }
            break;
        } else if (!arg.empty() && arg[0] == '-' && !is_negative_number(arg) && remaining.empty()) {
            std::cerr << "Error: Unknown option '" << arg << "'\n";
            print_usage(std::cerr);
            return 1;
        } else {
            remaining.push_back(arg);
        }
    }

    if (show_help) {
        print_usage(std::cout);
        return 0;
    }

    if (show_version) {
        std::cout << "bose-700-ctl 0.1.0\n";
        return 0;
    }

    if (remaining.empty()) {
        print_usage(std::cerr);
        return 1;
    }

    if (socket_path.empty()) {
        socket_path = get_default_socket_path();
    }

    std::string subcmd = to_lower(remaining[0]);

    if (subcmd == "status") {
        return send_command(socket_path, "status\n", true);
    }

    if (subcmd == "cnc") {
        int level = 0;
        if (remaining.size() < 2 || !parse_int(remaining[1], level) || level < 0 || level > 10) {
            std::cerr << "Error: 'cnc' requires an integer between 0 and 10\n";
            return 1;
        }
        return send_command(socket_path, "cnc " + std::to_string(level) + "\n", false);
    }

    if (subcmd == "eq") {
        if (remaining.size() < 4) {
            std::cerr << "Error: 'eq' requires 3 band values: eq <bass> <mid> <treble> (each -10..10)\n";
            return 1;
        }
        std::array<int, 3> bands{};
        for (size_t i = 0; i < 3; ++i) {
            if (!parse_int(remaining[1 + i], bands[i])) {
                std::cerr << "Error: EQ values must be valid integers\n";
                return 1;
            }
            if (bands[i] < -10 || bands[i] > 10) {
                std::cerr << "Error: EQ values must be between -10 and 10\n";
                return 1;
            }
        }
        std::string cmd = "eq " + std::to_string(bands[0]) + " "
                                + std::to_string(bands[1]) + " "
                                + std::to_string(bands[2]) + "\n";
        return send_command(socket_path, cmd, false);
    }

    if (subcmd == "sidetone") {
        static const std::vector<std::string> valid = {"off", "low", "medium", "high"};
        if (remaining.size() < 2 ||
            std::find(valid.begin(), valid.end(), to_lower(remaining[1])) == valid.end()) {
            std::cerr << "Error: 'sidetone' requires one of: off, low, medium, high\n";
            return 1;
        }
        return send_command(socket_path, "sidetone " + to_lower(remaining[1]) + "\n", false);
    }

    if (subcmd == "prompts" || subcmd == "multipoint") {
        if (remaining.size() < 2) {
            std::cerr << "Error: '" << subcmd << "' requires 'on' or 'off'\n";
            return 1;
        }
        std::string val = to_lower(remaining[1]);
        if (val != "on" && val != "off") {
            std::cerr << "Error: '" << subcmd << "' requires 'on' or 'off'\n";
            return 1;
        }
        return send_command(socket_path, subcmd + " " + val + "\n", false);
    }

    if (subcmd == "reconnect") {
        return send_command(socket_path, "reconnect\n", false);
    }

    std::cerr << "Error: Unknown subcommand '" << remaining[0] << "'\n";
    return 1;
}
