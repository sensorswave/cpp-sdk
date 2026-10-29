#pragma once

#include <functional>
#include <string>
#include <vector>

#include "event/property_value.h"
#include "util/clock.h"
#include "util/random.h"

namespace sensorswave {

// A single user-property operation ($UserSet only): the top-level
// user_properties key is the operator and the value is the payload object;
// $delete is the exception, expressed as boolean true. v1 allows at most one
// operation per event.
struct UserOp {
    std::string
        op;  // "$set" / "$set_once" / "$increment" / "$append" / "$union" / "$unset" / "$delete"
    PropertyMap payload;      // payload pairs (null passed through for $unset; empty for $delete)
    bool deleteFlag = false;  // true means the "$delete": true form
};

// Internal event object (matches the upload protocol; serialized by event_json
// before enqueueing / upload)
struct Event {
    std::string name;
    int64_t timeMs = 0;
    std::string traceId;
    std::string anonId;
    std::string loginId;  // empty = not logged in, omitted at serialization
    PropertyMap properties;
    // User-property operation channel: serialized as top-level user_properties;
    // null payload values pass through ($unset semantics) and are not subject
    // to the null filtering applied to common / event properties
    std::vector<UserOp> userOps;
};

// Per-event network-type query callback: returning false or an empty string
// means the value is unavailable and neither key is injected. The callback must
// not throw (same contract as ISystemInfo); the builder still guards with
// try/catch
using NetworkTypeProvider = std::function<bool(std::string*)>;

// Event builder: fills required fields, merges the property layers, and injects
// $session_id last.
// Merge precedence: preset < common < event < $session_id.
// Dynamic common-property getters are evaluated once per event; a getter that
// throws skips only that property.
// Invalid pairs (empty keys / null values) are dropped.
class EventBuilder {
   public:
    // Does not own clock / random; their lifetimes must cover this object
    EventBuilder(const IClock* clock, IRandom* random);

    // Preset property set (built once at init; the host may override it via
    // common / event properties)
    void setPresetProperties(PropertyMap props);
    // Network-type provider: $network_type / $wifi change with network state, so
    // they are not baked into the preset snapshot; queried once per event build.
    // Both keys derive from the same source and are injected before common /
    // event properties (the host may override them via common / event
    // properties)
    void setNetworkTypeProvider(NetworkTypeProvider provider);
    // Snapshot of the common-property registry (dynamic getters included,
    // unevaluated)
    void setCommonProperties(PropertyMap props);
    void setSessionId(std::string sessionId);
    void setIdentity(std::string anonId, std::string loginId);

    const std::string& sessionId() const { return sessionId_; }
    const std::string& anonId() const { return anonId_; }
    const std::string& loginId() const { return loginId_; }

    // Builds an event. Getters in eventProps are evaluated the same way (a throw
    // skips the pair); null values / empty keys are dropped.
    Event build(const std::string& eventName, const PropertyMap& eventProps) const;

    // $UserSet construction: evaluates getters in userOp.payload key by key,
    // then writes the payload into userOps under the operator; null values are
    // kept ($unset) and only empty keys are dropped.
    Event build(const std::string& eventName, const PropertyMap& eventProps, UserOp userOp) const;

    // Snapshot of preset + resolved common properties (AB evalall user.props
    // channel, aligned with the JS
    // {...getPresetEventProps(), ...resolveDynamicProps(getCommonProps())}):
    // merge precedence matches the event-property layer (preset < common); a
    // dynamic getter that throws skips its key.
    // Must be called under the facade lock (builder state is not atomic)
    PropertyMap snapshotUserProperties() const;

   private:
    PropertyMap resolveCommonProperties() const;

    const IClock* clock_;
    IRandom* random_;
    PropertyMap presetProperties_;
    NetworkTypeProvider networkProvider_;
    PropertyMap commonProperties_;
    std::string sessionId_;
    std::string anonId_;
    std::string loginId_;
};

}  // namespace sensorswave
