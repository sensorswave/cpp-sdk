#include "event/event_builder.h"

#include <utility>

#include "util/logger.h"
#include "util/uuid.h"

namespace sensorswave {

EventBuilder::EventBuilder(const IClock* clock, IRandom* random) : clock_(clock), random_(random) {}

void EventBuilder::setPresetProperties(PropertyMap props) {
    presetProperties_ = std::move(props);
}

void EventBuilder::setNetworkTypeProvider(NetworkTypeProvider provider) {
    networkProvider_ = std::move(provider);
}

void EventBuilder::setCommonProperties(PropertyMap props) {
    commonProperties_ = std::move(props);
}

void EventBuilder::setSessionId(std::string sessionId) {
    sessionId_ = std::move(sessionId);
}

void EventBuilder::setIdentity(std::string anonId, std::string loginId) {
    anonId_ = std::move(anonId);
    loginId_ = std::move(loginId);
}

PropertyMap EventBuilder::resolveCommonProperties() const {
    PropertyMap resolved;
    for (const auto& entry : commonProperties_) {
        if (entry.first.empty()) {
            continue;  // drop empty keys
        }
        try {
            PropertyValue value = entry.second.resolve();
            if (!value.isNull()) {
                resolved[entry.first] = std::move(value);
            }
        } catch (const std::exception& e) {
            // getter threw: skip this property, the event still goes out
            Logger::warn("common property getter failed: " + entry.first + ": " + e.what());
        } catch (...) {
            Logger::warn("common property getter failed: " + entry.first);
        }
    }
    return resolved;
}

PropertyMap EventBuilder::snapshotUserProperties() const {
    PropertyMap snapshot;
    // Merge order matches the event-property layer: preset < network < common
    // (same-named keys: later wins); no event properties / $session_id
    // (the user.props channel mirrors the JS preset + common layers)
    for (const auto& entry : presetProperties_) {
        PropertyValue value = entry.second.resolve();
        if (!entry.first.empty() && !value.isNull()) {
            snapshot[entry.first] = std::move(value);
        }
    }
    if (networkProvider_) {
        try {
            std::string networkType;
            if (networkProvider_(&networkType) && !networkType.empty()) {
                snapshot["$network_type"] = PropertyValue::fromString(networkType);
                snapshot["$wifi"] = PropertyValue::fromBool(networkType == "wifi");
            }
        } catch (...) {
            Logger::warn("network type provider failed");
        }
    }
    for (auto& entry : resolveCommonProperties()) {
        snapshot[entry.first] = std::move(entry.second);
    }
    return snapshot;
}

Event EventBuilder::build(const std::string& eventName, const PropertyMap& eventProps) const {
    Event event;
    event.name = eventName;
    event.timeMs = clock_ != nullptr ? clock_->nowMs() : 0;
    event.anonId = anonId_;
    event.loginId = loginId_;

    Uuid uuid(random_);
    event.traceId = uuid.v4();

    // Merge order: preset < common < event (same-named keys: later wins)
    for (const auto& entry : presetProperties_) {
        PropertyValue value = entry.second.resolve();
        if (!entry.first.empty() && !value.isNull()) {
            event.properties[entry.first] = std::move(value);
        }
    }
    // $network_type / $wifi queried per event: network state changes over time
    // (switching / losing connectivity), so they are not baked into the init
    // preset snapshot. Both keys derive from the same source: if unavailable,
    // neither appears (no empty fallback). Injected before common / event
    // properties so the host can override them.
    if (networkProvider_) {
        try {
            std::string networkType;
            if (networkProvider_(&networkType) && !networkType.empty()) {
                event.properties["$network_type"] = PropertyValue::fromString(networkType);
                event.properties["$wifi"] = PropertyValue::fromBool(networkType == "wifi");
            }
        } catch (...) {
            Logger::warn("network type provider failed");
        }
    }
    for (auto& entry : resolveCommonProperties()) {
        event.properties[entry.first] = std::move(entry.second);
    }
    for (const auto& entry : eventProps) {
        if (entry.first.empty()) {
            continue;  // drop empty keys
        }
        try {
            PropertyValue value = entry.second.resolve();
            if (!value.isNull()) {
                event.properties[entry.first] = std::move(value);
            }
        } catch (const std::exception& e) {
            Logger::warn("event property evaluation failed: " + entry.first + ": " + e.what());
        } catch (...) {
            Logger::warn("event property evaluation failed: " + entry.first);
        }
    }

    // $session_id injected last: the SDK value is authoritative and overrides a
    // same-named caller key
    if (!sessionId_.empty()) {
        event.properties["$session_id"] = PropertyValue::fromString(sessionId_);
    }

    return event;
}

Event EventBuilder::build(const std::string& eventName, const PropertyMap& eventProps,
                          UserOp userOp) const {
    Event event = build(eventName, eventProps);
    PropertyMap payload;
    for (auto& entry : userOp.payload) {
        if (entry.first.empty()) {
            continue;  // drop empty keys
        }
        try {
            PropertyValue value = entry.second.resolve();
            payload[entry.first] = std::move(value);  // null kept ($unset)
        } catch (const std::exception& e) {
            Logger::warn("user property evaluation failed: " + entry.first + ": " + e.what());
        } catch (...) {
            Logger::warn("user property evaluation failed: " + entry.first);
        }
    }
    userOp.payload = std::move(payload);
    event.userOps.push_back(std::move(userOp));
    return event;
}

}  // namespace sensorswave
