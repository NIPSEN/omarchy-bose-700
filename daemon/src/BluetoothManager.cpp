// Bluetooth RFCOMM link manager implementation.
// Adapted from omarchy-sony-xm3 (Copyright (c) Kevin Cardwell, MIT license);
// bluetoothctl discovery adapted from bosectl (Copyright (c) aaronsb, MIT license).

#include "BluetoothManager.hpp"

#include <sys/socket.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <sstream>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

namespace omarchy::bose {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string connectionStateToString(ConnectionState state) {
    switch (state) {
        case ConnectionState::DISCONNECTED:      return "DISCONNECTED";
        case ConnectionState::DISCOVERING:       return "DISCOVERING";
        case ConnectionState::RESOLVING_SDP:     return "RESOLVING_SDP";
        case ConnectionState::CONNECTING:        return "CONNECTING";
        case ConnectionState::CONNECTED:         return "CONNECTED";
        case ConnectionState::RECONNECT_BACKOFF: return "RECONNECT_BACKOFF";
        default:                                 return "UNKNOWN";
    }
}

static int parseUuidString(const char* szSrc, uint8_t* dst) {
    if (!szSrc || std::strlen(szSrc) != 36) return -1;
    unsigned int b[16];
    int ret = std::sscanf(szSrc,
        "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7],
        &b[8], &b[9], &b[10], &b[11], &b[12], &b[13], &b[14], &b[15]);
    if (ret != 16) return -1;
    for (int i = 0; i < 16; ++i) {
        dst[i] = static_cast<uint8_t>(b[i]);
    }
    return 0;
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------------------
// Real Linux RFCOMM Transport
// ---------------------------------------------------------------------------

class RfcommTransport : public IBluetoothTransport {
public:
    RfcommTransport() = default;
    ~RfcommTransport() override { disconnect(); }

    int connect(const std::string& macAddress, uint8_t channel) override {
        disconnect();

        fd_ = ::socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_NONBLOCK, BTPROTO_RFCOMM);
        if (fd_ < 0) {
            lastError_ = "Failed to create RFCOMM socket: " + std::string(std::strerror(errno));
            return -1;
        }

        // Set security and encryption level (non-fatal if unsupported)
        unsigned int linkmode = RFCOMM_LM_AUTH | RFCOMM_LM_ENCRYPT;
        ::setsockopt(fd_, SOL_RFCOMM, RFCOMM_LM, &linkmode, sizeof(linkmode));

        struct sockaddr_rc addr{};
        addr.rc_family = AF_BLUETOOTH;
        addr.rc_channel = channel;
        if (str2ba(macAddress.c_str(), &addr.rc_bdaddr) < 0) {
            lastError_ = "Invalid MAC address: " + macAddress;
            disconnect();
            return -1;
        }

        int res = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (res == 0) {
            connected_ = true;
            return 0; // Immediate connection
        }

        if (errno == EINPROGRESS) {
            connected_ = false;
            return 1; // Connecting asynchronously
        }

        lastError_ = "Connect failed: " + std::string(std::strerror(errno));
        disconnect();
        return -1;
    }

    void disconnect() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
    }

    [[nodiscard]] bool isConnected() const override { return connected_; }
    [[nodiscard]] int getFd() const override { return fd_; }

    ssize_t send(const uint8_t* data, size_t length) override {
        if (fd_ < 0 || !connected_) return -1;
        ssize_t n = ::send(fd_, data, length, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            lastError_ = "Send error: " + std::string(std::strerror(errno));
            return -1;
        }
        return n;
    }

    ssize_t recv(uint8_t* buffer, size_t maxLength) override {
        if (fd_ < 0 || !connected_) return -1;
        ssize_t n = ::recv(fd_, buffer, maxLength, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                lastError_ = "Recv error: " + std::string(std::strerror(errno));
            }
            return -1;
        }
        return n;
    }

    int checkConnectResult() override {
        if (fd_ < 0) return ENOTCONN;
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
            return errno;
        }
        if (so_error == 0) {
            connected_ = true;
        } else {
            lastError_ = "Async connect failed: " + std::string(std::strerror(so_error));
        }
        return so_error;
    }

    [[nodiscard]] std::string getLastError() const override { return lastError_; }

private:
    int fd_{-1};
    bool connected_{false};
    std::string lastError_;
};

// ---------------------------------------------------------------------------
// Device Discovery via bluetoothctl (same approach as bosectl's
// discovery.cpp): no libdbus dependency, and the paired-device list is
// exactly what the user sees. A device qualifies when its Modalias carries
// the NC700 product ID (0x4024) or it advertises the BMAP service UUID.
// ---------------------------------------------------------------------------

class BluetoothctlDiscovery : public IDeviceDiscovery {
public:
    std::vector<BluetoothDeviceInfo> getAvailableDevices() override {
        std::vector<BluetoothDeviceInfo> devices;
        std::istringstream stream(exec("timeout 5 bluetoothctl devices Paired 2>/dev/null"));
        std::string line;
        while (std::getline(stream, line)) {
            // "Device XX:XX:XX:XX:XX:XX <name>"
            auto firstSpace = line.find(' ');
            if (firstSpace == std::string::npos) continue;
            auto secondSpace = line.find(' ', firstSpace + 1);
            if (secondSpace == std::string::npos) continue;
            auto mac = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            // The token comes from bluetoothd, but it is about to go through a
            // shell: accept only a literal MAC.
            if (!isMac(mac)) continue;

            BluetoothDeviceInfo dev;
            dev.macAddress = mac;
            dev.name = line.substr(secondSpace + 1);
            dev.paired = true;

            auto info = exec("timeout 5 bluetoothctl info " + mac + " 2>/dev/null");
            dev.connected = info.find("Connected: yes") != std::string::npos;
            auto alias = extractField(info, "Alias:");
            if (!alias.empty()) dev.name = alias;
            devices.push_back(std::move(dev));
        }
        return devices;
    }

    std::optional<BluetoothDeviceInfo> findBoseHeadphones(const std::string& preferredMac = "") override {
        // Explicit MAC: trust the user, but still ask BlueZ for name/link state.
        if (!preferredMac.empty()) {
            BluetoothDeviceInfo dev;
            dev.macAddress = preferredMac;
            dev.name = preferredMac;
            auto info = exec("timeout 5 bluetoothctl info " + preferredMac + " 2>/dev/null");
            if (info.find("Device " + preferredMac) != std::string::npos) {
                dev.paired = info.find("Paired: yes") != std::string::npos;
                dev.connected = info.find("Connected: yes") != std::string::npos;
                auto alias = extractField(info, "Alias:");
                if (!alias.empty()) dev.name = alias;
                if (dev.name == preferredMac) {
                    auto name = extractField(info, "Name:");
                    if (!name.empty()) dev.name = name;
                }
            } else {
                dev.paired = true; // Unknown to BlueZ; try anyway.
            }
            return dev;
        }

        std::istringstream stream(exec("timeout 5 bluetoothctl devices Paired 2>/dev/null"));
        std::string line;
        std::vector<BluetoothDeviceInfo> candidates;

        while (std::getline(stream, line)) {
            auto firstSpace = line.find(' ');
            if (firstSpace == std::string::npos) continue;
            auto secondSpace = line.find(' ', firstSpace + 1);
            if (secondSpace == std::string::npos) continue;
            auto mac = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            if (!isMac(mac)) continue;

            auto info = exec("timeout 5 bluetoothctl info " + mac + " 2>/dev/null");
            const std::string infoLower = toLower(info);

            // Modalias "bluetooth:v05A7p4024..." or the BMAP service UUID.
            char modaliasNeedle[24];
            std::snprintf(modaliasNeedle, sizeof(modaliasNeedle), "p%04x", NC700_PRODUCT_ID);
            const bool matchesModalias = infoLower.find(modaliasNeedle) != std::string::npos;
            const bool matchesBmapUuid = infoLower.find(BMAP_UUID) != std::string::npos;
            if (!matchesModalias && !matchesBmapUuid) continue;

            BluetoothDeviceInfo dev;
            dev.macAddress = mac;
            dev.name = line.substr(secondSpace + 1);
            dev.paired = true;
            dev.connected = info.find("Connected: yes") != std::string::npos;
            auto alias = extractField(info, "Alias:");
            if (!alias.empty()) dev.name = alias;
            candidates.push_back(std::move(dev));
        }

        if (candidates.empty()) return std::nullopt;

        // Prefer the device whose ACL link is already up.
        for (const auto& dev : candidates) {
            if (dev.connected) return dev;
        }
        return candidates.front();
    }

private:
    static bool isMac(const std::string& s) {
        if (s.size() != 17) return false;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i % 3 == 2) { if (s[i] != ':') return false; }
            else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
        }
        return true;
    }

    static std::string exec(const std::string& cmd) {
        std::array<char, 256> buf;
        std::string result;
        std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe) return "";
        while (fgets(buf.data(), buf.size(), pipe.get())) {
            result += buf.data();
        }
        return result;
    }

    // Extracts "\t<Field>: <value>" lines from `bluetoothctl info` output.
    static std::string extractField(const std::string& info, const std::string& field) {
        auto pos = info.find(field);
        if (pos == std::string::npos) return "";
        pos += field.size();
        while (pos < info.size() && (info[pos] == ' ' || info[pos] == '\t')) ++pos;
        auto end = info.find('\n', pos);
        return info.substr(pos, end == std::string::npos ? end : end - pos);
    }

public:
    // Standard BlueZ connection path — the clean way to bring the ACL link up
    // (and to wake a paired but idle headset) without touching RFCOMM.
    bool requestConnect(const std::string& macAddress) override {
        if (!isMac(macAddress)) return false;
        exec("timeout 10 bluetoothctl connect " + macAddress + " 2>/dev/null");
        return true;
    }
};

// ---------------------------------------------------------------------------
// BlueZ SDP Service Resolver
// ---------------------------------------------------------------------------

class BlueZSdpResolver : public ISdpResolver {
public:
    int resolveRfcommChannel(const std::string& macAddress,
                             const std::string& uuid,
                             uint32_t timeoutMs) override {
        uint8_t uuidBytes[16];
        if (parseUuidString(uuid.c_str(), uuidBytes) != 0) {
            return -1;
        }

        bdaddr_t target;
        if (str2ba(macAddress.c_str(), &target) < 0) {
            return -1;
        }

        const bdaddr_t bdaddr_any = {{0, 0, 0, 0, 0, 0}};
        sdp_session_t* session = sdp_connect(&bdaddr_any, &target, SDP_NON_BLOCKING);
        if (!session) {
            return -1;
        }

        int sdp_sock = sdp_get_socket(session);
        struct pollfd pfd{
            sdp_sock,
            POLLIN | POLLOUT,
            0
        };

        int poll_rc = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
        if (poll_rc <= 0 || (pfd.revents & (POLLERR | POLLHUP))) {
            sdp_close(session);
            return -1;
        }

        // Restore blocking mode for SDP query
        int flags = fcntl(sdp_sock, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(sdp_sock, F_SETFL, flags & ~O_NONBLOCK);
        }

        uuid_t svc_uuid;
        sdp_uuid128_create(&svc_uuid, uuidBytes);

        sdp_list_t* search_list = sdp_list_append(nullptr, &svc_uuid);
        uint32_t range = 0x0000ffff;
        sdp_list_t* attrid_list = sdp_list_append(nullptr, &range);
        sdp_list_t* response_list = nullptr;

        int status = sdp_service_search_attr_req(
            session, search_list, SDP_ATTR_REQ_RANGE, attrid_list, &response_list);

        uint8_t channel = 0;
        if (status == 0 && response_list) {
            for (sdp_list_t* r = response_list; r; r = r->next) {
                auto* rec = reinterpret_cast<sdp_record_t*>(r->data);
                sdp_list_t* proto_list = nullptr;
                if (sdp_get_access_protos(rec, &proto_list) == 0) {
                    int p = sdp_get_proto_port(proto_list, RFCOMM_UUID);
                    if (p > 0 && p <= 30) {
                        channel = static_cast<uint8_t>(p);
                    }
                    sdp_list_free(proto_list, nullptr);
                    if (channel > 0) break;
                }
                sdp_record_free(rec);
            }
        }

        if (response_list) sdp_list_free(response_list, nullptr);
        sdp_list_free(search_list, nullptr);
        sdp_list_free(attrid_list, nullptr);
        sdp_close(session);

        return channel > 0 ? static_cast<int>(channel) : -1;
    }
};

// ---------------------------------------------------------------------------
// Mock Implementations
// ---------------------------------------------------------------------------

MockTransport::MockTransport(int* outPeerFd) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0) {
        fd_ = sv[0];
        peerFd_ = sv[1];
        if (outPeerFd) *outPeerFd = peerFd_;
    }
}

MockTransport::~MockTransport() {
    disconnect();
}

int MockTransport::connect(const std::string&, uint8_t) {
    if (fd_ < 0) {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0) {
            fd_ = sv[0];
            peerFd_ = sv[1];
        } else {
            lastError_ = "Failed to allocate mock socketpair";
            return -1;
        }
    }
    connected_ = true;
    return 0; // Immediate connection in mock mode
}

void MockTransport::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (peerFd_ >= 0) {
        ::close(peerFd_);
        peerFd_ = -1;
    }
    connected_ = false;
}

void MockTransport::simulateRemoteDisconnect() {
    if (peerFd_ >= 0) {
        ::close(peerFd_);
        peerFd_ = -1;
    }
}

ssize_t MockTransport::send(const uint8_t* data, size_t length) {
    if (fd_ < 0 || !connected_) return -1;
    ssize_t n = ::send(fd_, data, length, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return n;
}

ssize_t MockTransport::recv(uint8_t* buffer, size_t maxLength) {
    if (fd_ < 0 || !connected_) return -1;
    ssize_t n = ::recv(fd_, buffer, maxLength, 0);
    if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
        lastError_ = "Recv error: " + std::string(std::strerror(errno));
    }
    return n;
}

int MockTransport::checkConnectResult() {
    connected_ = (fd_ >= 0);
    return connected_ ? 0 : ECONNREFUSED;
}

std::optional<BluetoothDeviceInfo> MockDeviceDiscovery::findBoseHeadphones(const std::string& preferredMac) {
    if (devices.empty()) {
        BluetoothDeviceInfo dev;
        dev.macAddress = "4C:87:5D:A3:D1:4F";
        dev.name = "Panthère";
        dev.paired = true;
        dev.connected = true;
        return dev;
    }

    if (!preferredMac.empty()) {
        for (const auto& dev : devices) {
            if (strcasecmp(dev.macAddress.c_str(), preferredMac.c_str()) == 0) {
                return dev;
            }
        }
        return std::nullopt;
    }

    return devices.front();
}

// ---------------------------------------------------------------------------
// BluetoothManager Implementation
// ---------------------------------------------------------------------------

BluetoothManager::BluetoothManager(BluetoothConfig config,
                                   std::unique_ptr<IBluetoothTransport> transport,
                                   std::unique_ptr<IDeviceDiscovery> discovery,
                                   std::unique_ptr<ISdpResolver> sdpResolver)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      discovery_(std::move(discovery)),
      sdpResolver_(std::move(sdpResolver)),
      currentBackoffMs_(config_.initialBackoffMs) {
    lastStateChangeTime_ = std::chrono::steady_clock::now();
}

BluetoothManager::~BluetoothManager() {
    stop();
}

std::unique_ptr<BluetoothManager> BluetoothManager::createLinux(BluetoothConfig config) {
    config.mockMode = false;
    auto transport = std::make_unique<RfcommTransport>();
    auto discovery = std::make_unique<BluetoothctlDiscovery>();
    auto sdp = std::make_unique<BlueZSdpResolver>();
    return std::make_unique<BluetoothManager>(
        std::move(config), std::move(transport), std::move(discovery), std::move(sdp));
}

std::unique_ptr<BluetoothManager> BluetoothManager::createMock(BluetoothConfig config, int* outPeerFd) {
    config.mockMode = true;
    auto transport = std::make_unique<MockTransport>(outPeerFd);
    auto discovery = std::make_unique<MockDeviceDiscovery>();
    auto sdp = std::make_unique<MockSdpResolver>();
    return std::make_unique<BluetoothManager>(
        std::move(config), std::move(transport), std::move(discovery), std::move(sdp));
}

void BluetoothManager::setCallbacks(BluetoothCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void BluetoothManager::setState(ConnectionState newState) {
    if (state_ != newState) {
        state_ = newState;
        lastStateChangeTime_ = std::chrono::steady_clock::now();
        if (callbacks_.onStateChanged) {
            callbacks_.onStateChanged(newState);
        }
    }
}

void BluetoothManager::start() {
    if (state_ == ConnectionState::CONNECTED || state_ == ConnectionState::CONNECTING) {
        return;
    }
    retryCount_ = 0;
    currentBackoffMs_ = config_.initialBackoffMs;
    attemptDiscovery();
}

void BluetoothManager::stop() {
    disconnect();
    setState(ConnectionState::DISCONNECTED);
}

void BluetoothManager::disconnect() {
    if (transport_) {
        transport_->disconnect();
    }
    sendQueue_.clear();
    setState(ConnectionState::DISCONNECTED);
}

void BluetoothManager::dropLink(const std::string& reason) {
    if (state_ == ConnectionState::DISCONNECTED || state_ == ConnectionState::RECONNECT_BACKOFF) {
        return;
    }
    scheduleReconnect(reason);
}

void BluetoothManager::attemptDiscovery() {
    setState(ConnectionState::DISCOVERING);
    if (!discovery_) {
        scheduleReconnect("No discovery engine available");
        return;
    }

    auto devOpt = discovery_->findBoseHeadphones(config_.preferredMac);
    if (!devOpt.has_value()) {
        scheduleReconnect("No paired Bose NC700 found");
        return;
    }

    currentDevice_ = *devOpt;
    if (!currentDevice_.paired) {
        scheduleReconnect("Headphone device is not paired");
        return;
    }

    // Never open RFCOMM while BlueZ reports the ACL link down. Hammering
    // connect() on a powered-off headset stresses the kernel Bluetooth stack
    // (and froze this machine); the link state is known locally via D-Bus, so
    // poll it cheaply and let BlueZ bring the link up instead.
    if (!currentDevice_.connected) {
        auto now = std::chrono::steady_clock::now();
        auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastConnectRequest_).count();
        if (sinceLast >= static_cast<int64_t>(config_.connectRequestIntervalMs)) {
            lastConnectRequest_ = now;
            discovery_->requestConnect(currentDevice_.macAddress);
        }
        scheduleDiscoveryRetry("Headset not connected at ACL level");
        return;
    }

    attemptSdp();
}

void BluetoothManager::attemptSdp() {
    setState(ConnectionState::RESOLVING_SDP);

    // The NC700's BMAP service lives on RFCOMM channel 8 (verified on firmware
    // 1.8.2). We deliberately do NOT trust an SDP lookup of the BMAP UUID:
    // this headset's SDP record for that UUID resolves to channel 14, which
    // accepts the RFCOMM connection but never answers a BMAP request, while
    // channel 8 is the one that actually speaks BMAP. --channel overrides.
    currentChannel_ = config_.defaultChannel;

    attemptConnect();
}

void BluetoothManager::attemptConnect() {
    setState(ConnectionState::CONNECTING);
    if (!transport_) {
        scheduleReconnect("No transport engine available");
        return;
    }

    fprintf(stderr, "[BT] Attempting RFCOMM connect to %s channel %u\n",
            currentDevice_.macAddress.c_str(), (unsigned)currentChannel_);
    fflush(stderr);
    int rc = transport_->connect(currentDevice_.macAddress, currentChannel_);
    if (rc == 0) {
        fprintf(stderr, "[BT] RFCOMM connected immediately\n");
        fflush(stderr);
        setState(ConnectionState::CONNECTED);
        retryCount_ = 0;
        currentBackoffMs_ = config_.initialBackoffMs;
        if (callbacks_.onConnected) {
            callbacks_.onConnected();
        }
    } else if (rc == 1) {
        fprintf(stderr, "[BT] RFCOMM async connect in progress...\n");
        fflush(stderr);
    } else {
        scheduleReconnect("RFCOMM connect initiation failed: " + transport_->getLastError());
    }
}

int BluetoothManager::getPollFd() const {
    if (!transport_) return -1;
    return transport_->getFd();
}

short BluetoothManager::getPollEvents() const {
    if (state_ == ConnectionState::CONNECTING) {
        return POLLOUT | POLLERR | POLLHUP;
    }
    if (state_ == ConnectionState::CONNECTED) {
        short events = POLLIN | POLLERR | POLLHUP;
        if (!sendQueue_.empty()) {
            events |= POLLOUT;
        }
        return events;
    }
    return 0;
}

void BluetoothManager::handleSocketEvent(short revents) {
    if (state_ == ConnectionState::CONNECTING) {
        handleConnecting(revents);
    } else if (state_ == ConnectionState::CONNECTED) {
        if (revents & (POLLERR | POLLHUP)) {
            scheduleReconnect("Socket hangup/error detected");
            return;
        }
        if (revents & POLLIN) {
            handleConnectedRead();
        }
        if (state_ == ConnectionState::CONNECTED && (revents & POLLOUT)) {
            handleConnectedWrite();
        }
    }
}

void BluetoothManager::handleConnecting(short revents) {
    if (revents & POLLOUT) {
        int err = transport_->checkConnectResult();
        if (err == 0) {
            fprintf(stderr, "[BT] RFCOMM async connect succeeded\n");
            fflush(stderr);
            setState(ConnectionState::CONNECTED);
            retryCount_ = 0;
            currentBackoffMs_ = config_.initialBackoffMs;
            if (callbacks_.onConnected) {
                callbacks_.onConnected();
            }
        } else {
            scheduleReconnect("Async connect check failed: " + transport_->getLastError());
        }
    } else if (revents & (POLLERR | POLLHUP)) {
        scheduleReconnect("Async connect socket hangup/error");
    }
}

void BluetoothManager::handleConnectedRead() {
    uint8_t buf[1024];
    ssize_t n = transport_->recv(buf, sizeof(buf));
    if (n > 0) {
        if (callbacks_.onDataReceived) {
            callbacks_.onDataReceived(buf, static_cast<size_t>(n));
        }
    } else if (n == 0) {
        scheduleReconnect("Remote peer closed RFCOMM connection");
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        scheduleReconnect("RFCOMM read error: " + transport_->getLastError());
    }
}

void BluetoothManager::handleConnectedWrite() {
    while (!sendQueue_.empty()) {
        std::vector<uint8_t> chunk(sendQueue_.begin(), sendQueue_.end());
        ssize_t n = transport_->send(chunk.data(), chunk.size());
        if (n > 0) {
            sendQueue_.erase(sendQueue_.begin(), sendQueue_.begin() + n);
        } else if (n == 0) {
            break; // Socket buffer full, wait for next POLLOUT
        } else {
            scheduleReconnect("RFCOMM send failed: " + transport_->getLastError());
            break;
        }
    }
}

bool BluetoothManager::sendPacket(const uint8_t* data, size_t length) {
    if (state_ != ConnectionState::CONNECTED || !transport_) {
        return false;
    }

    if (sendQueue_.empty()) {
        ssize_t n = transport_->send(data, length);
        if (n >= 0 && static_cast<size_t>(n) == length) {
            return true;
        }
        if (n > 0) {
            sendQueue_.insert(sendQueue_.end(), data + n, data + length);
            return true;
        }
        if (n == 0) {
            sendQueue_.insert(sendQueue_.end(), data, data + length);
            return true;
        }
        scheduleReconnect("Send error: " + transport_->getLastError());
        return false;
    }

    sendQueue_.insert(sendQueue_.end(), data, data + length);
    return true;
}

bool BluetoothManager::sendPacket(const std::vector<uint8_t>& packet) {
    return sendPacket(packet.data(), packet.size());
}

void BluetoothManager::scheduleDiscoveryRetry(const std::string& reason) {
    lastError_ = reason;
    if (transport_) {
        transport_->disconnect();
    }
    sendQueue_.clear();
    setState(ConnectionState::RECONNECT_BACKOFF);
    if (callbacks_.onDisconnected) {
        callbacks_.onDisconnected(reason);
    }
    // Fixed local-poll cadence; does not grow retryCount_ (no failing I/O here).
    nextReconnectTime_ = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(config_.discoveryRetryMs);
}

void BluetoothManager::scheduleReconnect(const std::string& reason) {
    lastError_ = reason;
    fprintf(stderr, "[BT] Reconnect scheduled: %s\n", reason.c_str());
    fflush(stderr);
    if (transport_) {
        transport_->disconnect();
    }
    sendQueue_.clear();
    setState(ConnectionState::RECONNECT_BACKOFF);

    if (callbacks_.onDisconnected) {
        callbacks_.onDisconnected(reason);
    }

    if (!config_.autoReconnect) {
        setState(ConnectionState::DISCONNECTED);
        return;
    }

    retryCount_++;
    currentBackoffMs_ = computeBackoffMs(config_, retryCount_);
    nextReconnectTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(currentBackoffMs_);
    fprintf(stderr, "[BT] Will retry in %u ms (attempt #%u)\n", currentBackoffMs_, retryCount_);
    fflush(stderr);
}

void BluetoothManager::tick() {
    auto now = std::chrono::steady_clock::now();

    // Check connect timeout
    if (state_ == ConnectionState::CONNECTING) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStateChangeTime_).count();
        if (elapsed > static_cast<int64_t>(config_.connectTimeoutMs)) {
            scheduleReconnect("RFCOMM connection timed out");
            return;
        }
    }

    // Check reconnect backoff timer
    if (state_ == ConnectionState::RECONNECT_BACKOFF && config_.autoReconnect) {
        if (now >= nextReconnectTime_) {
            attemptDiscovery();
        }
    }
}

} // namespace omarchy::bose
