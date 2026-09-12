#pragma once

// ---------------------------------------------------------------------------
// UNIX-domain-socket IPC server for bose-700-daemon.
//
// Socket lifecycle/framing adapted from omarchy-sony-xm3 (Copyright (c)
// Kevin Cardwell, MIT license). The wire format here is one text command per
// line, answered with one JSON object per line:
//   {"ok":true,...}  or  {"ok":false,"error":"..."}
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <span>
#include <memory>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <sys/poll.h>

namespace omarchy::bose {

struct ClientSession {
    int fd{-1};
    std::string inBuffer;
    std::string outBuffer;
    std::chrono::steady_clock::time_point connectedAt;
};

// Delegate callbacks for decoupling IpcServer from StateEngine and the BMAP link
struct IpcCallbacks {
    // Full status JSON object (same content as status.json).
    std::function<std::string()> getStatusJson;
    std::function<bool(int level, std::string& errorMsg)> setCnc;
    std::function<bool(int bass, int mid, int treble, std::string& errorMsg)> setEq;
    std::function<bool(const std::string& level, std::string& errorMsg)> setSidetone;
    std::function<bool(bool enabled, std::string& errorMsg)> setPrompts;
    std::function<bool(bool enabled, std::string& errorMsg)> setMultipoint;
    std::function<bool(std::string& errorMsg)> reconnect;
};

class IpcServer {
public:
    using CommandHandler = std::function<std::string(const std::string& commandLine)>;

    explicit IpcServer(std::string socketPath = "");
    ~IpcServer();

    // Non-copyable, movable
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) noexcept;
    IpcServer& operator=(IpcServer&&) noexcept;

    // Path resolution utility
    [[nodiscard]] static std::string resolveSocketPath(const std::string& overridePath = "");

    // Lifecycle
    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept { return running_; }

    // Handlers & Callbacks
    void setCommandHandler(CommandHandler handler) { customHandler_ = std::move(handler); }
    void setCallbacks(IpcCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    // Inspection
    [[nodiscard]] const std::string& getSocketPath() const noexcept { return actualSocketPath_; }
    [[nodiscard]] int getListenFd() const noexcept { return listenFd_; }
    [[nodiscard]] size_t getClientCount() const noexcept { return clients_.size(); }

    // Command parser (public for unit testing without sockets)
    [[nodiscard]] std::string handleCommandLine(const std::string& line);
    [[nodiscard]] std::string handleBuiltinCommand(const std::string& line);

    // Event loop integration (poll/epoll)
    void appendPollFds(std::vector<struct pollfd>& pfds) const;
    void handleSocketEvent(int fd, short revents);
    void handlePollEvents(std::span<const struct pollfd> pfds);

private:
    void acceptClients();
    void handleClientRead(int clientFd);
    void handleClientWrite(int clientFd);
    void closeClient(int clientFd);
    bool queueResponse(ClientSession& session, const std::string& response);
    bool flushClientOutBuffer(ClientSession& session);

    std::string socketPathConfig_;
    std::string actualSocketPath_;
    int listenFd_{-1};
    bool running_{false};
    size_t maxLineLength_{4096};

    CommandHandler customHandler_;
    IpcCallbacks callbacks_;
    std::unordered_map<int, ClientSession> clients_;
};

} // namespace omarchy::bose
