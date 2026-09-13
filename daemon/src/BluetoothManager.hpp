#pragma once

// ---------------------------------------------------------------------------
// Bluetooth RFCOMM link manager for the Bose NC Headphones 700.
//
// State machine, transport/discovery/SDP structure and reconnect-backoff
// behaviour adapted from omarchy-sony-xm3 (Copyright (c) Kevin Cardwell,
// MIT license). Device discovery via bluetoothctl adapted from bosectl
// (Copyright (c) aaronsb, MIT license).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <deque>
#include <chrono>
#include <cmath>

namespace omarchy::bose {

// BMAP Bluetooth service UUID (SDP record advertised by Bose devices).
constexpr const char* BMAP_UUID = "00000000-deca-fade-deca-deafdecacaff";
// Bluetooth Modalias product ID of the NC Headphones 700 (vendor 0x05A7).
constexpr uint16_t NC700_PRODUCT_ID = 0x4024;

// Verified on the NC700; also what an SDP lookup of the BMAP UUID returns.
constexpr uint8_t DEFAULT_RFCOMM_CHANNEL = 8;

enum class ConnectionState {
    DISCONNECTED,
    DISCOVERING,
    RESOLVING_SDP,
    CONNECTING,
    CONNECTED,
    RECONNECT_BACKOFF
};

std::string connectionStateToString(ConnectionState state);

struct BluetoothDeviceInfo {
    std::string macAddress;         // Colon-separated MAC (e.g. 4C:87:5D:A3:D1:4F)
    std::string name;               // Advertised/aliased device name
    bool paired{false};
    bool connected{false};          // Whether the underlying ACL link is up
};

struct BluetoothConfig {
    std::string preferredMac;          // If set, only connect to this MAC
    bool autoReconnect{true};
    uint32_t initialBackoffMs{2000};
    uint32_t maxBackoffMs{60000};
    float backoffMultiplier{1.5f};
    // While BlueZ reports the headset's ACL link down, we never attempt RFCOMM:
    // we re-check the (purely local) BlueZ state on this cadence and ask BlueZ
    // itself to connect at most every connectRequestIntervalMs.
    uint32_t discoveryRetryMs{15000};
    uint32_t connectRequestIntervalMs{60000};
    uint32_t sdpTimeoutMs{3000};
    uint32_t connectTimeoutMs{8000};
    uint8_t defaultChannel{DEFAULT_RFCOMM_CHANNEL}; // Fallback channel if SDP fails
    bool mockMode{false};
};

// Backoff for real link failures: initialBackoffMs * multiplier^n, capped at
// maxBackoffMs, with a deterministic +/-20% jitter to decorrelate retries.
inline uint32_t computeBackoffMs(const BluetoothConfig& c, uint32_t retryCount) {
    float d = c.initialBackoffMs * std::pow(c.backoffMultiplier, std::min(retryCount, 12u));
    uint32_t base = static_cast<uint32_t>(d);
    if (base > c.maxBackoffMs) base = c.maxBackoffMs;
    uint32_t jitter = base / 5 * (retryCount % 3) / 2;  // 0%, +10%, +20%
    return base + jitter;
}

// ---------------------------------------------------------------------------
// Abstract Interfaces
// ---------------------------------------------------------------------------

// Transport abstraction (RFCOMM socket or Mock UNIX socketpair)
class IBluetoothTransport {
public:
    virtual ~IBluetoothTransport() = default;

    // Returns 0 if connected immediately, 1 if async in progress (EINPROGRESS), -1 on error.
    virtual int connect(const std::string& macAddress, uint8_t channel) = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool isConnected() const = 0;
    [[nodiscard]] virtual int getFd() const = 0;

    virtual ssize_t send(const uint8_t* data, size_t length) = 0;
    virtual ssize_t recv(uint8_t* buffer, size_t maxLength) = 0;

    // Inspects async connect result on POLLOUT. Returns 0 on success, errno on error.
    virtual int checkConnectResult() = 0;
    [[nodiscard]] virtual std::string getLastError() const = 0;
};

// Discovery abstraction (bluetoothctl scrape or Mock)
class IDeviceDiscovery {
public:
    virtual ~IDeviceDiscovery() = default;
    virtual std::vector<BluetoothDeviceInfo> getAvailableDevices() = 0;
    virtual std::optional<BluetoothDeviceInfo> findBoseHeadphones(const std::string& preferredMac = "") = 0;
    // Ask BlueZ to bring the ACL link up (standard connection path — this is
    // also what wakes a paired but idle headset). Fire-and-forget.
    virtual bool requestConnect(const std::string& macAddress) = 0;
};

// SDP Resolver abstraction (BlueZ sdp_lib or Mock)
class ISdpResolver {
public:
    virtual ~ISdpResolver() = default;
    virtual int resolveRfcommChannel(const std::string& macAddress,
                                     const std::string& uuid,
                                     uint32_t timeoutMs = 3000) = 0;
};

// ---------------------------------------------------------------------------
// Mock Transport Implementations (Exposed for Testing)
// ---------------------------------------------------------------------------

class MockTransport : public IBluetoothTransport {
public:
    explicit MockTransport(int* outPeerFd = nullptr);
    ~MockTransport() override;

    int connect(const std::string& macAddress, uint8_t channel) override;
    void disconnect() override;
    [[nodiscard]] bool isConnected() const override { return connected_; }
    [[nodiscard]] int getFd() const override { return fd_; }
    [[nodiscard]] int getPeerFd() const noexcept { return peerFd_; }

    ssize_t send(const uint8_t* data, size_t length) override;
    ssize_t recv(uint8_t* buffer, size_t maxLength) override;
    int checkConnectResult() override;
    [[nodiscard]] std::string getLastError() const override { return lastError_; }

    void simulateRemoteDisconnect();

private:
    int fd_{-1};
    int peerFd_{-1};
    bool connected_{false};
    std::string lastError_;
};

class MockDeviceDiscovery : public IDeviceDiscovery {
public:
    std::vector<BluetoothDeviceInfo> devices;

    std::vector<BluetoothDeviceInfo> getAvailableDevices() override {
        return devices;
    }

    std::optional<BluetoothDeviceInfo> findBoseHeadphones(const std::string& preferredMac = "") override;

    bool requestConnect(const std::string& macAddress) override {
        lastConnectRequest = macAddress;
        return true;
    }

    std::string lastConnectRequest;
};

class MockSdpResolver : public ISdpResolver {
public:
    int channelToReturn{DEFAULT_RFCOMM_CHANNEL};
    int resolveRfcommChannel(const std::string& macAddress,
                             const std::string& uuid,
                             uint32_t timeoutMs = 3000) override {
        return channelToReturn;
    }
};

// ---------------------------------------------------------------------------
// Event Callbacks
// ---------------------------------------------------------------------------
struct BluetoothCallbacks {
    std::function<void()> onConnected;
    std::function<void(const std::string& reason)> onDisconnected;
    std::function<void(const uint8_t* data, size_t length)> onDataReceived;
    std::function<void(ConnectionState newState)> onStateChanged;
};

// ---------------------------------------------------------------------------
// BluetoothManager Subsystem
// ---------------------------------------------------------------------------
class BluetoothManager {
public:
    explicit BluetoothManager(BluetoothConfig config = {},
                              std::unique_ptr<IBluetoothTransport> transport = nullptr,
                              std::unique_ptr<IDeviceDiscovery> discovery = nullptr,
                              std::unique_ptr<ISdpResolver> sdpResolver = nullptr);
    ~BluetoothManager();

    // Factory methods
    static std::unique_ptr<BluetoothManager> createLinux(BluetoothConfig config = {});
    static std::unique_ptr<BluetoothManager> createMock(BluetoothConfig config = {},
                                                        int* outPeerFd = nullptr);

    void setCallbacks(BluetoothCallbacks callbacks);

    // Lifecycle
    void start();
    void stop();
    void disconnect();

    // Drop the current link and schedule a reconnect (used by the protocol
    // layer when a synchronous transaction observes a dead link).
    void dropLink(const std::string& reason);

    // Packet transmission
    bool sendPacket(const uint8_t* data, size_t length);
    bool sendPacket(const std::vector<uint8_t>& packet);

    // Event loop integration (poll/epoll)
    [[nodiscard]] int getPollFd() const;
    [[nodiscard]] short getPollEvents() const;
    void handleSocketEvent(short revents);
    void tick();

    // Inspection
    [[nodiscard]] ConnectionState getState() const noexcept { return state_; }
    [[nodiscard]] const BluetoothDeviceInfo& getCurrentDevice() const noexcept { return currentDevice_; }
    [[nodiscard]] uint8_t getCurrentChannel() const noexcept { return currentChannel_; }
    [[nodiscard]] std::string getLastError() const noexcept { return lastError_; }

private:
    void setState(ConnectionState newState);
    void handleConnecting(short revents);
    void handleConnectedRead();
    void handleConnectedWrite();
    void scheduleReconnect(const std::string& reason);
    // Headset paired but ACL link down: poll BlueZ state on a fixed cadence
    // (local D-Bus query, no radio impact) and let BlueZ bring the link up.
    void scheduleDiscoveryRetry(const std::string& reason);
    void attemptDiscovery();
    void attemptSdp();
    void attemptConnect();

    BluetoothConfig config_;
    BluetoothCallbacks callbacks_;
    ConnectionState state_{ConnectionState::DISCONNECTED};
    BluetoothDeviceInfo currentDevice_;
    uint8_t currentChannel_{DEFAULT_RFCOMM_CHANNEL};
    std::string lastError_;

    std::unique_ptr<IBluetoothTransport> transport_;
    std::unique_ptr<IDeviceDiscovery> discovery_;
    std::unique_ptr<ISdpResolver> sdpResolver_;

    // Outbound queue for non-blocking partial writes
    std::deque<uint8_t> sendQueue_;

    // Timing & backoff
    uint32_t currentBackoffMs_{2000};
    uint32_t retryCount_{0};
    std::chrono::steady_clock::time_point lastStateChangeTime_;
    std::chrono::steady_clock::time_point nextReconnectTime_;
    std::chrono::steady_clock::time_point lastConnectRequest_{};
};

} // namespace omarchy::bose
