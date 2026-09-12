#pragma once

// ---------------------------------------------------------------------------
// UNIX-domain-socket IPC server for bose-700-daemon.
//
// Socket lifecycle/framing adapted from omarchy-sony-xm3 (Copyright (c)
// Kevin Cardwell, MIT license). The wire format here is one text command per
// line, answered with one JSON object per line:
//   {"ok":true,...}  or  {"ok":false,"error":"..."}
// The exception is `subscribe`: a subscribed session is pushed the raw
// status.json object (one per line) immediately and on every state change,
// until it sends `unsubscribe` or disconnects.
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
    // Set by the `subscribe` command: the session is sent every status change.
    bool subscribed{false};
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

    // Adopts the listening socket systemd passed in (LISTEN_FDS), if any.
    // Returns the descriptor, or -1 when the daemon was not socket-activated.
    // The environment variables are cleared either way, so a descriptor is
    // adopted at most once.
    [[nodiscard]] static int takeSystemdListenFd();

    // True when the listening socket came from the service manager rather than
    // from bind() in this process.
    [[nodiscard]] bool isSocketActivated() const noexcept { return socketActivated_; }

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

    // Sends one status line to every subscribed client. A client that cannot
    // keep up (its queue passes maxOutBuffer_) is dropped rather than allowed
    // to grow the daemon's memory.
    void broadcastStatus(const std::string& statusJson);
    [[nodiscard]] size_t getSubscriberCount() const noexcept;

    // Command parser (public for unit testing without sockets)
    [[nodiscard]] std::string handleCommandLine(const std::string& line);
    [[nodiscard]] std::string handleBuiltinCommand(const std::string& line);

    // Event loop integration (poll/epoll)
    void appendPollFds(std::vector<struct pollfd>& pfds) const;
    void handleSocketEvent(int fd, short revents);
    void handlePollEvents(std::span<const struct pollfd> pfds);

    // Standalone polling helper
    int pollOnce(int timeoutMs = 0);

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
    bool socketActivated_{false};
    size_t maxLineLength_{4096};
    size_t maxOutBuffer_{262144};

    CommandHandler customHandler_;
    IpcCallbacks callbacks_;
    std::unordered_map<int, ClientSession> clients_;
};

} // namespace omarchy::bose
