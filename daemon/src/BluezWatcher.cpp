// Event-driven BlueZ link-state watcher over sd-bus.

#include "BluezWatcher.hpp"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <sys/poll.h>

namespace omarchy::bose {

// "4C:87:5D:A3:D1:4F" -> "dev_4C_87_5D_A3_D1_4F"
static std::string macToDeviceSuffix(std::string mac) {
    for (auto& c : mac) {
        if (c == ':') c = '_';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return "dev_" + mac;
}

bool bluezDevicePathMatches(const std::string& path, const std::string& macAddress) {
    if (macAddress.size() != 17) return false;
    const std::string suffix = macToDeviceSuffix(macAddress);
    if (path.size() < suffix.size()) return false;
    if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    // Must be an /org/bluez/<adapter>/ object, not just any path whose tail
    // happens to look like our MAC.
    return path.rfind("/org/bluez/", 0) == 0;
}

// ---------------------------------------------------------------------------

BluezWatcher::~BluezWatcher() {
    stop();
}

bool BluezWatcher::start() {
    if (bus_) return true;

    sd_bus* bus = nullptr;
    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        lastError_ = "sd_bus_open_system: " + std::string(strerror(-r));
        return false;
    }

    r = sd_bus_add_match(bus, nullptr,
                         "type='signal'"
                         ",sender='org.bluez'"
                         ",interface='org.freedesktop.DBus.Properties'"
                         ",member='PropertiesChanged'"
                         ",arg0='org.bluez.Device1'",
                         &BluezWatcher::onPropertiesChanged, this);
    if (r < 0) {
        lastError_ = "sd_bus_add_match: " + std::string(strerror(-r));
        sd_bus_unref(bus);
        return false;
    }

    bus_ = bus;

    // Flush any already queued messages so getPollEvents()/getPollTimeoutMs()
    // report a state the poll loop can act on from the first iteration.
    process();

    fprintf(stderr, "[BT] Watching BlueZ PropertiesChanged on the system bus (event-driven ACL link tracking)\n");
    fflush(stderr);
    return true;
}

void BluezWatcher::stop() {
    if (bus_) {
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
    }
}

int BluezWatcher::getPollFd() const {
    if (!bus_) return -1;
    return sd_bus_get_fd(bus_);
}

short BluezWatcher::getPollEvents() const {
    if (!bus_) return 0;
    int events = sd_bus_get_events(bus_);
    if (events < 0) return 0;
    return static_cast<short>(events);
}

int BluezWatcher::getPollTimeoutMs() const {
    if (!bus_) return -1;
    uint64_t usec = UINT64_MAX;
    if (sd_bus_get_timeout(bus_, &usec) < 0) return -1;
    if (usec == UINT64_MAX) return -1;
    uint64_t ms = usec / 1000 + ((usec % 1000) ? 1 : 0);
    return ms > static_cast<uint64_t>(INT32_MAX) ? INT32_MAX : static_cast<int>(ms);
}

void BluezWatcher::process() {
    if (!bus_) return;
    for (;;) {
        int r = sd_bus_process(bus_, nullptr);
        if (r < 0) {
            fprintf(stderr, "[BT] sd_bus_process failed: %s — falling back to polling\n",
                    strerror(-r));
            fflush(stderr);
            stop();
            return;
        }
        if (r == 0) break; // queue drained
    }
}

// ---------------------------------------------------------------------------

int BluezWatcher::onPropertiesChanged(sd_bus_message* m, void* userdata, sd_bus_error* /*retError*/) {
    auto* self = static_cast<BluezWatcher*>(userdata);
    if (self) {
        self->handleMessage(m);
    }
    return 0;
}

// PropertiesChanged body: (s a{sv} as) — interface name, changed properties,
// invalidated property names. We care about the `Connected` boolean.
void BluezWatcher::handleMessage(sd_bus_message* m) {
    if (targetMac_.empty() || !callback_) return;

    const char* path = sd_bus_message_get_path(m);
    if (!path || !bluezDevicePathMatches(path, targetMac_)) return;

    const char* iface = nullptr;
    if (sd_bus_message_read(m, "s", &iface) < 0 || !iface) return;
    if (std::string(iface) != "org.bluez.Device1") return;

    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") < 0) return;
    while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
        const char* key = nullptr;
        if (sd_bus_message_read(m, "s", &key) < 0 || !key) break;
        const bool isConnectedKey = std::string(key) == "Connected";
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "b") < 0) break;
        if (isConnectedKey) {
            int value = 0;
            if (sd_bus_message_read(m, "b", &value) >= 0) {
                callback_(value != 0);
            }
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m); // variant
        sd_bus_message_exit_container(m); // dict entry
    }
    sd_bus_message_exit_container(m); // a{sv}
}

} // namespace omarchy::bose
