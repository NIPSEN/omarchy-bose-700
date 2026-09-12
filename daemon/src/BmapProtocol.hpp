#pragma once

// ---------------------------------------------------------------------------
// Bose BMAP protocol layer for the NC Headphones 700 (codename "goodyear").
//
// Packet encoding/decoding and the field parsers are adapted from bosectl
// (https://github.com/aaronsb/bosectl), Copyright (c) aaronsb, MIT license.
// The goodyear device addresses were verified against a real NC700 running
// firmware 1.8.2-11524+e0f7590 (RFCOMM channel 8).
//
// BMAP packet format: [fblock, func, flags = (operator & 0x0F), len, payload...]
// Block 31 (AudioModes) is NOT supported on this firmware (FblockNotSupp);
// CNC is driven by a direct SETGET on [1.5].
// ---------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bmap {

enum class Operator : uint8_t {
    Set = 0, Get = 1, SetGet = 2, Status = 3,
    Error = 4, Start = 5, Result = 6, Processing = 7,
};

inline const char* operator_name(Operator op) {
    switch (op) {
        case Operator::Set:        return "SET";
        case Operator::Get:        return "GET";
        case Operator::SetGet:     return "SETGET";
        case Operator::Status:     return "STATUS";
        case Operator::Error:      return "ERROR";
        case Operator::Start:      return "START";
        case Operator::Result:     return "RESULT";
        case Operator::Processing: return "PROCESSING";
    }
    return "UNKNOWN";
}

inline const char* error_name(uint8_t code) {
    switch (code) {
        case 0:  return "Unknown";
        case 1:  return "Length";
        case 2:  return "Chksum";
        case 3:  return "FblockNotSupp";
        case 4:  return "FuncNotSupp";
        case 5:  return "OpNotSupp(auth)";
        case 6:  return "InvalidData";
        case 7:  return "DataUnavail";
        case 8:  return "Runtime";
        case 9:  return "Timeout";
        case 10: return "InvalidState";
        case 15: return "InvalidTransition";
        case 20: return "InsecureTransport";
        default: return "Unknown";
    }
}

struct BmapResponse {
    uint8_t fblock;
    uint8_t func;
    Operator op;
    std::vector<uint8_t> payload;

    std::string fmt() const {
        std::string hex;
        for (auto b : payload) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            hex += buf;
        }
        std::string prefix = "[" + std::to_string(fblock) + "." +
                              std::to_string(func) + "] " + operator_name(op);
        if (op == Operator::Error && !payload.empty()) {
            return prefix + ": " + error_name(payload[0]) + " (" + hex + ")";
        }
        return prefix + ": " + hex;
    }
};

inline std::vector<uint8_t> bmap_packet(uint8_t fblock, uint8_t func,
                                        Operator op, const std::vector<uint8_t>& payload = {}) {
    std::vector<uint8_t> pkt;
    pkt.reserve(4 + payload.size());
    pkt.push_back(fblock);
    pkt.push_back(func);
    pkt.push_back(static_cast<uint8_t>(op) & 0x0F);
    if (payload.size() > 255) {
        throw std::length_error("BMAP payload exceeds single-byte length field");
    }
    pkt.push_back(static_cast<uint8_t>(payload.size()));
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

inline std::optional<BmapResponse> parse_response(const std::vector<uint8_t>& data) {
    if (data.size() < 4) return std::nullopt;
    BmapResponse resp;
    resp.fblock = data[0];
    resp.func = data[1];
    resp.op = static_cast<Operator>(data[2] & 0x0F);
    uint8_t length = data[3];
    size_t end = std::min<size_t>(4 + length, data.size());
    resp.payload.assign(data.begin() + 4, data.begin() + end);
    return resp;
}

// ---------------------------------------------------------------------------
// Stream framer: BMAP packets ride a byte stream (RFCOMM) with no outer
// framing, so complete packets are split off an append buffer by their
// length byte. A packet is at most 4 + 255 bytes.
// ---------------------------------------------------------------------------
class StreamParser {
public:
    void feed(const uint8_t* data, size_t len) {
        buf_.insert(buf_.end(), data, data + len);
    }

    // Returns the next complete packet, if one is buffered.
    std::optional<BmapResponse> next() {
        if (buf_.size() < 4) return std::nullopt;
        const size_t total = 4u + buf_[3];
        if (buf_.size() < total) return std::nullopt;
        BmapResponse resp;
        resp.fblock = buf_[0];
        resp.func = buf_[1];
        resp.op = static_cast<Operator>(buf_[2] & 0x0F);
        resp.payload.assign(buf_.begin() + 4, buf_.begin() + total);
        buf_.erase(buf_.begin(), buf_.begin() + total);
        return resp;
    }

    void reset() { buf_.clear(); }
    [[nodiscard]] size_t buffered() const noexcept { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// NC700 ("goodyear") function-block addresses. Verified on firmware 1.8.2.
// ---------------------------------------------------------------------------
namespace goodyear {

inline constexpr uint8_t kRfcommChannel = 8;

inline constexpr uint8_t kFirmwareFblock = 0,  kFirmwareFunc = 5;    // ASCII version string
inline constexpr uint8_t kNameFblock = 1,      kNameFunc = 2;        // [flag] + UTF-8 name
inline constexpr uint8_t kPromptsFblock = 1,   kPromptsFunc = 3;     // voice prompts
inline constexpr uint8_t kCncFblock = 1,       kCncFunc = 5;         // 0..10 ANC strength (wire axis inverted, see parse_cnc)
inline constexpr uint8_t kEqFblock = 1,        kEqFunc = 7;          // bass/mid/treble
inline constexpr uint8_t kMultipointFblock = 1, kMultipointFunc = 10;
inline constexpr uint8_t kSidetoneFblock = 1,  kSidetoneFunc = 11;
inline constexpr uint8_t kBatteryFblock = 2,   kBatteryFunc = 2;     // byte0 = percent

} // namespace goodyear

// ---------------------------------------------------------------------------
// Field parsers (payload in, typed value out). Verified NC700 payloads:
//   firmware  [0.5]  -> ASCII "1.8.2-11524+e0f7590"
//   name      [1.2]  -> 00 "Panthère"      (first byte is a flag)
//   prompts   [1.3]  -> 82 00 01 81 5e     (byte0: bit5 = enabled, bits0-4 = lang)
//   cnc       [1.5]  -> 0b 00 01           (raw current = p[1], max = p[0] - 1; raw 0 = max ANC)
//   eq        [1.7]  -> 3 x [min, max, current_signed, band_id]
//   multipoint[1.10] -> 03                 (bit1 = enabled)
//   sidetone  [1.11] -> 01 02 0f           (p[1]: 0 off, 1 high, 2 medium, 3 low)
//   battery   [2.2]  -> 58                 (byte0 = percent)
// ---------------------------------------------------------------------------

inline uint8_t parse_battery(const std::vector<uint8_t>& p) {
    return p.empty() ? 0 : p[0];
}

inline std::string parse_firmware(const std::vector<uint8_t>& p) {
    return {p.begin(), p.end()};
}

inline std::string parse_product_name(const std::vector<uint8_t>& p) {
    return p.size() > 1 ? std::string(p.begin() + 1, p.end()) : "";
}

// Returns {current, max}: payload[1] is the raw wire level, payload[0] - 1 the
// maximum. The raw axis is inverted relative to the Bose Music app: raw 0 is
// maximum ANC and raw 10 is full passthrough. We publish ANC strength instead
// (0 = full passthrough, 10 = max ANC), matching the app's slider.
inline std::pair<uint8_t, uint8_t> parse_cnc(const std::vector<uint8_t>& p) {
    if (p.size() >= 3) {
        uint8_t max = static_cast<uint8_t>(p[0] - 1);
        uint8_t raw = p[1];
        uint8_t strength = raw > max ? 0 : static_cast<uint8_t>(max - raw);
        return {strength, max};
    }
    return {0, 10};
}

struct EqBand {
    uint8_t band_id;
    int8_t current;
    int8_t min_val;
    int8_t max_val;
};

// Three 4-byte groups [min, max, current, band_id]; band ids 0/1/2 = bass/mid/treble.
inline std::vector<EqBand> parse_eq(const std::vector<uint8_t>& p) {
    std::vector<EqBand> bands;
    for (size_t i = 0; i + 3 < p.size(); i += 4) {
        EqBand b;
        b.min_val = static_cast<int8_t>(p[i]);
        b.max_val = static_cast<int8_t>(p[i + 1]);
        b.current = static_cast<int8_t>(p[i + 2]);
        b.band_id = p[i + 3];
        bands.push_back(b);
    }
    return bands;
}

// Empirical on this NC700 (fw 1.8.2): writing {0} reads back 0x02, writing
// {1} or {3} reads back 0x03 — bit 1 is stuck on (capability/availability
// flag) and bit 0 is the actual toggle. bosectl's parser checks bit 1, which
// therefore always reads "on" on this firmware; bit 0 matches the writes.
inline bool parse_multipoint(const std::vector<uint8_t>& p) {
    return !p.empty() && (p[0] & 0x01) != 0;
}

// The byte layout of [1.11] is only partially understood: p[0] looks like an
// enable/capability flag (always 01 here), p[1] is the level, p[2] unknown
// (0x0f). This matches bosectl's parser, which reads p[1].
inline std::string parse_sidetone(const std::vector<uint8_t>& p) {
    if (p.size() >= 2) {
        switch (p[1]) {
            case 0: return "off";
            case 1: return "high";
            case 2: return "medium";
            case 3: return "low";
        }
    }
    return "off";
}

inline std::optional<uint8_t> sidetone_id_from_name(const std::string& name) {
    if (name == "off") return 0;
    if (name == "high") return 1;
    if (name == "medium") return 2;
    if (name == "low") return 3;
    return std::nullopt;
}

// Voice prompt languages (bosectl python/pybmap/constants.py VOICE_LANGUAGES).
inline const char* const kVoiceLanguages[] = {
    "UK English", "US English", "French", "Italian", "German",
    "EU Spanish", "MX Spanish", "BR Portuguese", "Mandarin",
    "Korean", "Russian", "Polish", "Hebrew", "Turkish",
    "Dutch", "Japanese", "Cantonese", "Arabic", "Swedish",
    "Danish", "Norwegian", "Finnish", "Hindi",
};
inline constexpr size_t kVoiceLanguageCount = 23;

// Returns {enabled, language_name}: byte0 bit 5 = enabled, bits 0-4 = language id.
inline std::pair<bool, std::string> parse_voice_prompts(const std::vector<uint8_t>& p) {
    if (p.empty()) return {false, "Unknown"};
    bool enabled = ((p[0] >> 5) & 1) != 0;
    uint8_t lang = p[0] & 0x1F;
    std::string lang_name = (lang < kVoiceLanguageCount) ? kVoiceLanguages[lang] : "Unknown";
    return {enabled, lang_name};
}

// ---------------------------------------------------------------------------
// Write payload builders (SETGET on the Settings block is unauthenticated on
// this firmware — that is what makes writes work at all).
// ---------------------------------------------------------------------------

// CNC ANC strength 0..10 (0 = full passthrough / transparent, 10 = max ANC).
// The wire axis is inverted: raw 0 = max ANC, raw 10 = passthrough.
inline std::vector<uint8_t> build_cnc(uint8_t level) {
    return {static_cast<uint8_t>(level > 10 ? 0 : 10 - level), 1};
}

// One EQ band per SETGET: {signed value, band id}.
inline std::vector<uint8_t> build_eq_band(int8_t value, uint8_t band_id) {
    return {static_cast<uint8_t>(value), band_id};
}

// Sidetone: {0x01, level id}; the leading 01 mirrors what GET returns in p[0].
inline std::vector<uint8_t> build_sidetone(uint8_t level_id) {
    return {1, level_id};
}

// Voice prompts on/off preserving the current language (low 5 bits).
inline std::vector<uint8_t> build_voice_prompts(bool enabled, uint8_t current_byte0) {
    uint8_t lang = current_byte0 & 0x1F;
    return {static_cast<uint8_t>(((enabled ? 1 : 0) << 5) | lang)};
}

inline std::vector<uint8_t> build_multipoint(bool on) {
    return {static_cast<uint8_t>(on ? 1 : 0)};
}

} // namespace bmap
