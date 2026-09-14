#pragma once

// ---------------------------------------------------------------------------
// Event-driven BlueZ link-state watcher over sd-bus (libsystemd).
//
// Subscribes on the SYSTEM bus to
// org.freedesktop.DBus.Properties.PropertiesChanged signals for
// org.bluez.Device1 and reports the `Connected` boolean transitions of one
// target headset. The daemon multiplexes the sd-bus fd in its existing poll
// loop, replacing the periodic bluetoothctl state scrape while the headset
// is paired but ACL-down.
// ---------------------------------------------------------------------------

#include <functional>
#include <optional>
#include <string>

#include <systemd/sd-bus.h>

namespace omarchy::bose {

// Pure, bus-free helper: does `path` refer to the BlueZ device object of the
// given headset? Matches any adapter (/org/bluez/hci*/dev_XX_XX_...) by the
// MAC-derived suffix.
bool bluezDevicePathMatches(const std::string& path, const std::string& macAddress);

class BluezWatcher {
public:
    BluezWatcher() = default;
    ~BluezWatcher();

    BluezWatcher(const BluezWatcher&) = delete;
    BluezWatcher& operator=(const BluezWatcher&) = delete;

    // Headset whose Connected transitions we report, matched case-insensitively
    // against the dev_XX_XX_..._XX path suffix.
    void setTargetMac(const std::string& macAddress) { targetMac_ = macAddress; }

    // Invoked with the new Connected value whenever the target device's
    // org.bluez.Device1 Connected property changes.
    void setCallback(std::function<void(bool connected)> cb) { callback_ = std::move(cb); }

    // Opens the system bus and installs the PropertiesChanged match. On any
    // failure the watcher degrades to inactive: start() returns false,
    // lastError() explains why, and poll integration must be skipped (the
    // caller falls back to timer-based polling).
    bool start();
    void stop();

    [[nodiscard]] bool isActive() const noexcept { return bus_ != nullptr; }
    [[nodiscard]] std::string getLastError() const { return lastError_; }

    // Event-loop integration: register the sd-bus fd with the events/timeout
    // BlueZ asks for, call process() when it becomes readable (or when the
    // timeout expired). getPollTimeoutMs() is in poll(2) units (-1 = wait
    // forever); a returned 0 means there is pending work to process now.
    [[nodiscard]] int getPollFd() const;
    [[nodiscard]] short getPollEvents() const;
    [[nodiscard]] int getPollTimeoutMs() const;
    void process();

private:
    static int onPropertiesChanged(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    void handleMessage(sd_bus_message* m);

    sd_bus* bus_{nullptr};
    std::string targetMac_;
    std::function<void(bool)> callback_;
    std::string lastError_;
};

} // namespace omarchy::bose
