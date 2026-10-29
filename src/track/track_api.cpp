#include "track/track_api.h"

#include <string>

#include "constants.h"
#include "util/logger.h"

namespace sensorswave {
namespace {

// Per-key fallback: implementations promise not to throw, but this still
// catches defensively — a failed key only skips itself and never blocks the
// other keys or the event itself. Unavailable or empty value → key absent, no
// empty fallback.
void putString(PropertyMap* props, const char* key, ISystemInfo* info,
               bool (ISystemInfo::*query)(std::string*)) {
    try {
        std::string value;
        if ((info->*query)(&value) && !value.empty()) {
            (*props)[key] = PropertyValue::fromString(value);
        }
    } catch (const std::exception& e) {
        Logger::warn(std::string("preset property skipped: ") + key + ": " + e.what());
    } catch (...) {
        Logger::warn(std::string("preset property skipped: ") + key);
    }
}

void putInt64(PropertyMap* props, const char* key, ISystemInfo* info,
              bool (ISystemInfo::*query)(int64_t*)) {
    try {
        int64_t value = 0;
        if ((info->*query)(&value)) {
            (*props)[key] = PropertyValue::fromInt64(value);
        }
    } catch (const std::exception& e) {
        Logger::warn(std::string("preset property skipped: ") + key + ": " + e.what());
    } catch (...) {
        Logger::warn(std::string("preset property skipped: ") + key);
    }
}

void putInt(PropertyMap* props, const char* key, ISystemInfo* info,
            bool (ISystemInfo::*query)(int*)) {
    try {
        int value = 0;
        if ((info->*query)(&value)) {
            (*props)[key] = PropertyValue::fromInt64(value);
        }
    } catch (const std::exception& e) {
        Logger::warn(std::string("preset property skipped: ") + key + ": " + e.what());
    } catch (...) {
        Logger::warn(std::string("preset property skipped: ") + key);
    }
}

// $region centralized validation: platform implementations normalize on their
// own, and this adds a strict alpha-2 check as a backstop — invalid values
// (M.49 numeric codes / GEOIDs / overlong strings) drop the key so no dirty
// data is shipped
void putRegion(PropertyMap* props, ISystemInfo* info) {
    try {
        std::string raw;
        if (!info->region(&raw) || raw.empty()) {
            return;
        }
        std::string normalized;
        if (normalizeRegionCode(raw, &normalized)) {
            (*props)["$region"] = PropertyValue::fromString(normalized);
        } else {
            Logger::warn("preset property skipped: $region (not ISO 3166-1 alpha-2): " + raw);
        }
    } catch (const std::exception& e) {
        Logger::warn(std::string("preset property skipped: $region: ") + e.what());
    } catch (...) {
        Logger::warn("preset property skipped: $region");
    }
}

// $device_id centralized normalization: platform sources differ in case —
// macOS IOPlatformUUID is uppercase, Linux machine-id / Windows MachineGuid are
// lowercase but not guaranteed — so preset values are lowercased uniformly and
// the data pipeline never needs per-platform normalization again. Value /
// empty / exception semantics match putString; only the normalization is added
void putDeviceId(PropertyMap* props, ISystemInfo* info) {
    putString(props, "$device_id", info, &ISystemInfo::deviceId);
    const auto it = props->find("$device_id");
    if (it != props->end()) {
        it->second = PropertyValue::fromString(lowerAsciiCopy(it->second.asString()));
    }
}

}  // namespace

PropertyMap buildPresetProperties(ISystemInfo* systemInfo) {
    PropertyMap props;

    // Platform-independent automatic set: always present, independent of
    // systemInfo availability
    props["$lib"] = PropertyValue::fromString(LIB_NAME);
    props["$lib_version"] = PropertyValue::fromString(LIB_VERSION);

    if (systemInfo == nullptr) {
        return props;  // defensive: only the platform-independent keys remain,
                       // events still go out
    }

    // Automatic set (platform-queried keys): built once at init
    putString(&props, "$os", systemInfo, &ISystemInfo::osName);
    putString(&props, "$os_version", systemInfo, &ISystemInfo::osVersion);
    putInt64(&props, "$timezone_offset", systemInfo, &ISystemInfo::timezoneOffsetSeconds);
    putString(&props, "$language", systemInfo, &ISystemInfo::language);

    // Best-effort set: unavailable → key absent, no empty fallback
    putString(&props, "$model", systemInfo, &ISystemInfo::model);
    putInt(&props, "$screen_width", systemInfo, &ISystemInfo::screenWidth);
    putInt(&props, "$screen_height", systemInfo, &ISystemInfo::screenHeight);
    putString(&props, "$app_id", systemInfo, &ISystemInfo::appId);
    putString(&props, "$app_name", systemInfo, &ISystemInfo::appName);
    putString(&props, "$app_version", systemInfo, &ISystemInfo::appVersion);

    // Best-effort set (App environment properties, completing the full set):
    // same absence semantics as above.
    // $network_type / $wifi are per-event dynamic keys, not built here —
    // EventBuilder queries them per event via NetworkTypeProvider (network
    // state changes over time)
    putString(&props, "$brand", systemInfo, &ISystemInfo::brand);
    putString(&props, "$manufacturer", systemInfo, &ISystemInfo::manufacturer);
    putDeviceId(&props, systemInfo);
    putRegion(&props, systemInfo);

    return props;
}

bool isValidEventName(const std::string& eventName) {
    // Only an empty name is invalid; the tracking entry point silently drops it
    // and no request is produced. A '$' prefix is not rejected.
    return !eventName.empty();
}

bool isReservedEventName(const std::string& eventName) {
    // The '$' prefix is reserved for system preset events. On a hit the tracking
    // entry point still sends and only emits a warning (hosts may report
    // preset-shaped events on demand; they must not be silently dropped).
    return !eventName.empty() && eventName.front() == '$';
}

void warnReservedPropertyKeys(const PropertyMap& props, const char* api) {
    // Custom property keys follow the same policy as event names — not blocked,
    // per-key warning
    for (const auto& entry : props) {
        if (!entry.first.empty() && entry.first.front() == '$') {
            Logger::warn(
                std::string(api) +
                ": '$'-prefixed property key is reserved for preset properties: " + entry.first);
        }
    }
}

}  // namespace sensorswave
