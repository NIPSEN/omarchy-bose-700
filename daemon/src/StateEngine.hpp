#pragma once

// ---------------------------------------------------------------------------
// Daemon state model + atomic status.json persistence for the Bose NC700.
//
// Atomic-write engine adapted from omarchy-sony-xm3 (Copyright (c)
// Kevin Cardwell, MIT license).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <filesystem>
#include <mutex>
#include <functional>
#include <vector>

namespace omarchy::bose {

// ---------------------------------------------------------------------------
// Headset state. -1 / empty means "unknown": unknown fields are omitted from
// the JSON rather than filled with plausible-looking defaults.
// ---------------------------------------------------------------------------
struct BoseState {
    int schema{1};
    bool connected{false};

    // device
    std::string deviceName;
    std::string deviceModel{"Bose NC Headphones 700"};
    std::string firmware;
    std::string address;

    // battery
    int batteryLevel{-1};           // percent

    // cnc (0 = full passthrough/transparent, max = strongest ANC)
    int cncLevel{-1};
    int cncMax{-1};

    // eq
    bool eqKnown{false};
    int eqBass{0};
    int eqMid{0};
    int eqTreble{0};
    int eqMin{-10};
    int eqMax{10};

    // sidetone: "", else "off" | "low" | "medium" | "high"
    std::string sidetone;

    // voice prompts
    bool promptsKnown{false};
    bool promptsEnabled{false};
    std::string promptsLanguage;

    // multipoint: -1 unknown
    int multipoint{-1};

    int64_t lastUpdated{0};         // epoch seconds

    [[nodiscard]] std::string toJson() const;
};

// Minimal JSON string escaping ( enough for device names / languages).
std::string jsonEscape(const std::string& s);

// Epoch seconds -> ISO8601 UTC ("2026-09-12T07:00:00Z").
std::string iso8601Utc(int64_t epochSeconds);

class StateEngine {
public:
    explicit StateEngine(const std::filesystem::path& customStatePath = "");
    ~StateEngine();

    StateEngine(const StateEngine&) = delete;
    StateEngine& operator=(const StateEngine&) = delete;
    StateEngine(StateEngine&&) = delete;
    StateEngine& operator=(StateEngine&&) = delete;

    // -----------------------------------------------------------------------
    // Path Resolution & Filesystem Lifecycle
    // -----------------------------------------------------------------------
    static std::filesystem::path resolveStateFilePath(const std::filesystem::path& customStatePath = "");
    static bool ensureStateDirectory(const std::filesystem::path& dirPath);

    bool initialize(bool initialConnected = false);

    // Shutdown cleanup: unlinks status.json and any stale temporary files
    void cleanup();

    // -----------------------------------------------------------------------
    // Atomic Persistence Engine
    // -----------------------------------------------------------------------
    // Writes JSON content to <status.json>.tmp.<pid> with mode 0600,
    // calls fsync(), closes, and renames atomically to <status.json>.
    bool writeAtomic(const std::string& jsonContent);

    bool commit();
    bool save() { return commit(); }

    // -----------------------------------------------------------------------
    // State Accessors (Thread-Safe)
    // -----------------------------------------------------------------------
    [[nodiscard]] BoseState getState() const;
    [[nodiscard]] std::string getStatusJson() const;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] const std::filesystem::path& getStateFilePath() const noexcept { return stateFilePath_; }

    // -----------------------------------------------------------------------
    // State Mutators (each persists atomically)
    // -----------------------------------------------------------------------
    void setConnected(bool connected);
    void setDeviceInfo(const std::string& name, const std::string& firmware,
                       const std::string& address);
    void setDeviceName(const std::string& name);
    void setFirmware(const std::string& firmware);
    void setBatteryLevel(int level);
    void setCnc(int level, int max);
    void setEq(int bass, int mid, int treble, int min, int max);
    void setSidetone(const std::string& level);
    void setVoicePrompts(bool enabled, const std::string& language);
    void setMultipoint(bool enabled);

    // Transactional mutation
    void modifyState(const std::function<void(BoseState&)>& mutator);

    using StateListener = std::function<void(const BoseState&)>;
    void addListener(StateListener listener);

private:
    std::string serializeStateLocked() const;
    void notifyListenersLocked();
    void touchAndPublishLocked(std::string& outJson);

    mutable std::mutex mutex_;
    std::filesystem::path stateFilePath_;
    std::filesystem::path stateDir_;
    BoseState state_;
    std::vector<StateListener> listeners_;
    bool cleanedUp_{false};
};

} // namespace omarchy::bose
