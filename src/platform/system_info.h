#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace sensorswave {

// Platform system info collection interface (data source for preset properties).
// Per-key best-effort: return false when unavailable and the key is left out of
// the preset properties (a missing key is absent, never filled with a placeholder).
// Implementations must not throw (catch all errors internally); test doubles are
// injected to verify the missing-key semantics.
class ISystemInfo {
   public:
    virtual ~ISystemInfo() = default;

    virtual bool osName(std::string* out) = 0;             // $os: macOS / Windows / Linux
    virtual bool osVersion(std::string* out) = 0;          // $os_version (version number only)
    virtual bool model(std::string* out) = 0;              // $model
    virtual bool screenWidth(int* out) = 0;                // $screen_width (pixels)
    virtual bool screenHeight(int* out) = 0;               // $screen_height (pixels)
    virtual bool timezoneOffsetSeconds(int64_t* out) = 0;  // $timezone_offset (east-positive)
    virtual bool language(std::string* out) = 0;           // $language (BCP 47)
    virtual bool appId(std::string* out) = 0;              // $app_id (best-effort)
    virtual bool appName(std::string* out) = 0;            // $app_name (best-effort)
    virtual bool appVersion(std::string* out) = 0;         // $app_version (best-effort)
    virtual bool brand(std::string* out) = 0;              // $brand (best-effort)
    virtual bool manufacturer(std::string* out) = 0;       // $manufacturer (best-effort)
    virtual bool deviceId(
        std::string* out) = 0;  // $device_id (read-only platform machine id; lowercased)
    virtual bool networkType(std::string* out) = 0;  // $network_type (all-lowercase enum)
    virtual bool region(std::string* out) = 0;       // $region (ISO 3166-1 alpha-2, uppercase)
};

// ASCII uppercase normalization ($region etc.): letter range only, avoiding
// locale dependence; shared by the platform implementations
inline std::string upperAsciiCopy(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return value;
}

// ASCII lowercase normalization ($device_id is uniformly lowercased): letter
// range normalization symmetric to upperAsciiCopy, avoiding locale dependence;
// other bytes are left untouched
inline std::string lowerAsciiCopy(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

// Strict $region validation: after uppercase normalization the value must be
// exactly 2 ASCII letters (ISO 3166-1 alpha-2).
// M.49 numeric codes ("001"/"419"), Windows GEOIDs ("999"), overlong values
// ("usa"), and empty values -> false and the key is omitted (best-effort
// semantics: prefer a missing key over shipping dirty data)
inline bool normalizeRegionCode(std::string value, std::string* out) {
    value = upperAsciiCopy(value);
    if (value.size() != 2) {
        return false;
    }
    for (const char c : value) {
        if (c < 'A' || c > 'Z') {
            return false;
        }
    }
    *out = std::move(value);
    return true;
}

// Normalizes host language identifiers to a BCP 47 approximation (uniform
// normalization of each host's raw value):
// CFLocale / POSIX identifiers get '_' converted to '-' and the '@keyword'
// locale extension suffix truncated (e.g. "zh_Hans_CN@calendar=gregorian" ->
// "zh-Hans-CN"); values already in BCP 47 form are returned unchanged
inline std::string normalizeLanguageTag(std::string value) {
    const std::size_t at = value.find('@');
    if (at != std::string::npos) {
        value.resize(at);
    }
    for (char& c : value) {
        if (c == '_') {
            c = '-';
        }
    }
    return value;
}

// Platform defaults: macOS / Linux -> system_info_posix.cpp, Windows -> system_info_windows.cpp
std::unique_ptr<ISystemInfo> createPlatformSystemInfo();

}  // namespace sensorswave
