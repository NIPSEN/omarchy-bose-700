// BmapLink implementation: blocking BMAP round-trips on the RFCOMM fd.

#include "BmapLink.hpp"

#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace omarchy::bose {

namespace {
constexpr const char* kLogPrefix = "[BMAP]";
}

void BmapLink::feed(const uint8_t* data, size_t len) {
    parser_.feed(data, len);
    dispatchUnsolicited();
}

void BmapLink::dispatchUnsolicited() {
    while (auto pkt = parser_.next()) {
        if (unsolicited_) {
            unsolicited_(*pkt);
        }
    }
}

bool BmapLink::transact(BluetoothManager& bt,
                        uint8_t fblock, uint8_t func, bmap::Operator op,
                        const std::vector<uint8_t>& payload,
                        bmap::BmapResponse& out, std::string& err,
                        bool& linkDead, int timeoutMs) {
    linkDead = false;
    if (bt.getState() != ConnectionState::CONNECTED) {
        err = "headset not connected";
        return false;
    }
    const int fd = bt.getPollFd();
    if (fd < 0) {
        err = "no transport fd";
        linkDead = true;
        return false;
    }

    const auto packet = bmap::bmap_packet(fblock, func, op, payload);

    // Flush any bytes already buffered in the parser: a late answer to a
    // previous, timed-out request would otherwise be mistaken for ours.
    dispatchUnsolicited();

    size_t sent = 0;
    while (sent < packet.size()) {
        ssize_t n = ::send(fd, packet.data() + sent, packet.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd{fd, POLLOUT, 0};
            if (::poll(&pfd, 1, timeoutMs) <= 0) {
                err = "send timeout";
                return false;
            }
            continue;
        }
        err = std::string("send failed: ") + std::strerror(errno);
        linkDead = (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN);
        return false;
    }

    return waitForResponse(fd, fblock, func, out, err, linkDead, timeoutMs);
}

bool BmapLink::waitForResponse(int fd, uint8_t fblock, uint8_t func,
                               bmap::BmapResponse& out, std::string& err,
                               bool& linkDead, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            err = "response timeout";
            return false;
        }

        struct pollfd pfd{fd, POLLIN, 0};
        int rc = ::poll(&pfd, 1, static_cast<int>(remaining));
        if (rc < 0) {
            if (errno == EINTR) continue;
            err = std::string("poll failed: ") + std::strerror(errno);
            return false;
        }
        if (rc == 0) {
            err = "response timeout";
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            err = "link error while waiting for response";
            linkDead = true;
            return false;
        }
        if (!(pfd.revents & POLLIN)) continue;

        uint8_t buf[1024];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) {
            err = "link closed by headset";
            linkDead = true;
            return false;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            err = std::string("recv failed: ") + std::strerror(errno);
            linkDead = true;
            return false;
        }

        parser_.feed(buf, static_cast<size_t>(n));
        while (auto pkt = parser_.next()) {
            if (pkt->fblock == fblock && pkt->func == func) {
                if (pkt->op == bmap::Operator::Error) {
                    err = pkt->payload.empty()
                        ? "device error"
                        : std::string("device error: ") + bmap::error_name(pkt->payload[0]);
                    return false;
                }
                out = std::move(*pkt);
                return true;
            }
            // Not ours: a notification or a late answer to an older request.
            if (unsolicited_) unsolicited_(*pkt);
        }
    }
}

} // namespace omarchy::bose
