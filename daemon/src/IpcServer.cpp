// UNIX-domain-socket IPC server implementation.
// Socket lifecycle/framing adapted from omarchy-sony-xm3 (Copyright (c)
// Kevin Cardwell, MIT license); responses are JSON per the bose-700 IPC contract.

#include "IpcServer.hpp"
#include "StateEngine.hpp" // jsonEscape

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace omarchy::bose {

namespace {

std::string trim(const std::string& str) {
    size_t start = 0;
    while (start < str.size() && (str[start] == ' ' || str[start] == '\t' || str[start] == '\r' || str[start] == '\n')) {
        start++;
    }
    if (start == str.size()) return "";
    size_t end = str.size();
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\r' || str[end - 1] == '\n')) {
        end--;
    }
    return str.substr(start, end - start);
}

std::string toLower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

bool parseInt(const std::string& str, int& outVal) {
    if (str.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long val = std::strtol(str.c_str(), &end, 10);
    if (errno != 0 || end == str.c_str() || *end != '\0') {
        return false;
    }
    outVal = static_cast<int>(val);
    return true;
}

std::string okJson() {
    return "{\"ok\":true}\n";
}

std::string errJson(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Path Resolution
// ---------------------------------------------------------------------------

// Must stay in step with get_default_socket_path() in cli/src/main.cpp: the two
// halves only find each other if they agree on this name.
static constexpr const char* kSocketName = "bose-700.sock";

std::string IpcServer::resolveSocketPath(const std::string& overridePath) {
    if (!overridePath.empty()) {
        std::filesystem::path p(overridePath);
        if (p.extension() == ".sock") {
            return overridePath;
        }
        return (p / kSocketName).string();
    }

    const char* xdgRuntime = std::getenv("XDG_RUNTIME_DIR");
    if (xdgRuntime != nullptr && *xdgRuntime != '\0') {
        return std::string(xdgRuntime) + "/" + kSocketName;
    }

    // Fallback: /tmp/bose-700-$UID.sock
    return "/tmp/bose-700-" + std::to_string(::getuid()) + ".sock";
}

// ---------------------------------------------------------------------------
// Construction & Lifecycle
// ---------------------------------------------------------------------------

IpcServer::IpcServer(std::string socketPath)
    : socketPathConfig_(std::move(socketPath)) {
    actualSocketPath_ = resolveSocketPath(socketPathConfig_);
}

IpcServer::~IpcServer() {
    stop();
}

IpcServer::IpcServer(IpcServer&& other) noexcept
    : socketPathConfig_(std::move(other.socketPathConfig_)),
      actualSocketPath_(std::move(other.actualSocketPath_)),
      listenFd_(other.listenFd_),
      running_(other.running_),
      maxLineLength_(other.maxLineLength_),
      customHandler_(std::move(other.customHandler_)),
      callbacks_(std::move(other.callbacks_)),
      clients_(std::move(other.clients_)) {
    other.listenFd_ = -1;
    other.running_ = false;
}

IpcServer& IpcServer::operator=(IpcServer&& other) noexcept {
    if (this != &other) {
        stop();
        socketPathConfig_ = std::move(other.socketPathConfig_);
        actualSocketPath_ = std::move(other.actualSocketPath_);
        listenFd_ = other.listenFd_;
        running_ = other.running_;
        maxLineLength_ = other.maxLineLength_;
        customHandler_ = std::move(other.customHandler_);
        callbacks_ = std::move(other.callbacks_);
        clients_ = std::move(other.clients_);

        other.listenFd_ = -1;
        other.running_ = false;
    }
    return *this;
}

bool IpcServer::start() {
    if (running_) {
        return true;
    }

    if (actualSocketPath_.empty()) {
        actualSocketPath_ = resolveSocketPath(socketPathConfig_);
    }

    // 1. Ensure parent directory exists with mode 0700
    std::filesystem::path sockPath(actualSocketPath_);
    if (sockPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(sockPath.parent_path(), ec);
        if (!ec) {
            ::chmod(sockPath.parent_path().c_str(), 0700);
        }
    }

    // 2. Unlink any stale existing socket file
    ::unlink(actualSocketPath_.c_str());

    // 3. Create non-blocking stream socket
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        return false;
    }

    // 4. Bind socket
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (actualSocketPath_.length() >= sizeof(addr.sun_path)) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, actualSocketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // 5. Enforce 0700 socket permissions
    if (::chmod(actualSocketPath_.c_str(), 0700) < 0) {
        ::unlink(actualSocketPath_.c_str());
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // 6. Listen
    if (::listen(listenFd_, SOMAXCONN) < 0) {
        ::unlink(actualSocketPath_.c_str());
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_ = true;
    return true;
}

void IpcServer::stop() {
    if (!running_ && listenFd_ < 0 && clients_.empty()) {
        return;
    }

    for (auto& [fd, session] : clients_) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    clients_.clear();

    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }

    if (!actualSocketPath_.empty()) {
        ::unlink(actualSocketPath_.c_str());
    }

    running_ = false;
}

// ---------------------------------------------------------------------------
// Client Management & I/O
// ---------------------------------------------------------------------------

void IpcServer::acceptClients() {
    while (running_ && listenFd_ >= 0) {
        struct sockaddr_un clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = ::accept4(listenFd_, reinterpret_cast<struct sockaddr*>(&clientAddr),
                                &addrLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        ClientSession session;
        session.fd = clientFd;
        session.connectedAt = std::chrono::steady_clock::now();
        clients_[clientFd] = std::move(session);
    }
}

void IpcServer::handleClientRead(int clientFd) {
    auto it = clients_.find(clientFd);
    if (it == clients_.end()) {
        return;
    }
    ClientSession& session = it->second;

    char buf[2048];
    while (true) {
        ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
        if (n > 0) {
            session.inBuffer.append(buf, static_cast<size_t>(n));

            if (session.inBuffer.size() > maxLineLength_) {
                queueResponse(session, errJson("line too long"));
                char drainBuf[8192];
                while (::recv(clientFd, drainBuf, sizeof(drainBuf), MSG_DONTWAIT) > 0) {}
                closeClient(clientFd);
                return;
            }

            size_t pos;
            while ((pos = session.inBuffer.find('\n')) != std::string::npos) {
                std::string line = session.inBuffer.substr(0, pos);
                session.inBuffer.erase(0, pos + 1);

                std::string response = handleCommandLine(line);
                if (!queueResponse(session, response)) {
                    closeClient(clientFd);
                    return;
                }
            }
        } else if (n == 0) {
            closeClient(clientFd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            closeClient(clientFd);
            return;
        }
    }
}

bool IpcServer::flushClientOutBuffer(ClientSession& session) {
    while (!session.outBuffer.empty()) {
        ssize_t sent = ::send(session.fd, session.outBuffer.data(), session.outBuffer.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            session.outBuffer.erase(0, static_cast<size_t>(sent));
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        } else {
            return true;
        }
    }
    return true;
}

void IpcServer::handleClientWrite(int clientFd) {
    auto it = clients_.find(clientFd);
    if (it != clients_.end()) {
        if (!flushClientOutBuffer(it->second)) {
            closeClient(clientFd);
        }
    }
}

bool IpcServer::queueResponse(ClientSession& session, const std::string& response) {
    session.outBuffer.append(response);
    return flushClientOutBuffer(session);
}

void IpcServer::closeClient(int clientFd) {
    auto it = clients_.find(clientFd);
    if (it != clients_.end()) {
        ::close(it->first);
        clients_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Event Loop Integration
// ---------------------------------------------------------------------------

void IpcServer::appendPollFds(std::vector<struct pollfd>& pfds) const {
    if (listenFd_ >= 0) {
        pfds.push_back({listenFd_, POLLIN, 0});
    }
    for (const auto& [fd, session] : clients_) {
        short events = POLLIN;
        if (!session.outBuffer.empty()) {
            events |= POLLOUT;
        }
        pfds.push_back({fd, events, 0});
    }
}

void IpcServer::handleSocketEvent(int fd, short revents) {
    if (fd == listenFd_) {
        if (revents & (POLLIN | POLLERR)) {
            acceptClients();
        }
        return;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        closeClient(fd);
        return;
    }

    if (revents & POLLOUT) {
        handleClientWrite(fd);
    }

    if ((revents & POLLIN) && clients_.find(fd) != clients_.end()) {
        handleClientRead(fd);
    }
}

void IpcServer::handlePollEvents(std::span<const struct pollfd> pfds) {
    std::vector<int> clientsToClose;

    for (const auto& pfd : pfds) {
        if (pfd.fd == listenFd_) {
            if (pfd.revents & (POLLIN | POLLERR)) {
                acceptClients();
            }
            continue;
        }

        auto it = clients_.find(pfd.fd);
        if (it == clients_.end()) {
            continue;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            clientsToClose.push_back(pfd.fd);
            continue;
        }

        if (pfd.revents & POLLOUT) {
            handleClientWrite(pfd.fd);
        }

        if ((pfd.revents & POLLIN) && clients_.find(pfd.fd) != clients_.end()) {
            handleClientRead(pfd.fd);
        }
    }

    for (int fd : clientsToClose) {
        closeClient(fd);
    }
}

// ---------------------------------------------------------------------------
// Command Execution
// ---------------------------------------------------------------------------

std::string IpcServer::handleCommandLine(const std::string& line) {
    if (customHandler_) {
        return customHandler_(line);
    }
    return handleBuiltinCommand(line);
}

std::string IpcServer::handleBuiltinCommand(const std::string& line) {
    std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return errJson("empty command");
    }

    auto tokens = tokenize(trimmed);
    if (tokens.empty()) {
        return errJson("empty command");
    }

    std::string verb = toLower(tokens[0]);

    // status -> {"ok":true, <status.json fields>}
    if (verb == "status") {
        if (callbacks_.getStatusJson) {
            std::string json = callbacks_.getStatusJson();
            if (!json.empty() && json.front() == '{') {
                return "{\"ok\":true," + json.substr(1) + "\n";
            }
        }
        return "{\"ok\":true,\"schema\":1,\"connected\":false}\n";
    }

    // cnc <0-10>
    if (verb == "cnc") {
        int level = 0;
        if (tokens.size() < 2 || !parseInt(tokens[1], level)) {
            return errJson("expected a CNC level 0-10 (0 = passthrough, 10 = max ANC)");
        }
        if (level < 0 || level > 10) {
            return errJson("cnc level out of range [0-10]");
        }
        if (callbacks_.setCnc) {
            std::string err;
            if (!callbacks_.setCnc(level, err)) {
                return errJson(err.empty() ? "failed to set cnc" : err);
            }
        }
        return okJson();
    }

    // eq <bass> <mid> <treble>, each -10..10
    if (verb == "eq") {
        if (tokens.size() < 4) {
            return errJson("expected: eq <bass> <mid> <treble> (each -10..10)");
        }
        std::array<int, 3> bands{};
        for (size_t i = 0; i < 3; ++i) {
            if (!parseInt(tokens[1 + i], bands[i])) {
                return errJson("invalid eq value '" + tokens[1 + i] + "'");
            }
            if (bands[i] < -10 || bands[i] > 10) {
                return errJson("eq band out of range [-10, 10]");
            }
        }
        if (callbacks_.setEq) {
            std::string err;
            if (!callbacks_.setEq(bands[0], bands[1], bands[2], err)) {
                return errJson(err.empty() ? "failed to set eq" : err);
            }
        }
        return okJson();
    }

    // sidetone <off|low|medium|high>
    if (verb == "sidetone") {
        if (tokens.size() < 2) {
            return errJson("expected off|low|medium|high");
        }
        const std::string val = toLower(tokens[1]);
        if (val != "off" && val != "low" && val != "medium" && val != "high") {
            return errJson("invalid sidetone level '" + tokens[1] + "'");
        }
        if (callbacks_.setSidetone) {
            std::string err;
            if (!callbacks_.setSidetone(val, err)) {
                return errJson(err.empty() ? "failed to set sidetone" : err);
            }
        }
        return okJson();
    }

    // prompts <on|off>, multipoint <on|off>
    if (verb == "prompts" || verb == "multipoint") {
        const std::string val = tokens.size() >= 2 ? toLower(tokens[1]) : "";
        if (val != "on" && val != "off") {
            return errJson("expected on|off");
        }
        const bool enabled = (val == "on");
        std::string err;
        const bool ok = verb == "prompts"
            ? (!callbacks_.setPrompts || callbacks_.setPrompts(enabled, err))
            : (!callbacks_.setMultipoint || callbacks_.setMultipoint(enabled, err));
        if (!ok) {
            return errJson(err.empty() ? "failed to set " + verb : err);
        }
        return okJson();
    }

    // reconnect: drop and re-establish the RFCOMM link
    if (verb == "reconnect") {
        if (callbacks_.reconnect) {
            std::string err;
            if (!callbacks_.reconnect(err)) {
                return errJson(err.empty() ? "failed to reconnect" : err);
            }
        }
        return okJson();
    }

    return errJson("unknown command '" + tokens[0] + "'");
}

} // namespace omarchy::bose
