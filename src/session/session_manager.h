#pragma once

#include <cstdint>
#include <string>

#include "storage/persistent_store.h"
#include "util/clock.h"
#include "util/uuid.h"

namespace sensorswave {

// Session state machine: lazy UUIDv7 creation and rotation (30min idle
// threshold / 12h lifetime cap), state persisted in sw_session
// ({session_id, started_at, last_active_at}).
//
// - touch(): event touchpoint — rotation check + last_active update (persist
//   throttled: the file is written only when a minute boundary is crossed,
//   avoiding one IO per event; rotation always writes immediately).
// - currentSessionId(): query touchpoint — same state machine but does not
//   update last_active; rotates and writes back if already expired.
// - forceRotate(): the session-rotation entry point for reset.
//
// A corrupt / missing sw_session is always treated as stateless and regenerated
// at the next touchpoint; persistence failure degrades to pure in-memory state
// and the session stays continuous within the process; a clock rollback that
// yields a negative delta within the threshold keeps the session — no hang, no
// crash.
// No $session_start / $session_end events — this class emits no events.
//
// Threading: called only by the facade under the facade lock; no locking here.
// store_ / uuid_ / clock_ are not owned; the facade guarantees their lifetimes.
class SessionManager {
   public:
    SessionManager(IPersistentStore* store, Uuid* uuid, const IClock* clock);

    // Loads sw_session at init; missing / corrupt → stateless (lazy: no
    // rotation threshold is checked here — a cold-start continuation is left to
    // the first touchpoint comparing now - last_active)
    void load();

    // Event touchpoint: returns the current session ID (rotating first if
    // needed) and updates last_active (throttled persistence)
    std::string touch();

    // Query touchpoint: returns the current session ID (rotates and writes back
    // first if expired); does not update last_active
    std::string currentSessionId();

    // Rotates and persists immediately (called on reset)
    void forceRotate();

   private:
    // Rotation check: idle beyond the threshold or lifetime beyond the cap; a
    // negative rollback delta counts as not expired
    bool expired(int64_t nowMs) const;

    // Generates a new UUIDv7 session and persists it immediately
    void rotate(int64_t nowMs);

    // Writes sw_session; throttling decisions belong to the caller — this
    // method only does the atomic write and records the minute mark. Failures
    // are silent (in-memory state continues)
    void persist(int64_t nowMs);

    IPersistentStore* store_;
    Uuid* uuid_;
    const IClock* clock_;
    bool hasState_ = false;
    std::string sessionId_;
    int64_t startedAtMs_ = 0;
    int64_t lastActiveMs_ = 0;
    int64_t persistedMinute_ = -1;  // minute mark already persisted for
                                    // last_active (throttle baseline)
};

}  // namespace sensorswave
