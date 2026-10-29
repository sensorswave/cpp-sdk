#include "session/session_manager.h"

#include <nlohmann/json.hpp>

#include "constants.h"
#include "util/logger.h"

namespace sensorswave {
namespace {

// sw_session layout: {session_id, started_at, last_active_at}
bool parseSessionState(const std::string& raw, std::string* sessionId, int64_t* startedAt,
                       int64_t* lastActiveAt) {
    try {
        const nlohmann::json state = nlohmann::json::parse(raw);
        if (!state.is_object()) {
            return false;
        }
        if (!state.contains("session_id") || !state["session_id"].is_string() ||
            !state.contains("started_at") || !state["started_at"].is_number() ||
            !state.contains("last_active_at") || !state["last_active_at"].is_number()) {
            return false;  // missing fields / wrong types → stateless
        }
        *sessionId = state["session_id"].get<std::string>();
        *startedAt = state["started_at"].get<int64_t>();
        *lastActiveAt = state["last_active_at"].get<int64_t>();
        return !sessionId->empty();
    } catch (const std::exception& e) {
        Logger::warn("sw_session parse failed, treat as stateless");
        Logger::debug(std::string("sw_session parse detail: ") + e.what());
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace

SessionManager::SessionManager(IPersistentStore* store, Uuid* uuid, const IClock* clock)
    : store_(store), uuid_(uuid), clock_(clock) {}

void SessionManager::load() {
    hasState_ = false;
    if (store_ == nullptr) {
        return;  // no storage: pure in-memory mode
    }
    std::string raw;
    if (!store_->read(SW_KEY_SESSION, &raw)) {
        return;  // key absent: cold start, stateless
    }
    std::string id;
    int64_t startedAt = 0;
    int64_t lastActiveAt = 0;
    if (!parseSessionState(raw, &id, &startedAt, &lastActiveAt)) {
        return;  // corrupt → stateless, regenerated at the next touchpoint
    }
    hasState_ = true;
    sessionId_ = id;
    startedAtMs_ = startedAt;
    lastActiveMs_ = lastActiveAt;
    persistedMinute_ = lastActiveAt / 60000;  // record the mark on load: the
                                              // first touch within the same
                                              // minute does not rewrite
}

std::string SessionManager::touch() {
    const int64_t now = clock_ != nullptr ? clock_->nowMs() : 0;
    if (!hasState_ || expired(now)) {
        rotate(now);
        return sessionId_;
    }
    lastActiveMs_ = now;
    const int64_t minute = now / 60000;
    if (minute != persistedMinute_) {
        persist(now);  // persist throttling: write only when crossing a minute mark
    }
    return sessionId_;
}

std::string SessionManager::currentSessionId() {
    const int64_t now = clock_ != nullptr ? clock_->nowMs() : 0;
    if (!hasState_ || expired(now)) {
        rotate(now);  // a query-triggered rotation writes back too
    }
    return sessionId_;
}

void SessionManager::forceRotate() {
    rotate(clock_ != nullptr ? clock_->nowMs() : 0);
}

bool SessionManager::expired(int64_t nowMs) const {
    // Clock rollback: the delta is applied to the threshold as an absolute
    // value — a rollback larger than the threshold still rotates instead of
    // being misjudged as "not expired" by the negative delta; the
    // direction-branched subtraction avoids unary-minus undefined behavior on
    // two's-complement extremes
    const int64_t idle = nowMs > lastActiveMs_ ? nowMs - lastActiveMs_ : lastActiveMs_ - nowMs;
    if (idle > SESSION_IDLE_MS) {
        return true;  // idle over 30min
    }
    const int64_t lifetime = nowMs > startedAtMs_ ? nowMs - startedAtMs_ : startedAtMs_ - nowMs;
    return lifetime >= SESSION_MAX_MS;  // lifetime ≥ 12h forces rotation
}

void SessionManager::rotate(int64_t nowMs) {
    sessionId_ = uuid_ != nullptr ? uuid_->v7(nowMs) : std::string();
    startedAtMs_ = nowMs;
    lastActiveMs_ = nowMs;
    hasState_ = true;
    persist(nowMs);  // rotation always writes immediately
}

void SessionManager::persist(int64_t nowMs) {
    persistedMinute_ = nowMs / 60000;  // record the mark first: a failed write
                                       // does not retry repeatedly within the
                                       // same minute
    if (store_ == nullptr) {
        return;  // pure in-memory mode: state continuity is carried by members
    }
    nlohmann::json state;
    state["session_id"] = sessionId_;
    state["started_at"] = startedAtMs_;
    state["last_active_at"] = lastActiveMs_;
    if (!store_->write(SW_KEY_SESSION, state.dump())) {
        // persistence failure degrades to in-memory state; no error surfaces to
        // the caller
        Logger::warn("persist sw_session failed, keep in-memory session");
    }
}

}  // namespace sensorswave
