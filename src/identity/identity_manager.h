#pragma once

#include <functional>
#include <string>

#include "storage/persistent_store.h"
#include "util/uuid.h"

namespace sensorswave {

// User-identity state machine: lazy anon_id generation + persistence;
// identify persists the login and emits the $Identify binding event through the
// injected emitter; setLoginId only persists, zero events; reset clears login
// and keeps anon, reset(true) additionally rotates anon, and both trigger the
// session-rotation callback.
// Empty-string identify / setLoginId is a no-op plus a warning; persistence
// failure degrades to in-memory state.
// store_ / uuid_ are not owned; the facade guarantees their lifetimes.
class IdentityManager {
   public:
    // $Identify event emitter: the facade fills required fields via EventBuilder
    // and enqueues (with preset + common properties)
    using IdentifyEmitter =
        std::function<void(const std::string& anonId, const std::string& loginId)>;
    // Session-rotation notification for reset (getSessionId() returns the new
    // session immediately after reset)
    using SessionRotator = std::function<void()>;

    IdentityManager(IPersistentStore* store, Uuid* uuid, IdentifyEmitter emitter,
                    SessionRotator sessionRotator);

    // Loads sw_anon_id / sw_login_id at init; a missing anon is generated and
    // persisted on the spot
    void load();

    const std::string& anonId() const { return anonId_; }
    const std::string& loginId() const { return loginId_; }

    void identify(const std::string& loginId);    // persist + $Identify
    void setLoginId(const std::string& loginId);  // persist only, zero events
    void reset(bool resetAnonId);                 // clears login; rotates anon too when true

   private:
    void persistAnon();
    void persistLogin();

    IPersistentStore* store_;
    Uuid* uuid_;
    IdentifyEmitter emitter_;
    SessionRotator sessionRotator_;
    std::string anonId_;
    std::string loginId_;
};

}  // namespace sensorswave
