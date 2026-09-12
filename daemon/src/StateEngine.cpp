// State model + atomic status.json persistence.
// Atomic-write engine adapted from omarchy-sony-xm3 (Copyright (c)
// Kevin Cardwell, MIT license).

#include "StateEngine.hpp"

#include <iostream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>

namespace omarchy::bose {

namespace {

constexpr const char* kStateDirName = "bose-700";

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string iso8601Utc(int64_t epochSeconds) {
    std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string BoseState::toJson() const {
    std::ostringstream j;
    j << "{\"schema\":" << schema
      << ",\"connected\":" << (connected ? "true" : "false");

    // Last-known device identity is kept when disconnected; `connected` is
    // the source of truth.
    if (!deviceName.empty() || !firmware.empty() || !address.empty()) {
        j << ",\"device\":{";
        bool first = true;
        auto field = [&](const char* key, const std::string& val) {
            if (val.empty()) return;
            if (!first) j << ",";
            first = false;
            j << "\"" << key << "\":\"" << jsonEscape(val) << "\"";
        };
        field("name", deviceName);
        field("model", deviceModel);
        field("firmware", firmware);
        field("address", address);
        j << "}";
    }

    if (batteryLevel >= 0) {
        j << ",\"battery\":{\"level\":" << batteryLevel << "}";
    }
    if (cncLevel >= 0) {
        j << ",\"cnc\":{\"level\":" << cncLevel;
        if (cncMax >= 0) j << ",\"max\":" << cncMax;
        j << "}";
    }
    if (eqKnown) {
        j << ",\"eq\":{\"bass\":" << eqBass
          << ",\"mid\":" << eqMid
          << ",\"treble\":" << eqTreble
          << ",\"min\":" << eqMin
          << ",\"max\":" << eqMax << "}";
    }
    if (!sidetone.empty()) {
        j << ",\"sidetone\":\"" << jsonEscape(sidetone) << "\"";
    }
    if (promptsKnown) {
        j << ",\"voice_prompts\":{\"enabled\":" << (promptsEnabled ? "true" : "false");
        if (!promptsLanguage.empty()) {
            j << ",\"language\":\"" << jsonEscape(promptsLanguage) << "\"";
        }
        j << "}";
    }
    if (multipoint >= 0) {
        j << ",\"multipoint\":" << (multipoint ? "true" : "false");
    }

    j << ",\"updated_at\":\"" << iso8601Utc(lastUpdated) << "\"}";
    return j.str();
}

// ---------------------------------------------------------------------------
// Path Resolution
// ---------------------------------------------------------------------------

std::filesystem::path StateEngine::resolveStateFilePath(const std::filesystem::path& customStatePath) {
    if (!customStatePath.empty()) {
        if (customStatePath.filename() == "status.json") {
            return customStatePath;
        }
        if (customStatePath.filename() == kStateDirName) {
            return customStatePath / "status.json";
        }
        return customStatePath / kStateDirName / "status.json";
    }

    // 1. $XDG_STATE_HOME
    const char* xdgState = std::getenv("XDG_STATE_HOME");
    if (xdgState && *xdgState != '\0') {
        std::filesystem::path p(xdgState);
        if (p.filename() == kStateDirName) {
            return p / "status.json";
        }
        return p / kStateDirName / "status.json";
    }

    // 2. Fallback: $HOME/.local/state
    const char* home = std::getenv("HOME");
    if (home && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "state" / kStateDirName / "status.json";
    }

    // 3. Fallback: getpwuid(getuid())
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *(pw->pw_dir) != '\0') {
        return std::filesystem::path(pw->pw_dir) / ".local" / "state" / kStateDirName / "status.json";
    }

    // 4. Absolute fallback
    return std::filesystem::path("/tmp") / kStateDirName / "status.json";
}

bool StateEngine::ensureStateDirectory(const std::filesystem::path& dirPath) {
    std::error_code ec;
    std::filesystem::create_directories(dirPath, ec);
    if (ec) {
        std::cerr << "[StateEngine] Failed to create directories " << dirPath << ": " << ec.message() << std::endl;
        return false;
    }

    if (::chmod(dirPath.c_str(), S_IRWXU) != 0) {
        std::cerr << "[StateEngine] Failed to chmod 0700 on " << dirPath << ": " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Construction & Lifecycle
// ---------------------------------------------------------------------------

StateEngine::StateEngine(const std::filesystem::path& customStatePath)
    : stateFilePath_(resolveStateFilePath(customStatePath)),
      stateDir_(stateFilePath_.parent_path()) {
    state_.lastUpdated = nowSeconds();
}

StateEngine::~StateEngine() {
    cleanup();
}

bool StateEngine::initialize(bool initialConnected) {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanedUp_ = false;
    state_.connected = initialConnected;
    state_.lastUpdated = nowSeconds();

    if (!ensureStateDirectory(stateDir_)) {
        return false;
    }

    return writeAtomic(serializeStateLocked());
}

void StateEngine::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cleanedUp_) {
        return;
    }
    cleanedUp_ = true;

    if (!stateFilePath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(stateFilePath_, ec);

        std::string tmpPath = stateFilePath_.string() + ".tmp." + std::to_string(::getpid());
        std::filesystem::remove(tmpPath, ec);
    }
}

// ---------------------------------------------------------------------------
// Atomic Persistence
// ---------------------------------------------------------------------------

bool StateEngine::writeAtomic(const std::string& jsonContent) {
    if (stateFilePath_.empty()) {
        return false;
    }

    if (!ensureStateDirectory(stateDir_)) {
        return false;
    }

    std::string tmpPath = stateFilePath_.string() + ".tmp." + std::to_string(::getpid());

    int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        std::cerr << "[StateEngine] Failed to open tmp file " << tmpPath << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    // Guarantee strictly 0600 mode regardless of umask
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        std::cerr << "[StateEngine] Failed to fchmod 0600 on " << tmpPath << ": " << std::strerror(errno) << std::endl;
        ::close(fd);
        ::unlink(tmpPath.c_str());
        return false;
    }

    const char* buf = jsonContent.data();
    size_t remaining = jsonContent.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, buf, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[StateEngine] Write error: " << std::strerror(errno) << std::endl;
            ::close(fd);
            ::unlink(tmpPath.c_str());
            return false;
        }
        buf += written;
        remaining -= static_cast<size_t>(written);
    }

    if (!jsonContent.empty() && jsonContent.back() != '\n') {
        char nl = '\n';
        while (::write(fd, &nl, 1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
    }

    if (::fsync(fd) != 0) {
        std::cerr << "[StateEngine] fsync failed: " << std::strerror(errno) << std::endl;
        ::close(fd);
        ::unlink(tmpPath.c_str());
        return false;
    }

    if (::close(fd) != 0) {
        ::unlink(tmpPath.c_str());
        return false;
    }

    if (::rename(tmpPath.c_str(), stateFilePath_.c_str()) != 0) {
        std::cerr << "[StateEngine] rename failed (" << tmpPath << " -> " << stateFilePath_ << "): "
                  << std::strerror(errno) << std::endl;
        ::unlink(tmpPath.c_str());
        return false;
    }

    return true;
}

bool StateEngine::commit() {
    std::string payload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        touchAndPublishLocked(payload);
    }
    return writeAtomic(payload);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string StateEngine::serializeStateLocked() const {
    return state_.toJson() + "\n";
}

void StateEngine::touchAndPublishLocked(std::string& outJson) {
    state_.lastUpdated = nowSeconds();
    outJson = serializeStateLocked();
    notifyListenersLocked();
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

BoseState StateEngine::getState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string StateEngine::getStatusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.toJson();
}

bool StateEngine::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.connected;
}

// ---------------------------------------------------------------------------
// State Mutators
// ---------------------------------------------------------------------------

void StateEngine::setConnected(bool connected) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.connected = connected;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setDeviceInfo(const std::string& name, const std::string& firmware,
                                const std::string& address) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!name.empty()) state_.deviceName = name;
        if (!firmware.empty()) state_.firmware = firmware;
        if (!address.empty()) state_.address = address;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setDeviceName(const std::string& name) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.deviceName = name;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setFirmware(const std::string& firmware) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.firmware = firmware;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setBatteryLevel(int level) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.batteryLevel = level;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setCnc(int level, int max) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.cncLevel = level;
        state_.cncMax = max;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setEq(int bass, int mid, int treble, int min, int max) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.eqKnown = true;
        state_.eqBass = bass;
        state_.eqMid = mid;
        state_.eqTreble = treble;
        state_.eqMin = min;
        state_.eqMax = max;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setSidetone(const std::string& level) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.sidetone = level;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setVoicePrompts(bool enabled, const std::string& language) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.promptsKnown = true;
        state_.promptsEnabled = enabled;
        state_.promptsLanguage = language;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::setMultipoint(bool enabled) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.multipoint = enabled ? 1 : 0;
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

void StateEngine::modifyState(const std::function<void(BoseState&)>& mutator) {
    std::string payloadJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mutator(state_);
        touchAndPublishLocked(payloadJson);
    }
    writeAtomic(payloadJson);
}

// ---------------------------------------------------------------------------
// Listeners
// ---------------------------------------------------------------------------

void StateEngine::addListener(StateListener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.push_back(std::move(listener));
}

void StateEngine::notifyListenersLocked() {
    for (const auto& listener : listeners_) {
        if (listener) {
            listener(state_);
        }
    }
}

} // namespace omarchy::bose
