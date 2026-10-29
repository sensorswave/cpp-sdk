#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ab/ab_request.h"
#include "constants.h"
#include "storage/persistent_store.h"
#include "util/clock.h"

namespace sensorswave {

// In-flight fetch session (internal): shared via shared_ptr between the
// detached fetch thread and AbManager; its lifetime may outlive AbManager
// itself (in-flight requests finish naturally after destroy). Defined in
// ab_manager.cpp
struct AbFetchSession;

// AB kind (evalall typ value); explicit smallest underlying type (performance-enum-size)
enum class AbKind : std::uint8_t { kGate = 1, kConfig = 2, kExperiment = 3 };

// Query answer: kGate -> gateValue; kConfig / kExperiment -> text (JSON text)
struct AbAnswer {
    bool gateValue = false;
    std::string text;
};

// Impression plan: emit=false means no impression event (missing key /
// disable_impress on the data side); unset=true means an unset impression
// (key present without vid); id is the data entry's numeric ID (the basis for
// building the impression user_properties keys $feature_<id> / $exp_<id>,
// matching the JS getABPropKey)
struct ImpressPlan {
    bool emit = false;
    AbKind kind = AbKind::kGate;
    std::string key;
    int64_t id = 0;
    std::string vid;
    bool unset = false;
};

// Refresh interval normalization (matching the JS Math.max(interval, 30000)):
// values <= 0 or < 30s are clamped to the 30s floor (under JS semantics,
// negative / zero values are also pulled up to 30000 by max, rather than
// falling back to the 10-minute default). When unconfigured, the facade
// supplies AB_DEFAULT_REFRESH_MS first and normalizes here
inline int64_t normalizeAbRefreshInterval(int64_t configuredMs) {
    return configuredMs < AB_MIN_REFRESH_MS ? AB_MIN_REFRESH_MS : configuredMs;
}

// AB data manager: fastFetch cache evaluation + evalall fetch + impression
// plans. An internal mutex protects the cache and session state; networking
// runs on a detached thread and never occupies the calling thread. Constructed
// / destroyed / adoptCache / load only under the facade lock; query is invoked
// by the facade under the lock (callbacks and impression emission happen after
// the facade unlocks)
class AbManager {
   public:
    // Answer delivery (exactly once): invoked synchronously on the ready path;
    // on the fetch path the thread invokes it on success or failure fallback.
    // Implementations must tolerate duplicate invocations themselves (idempotent)
    using DeliverAnswer = std::function<void(const AbAnswer& answer)>;
    // user.props supplier (lazy): resolved only when an evalall is actually
    // started (when startFetchLocked builds the request, synchronously on the
    // caller's thread) — cache hits / in-flight attaches / window-suppressed
    // paths pay zero getter overhead. Implementations must return quickly and
    // must not re-enter locks the caller already holds (facade lock / mutex_)
    using UserPropsSupplier = std::function<nlohmann::json()>;
    // Fetch-complete callback (fetch thread): the facade runs its generation
    // check, then persists sw_ab_data + adoptCache
    using FetchComplete =
        std::function<void(const std::vector<AbDataItem>& results, const std::string& anonId,
                           const std::string& loginId)>;
    // Impress outlet (fetch thread): the facade runs its generation check, then
    // constructs and enqueues the events
    using ImpressEmitter = std::function<void(const ImpressPlan& plan)>;

    // client is held independently (shared_ptr copy): the fetch thread's
    // lifetime is independent of the facade; clock is not owned and used only
    // synchronously on query threads; refreshIntervalMs is the effective
    // refresh interval after the init clamp. Completion / impress callbacks are
    // injected via set* (must happen before the first query)
    AbManager(std::shared_ptr<IHttpClient> client, const IClock* clock, int64_t refreshIntervalMs,
              std::string apiHost, std::string sourceToken);
    ~AbManager();

    AbManager(const AbManager&) = delete;
    AbManager& operator=(const AbManager&) = delete;

    // Callback injection: not thread-safe, must be called before the first query
    // (the fetch thread copies the callbacks at fetch start; later injections
    // have no effect) — successful completion -> persist + adoptCache;
    // impressions -> the facade's event outlet
    void setFetchComplete(FetchComplete onComplete) { onComplete_ = std::move(onComplete); }
    void setImpressEmitter(ImpressEmitter emitImpress) { emitImpress_ = std::move(emitImpress); }

    // Timer refresh identity supplier (called by the timer thread on each tick):
    // returning false (opt-out / destroyed and other disabled states) or leaving
    // anonId empty -> the round is silently skipped, no request; userProps is
    // snapshotted along with the identity (the evalall user.props segmentation
    // channel). Implementations must return quickly and must not block for long
    // (the facade guards access with a bounded lock attempt). Unlike
    // setFetchComplete: protected by mutex_, safe to inject at any time;
    // passing an empty function stops the refresh flow
    using RefreshIdentitySupplier =
        std::function<bool(std::string* anonId, std::string* loginId, nlohmann::json* userProps)>;
    void setIdentitySupplier(RefreshIdentitySupplier supplier) {
        std::lock_guard<std::mutex> lock(mutex_);
        identitySupplier_ = std::move(supplier);
    }

    // Restores sw_ab_data at init; missing / corrupt -> no-cache state.
    // store is used only during the call, not retained
    void load(IPersistentStore* store);

    // Main fastFetch query entry. deliver is invoked exactly once:
    // synchronously on the valid-cache / fallback paths; on the fetch path by
    // the thread (success with fresh data, failure with the start-of-fetch
    // cache snapshot or defaults). The synchronous path writes impression plans
    // into impressOut (the caller constructs events via the facade outlet after
    // unlocking); on the fetch path impressions are emitted on the fetch thread
    // via emitImpress_ (the failure fallback emits no impressions).
    // anonId / loginId are the current identity (cache ownership check);
    // the userProps supplier lazily resolves preset + public properties (the
    // evalall user.props segmentation channel, matching the JS fetchABData —
    // see the UserPropsSupplier comment)
    void query(AbKind kind, const std::string& key, const std::string& anonId,
               const std::string& loginId, const UserPropsSupplier& userProps,
               const DeliverAnswer& deliver, std::vector<ImpressPlan>* impressOut);

    // Fetch-completion write-back (called after the facade's generation check;
    // fetchedAtMs comes from the facade clock). Empty results do not adopt (matching the JS
    // saveABData guard that keeps empty arrays out of the cache): the old cache
    // is not overwritten (including identity ownership; old data serves until
    // TTL); only "produced no cache" is recorded, so queries are not suppressed
    // by the 30s window gate
    void adoptCache(std::vector<AbDataItem> results, int64_t fetchedAtMs, std::string anonId,
                    std::string loginId);

    // init prefetch (matching the cache-first semantics of the JS init's
    // fastFetch): returns without networking on a valid cache hit; otherwise
    // starts one waiter-less evalall under the same rules as refreshOnly. Must
    // be called under the facade lock (init / optIn rebuild paths); networking
    // runs on a detached thread and this call returns immediately
    void prefetch(const std::string& anonId, const std::string& loginId,
                  const nlohmann::json& userProps);

    // Refresh only: starts one evalall with no waiters (success persists via
    // onComplete + adoptCache, failure has zero side effects). In-flight dedup
    // and the 30s minimum refresh gate follow the same rules as query; produces
    // no impressions (matching the JS fetchNewData, fetch-only). Called directly
    // by the timer thread and tests; networking runs on a detached thread and
    // this call returns immediately
    void refreshOnly(const std::string& anonId, const std::string& loginId,
                     const nlohmann::json& userProps);

    // ── Pure functions (unit-testable) ──

    // Answer rules: gate hits when vid is present and vid != "fail"; config /
    // experiment use the JSON text of value; missing key / invalid value ->
    // "{}" (a config string value is first tried as JSON, falling back to the
    // raw text)
    static AbAnswer computeAnswer(AbKind kind, const std::string& key,
                                  const std::vector<AbDataItem>& results);

    // Impression rules: missing key / disable_impress -> no impression;
    // hit -> regular impression (vid included); key present without vid ->
    // unset impression
    static ImpressPlan computeImpress(AbKind kind, const std::string& key,
                                      const std::vector<AbDataItem>& results);

   private:
    // Cache hit condition: cache present and results non-empty and within the
    // refresh interval and anon matches and (cached login empty = wildcard, or
    // matches the current login)
    bool cacheValidLocked(int64_t nowMs, const std::string& anonId,
                          const std::string& loginId) const;
    // Identity change detection: the adopted cache identity, or the identity
    // snapshotted at the last fetch start, differs from the query identity
    // (different anon, or that identity's login non-empty and different from
    // the current one). The cache alone misses a reset inside the "fetch
    // complete -> adoptCache" window (see the implementation comment)
    bool identityChangedLocked(const std::string& anonId, const std::string& loginId) const;
    // Starts an evalall: builds a session attaching the current caller as a
    // waiter, executed on a detached thread. Thread creation failure ->
    // complete synchronously from the start-of-fetch snapshot (answers only,
    // no impressions — fallback answers are not authoritative assignments)
    void startFetchLocked(AbKind kind, const std::string& key, const DeliverAnswer& deliver,
                          const std::string& anonId, const std::string& loginId,
                          const UserPropsSupplier& userProps, int64_t nowMs);

    // ── Timer refresh (matching the JS init's setInterval(fetchNewData, abRefreshInterval)) ──
    void runTimer();
    void tickRefresh();

    std::mutex mutex_;
    std::shared_ptr<IHttpClient> client_;
    const IClock* clock_;
    int64_t refreshIntervalMs_;
    std::string apiHost_;
    std::string sourceToken_;
    FetchComplete onComplete_;
    ImpressEmitter emitImpress_;
    RefreshIdentitySupplier
        identitySupplier_;  // mutex_-protected (copied concurrently by the timer thread)

    // Timer refresh thread: started at construction, stopped and joined at
    // destruction. Holds no business lock while waiting; tick logic runs outside
    // timerMutex_ and never waits on the network / facade lock
    std::thread timer_;
    std::mutex timerMutex_;
    std::condition_variable timerCv_;
    bool timerStop_ = false;  // guarded by timerMutex_

    bool hasCache_ = false;  // Cache present (only non-empty results adopt; empty = not produced)
    std::vector<AbDataItem> results_;
    int64_t fetchedAtMs_ = 0;
    std::string anonId_;
    std::string loginId_;
    int64_t lastFetchMs_ = 0;              // Last fetch start (baseline for the 30s gate)
    bool lastFetchProducedCache_ = false;  // Whether the last fetch produced a usable
                                           // cache (non-empty results, set by
                                           // adoptCache); when false, queries are not
                                           // forced to defaults by the 30s gate
    bool hasFetchIdentity_ = false;        // Any fetch started (incl. in-flight / unadopted)
    std::string fetchAnonId_;              // Identity snapshot at fetch start (identityChanged)
    std::string fetchLoginId_;
    std::shared_ptr<AbFetchSession> inflight_;
};

}  // namespace sensorswave
