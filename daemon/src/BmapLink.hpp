#pragma once

// ---------------------------------------------------------------------------
// BmapLink: request/response BMAP transactions over the RFCOMM link owned by
// BluetoothManager.
//
// BMAP has no ACK/sequence machinery (unlike Sony's MDR): the device answers
// each request with a STATUS (or ERROR) packet carrying the same
// [fblock, func]. Calls here are blocking-with-timeout and must only be made
// from the daemon's event-loop thread; packets that do not match the pending
// request (unsolicited STATUS notifications) are forwarded to a handler.
// ---------------------------------------------------------------------------

#include "BmapProtocol.hpp"
#include "BluetoothManager.hpp"

#include <functional>
#include <string>
#include <vector>

namespace omarchy::bose {

class BmapLink {
public:
    using UnsolicitedHandler = std::function<void(const bmap::BmapResponse&)>;

    BmapLink() = default;

    void setUnsolicitedHandler(UnsolicitedHandler handler) { unsolicited_ = std::move(handler); }

    // Drop any partial frame (call on every (re)connect).
    void reset() { parser_.reset(); }

    // Feed bytes read by BluetoothManager's onDataReceived callback; complete
    // packets are dispatched to the unsolicited handler.
    void feed(const uint8_t* data, size_t len);

    // Blocking round-trip against the link's current fd. Sends the packet,
    // then reads (bypassing the manager's recv path, which is idle while the
    // event loop is inside this call) until a packet with matching
    // [fblock, func] arrives or the timeout expires.
    //
    // Returns false and sets `err` on timeout, send failure, dead link, or a
    // BMAP ERROR response. `linkDead` is set when the RFCOMM link itself broke
    // (caller should trigger a reconnect).
    bool transact(BluetoothManager& bt,
                  uint8_t fblock, uint8_t func, bmap::Operator op,
                  const std::vector<uint8_t>& payload,
                  bmap::BmapResponse& out, std::string& err,
                  bool& linkDead, int timeoutMs = 2500);

private:
    bool waitForResponse(int fd, uint8_t fblock, uint8_t func,
                         bmap::BmapResponse& out, std::string& err,
                         bool& linkDead, int timeoutMs);
    void dispatchUnsolicited();

    bmap::StreamParser parser_;
    UnsolicitedHandler unsolicited_;
};

} // namespace omarchy::bose
