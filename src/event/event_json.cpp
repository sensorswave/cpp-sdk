#include "event/event_json.h"

#include <cmath>

#include <nlohmann/json.hpp>

namespace sensorswave {
namespace {

// Nesting depth limit for property serialization: guards against a host-built,
// over-deep kMapList chain blowing the recursion stack
constexpr int kMaxPropertyDepth = 8;

// JSON output always uses the replace error strategy: invalid UTF-8 bytes
// become U+FFFD instead of throwing. Serialization is the last gate for an
// event and must never become the source of a whole event being dropped, let
// alone a sender-thread terminate — GBK fragments / binary blobs from the host
// are common inputs
std::string dumpReplace(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// PropertyValue → JSON: type-switched serialization (getters / null already
// resolved at build time). Returns false = skip the whole pair (invalid values
// must not abort the event): non-finite doubles (NaN/Inf have no JSON numeric
// representation) and over-deep nesting
bool toJson(const PropertyValue& value, int depth, nlohmann::json* out) {
    switch (value.type()) {
        case PropertyValue::Type::kString:
            *out = value.asString();
            return true;
        case PropertyValue::Type::kDouble:
            if (!std::isfinite(value.asDouble())) {
                return false;
            }
            *out = value.asDouble();
            return true;
        case PropertyValue::Type::kInt64:
            *out = value.asInt64();
            return true;
        case PropertyValue::Type::kBool:
            *out = value.asBool();
            return true;
        case PropertyValue::Type::kStringList: {
            nlohmann::json array = nlohmann::json::array();
            for (const std::string& item : value.asStringList()) {
                array.push_back(item);
            }
            *out = std::move(array);
            return true;
        }
        case PropertyValue::Type::kMapList: {
            // Structured properties (e.g. $exception_frames): object frames in
            // an array. Depth is already vetted by the exceedsDepth precheck;
            // this check is defense in depth
            if (depth > kMaxPropertyDepth) {
                return false;
            }
            nlohmann::json array = nlohmann::json::array();
            for (const PropertyMap& item : value.asMapList()) {
                nlohmann::json object = nlohmann::json::object();
                for (const auto& entry : item) {
                    nlohmann::json child;
                    if (toJson(entry.second, depth + 1, &child)) {
                        object[entry.first] = std::move(child);
                    }
                }
                array.push_back(std::move(object));
            }
            *out = std::move(array);
            return true;
        }
        case PropertyValue::Type::kNull:
        case PropertyValue::Type::kGetter:
            break;
    }
    return false;  // already filtered at build time; defensive fallback
}

// Nesting-depth precheck (early exit: short-circuits once the limit is hit, so
// recursion depth stays bounded). An over-deep property skips its whole key
// rather than silently truncating intermediate layers
bool exceedsDepth(const PropertyValue& value, int depth) {
    if (depth > kMaxPropertyDepth) {
        return true;
    }
    if (value.type() != PropertyValue::Type::kMapList) {
        return false;
    }
    for (const PropertyMap& item : value.asMapList()) {
        for (const auto& entry : item) {
            if (exceedsDepth(entry.second, depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

// user_properties payload → JSON object: same shape as propertiesToJson, but
// kNull emits JSON null — $unset semantics require passing key: null through
// (aligned with the JS profileUnset); null is a legitimate payload on this
// channel, not an invalid value. kGetter is resolved at build time; any
// occurrence here is skipped defensively
nlohmann::json userOpPayloadToJson(const PropertyMap& properties) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& entry : properties) {
        if (exceedsDepth(entry.second, 0)) {
            continue;
        }
        if (entry.second.type() == PropertyValue::Type::kNull) {
            object[entry.first] = nullptr;
            continue;
        }
        nlohmann::json value;
        if (toJson(entry.second, 0, &value)) {
            object[entry.first] = std::move(value);
        }
    }
    return object;
}

// Property map → JSON object: tries each pair and skips a key on failure
// (without interrupting the other properties)
}  // namespace

nlohmann::json propertiesToJson(const PropertyMap& properties) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& entry : properties) {
        if (exceedsDepth(entry.second, 0)) {
            continue;
        }
        nlohmann::json value;
        if (toJson(entry.second, 0, &value)) {
            object[entry.first] = std::move(value);
        }
    }
    return object;
}

std::string serializeEventJson(const Event& event) {
    nlohmann::json root;
    root["event"] = event.name;
    root["time"] = event.timeMs;
    root["trace_id"] = event.traceId;
    root["anon_id"] = event.anonId;
    if (!event.loginId.empty()) {
        root["login_id"] = event.loginId;  // omitted when not logged in
    }

    nlohmann::json properties = propertiesToJson(event.properties);
    root["properties"] = std::move(properties);

    // User-property operation channel: user_properties[op] = payload;
    // $delete is boolean true; null serializes as JSON null
    if (!event.userOps.empty()) {
        nlohmann::json userProperties = nlohmann::json::object();
        for (const UserOp& op : event.userOps) {
            if (op.deleteFlag) {
                userProperties[op.op] = true;
                continue;
            }
            userProperties[op.op] = userOpPayloadToJson(op.payload);
        }
        root["user_properties"] = std::move(userProperties);
    }

    return dumpReplace(root);
}

// JSON value → PropertyValue; types outside the supported model (nested
// objects etc.) are skipped
bool propertyFromJson(const nlohmann::json& value, PropertyValue* out) {
    if (value.is_string()) {
        *out = PropertyValue::fromString(value.get<std::string>());
        return true;
    }
    if (value.is_number_integer()) {
        *out = PropertyValue::fromInt64(value.get<int64_t>());
        return true;
    }
    if (value.is_number_float()) {
        *out = PropertyValue::fromDouble(value.get<double>());
        return true;
    }
    if (value.is_boolean()) {
        *out = PropertyValue::fromBool(value.get<bool>());
        return true;
    }
    if (value.is_array()) {
        // All-string elements → string list; any object element → structured
        // property (frame array, kept faithful across the retransmission
        // round-trip); mixed arrays are treated as object arrays with string
        // items dropped
        bool hasObject = false;
        for (const nlohmann::json& item : value) {
            if (item.is_object()) {
                hasObject = true;
                break;
            }
        }
        if (hasObject) {
            std::vector<PropertyMap> maps;
            for (const nlohmann::json& item : value) {
                if (!item.is_object()) {
                    continue;
                }
                PropertyMap map;
                for (auto kv = item.begin(); kv != item.end(); ++kv) {
                    PropertyValue entry;
                    if (propertyFromJson(kv.value(), &entry)) {
                        map[kv.key()] = std::move(entry);
                    }
                }
                maps.push_back(std::move(map));
            }
            *out = PropertyValue::fromMapList(std::move(maps));
            return true;
        }
        std::vector<std::string> items;
        for (const nlohmann::json& item : value) {
            if (item.is_string()) {
                items.push_back(item.get<std::string>());
            }
        }
        *out = PropertyValue::fromStringList(items);
        return true;
    }
    return false;
}

bool deserializeEventJson(const std::string& json, Event* out) {
    try {
        nlohmann::json root = nlohmann::json::parse(json);
        if (!root.is_object()) {
            return false;
        }
        out->name = root.value("event", std::string());
        out->timeMs = root.value("time", static_cast<int64_t>(0));
        out->traceId = root.value("trace_id", std::string());
        out->anonId = root.value("anon_id", std::string());
        out->loginId = root.value("login_id", std::string());
        if (root.contains("properties") && root["properties"].is_object()) {
            for (auto it = root["properties"].begin(); it != root["properties"].end(); ++it) {
                PropertyValue value;
                if (propertyFromJson(it.value(), &value)) {
                    out->properties[it.key()] = std::move(value);
                }
            }
        }
        // user_properties replay round-trip: operator → payload object /
        // boolean ($delete); null values from $unset must be restored,
        // otherwise the persisted operator payload is corrupted on
        // retransmission
        if (root.contains("user_properties") && root["user_properties"].is_object()) {
            for (auto it = root["user_properties"].begin(); it != root["user_properties"].end();
                 ++it) {
                UserOp op;
                op.op = it.key();
                if (it.value().is_boolean()) {
                    op.deleteFlag = it.value().get<bool>();  // "$delete": true
                    out->userOps.push_back(std::move(op));
                    continue;
                }
                if (!it.value().is_object()) {
                    continue;  // unexpected shape, skip defensively
                }
                for (auto kv = it.value().begin(); kv != it.value().end(); ++kv) {
                    if (kv.value().is_null()) {
                        op.payload[kv.key()] = PropertyValue();  // key: null
                        continue;
                    }
                    PropertyValue value;
                    if (propertyFromJson(kv.value(), &value)) {
                        op.payload[kv.key()] = std::move(value);
                    }
                }
                out->userOps.push_back(std::move(op));
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

std::string serializeEventsJson(const std::vector<Event>& events) {
    nlohmann::json array = nlohmann::json::array();
    for (const Event& event : events) {
        array.push_back(nlohmann::json::parse(serializeEventJson(event)));
    }
    return dumpReplace(array);
}

}  // namespace sensorswave
