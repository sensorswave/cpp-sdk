#include "ab/ab_manager.h"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include "constants.h"
#include "util/logger.h"

namespace sensorswave {

// In-flight fetch session: waiters + request + cache snapshot taken at fetch
// start. The snapshot serves as the failure fallback (complete with cached
// values or defaults — the cache is not rewritten by other paths during the
// fetch, so the snapshot is current as of the start)
struct AbFetchSession {
    struct Waiter {
        AbKind kind = AbKind::kGate;
        std::string key;
        AbManager::DeliverAnswer deliver;
    };

    std::mutex mutex;
    std::vector<std::unique_ptr<Waiter>> waiters;
    std::shared_ptr<IHttpClient> client;
    HttpRequest request;
    std::vector<AbDataItem> fallback;  // Cache snapshot at fetch start (empty -> defaults)
    std::string anonId;
    std::string loginId;
    AbManager::FetchComplete onComplete;
    // Impress emitter: set when a query-initiated session is created; refreshOnly
    // sessions start empty and are upgraded under mutex when a query attaches
    // (see the attach path in query). resolveWaiters reads it after takeWaiters
    // under the same lock, so the write happens-before the read via session->mutex
    AbManager::ImpressEmitter emit;
    bool completed =
        false;  // Completion flag: later queries stop attaching (query in-flight check)
};

namespace {

// Marks the session completed and takes all waiters (subsequent queries start a fresh round)
std::vector<std::unique_ptr<AbFetchSession::Waiter>> takeWaiters(AbFetchSession* session) {
    std::vector<std::unique_ptr<AbFetchSession::Waiter>> taken;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->completed = true;
        taken.swap(session->waiters);
    }
    return taken;
}

// Completes all waiters with the given result set: answers are delivered
// exactly once via deliver; allowImpress controls whether impressions are
// emitted (false on the failure fallback path — fallback answers are not
// authoritative server assignments, and emitting impressions would record
// non-actual assignments as impressions). One waiter's failed delivery does
// not affect the others
void resolveWaiters(AbFetchSession* session, const std::vector<AbDataItem>& results,
                    bool allowImpress) {
    for (std::unique_ptr<AbFetchSession::Waiter>& waiter : takeWaiters(session)) {
        try {
            if (waiter->deliver) {
                waiter->deliver(AbManager::computeAnswer(waiter->kind, waiter->key, results));
            }
            if (!allowImpress) {
                continue;
            }
            const ImpressPlan plan = AbManager::computeImpress(waiter->kind, waiter->key, results);
            if (plan.emit && session->emit) {
                session->emit(
                    plan);  // Facade outlet: validates the plan, constructs and enqueues the event
            }
        } catch (...) {
            Logger::warn("ab waiter resolve failed");
        }
    }
}

// Fetch thread body: holds only a session copy (shared_ptr), never touches
// AbManager — in-flight requests finish naturally after destroy, and their
// callbacks are dropped when the facade's generation check fails
void runFetch(const std::shared_ptr<AbFetchSession>& session) {
    if (Logger::debugEnabled()) {
        Logger::debug("ab evalall post body: " + session->request.body);
    }
    HttpResponse response;
    try {
        if (!session->client) {
            throw std::runtime_error(
                "no http client");  // Missing injection: degrade as a transport failure
        }
        response = session->client->execute(session->request);
    } catch (...) {
        response = HttpResponse();  // Implementations must not throw; safety net (statusCode=0 ->
                                    // retryable failure)
    }
    if (Logger::debugEnabled()) {
        Logger::debug("ab evalall response (http " + std::to_string(response.statusCode) +
                      "): " + response.body);
    }
    std::vector<AbDataItem> results;
    if (response.ok() && parseAbResults(response.body, &results)) {
        // onComplete (facade: persist + adoptCache) runs before answer delivery:
        // once the session is completed, whether the cache was adopted is already
        // decided, so queries arriving after delivery can hit the cache right
        // away — no duplicate evalall inside a "completed but not adopted" window.
        // A persist failure only logs a warning; answers are delivered as usual
        if (session->onComplete) {
            try {
                session->onComplete(results, session->anonId, session->loginId);
            } catch (...) {
                Logger::warn("ab fetch complete callback failed");  // Persist failure does not
                                                                    // affect answers
            }
        }
        resolveWaiters(session.get(), results,
                       true);  // Success: complete with fresh data (empty results -> defaults)
    } else {
        // Failure (network / non-2xx / unparseable response): complete answers
        // from the start-of-fetch snapshot or defaults, emitting no impressions —
        // fallback answers are not authoritative server assignments
        resolveWaiters(session.get(), session->fallback, false);
    }
}

}  // namespace

AbManager::AbManager(std::shared_ptr<IHttpClient> client, const IClock* clock,
                     int64_t refreshIntervalMs, std::string apiHost, std::string sourceToken)
    : client_(std::move(client)),
      clock_(clock),
      refreshIntervalMs_(refreshIntervalMs),
      apiHost_(std::move(apiHost)),
      sourceToken_(std::move(sourceToken)) {
    // The timer thread starts with construction (matching the JS init's
    // setInterval): it waits a full refresh period before the first tick, so
    // injecting callbacks / the identity supplier afterwards opens no race
    // window. If creation fails, only periodic refresh is lost; the query
    // path's lazy fetch covers it, so semantics stay intact
    try {
        std::thread timer(&AbManager::runTimer, this);
        timer_ = std::move(timer);
    } catch (...) {
        Logger::warn("ab timer thread creation failed");
    }
}

// The detached fetch thread holds only a session copy and needs no waiting;
// the timer thread is stopped and joined at destruction (the supplier /
// refreshOnly inside a tick are fast-return paths, so joining is bounded)
AbManager::~AbManager() {
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        timerStop_ = true;
    }
    timerCv_.notify_all();
    if (timer_.joinable()) {
        timer_.join();
    }
}

// Timer refresh thread body: wakes periodically per refreshIntervalMs (matching
// the JS setInterval). Holds no business lock while waiting; tick logic runs
// outside timerMutex_ and never waits on the network
void AbManager::runTimer() {
    std::unique_lock<std::mutex> lock(timerMutex_);
    while (!timerStop_) {
        if (timerCv_.wait_for(lock, std::chrono::milliseconds(refreshIntervalMs_),
                              [this] { return timerStop_; })) {
            return;  // Stop flag set: exit
        }
        lock.unlock();
        tickRefresh();
        lock.lock();
    }
}

// One timer tick: fetch the current identity, then refreshOnly. The supplier
// is copied under mutex_ and invoked outside the lock (the supplier may take
// the facade lock internally; avoid nested lock ordering). An absent /
// rejecting / throwing supplier -> the round is silently skipped (silent
// failure convention)
void AbManager::tickRefresh() {
    RefreshIdentitySupplier supplier;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        supplier = identitySupplier_;
    }
    if (!supplier) {
        return;
    }
    std::string anonId;
    std::string loginId;
    nlohmann::json userProps = nlohmann::json::object();
    try {
        if (!supplier(&anonId, &loginId, &userProps) || anonId.empty()) {
            return;  // Opt-out / destroyed / no identity: no request in disabled states
                     // (compliance)
        }
    } catch (...) {
        Logger::warn("ab timer identity supplier failed");
        return;
    }
    refreshOnly(anonId, loginId, userProps);
}

// init prefetch: returns immediately on a valid cache hit (matching the JS
// fastFetch cache-first behavior, no network); on a miss goes through the same
// start gate as refreshOnly (in-flight dedup + 30s minimum refresh)
void AbManager::prefetch(const std::string& anonId, const std::string& loginId,
                         const nlohmann::json& userProps) {
    try {
        const int64_t nowMs = clock_ != nullptr ? clock_->nowMs() : 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (cacheValidLocked(nowMs, anonId, loginId)) {
            return;  // Cache first: no network
        }
    } catch (...) {
        Logger::warn("ab prefetch failed");
        return;
    }
    refreshOnly(anonId, loginId, userProps);
}

// Refresh only: an evalall with no waiters. On success -> onComplete persist +
// adoptCache (same path as query-driven fetches); on failure -> zero waiters,
// no side effects. In-flight dedup and the 30s minimum refresh gate follow the
// same rules as query, keeping the timer and query paths from double-firing
void AbManager::refreshOnly(const std::string& anonId, const std::string& loginId,
                            const nlohmann::json& userProps) {
    try {
        const int64_t nowMs = clock_ != nullptr ? clock_->nowMs() : 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (inflight_) {
            std::unique_lock<std::mutex> sessionLock(inflight_->mutex);
            if (!inflight_->completed) {
                return;  // In flight: share the same evalall (matches the JS fetchingPromise
                         // request lock)
            }
        }
        if (nowMs - lastFetchMs_ < AB_MIN_REFRESH_MS) {
            return;  // 30s minimum refresh gate: skip if a query / the timer just fetched
        }
        startFetchLocked(
            AbKind::kGate, "", nullptr, anonId, loginId, [&userProps] { return userProps; }, nowMs);
    } catch (...) {
        Logger::warn("ab timer refresh failed");
    }
}

void AbManager::load(IPersistentStore* store) {
    if (store == nullptr) {
        return;
    }
    std::string raw;
    if (!store->read(SW_KEY_AB_DATA, &raw)) {
        return;  // No cache: cold start (the first query triggers a fetch)
    }
    std::vector<AbDataItem> results;
    int64_t fetchedAtMs = 0;
    std::string anonId;
    std::string loginId;
    if (!parseAbCache(raw, &results, &fetchedAtMs, &anonId, &loginId)) {
        Logger::warn("sw_ab_data corrupt, cache discarded");  // Corruption means the no-cache state
        return;
    }
    adoptCache(std::move(results), fetchedAtMs, std::move(anonId), std::move(loginId));
}

bool AbManager::cacheValidLocked(int64_t nowMs, const std::string& anonId,
                                 const std::string& loginId) const {
    return hasCache_ && !results_.empty() && nowMs - fetchedAtMs_ < refreshIntervalMs_ &&
           anonId_ == anonId && (loginId_.empty() || loginId_ == loginId);
}

// Fetch start and cache adoption are two distinct moments: on the success path
// adoptCache lands via the facade callback before answers are delivered (see
// runFetch), but refreshOnly has no answers to deliver, the failure path never
// adopts, and hasCache_ stays false while a fetch is in flight. If an identity
// change (reset/login) lands inside that window, judging by cache identity
// alone would miss it -> the query would be throttled by the 30s window gate
// instead of refetching immediately. So the identity is snapshotted
// synchronously at fetch start and compared alongside the cache identity; the
// login wildcard semantics match the cache (empty = no constraint)
bool AbManager::identityChangedLocked(const std::string& anonId, const std::string& loginId) const {
    if (hasFetchIdentity_ &&
        (fetchAnonId_ != anonId || (!fetchLoginId_.empty() && fetchLoginId_ != loginId))) {
        return true;
    }
    return hasCache_ && (anonId_ != anonId || (!loginId_.empty() && loginId_ != loginId));
}

void AbManager::query(AbKind kind, const std::string& key, const std::string& anonId,
                      const std::string& loginId, const UserPropsSupplier& userProps,
                      const DeliverAnswer& deliver, std::vector<ImpressPlan>* impressOut) {
    std::vector<ImpressPlan> fallbackPlans;
    if (impressOut == nullptr) {
        impressOut = &fallbackPlans;
    }
    try {
        const int64_t nowMs = clock_ != nullptr ? clock_->nowMs() : 0;
        std::lock_guard<std::mutex> lock(mutex_);

        // Valid cache hit: synchronous answer + impression plan (emitted after the facade unlocks)
        if (cacheValidLocked(nowMs, anonId, loginId)) {
            deliver(computeAnswer(kind, key, results_));
            impressOut->push_back(computeImpress(kind, key, results_));
            return;
        }

        // In-flight dedup: attach a waiter to the unfinished session, sharing one evalall
        if (inflight_) {
            std::unique_lock<std::mutex> sessionLock(inflight_->mutex);
            if (!inflight_->completed) {
                // Attaching upgrades the session's impress emitter: sessions started
                // by refreshOnly (init prefetch / timer refresh) have a null emit,
                // and without the upgrade the attaching query's $FeatureImpress /
                // $ExpImpress would be silently lost. The write goes through
                // session->mutex; the fetch thread reads after takeWaiters under
                // the same lock, so the ordering holds. Never changed afterwards
                // (guarded by completed)
                inflight_->emit = emitImpress_;
                std::unique_ptr<AbFetchSession::Waiter> waiter(new AbFetchSession::Waiter());
                waiter->kind = kind;
                waiter->key = key;
                waiter->deliver = deliver;
                inflight_->waiters.push_back(std::move(waiter));
                return;  // deliver is invoked exactly once by the fetch thread
            }
        }

        // The 30s minimum refresh only constrains queries with a stable identity;
        // an identity change refetches immediately, otherwise a cache belonging
        // to the old identity would be read across identities (invalid cache
        // ownership means invalid). The window gate counts from the last fetch
        // start (lastFetchMs_ is set in startFetchLocked). On the success path
        // adoptCache lands before answers are delivered (see runFetch): once the
        // session is completed, adoption is decided and in-flight queries always
        // attach instead of refetching; past the window, an unusable cache
        // (missing / empty results / TTL expired) refetches.
        // Exception: when the last fetch produced no cache (init prefetch failed,
        // unparseable response, empty results — e.g. init without login_id
        // fetching an empty array), the window gate does not suppress the
        // refetch — matching the JS fastFetch "fetch whenever there is no valid
        // cache" rule, so the first query must not get defaults within 30s of
        // init
        if (identityChangedLocked(anonId, loginId)) {
            startFetchLocked(kind, key, deliver, anonId, loginId, userProps, nowMs);
            return;
        }
        const bool pastMinRefresh = nowMs - lastFetchMs_ >= AB_MIN_REFRESH_MS;
        // Identity unchanged: cache usability matches the check at the top
        const bool cacheUsable = cacheValidLocked(nowMs, anonId, loginId);
        if (!cacheUsable && (pastMinRefresh || !lastFetchProducedCache_)) {
            startFetchLocked(kind, key, deliver, anonId, loginId, userProps, nowMs);
            return;
        }

        // Cache invalid within the window but the last fetch produced a cache
        // (reachable only by constructing with interval < 30s; the effective
        // interval is clamped to >= 30s): complete synchronously with the
        // current results (stale values as fallback; empty results -> defaults),
        // no request
        deliver(computeAnswer(kind, key, results_));
        if (!results_.empty()) {
            impressOut->push_back(computeImpress(kind, key, results_));
        }
    } catch (...) {
        // Exactly-once guarantee: any internal exception synchronously degrades to
        // defaults; when deliver has already run this is an idempotent no-op
        try {
            deliver(computeAnswer(kind, key, std::vector<AbDataItem>()));
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Idempotent no-op: deliver already ran (promise_already_satisfied); exactly-once holds
        }
    }
}

void AbManager::adoptCache(std::vector<AbDataItem> results, int64_t fetchedAtMs, std::string anonId,
                           std::string loginId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (results.empty()) {
        // Empty results do not form a cache (matching the JS saveABData guard
        // that keeps empty arrays out of the cache): the existing cache is not
        // overwritten (including identity ownership — when a timer refresh
        // fetches an empty array, the old data keeps serving until TTL, same as
        // JS); only "the last fetch produced no cache" is recorded, so the 30s
        // window gate does not suppress queries and the next query re-runs
        // evalall immediately with the latest identity. Scenario: init prefetch
        // (without login_id) fetches an empty array and setLoginId is then
        // called — subsequent AB queries must refetch synchronously rather than
        // return defaults within the window (matching the JS fastFetch "fetch
        // whenever there is no valid cache" rule)
        lastFetchProducedCache_ = false;
        return;
    }
    results_ = std::move(results);
    fetchedAtMs_ = fetchedAtMs;
    anonId_ = std::move(anonId);
    loginId_ = std::move(loginId);
    hasCache_ = true;
    lastFetchProducedCache_ =
        true;  // Releases the query-side "failure suppression": see the window logic in query
}

void AbManager::startFetchLocked(AbKind kind, const std::string& key, const DeliverAnswer& deliver,
                                 const std::string& anonId, const std::string& loginId,
                                 const UserPropsSupplier& userProps, int64_t nowMs) {
    std::shared_ptr<AbFetchSession> session(new AbFetchSession());
    session->client = client_;
    // Lazy resolution point: the supplier runs only here (when the request is
    // actually built), synchronously on the caller's thread; implementations
    // must return quickly and must not re-enter locks the caller already holds
    session->request = buildAbRequest(apiHost_, sourceToken_, anonId, loginId, userProps());
    session->anonId = anonId;
    session->loginId = loginId;
    session->fallback = results_;  // Fallback snapshot for failures (empty cache -> defaults)
    session->onComplete = onComplete_;
    if (deliver != nullptr) {
        session->emit = emitImpress_;
        std::unique_ptr<AbFetchSession::Waiter> waiter(new AbFetchSession::Waiter());
        waiter->kind = kind;
        waiter->key = key;
        waiter->deliver = deliver;
        session->waiters.push_back(std::move(waiter));
    } else {
        // Refresh-only session (refreshOnly, deliver is null): no placeholder
        // waiter and no impress emitter. Impressions arise only on the query
        // path (the JS fetchNewData only fetches and never calls sendABEvent) —
        // otherwise resolveWaiters would run computeImpress per response entry
        // against the placeholder key, and any matchable entry (including
        // adversarial empty-key entries) would be wrongly impressed on every
        // timer refresh
        session->emit = nullptr;
    }

    lastFetchMs_ = nowMs;             // 30s baseline = fetch start time
    lastFetchProducedCache_ = false;  // Settled by adoptCache / the failure path
    fetchAnonId_ = anonId;            // Identity snapshot at fetch start: covers identity
    fetchLoginId_ = loginId;          // change detection (see the identityChangedLocked note)
    hasFetchIdentity_ = true;
    inflight_ = session;
    try {
        std::thread(runFetch, session).detach();
    } catch (...) {
        // Thread creation failed (resource exhaustion): complete this round's
        // waiters synchronously, keeping deliver exactly-once intact. Answers
        // only, no impressions — fallback answers are not authoritative server
        // assignments
        {
            std::lock_guard<std::mutex> sessionLock(session->mutex);
            session->completed = true;
            session->waiters.clear();
        }
        inflight_.reset();
        if (deliver) {
            deliver(computeAnswer(kind, key, session->fallback));
        }
    }
}

AbAnswer AbManager::computeAnswer(AbKind kind, const std::string& key,
                                  const std::vector<AbDataItem>& results) {
    AbAnswer answer;
    const AbDataItem* found = nullptr;
    for (const AbDataItem& item : results) {  // First typ+key match
        if (item.typ == static_cast<int>(kind) && item.key == key) {
            found = &item;
            break;
        }
    }
    if (kind == AbKind::kGate) {
        // gate: hit requires vid present and vid != "fail" ("fail" is a server sentinel = miss)
        answer.gateValue = found != nullptr && found->hasVid && found->vid != "fail";
        return answer;
    }
    if (found == nullptr || found->value.is_null()) {
        answer.text = "{}";  // Missing key / invalid value -> empty object text
        return answer;
    }
    try {
        // Falsy scalars collapse to "{}" (matching the JS `data?.value || {}`
        // and `if (rawValue)` falsy short-circuit): 0 / false / "" are treated
        // like null instead of being stringified as "0" / "false" / ""
        const nlohmann::json& value = found->value;
        const bool falsyScalar = (value.is_boolean() && !value.get<bool>()) ||
                                 (value.is_number() && value.get<double>() == 0.0) ||
                                 (value.is_string() && value.get<std::string>().empty());
        if (falsyScalar) {
            answer.text = "{}";
            return answer;
        }
        if (kind == AbKind::kConfig && value.is_string()) {
            // config string value: parsed as JSON when possible, otherwise used verbatim
            const std::string raw = value.get<std::string>();
            try {
                answer.text = nlohmann::json::parse(raw).dump();
            } catch (...) {
                answer.text = raw;
            }
            return answer;
        }
        answer.text = value.dump();  // Object / array / scalar -> JSON text
    } catch (...) {
        answer.text = "{}";
    }
    return answer;
}

ImpressPlan AbManager::computeImpress(AbKind kind, const std::string& key,
                                      const std::vector<AbDataItem>& results) {
    ImpressPlan plan;
    const AbDataItem* found = nullptr;
    for (const AbDataItem& item : results) {
        if (item.typ == static_cast<int>(kind) && item.key == key) {
            found = &item;
            break;
        }
    }
    if (found == nullptr || found->disableImpress) {
        plan.emit = false;  // Missing key / impress disabled on the data side: no event
        return plan;
    }
    plan.emit = true;
    plan.kind = kind;
    plan.key = key;
    plan.id = found->id;  // Impression user_properties keys $feature_<id> / $exp_<id> (JS-aligned)
    plan.unset = !found->hasVid;  // Key present without vid -> unset impression
    plan.vid = found->hasVid ? found->vid : "";
    return plan;
}

}  // namespace sensorswave
