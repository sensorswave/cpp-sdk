#include "sensors_wave_client.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <utility>

#include "ab/ab_manager.h"
#include "consent/opt_out_manager.h"
#include "constants.h"
#include "crash/crash_handler.h"
#include "event/event_builder.h"
#include "event/event_json.h"
#include "exception/exception_collector.h"
#include "exception/stack_trace.h"
#include "http/http_client.h"
#include "identity/identity_manager.h"
#include "platform/system_info.h"
#include "profile/profile_api.h"
#include "props/common_properties.h"
#include "queue/event_queue.h"
#include "queue/persistent_queue.h"
#include "sender/batch_sender.h"
#include "session/session_manager.h"
#include "storage/file_store.h"
#include "storage/memory_store.h"
#include "storage/persistent_store.h"
#include "track/track_api.h"
#include "util/clock.h"
#include "util/logger.h"
#include "util/random.h"
#include "util/time_format.h"
#include "util/uuid.h"

#include <vector>

#ifdef SW_WITH_CURL
#include "http/curl_http_client.h"
#endif

namespace sensorswave {
namespace {

// Keep-alive table for infrastructure stranded by a self-thread destroy loop:
// when the host re-enters destroy() from inside IHttpClient::execute, the
// sender thread can only be detached (joining itself would deadlock), yet its
// run() epilogue still touches BatchSender / EventQueue / PersistentQueue /
// IHttpClient — the four are handed over here and never freed, so the thread
// winds down on live objects. This covers a rare re-entrant loop only; the
// cost is one-time and the OS reclaims the pages at process exit.
std::vector<std::shared_ptr<void>>& senderKeepAlive() {
    // Deliberately new'd and never freed: a function-local static would join
    // static destruction at process exit, while the detached sender thread may
    // still be touching table objects from its run() epilogue at that point —
    // racing its destructor is a use-after-free. The OS reclaims the object
    // when the process exits.
    static std::vector<std::shared_ptr<void>>* kept = new std::vector<std::shared_ptr<void>>();
    return *kept;
}

// Facade shared state (coarse-grained protection with a single mutex; the
// function-local static is unique across translation units).
// destroy() only releases infrastructure and never destroys this object
// (static destruction order is uncontrollable).
// Declaration order = construction order: random / clock must precede the
// uuid / builder that depend on them.
struct FacadeState {
    std::timed_mutex mutex;  // timed: terminate path locks with a timeout
    bool inited = false;
    bool closed = false;             // destroy idempotency flag; all APIs guard once set
    bool capturingDisabled = false;  // runtime Opt-Out flag

    // Effective configuration merged from defaults (init step 1)
    bool debug = false;
    bool batchSend = false;
    bool enableAB = false;
    bool enableCrashTrack = false;  // crash-capture registration switch
    bool autoCapture = true;        // auto-capture master switch (v1: $AppInstall only)
    int64_t abRefreshIntervalMs = AB_DEFAULT_REFRESH_MS;
    bool persistOptOut = false;
    ExceptionConfig exceptionConfig;
    std::string sourceToken;
    std::string apiHost;
    std::string dataDir;

    // Infrastructure (not created on the Opt-Out init path; optInCapturing
    // rebuilds it idempotently)
    std::shared_ptr<IHttpClient> httpClient;  // injected takes priority, else default curl
    std::unique_ptr<IPersistentStore> store;  // FileStore, or degraded MemoryStore
    std::unique_ptr<PersistentQueue> persistent;
    std::unique_ptr<EventQueue> queue;
    // shared_ptr: while the terminate path waits outside the lock and joins
    // the drain, destroy may concurrently complete and release — the copy we
    // hold keeps the sender alive until join returns (terminal-state race
    // protection)
    std::shared_ptr<BatchSender> sender;
    std::unique_ptr<ISystemInfo> systemInfo;  // preset property source, same lifetime as builder
    std::unique_ptr<IStackTrace> stackTrace;  // stack capture, invoked lazily, dies with infra

    SystemClock clock;
    SecureRandom random;
    Uuid uuid{&random};
    EventBuilder builder{&clock, &random};

    // Identity: IdentityManager state machine + $Identify outlet / the
    // reset session-rotation callback
    std::unique_ptr<IdentityManager> identity;

    // Common property registry: in-memory only, rebuilt on every init; also
    // created on the Opt-Out init path — register / clear stay usable while
    // capturing is disabled
    std::unique_ptr<CommonProperties> common;

    // Session state machine: created with the infrastructure (not on the
    // Opt-Out init path); accessed only under the facade lock, no internal
    // locking
    std::unique_ptr<SessionManager> session;

    // AB data manager: created only when enableAB is set and HTTP is available
    // (not created when enableAB=false / on the Opt-Out init path); queries
    // use their own mutex and never wait on the network under the facade lock
    std::unique_ptr<AbManager> ab;
    uint64_t abGeneration = 0;  // bumped on destroy / rebuild to invalidate in-flight callbacks

    // Opt-Out compliance state: pre-init setting + persisted encoding; the
    // runtime disable flag is capturingDisabled
    OptOutManager optOut;
};

FacadeState& facade() {
    // Heap-allocated and deliberately never freed: a function-local static
    // would be registered for atexit destruction, and during process exit the
    // sender thread may still be draining (drainFinal has no deadline) and
    // calling an already-destroyed host client — destruction order is
    // uncontrollable. The deliberate leak is what makes "never destroyed"
    // safe.
    static FacadeState* state = new FacadeState();
    return *state;
}

// Platform default data directory (Windows %APPDATA%\SensorsWave /
// macOS ~/Library/Application Support/SensorsWave /
// Linux $XDG_DATA_HOME/sensorswave, falling back to ~/.local/share/sensorswave).
// When every environment variable is missing, the final fallback is a relative
// path (which drifts with the process CWD): service-style hosts (daemons /
// containers running with a cleared environment) should configure dataDir
// explicitly and not rely on this fallback.
// Single-instance assumption: a given dataDir (including the default) serves
// one SDK instance only — multiple processes on one machine, or multiple
// instances in one process sharing a directory, will clobber each other's
// sw_* keys and queue files (last-writer-wins); the SDK does no cross-instance
// coordination. Hosts needing isolation should separate instances by dataDir.
std::string defaultDataDir() {
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    if (appData != nullptr && *appData != '\0') {
        return std::string(appData) + "\\SensorsWave";
    }
    return std::string("SensorsWave");
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return std::string(home) + "/Library/Application Support/SensorsWave";
    }
    return std::string("SensorsWave");
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        return std::string(xdg) + "/sensorswave";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return std::string(home) + "/.local/share/sensorswave";
    }
    return std::string("sensorswave");
#endif
}

// Guard: capture is allowed only when initialized, not destroyed, not opted
// out, and the infrastructure is in place. The sender is checked as well:
// after a mid-init exception (half-built infrastructure) is caught, inited is
// already set — a trackEvent that only looked at queue would dereference a
// null sender.
bool readyLocked(const FacadeState& s) {
    return s.inited && !s.closed && !s.capturingDisabled && s.queue != nullptr &&
           s.sender != nullptr;
}

// Session touchpoint: lazy rotation check + last_active update (throttled
// persistence), then syncs the value injected into the builder — $session_id
// is injected last, SDK-authoritative. Called on capture paths before build.
// The caller holds the lock and the infrastructure is in place.
void touchSessionLocked(FacadeState& s) {
    if (s.session) {
        s.builder.setSessionId(s.session->touch());
    }
}

// AB impression event: hit → $FeatureImpress / $ExpImpress (properties carry
// key / variant; user_properties.$set carries $feature_<id> / $exp_<id> = vid,
// aligned with the JS sendABEvent); key present but no vid → the same event
// name with the key only (unset semantics), and user_properties.$unset carries
// the same key = null. Impression events are enqueued through the same channel
// as regular events.
// Invalidated meanwhile (destroy / Opt-Out) → blocked by readyLocked. The
// caller holds the facade lock.
void emitImpressLocked(FacadeState& s, const ImpressPlan& plan) {
    if (!plan.emit || !readyLocked(s)) {
        return;
    }
    try {
        touchSessionLocked(s);
        const bool isExperiment = plan.kind == AbKind::kExperiment;
        const char* eventName = isExperiment ? "$ExpImpress" : "$FeatureImpress";
        const char* keyProp = isExperiment ? "$exp_key" : "$feature_key";
        const char* variantProp = isExperiment ? "$exp_variant" : "$feature_variant";
        PropertyMap props;
        props[keyProp] = PropertyValue::fromString(plan.key);
        if (!plan.unset) {
            props[variantProp] = PropertyValue::fromString(plan.vid);
        }
        // User property channel (aligned with the JS sendABEvent): hit →
        // $set { $feature_<id> / $exp_<id>: vid }; unset → $unset { same key:
        // null } (the user_properties protocol keeps null entries; the key is
        // the data entry's numeric id)
        UserOp userOp;
        userOp.op = plan.unset ? "$unset" : "$set";
        userOp.payload[(isExperiment ? "$exp_" : "$feature_") + std::to_string(plan.id)] =
            plan.unset ? PropertyValue() : PropertyValue::fromString(plan.vid);
        s.queue->enqueue(s.builder.build(eventName, props, std::move(userOp)));
        s.sender->notifyEvent();
    } catch (...) {
        Logger::warn("impress event failed");  // failure does not affect the query answer
    }
}

// Crash payload pre-serialization: at init, a probe event builds a full event
// JSON snapshot (mandatory fields + level=fatal + two marker frames) split
// into four segments that a signal context can write directly; this
// construction only exists in a normal context (neither the builder nor
// serialization may run inside a signal handler). The caller holds the facade
// lock.
CrashPayloadParts buildCrashPayloadPartsLocked(FacadeState& s) {
    PropertyMap props;
    props["$exception_level"] = PropertyValue::fromString("fatal");
    props["$exception_message"] = PropertyValue::fromString("process terminated by fatal signal");
    props["$exception_type"] = PropertyValue::fromString(CRASH_TYPE_MARKER);  // crash-time backfill
    std::vector<PropertyMap> probeFrames(2);
    for (int i = 0; i < 2; ++i) {
        probeFrames[i]["platform"] = PropertyValue::fromString(EXCEPTION_PLATFORM);
        probeFrames[i]["instruction_addr"] =
            PropertyValue::fromString(i == 0 ? CRASH_ADDR_MARKER : CRASH_ADDR_MARKER2);
        // Signal-path frames carry only an address, no module path (the
        // handler only calls writeHexAddr) — true matches the resolveInApp
        // convention that an empty path counts as in-app; false would
        // contradict that convention, and an all-false frame list makes the
        // symbolicator treat the whole stack as not in-app
        probeFrames[i]["in_app"] = PropertyValue::fromBool(true);
    }
    props["$exception_frames"] = PropertyValue::fromMapList(std::move(probeFrames));
    touchSessionLocked(s);  // snapshot taken at init; later rotations are not written back
    const Event probe = s.builder.build("$Exception", props);
    return splitCrashPayload(serializeEventJson(probe));
}

// Terminate-path hook (crash-layer callback, normal context): the fatal event
// is enqueued through the regular pipeline (and thus persisted) plus a
// short-timeout best-effort send. If the drain does not finish or an exception
// occurs, no pending file is written — the event is already persisted in
// sw_queue and re-send duty falls to queue recovery; pending is reserved for
// the signal path (avoids duplicate re-sends). The facade lock may be held by
// the terminating thread: lock with a timeout, and give up capture if it
// cannot be taken (prevents a mutual wait with the terminating thread).
// Returns: true = enqueued (a chained abort will not capture again);
// false = capture did not succeed, and the crash layer then lets the SIGABRT
// signal path write the pending fallback. The drain wait and join happen
// outside the facade lock (the same three-stage shape as destroy): an
// in-flight HTTP host implementation that re-enters the SDK can acquire the
// facade lock and complete (closed is already set → no-op), so join finishes
// in bounded time.
bool facadeTerminateCapture(const std::string& type, const std::string& message,
                            const std::vector<StackFrame>& frames) {
    FacadeState& s = facade();
    std::unique_lock<std::timed_mutex> lock(s.mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(500))) {
        return false;  // lock holder terminating: give up (process dying anyway, not a deadlock)
    }
    if (!s.inited || s.closed || s.capturingDisabled || s.queue == nullptr || s.sender == nullptr) {
        return false;  // infrastructure missing / disabled: no pipeline to run
    }
    // The terminal guard is set before the drain flag: the process is about to
    // die, so any later SDK call (re-entrant callbacks included) becomes a
    // no-op under the closed semantics; a concurrent destroy also returns
    // early here (the infrastructure is no longer released)
    s.closed = true;
    std::shared_ptr<BatchSender> sender = s.sender;
    bool captured = false;
    try {
        PropertyMap props;
        buildExceptionProperties(&props, type, message, frames, s.exceptionConfig, "fatal");
        touchSessionLocked(s);
        s.queue->enqueue(s.builder.build("$Exception", props));
        s.sender->requestStop(TERMINATE_DRAIN_TIMEOUT_MS);  // flag set only, instantaneous
        captured = true;
    } catch (...) {
        Logger::warn("terminate capture failed");  // no enqueue → the signal path writes pending
    }
    lock.unlock();
    if (sender) {
        try {
            sender->awaitDrainFinished(TERMINATE_DRAIN_TIMEOUT_MS);  // best-effort, no guarantee
            sender->joinStopped();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // The process is about to die: logging is best-effort
        }
    }
    return captured;
}

// Init step 5 (registers crash capture when enableCrashTrack=true). Reused by
// the idempotent rebuild path (optInCapturing). The caller holds the facade
// lock.
void installCrashHooksIfEnabledLocked(FacadeState& s) {
    if (!s.enableCrashTrack) {
        return;  // not enabled → nothing to register
    }
    try {
        const CrashPayloadParts parts = buildCrashPayloadPartsLocked(s);
        installCrashHooks(s.dataDir + "/" + SW_KEY_CRASH_PENDING, parts, &facadeTerminateCapture);
    } catch (...) {
        Logger::warn("crash hooks install failed");  // registration failure does not fail init
    }
}

// Init steps 3-5: storage layer + event infrastructure. Reused by the
// optInCapturing rebuild path. The caller holds the facade lock.
void buildInfraLocked(FacadeState& s) {
    // Step 3: storage layer. If the directory cannot be created or a fatal IO
    // error occurs → degrade wholesale to MemoryStore; per-key corruption is
    // handled by each consumer at load time
    std::unique_ptr<FileStore> fileStore(new FileStore(s.dataDir));
    if (fileStore->valid()) {
        s.store = std::move(fileStore);
    } else {
        Logger::warn("data dir unavailable, degrade to memory store: " + s.dataDir);
        s.store.reset(new MemoryStore());
    }

    // Identity: load generates / restores anon and login; $Identify goes out
    // through the builder + queue outlet (the outlet syncs builder identity
    // before building the event; identity fields are top-level only and the
    // property map stays empty)
    s.identity.reset(new IdentityManager(
        s.store.get(), &s.uuid,
        [&s](const std::string& anonId, const std::string& loginId) {
            try {
                s.builder.setIdentity(anonId, loginId);
                touchSessionLocked(s);  // $Identify also carries $session_id
                // after setIdentity, build writes anon_id / login_id as the
                // mandatory top-level fields; identity is not duplicated into
                // the property map, which stays empty
                const Event event = s.builder.build("$Identify", PropertyMap());
                s.queue->enqueue(event);
                s.sender->notifyEvent();
            } catch (...) {
                Logger::warn("identify event failed");  // does not affect persisting login state
            }
        },
        [&s] {
            try {
                if (s.session) {
                    s.session->forceRotate();  // rotate the session immediately on reset
                }
            } catch (...) {
                Logger::warn("session rotate failed");  // does not affect the logout flow
            }
        }));  // session rotation callback
    s.identity->load();
    s.builder.setIdentity(s.identity->anonId(), s.identity->loginId());

    // Session: loads sw_session (missing / corrupt → no state); rotation is
    // decided lazily at the first event touchpoint, which is how a cold start
    // continues the previous session
    s.session.reset(new SessionManager(s.store.get(), &s.uuid, &s.clock));
    s.session->load();

    // Step 4: event queue → crash re-send → batch sender thread (the re-send
    // runs before the sender starts and rides the normal batch / retry
    // pipeline)
    s.persistent.reset(new PersistentQueue(s.dataDir + "/" + SW_KEY_QUEUE));
    s.queue.reset(new EventQueue(s.persistent.get(), &s.clock));
    std::string pendingJson;
    if (s.store != nullptr && s.store->read(SW_KEY_CRASH_PENDING, &pendingJson)) {
        Event crashEvent;
        if (deserializeEventJson(pendingJson, &crashEvent)) {
            try {
                s.queue->enqueue(crashEvent);  // time keeps the original crash moment
            } catch (...) {
                Logger::warn("crash pending requeue failed");
            }
        } else if (!pendingJson.empty()) {
            // Only a non-empty unparsable value is real corruption; an empty
            // string is a normal-exit remnant (the empty O_TRUNC shell created
            // at install time) and is not warned about
            Logger::warn("crash pending unparsable, dropped");  // corrupt: drop the remnant
        }
        s.store->remove(SW_KEY_CRASH_PENDING);  // removed either way, prevents duplicate re-sends
    }
    s.sender.reset(new BatchSender(s.queue.get(), s.httpClient.get(), s.clock, s.apiHost,
                                   s.sourceToken, s.batchSend));
    s.sender->start();

    // AB: created only when enableAB is set and HTTP is available. The
    // completion callback and impression outlet run on worker threads; after
    // a generation check (dropped after destroy / rebuild) they take the
    // facade lock to build events / persist
    if (s.enableAB && s.httpClient) {
        ++s.abGeneration;
        const uint64_t generation = s.abGeneration;
        const AbManager::FetchComplete onComplete =
            [&s, generation](const std::vector<AbDataItem>& results, const std::string& anonId,
                             const std::string& loginId) {
                std::lock_guard<std::timed_mutex> lock(s.mutex);
                if (generation != s.abGeneration || !s.ab || !s.store) {
                    return;  // stale callback after destroy / rebuild: drop
                }
                const int64_t nowMs = s.clock.nowMs();
                // Empty results are not persisted (aligned with the JS
                // saveABData guard that skips the cache on an empty array):
                // the old cache (with its identity attribution) is kept as-is;
                // the in-memory adoptCache records "no cache produced", so
                // queries are not suppressed by the 30s window gate and a pull
                // with the latest identity happens at once
                if (!results.empty()) {
                    // Record the response time and identity at that moment
                    // (cache attribution check)
                    const std::string json = buildAbCacheJson(results, nowMs, anonId, loginId);
                    if (!json.empty() && !s.store->write(SW_KEY_AB_DATA, json)) {
                        Logger::warn("persist sw_ab_data failed, cache kept in memory");
                    }
                }
                s.ab->adoptCache(results, nowMs, anonId, loginId);
            };
        const AbManager::ImpressEmitter emitImpress = [&s, generation](const ImpressPlan& plan) {
            if (!plan.emit) {
                return;
            }
            std::lock_guard<std::timed_mutex> lock(s.mutex);
            if (generation != s.abGeneration) {
                return;
            }
            emitImpressLocked(s, plan);  // Opt-Out / destroy meanwhile → blocked by readyLocked
        };
        s.ab.reset(
            new AbManager(s.httpClient, &s.clock, s.abRefreshIntervalMs, s.apiHost, s.sourceToken));
        s.ab->setFetchComplete(onComplete);
        s.ab->setImpressEmitter(emitImpress);
        // Identity supplier for the periodic refresh (consumed by the AB timer
        // thread each tick): disabled states (uninitialized / destroyed /
        // Opt-Out) return false and skip silently — no AB request is started
        // while disabled (compliance). The facade lock is taken with
        // try_lock_for: destroy resets the AbManager under the facade lock and
        // joins the timer thread; a supplier waiting indefinitely on the lock
        // would deadlock with the destructor, so on timeout this round is
        // abandoned
        const AbManager::RefreshIdentitySupplier identitySupplier =
            [&s](std::string* anonId, std::string* loginId, nlohmann::json* userProps) {
                std::unique_lock<std::timed_mutex> lock(s.mutex, std::defer_lock);
                if (!lock.try_lock_for(std::chrono::milliseconds(50))) {
                    return false;  // facade busy (e.g. destroy finishing): skip this round
                }
                if (!s.inited || s.closed || s.capturingDisabled || !s.identity) {
                    return false;
                }
                *anonId = s.identity->anonId();
                *loginId = s.identity->loginId();
                // user.props snapshot under the same lock as the identity:
                // preset + resolved common properties
                try {
                    *userProps = propertiesToJson(s.builder.snapshotUserProperties());
                } catch (...) {
                    *userProps = nlohmann::json::object();  // snapshot failed: empty object is fine
                }
                return true;
            };
        s.ab->setIdentitySupplier(identitySupplier);
        s.ab->load(s.store.get());  // restores sw_ab_data (corrupt means no cache state)
    }

    // Preset property set: built once at init; a key that cannot be read is
    // left missing rather than filled with an empty value, and the host can
    // override it with a common / event property of the same name.
    // $network_type / $wifi are queried per event: the provider's raw pointer
    // is safe — systemInfo shares FacadeState lifetime with the builder and is
    // destroyed together with it on destroy
    s.systemInfo = createPlatformSystemInfo();
    s.builder.setPresetProperties(buildPresetProperties(s.systemInfo.get()));
    ISystemInfo* networkInfo = s.systemInfo.get();
    s.builder.setNetworkTypeProvider(
        [networkInfo](std::string* out) { return networkInfo->networkType(out); });

    // Init-time prefetch (aligned with the JS SDK, where init's fastFetch is
    // cache-first): a valid cache means no network call; on a miss, a single
    // waiter-less evalall is issued — otherwise the first pull would wait a
    // full refresh cycle. Must run after the preset property / network
    // provider injection: user.props drives server-side segmentation, so
    // running earlier would evaluate with empty props and cache a
    // mis-segmented result (same reason $AppInstall sits after the
    // injections). s.ab exists only when enableAB is set and HTTP is available
    if (s.ab != nullptr && s.identity != nullptr) {
        try {
            s.ab->prefetch(s.identity->anonId(), s.identity->loginId(),
                           propertiesToJson(s.builder.snapshotUserProperties()));
        } catch (...) {
            Logger::warn("ab prefetch failed");  // prefetch failure does not fail init
        }
    }

    // $AppInstall: first-launch check, shared by init and the optInCapturing
    // rebuild path. Placed after the preset property / network provider
    // injection so the event's properties are complete. The launched flag is
    // still written decoupled from autoCapture (the flag records the fact that
    // the app has launched; a host enabling the switch in a later version does
    // not backfill, preventing inflated install counts on upgrade); in the
    // Opt-Out disabled state no Store is created and no flag is written, so
    // the first launch counts at opt-in rebuild time. The flag is written
    // before enqueueing: on write failure the event is dropped — prefer losing
    // one over double-counting (install-count integrity first)
    if (s.store != nullptr) {
        std::string launched;
        // First-launch check → atomic flag write first (prefer loss over
        // duplicates) → autoCapture gate; the short-circuit order is the
        // business order
        if (!s.store->read(SW_KEY_LAUNCHED, &launched) && s.store->write(SW_KEY_LAUNCHED, "1") &&
            s.autoCapture) {
            try {
                touchSessionLocked(s);  // $AppInstall also carries $session_id
                // $install_time ($AppInstall-only preset property, DATETIME):
                // desktops have no system-level install-time API, so the
                // first-launch moment (when the flag is written; the two run
                // on the same call path, milliseconds apart) approximates the
                // install completion time; ISO 8601 UTC with milliseconds
                PropertyMap installProps;
                installProps["$install_time"] =
                    PropertyValue::fromString(formatIso8601UtcMs(s.clock.nowMs()));
                const Event event = s.builder.build("$AppInstall", installProps);
                s.queue->enqueue(event);
                s.sender->notifyEvent();
            } catch (...) {
                Logger::warn("app install event failed");  // silent failure, the flag is in place
            }
        }
    }

    // Stack trace capturer: on capture failure the implementation returns
    // empty and the event is still sent
    s.stackTrace = createPlatformStackTrace();
}

// Body of the six-step sequence; the caller holds the facade lock and is not
// yet initialized
void initLocked(FacadeState& s, const std::string& sourceToken, const SensorsWaveConfig& config) {
    // Step 1: merge config; abRefreshInterval is clamped to a lower bound;
    // sourceToken / apiHost are not blocked when empty, only warned about —
    // those sends will fail and be handled by the retry policy
    s.sourceToken = sourceToken;
    s.apiHost = config.apiHost;
    s.debug = config.debug;
    s.batchSend = config.batchSend;
    s.enableAB = config.enableAB;
    s.enableCrashTrack = config.enableCrashTrack;
    s.autoCapture = config.autoCapture;
    // Refresh interval normalization; see normalizeAbRefreshInterval
    s.abRefreshIntervalMs = normalizeAbRefreshInterval(config.abRefreshInterval);
    s.persistOptOut = config.persistOptOut;
    s.exceptionConfig = config.exceptionConfig;
    s.dataDir = config.dataDir.empty() ? defaultDataDir() : config.dataDir;
    Logger::setDebug(config.debug);
    if (sourceToken.empty() || config.apiHost.empty()) {
        Logger::warn("sourceToken / apiHost is empty; track requests will fail and retry");
    }

    // HTTP client: injected takes priority; with SW_WITH_CURL=0 and nothing
    // injected, only a warning is logged — events still enqueue and persist,
    // and sends are handled by BatchSender as network failures
    s.httpClient = config.httpClient;
#ifdef SW_WITH_CURL
    if (!s.httpClient) {
        s.httpClient.reset(new CurlHttpClient());
    }
#endif
    if (!s.httpClient) {
        Logger::warn("no http client available; events stay queued (inject IHttpClient)");
    }

    // Common property registry: created before Opt-Out resolution so register
    // / clear still work while disabled and their state is retained; the
    // builder's common table is cleared in sync to avoid stale entries
    s.common.reset(new CommonProperties());
    s.builder.setCommonProperties(PropertyMap());

    // Step 2: resolve Opt-Out (priority: pre-init API > config > persisted
    // value; delegated to OptOutManager). A final disabled result → no Store /
    // queue / sender is created; init is still marked complete and we return
    // early. With persistOptOut this step reads only the sw_opt_out key
    const OptOutManager::PreInitState preInit = s.optOut.consumePreInit();
    bool disabled = false;
    {
        FileStore probe(s.dataDir);
        IPersistentStore* probeStore = probe.valid() ? &probe : nullptr;
        disabled = OptOutManager::resolveInitDisabled(preInit, config.optOutCapturing,
                                                      config.persistOptOut, probeStore);
    }
    s.capturingDisabled = disabled;
    if (disabled) {
        s.inited = true;
        Logger::debug("sdk initialized, capturing disabled (opt-out)");
        return;
    }

    buildInfraLocked(s);

    // Step 5: register crash capture when enableCrashTrack is true;
    // other than the first-launch $AppInstall and the crash re-send, init
    // produces no other events or network requests
    installCrashHooksIfEnabledLocked(s);
    // Step 6: mark initialized, return synchronously
    s.inited = true;
    Logger::debug("sdk initialized successfully, dataDir: " + s.dataDir);
}

}  // namespace

// ── Lifecycle ──

void init(const std::string& sourceToken, const SensorsWaveConfig& config) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (s.inited) {
            // Single init: repeated calls are silently ignored; the first
            // configuration stays in effect
            Logger::debug("sdk already initialized, repeated init ignored");
            return;
        }
        s.closed = false;  // re-init after destroy is allowed (guards now match pre-init)
        try {
            initLocked(s, sourceToken, config);
        } catch (...) {
            // An exception in any of the six steps: completed steps are kept,
            // unfinished ones degrade along their own paths; init never fails
            // by throwing
            Logger::warn("sdk init failed, degraded after internal exception");
            s.inited = true;
        }
    } catch (...) {
        // Pathological exception before setup (lock acquisition, corrupted
        // mutex / OOM): no infrastructure was built and no degraded flag is
        // set; s.inited stays false and later calls hit the uninitialized
        // guard
        Logger::warn("sdk init failed before infrastructure setup");
    }
}

void destroy() noexcept {
    try {
        FacadeState& s = facade();
        std::shared_ptr<BatchSender> sender;
        {
            std::lock_guard<std::timed_mutex> lock(s.mutex);
            if (!s.inited || s.closed) {
                return;  // uninitialized no-op; closed is idempotent and also
                         // lets only one concurrent destroy enter the drain
                         // and join below
            }
            s.closed = true;  // guard flag first: concurrent destroy / capture /
                              // re-entrant callbacks become no-ops immediately
            // Uninstall crash capture first (the terminate hook depends on
            // facade infrastructure; the pending file is kept for re-send)
            uninstallCrashHooks();
            try {
                // Final drain, stage 1 (only sets a flag under the lock,
                // instantaneous): best-effort send within the budget; on
                // timeout events stay queued (already persisted) — not a
                // send-success guarantee
                if (s.sender) {
                    s.sender->requestStop(DESTROY_DRAIN_TIMEOUT_MS);
                    sender = s.sender;  // copy for the out-of-lock stages: safe even if a
                                        // concurrent destroy finishes first
                }
            } catch (...) {
                Logger::warn("destroy drain failed, persisted state kept");
            }
        }
        // Stages 2-3 (outside the facade lock): wait for the drain + join.
        // The lock is released by now — if an in-flight HTTP host
        // implementation re-enters the SDK, its callback can acquire the
        // facade lock and return as a no-op under the closed guard, letting
        // the sender thread exit so join finishes in bounded time (joining
        // under the lock would deadlock with it)
        bool joined = true;
        if (sender) {
            try {
                sender->awaitDrainFinished(DESTROY_DRAIN_TIMEOUT_MS);
                joined = sender->joinStopped();
            } catch (...) {
                Logger::warn("destroy drain join failed, persisted state kept");
                joined = false;  // join outcome unknown: the keep-alive path is the safe choice
            }
        }
        {
            std::lock_guard<std::timed_mutex> lock(s.mutex);
            try {
                if (s.queue) {
                    s.queue->rewriteDisk();  // one final full rewrite to disk
                }
            } catch (...) {
                Logger::warn("destroy final rewrite failed, persisted state kept");
            }
            // Release the infrastructure; from here on all APIs run with
            // pre-init guard semantics.
            // AB: bump the generation to invalidate in-flight callbacks before
            // releasing — the fetch thread only holds a session copy, so
            // destruction does not block on it
            ++s.abGeneration;
            s.ab.reset();
            if (joined) {
                s.sender.reset();
                s.queue.reset();
                s.persistent.reset();
                s.httpClient.reset();
            } else {
                // The sender thread initiated destroy on this very thread (the
                // host re-entered from execute, the only scenario): it is
                // detached, so the sender-related infrastructure is handed to
                // the keep-alive table and the run() epilogue keeps running on
                // live objects; the remaining members do not interact with the
                // sender thread and are released normally
                std::vector<std::shared_ptr<void>>& kept = senderKeepAlive();
                kept.push_back(std::shared_ptr<void>(s.sender));
                if (s.queue) {
                    kept.push_back(std::shared_ptr<void>(s.queue.release()));
                }
                if (s.persistent) {
                    kept.push_back(std::shared_ptr<void>(s.persistent.release()));
                }
                kept.push_back(std::shared_ptr<void>(s.httpClient));
                s.sender.reset();
                s.queue.reset();  // unique_ptr already released, reset is a no-op
                s.persistent.reset();
                s.httpClient.reset();
            }
            s.systemInfo.reset();
            s.stackTrace.reset();
            s.identity.reset();
            s.session.reset();  // dies with the infrastructure; queries return "" after destroy
            s.common.reset();   // common properties are memory-only, dropped here
            s.store.reset();
            s.builder.setIdentity("", "");
            s.builder.setSessionId("");  // the builder is reused across inits; clear residue
            s.builder.setCommonProperties(PropertyMap());
            s.builder.setNetworkTypeProvider(
                nullptr);  // avoid dangling: the provider's systemInfo raw pointer is gone
            s.inited = false;
        }
    } catch (...) {
        // Catch-all for pathological exceptions (lock acquisition / builder
        // cleanup); each stage already has its own inner catch. Persisted data
        // is unaffected
        Logger::warn("destroy failed, persisted state kept");
    }
}

// ── Events ──

void trackEvent(const std::string& eventName, const PropertyMap& properties) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;  // uninitialized / destroyed / Opt-Out: no-op
        }
        if (!isValidEventName(eventName)) {
            // Empty name: drop silently
            Logger::warn("trackEvent dropped: empty event name");
            return;
        }
        if (isReservedEventName(eventName)) {
            // '$'-prefixed names are reserved for preset events: allow with a
            // warning only — hosts may legitimately report preset-shaped
            // events on demand, so they must not be dropped silently
            Logger::warn("trackEvent: '$'-prefixed name is reserved for preset events: " +
                         eventName);
        }
        warnReservedPropertyKeys(properties, "trackEvent");  // same policy for property keys
        touchSessionLocked(s);  // event touchpoint: rotation check + $session_id injection
        const Event event = s.builder.build(eventName, properties);
        s.queue->enqueue(event);
        s.sender->notifyEvent();
    } catch (...) {
        Logger::warn("trackEvent failed");  // silent failure, never throws to the caller
    }
}

// Advanced entry: the host builds the event object; mandatory fields are still
// filled in by EventBuilder. Empty names are rejected; '$'-prefixed names are
// allowed with a warning only.
void track(const SendEvent& eventData) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;  // uninitialized / destroyed / Opt-Out: no-op
        }
        if (!isValidEventName(eventData.eventName)) {
            // Empty name: drop silently
            Logger::warn("track dropped: empty event name");
            return;
        }
        if (isReservedEventName(eventData.eventName)) {
            // '$'-prefixed names are allowed with a warning only
            Logger::warn("track: '$'-prefixed name is reserved for preset events: " +
                         eventData.eventName);
        }
        warnReservedPropertyKeys(eventData.properties, "track");  // same policy for property keys
        touchSessionLocked(s);                                    // event touchpoint
        const Event event = s.builder.build(eventData.eventName, eventData.properties);
        s.queue->enqueue(event);
        s.sender->notifyEvent();
    } catch (...) {
        Logger::warn("track failed");  // silent failure, never throws to the caller
    }
}

// $Exception capture outlet: both overloads share the main flow. The level is
// fixed to error and not gated by any switch; host properties are merged in
// first, then overridden by the SDK-authoritative $exception_* values
// ('$'-prefix reservation semantics). Fire-and-forget, returns synchronously.
// File-local helper: the anonymous namespace gives internal linkage, keeping
// FacadeState-related symbols out of the library's exports
namespace {

void trackExceptionLocked(FacadeState& s, const std::string& type, const std::string& message,
                          const PropertyMap& properties) {
    warnReservedPropertyKeys(properties, "trackException");  // same policy for property keys
    PropertyMap props = properties;
    std::vector<StackFrame> frames;
    if (s.stackTrace) {
        frames = s.stackTrace->capture(FRAME_LIMIT);  // up to 30 frames
    }
    buildExceptionProperties(&props, type, message, frames, s.exceptionConfig, "error");
    touchSessionLocked(s);  // event touchpoint: $Exception also carries $session_id
    s.queue->enqueue(s.builder.build("$Exception", props));
    s.sender->notifyEvent();
}

}  // namespace

void trackException(const std::exception& e, const PropertyMap& properties) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;  // uninitialized / destroyed / Opt-Out: no-op
        }
        trackExceptionLocked(s, exceptionTypeName(e), e.what(), properties);
    } catch (...) {
        Logger::warn("trackException failed");  // silent failure, never throws to the caller
    }
}

void trackException(const std::string& type, const std::string& message,
                    const PropertyMap& properties) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;  // uninitialized / destroyed / Opt-Out: no-op
        }
        trackExceptionLocked(s, type, message, properties);
    } catch (...) {
        Logger::warn("trackException failed");
    }
}

// ── Identity (state machine in IdentityManager; the facade handles guards
//    and builder identity sync) ──

void identify(const std::string& loginId) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;  // uninitialized / destroyed / Opt-Out no-op
        }
        s.identity->identify(loginId);  // the outlet syncs the builder and enqueues $Identify
    } catch (...) {
        Logger::warn("identify failed");  // silent failure
    }
}

void setLoginId(const std::string& loginId) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;  // uninitialized / destroyed / Opt-Out no-op
        }
        s.identity->setLoginId(loginId);
        s.builder.setIdentity(s.identity->anonId(),
                              s.identity->loginId());  // top-level login_id on later events
    } catch (...) {
        Logger::warn("setLoginId failed");
    }
}

void reset(bool resetAnonId) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;  // uninitialized / destroyed / Opt-Out no-op
        }
        s.identity->reset(resetAnonId);
        // Sync the builder after logout / anon rotation (later events always
        // carry anon; login follows the login state)
        s.builder.setIdentity(s.identity->anonId(), s.identity->loginId());
    } catch (...) {
        Logger::warn("reset failed");
    }
}

std::string getAnonId() noexcept {
    std::string out;  // returned by move construction (noexcept); the catch path builds no string
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!s.inited || s.closed || s.capturingDisabled) {
            return out;  // Opt-Out / uninitialized → "" (out is already empty)
        }
        if (s.identity) {
            out = s.identity->anonId();
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Silent failure: out stays empty
    }
    return out;
}

std::string getLoginId() noexcept {
    std::string out;  // same as getAnonId: the catch path builds no string
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!s.inited || s.closed || s.capturingDisabled) {
            return out;
        }
        if (s.identity) {
            out = s.identity->loginId();
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Silent failure: out stays empty
    }
    return out;
}

std::string getSessionId() noexcept {
    std::string out;  // same as getAnonId: the catch path builds no string
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!s.inited || s.closed || s.capturingDisabled) {
            return out;  // uninitialized / destroyed / Opt-Out → "" with no sw_session access
        }
        // Query touchpoint: last_active is not updated; if expired, rotates
        // and writes back
        if (s.session) {
            out = s.session->currentSessionId();
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Silent failure: out stays empty
    }
    return out;
}

// ── User properties: operator mapping and payload validation live in
//    profile_api; the facade handles guarding, $UserSet construction and
//    enqueueing (fire-and-forget); no-op during Opt-Out ──

namespace {

// Common profile outlet: the caller holds the lock, readyLocked has passed,
// and the payload is non-empty. UserOp carries the operator; the 3-arg build
// serializes it into the top-level nested user_properties form
void dispatchProfile(FacadeState& s, UserOp userOp) {
    try {
        if (!hasProfileIdentity(s.builder.loginId(), s.builder.anonId())) {
            // Identity gate: login_id or anon_id, at least one (when logged
            // out, operations are attributed to the anonymous identity); if
            // both are missing nothing is sent. Defensive path: init always
            // generates an anon_id, so public APIs cannot reach this
            Logger::warn("profile event requires login_id or anon_id; dropped");
            return;
        }
        touchSessionLocked(s);  // $UserSet also carries $session_id
        const Event event = s.builder.build("$UserSet", PropertyMap(), std::move(userOp));
        s.queue->enqueue(event);
        s.sender->notifyEvent();
    } catch (...) {
        Logger::warn("profile event failed");  // silent failure
    }
}

}  // namespace

void profileSet(const PropertyMap& props) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;
        }
        PropertyMap payload = sanitizeProfilePayload(ProfileOp::kSet, props);
        if (payload.empty()) {
            return;  // empty payload no-op
        }
        dispatchProfile(s, buildUserOp(ProfileOp::kSet, payload));
    } catch (...) {
        Logger::warn("profileSet failed");  // silent failure
    }
}

void profileSetOnce(const PropertyMap& props) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;
        }
        PropertyMap payload = sanitizeProfilePayload(ProfileOp::kSetOnce, props);
        if (payload.empty()) {
            return;
        }
        dispatchProfile(s, buildUserOp(ProfileOp::kSetOnce, payload));
    } catch (...) {
        Logger::warn("profileSetOnce failed");
    }
}

void profileIncrement(const PropertyMap& props) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;
        }
        PropertyMap payload = sanitizeProfilePayload(ProfileOp::kIncrement, props);
        if (payload.empty()) {
            return;  // no event once every non-numeric value has been skipped
        }
        dispatchProfile(s, buildUserOp(ProfileOp::kIncrement, payload));
    } catch (...) {
        Logger::warn("profileIncrement failed");
    }
}

void profileAppend(const PropertyMap& props) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;
        }
        PropertyMap payload = sanitizeProfilePayload(ProfileOp::kAppend, props);
        if (payload.empty()) {
            return;
        }
        dispatchProfile(s, buildUserOp(ProfileOp::kAppend, payload));
    } catch (...) {
        Logger::warn("profileAppend failed");
    }
}

void profileUnion(const PropertyMap& props) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;
        }
        PropertyMap payload = sanitizeProfilePayload(ProfileOp::kUnion, props);
        if (payload.empty()) {
            return;
        }
        dispatchProfile(s, buildUserOp(ProfileOp::kUnion, payload));
    } catch (...) {
        Logger::warn("profileUnion failed");
    }
}

void profileUnset(const std::vector<std::string>& keys) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;
        }
        UserOp op = buildUnsetOp(keys);
        if (op.payload.empty()) {
            return;  // no valid key names (empty strings dropped), no event
        }
        dispatchProfile(s, std::move(op));
    } catch (...) {
        Logger::warn("profileUnset failed");
    }
}

void profileDelete() noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!readyLocked(s)) {
            return;
        }
        dispatchProfile(s, buildDeleteOp());  // the identity gate is enforced in dispatchProfile
    } catch (...) {
        Logger::warn("profileDelete failed");
    }
}

// ── Common properties: the registry lives in FacadeState and stays usable
//    while disabled; changes sync the builder snapshot immediately, and
//    getters are evaluated by build per event ──

void registerCommonProperties(const PropertyMap& props) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!s.inited || s.closed || !s.common) {
            return;  // pre-init / destroyed no-op
        }
        warnReservedPropertyKeys(props,
                                 "registerCommonProperties");  // '$'-prefixed keys: warn, allow
        s.common->registerProperties(props);
        s.builder.setCommonProperties(s.common->snapshot());
    } catch (...) {
        Logger::warn("registerCommonProperties failed");  // silent failure
    }
}

void clearCommonProperties() noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!s.inited || s.closed || !s.common) {
            return;
        }
        s.common->clearAll();
        s.builder.setCommonProperties(s.common->snapshot());
    } catch (...) {
        Logger::warn("clearCommonProperties failed");
    }
}

void clearCommonProperties(const std::vector<std::string>& keys) noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!s.inited || s.closed || !s.common) {
            return;
        }
        s.common->clearKeys(keys);
        s.builder.setCommonProperties(s.common->snapshot());
    } catch (...) {
        Logger::warn("clearCommonProperties(keys) failed");
    }
}

// ── AB (fastFetch semantics; uninitialized / enableAB=false / Opt-Out →
//    complete immediately with the default value, no request) ──

// Impression re-emission for the synchronous completion path: the caller has
// already unlocked after the query, so each plan re-locks to build its event.
// Invalidation meanwhile is caught by emitImpressLocked → readyLocked. Called
// without the lock held.
// File-local helper: the anonymous namespace gives internal linkage, keeping
// FacadeState-related symbols out of the library's exports
namespace {

void emitImpressPlans(FacadeState& s, const std::vector<ImpressPlan>& plans) {
    for (const ImpressPlan& plan : plans) {
        if (!plan.emit) {
            continue;
        }
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        emitImpressLocked(s, plan);
    }
}

// user.props snapshot for AB queries: the caller holds the facade lock. A
// failed snapshot degrades to an empty object — it must never throw outward,
// or query would not have run and deliver would never be called, leaving the
// host's future broken (broken_promise) and violating silent failure
nlohmann::json abUserPropsSnapshot(FacadeState& s) {
    try {
        return propertiesToJson(s.builder.snapshotUserProperties());
    } catch (...) {
        return nlohmann::json::object();
    }
}

// Lazy user.props supplier factory: the three AB query APIs no longer resolve
// every common property getter synchronously per call (including the
// synchronous cache-hit path); the snapshot is deferred until
// AbManager::startFetchLocked actually builds the evalall request — the
// cache-hit / fetch-in-flight / 30s-window-gate paths pay zero getter cost.
// The closure captures s by reference: the supplier runs synchronously only
// within the query call stack (the facade lock is still held) and never
// escapes to the detached fetch thread
AbManager::UserPropsSupplier abUserPropsSupplier(FacadeState& s) {
    return [&s]() -> nlohmann::json { return abUserPropsSnapshot(s); };
}

}  // namespace

// Common shape of the AB queries: AbManager::query guarantees deliver is
// called exactly once (inline on the synchronous path; from a thread on the
// fetch path, including failure degradation and a thread-creation-failure
// fallback), and the facade holds its promise via shared_ptr → the host never
// gets a broken_promise. Impression plans arise only on the synchronous path
// and are re-emitted after unlocking

std::future<bool> checkFeatureGate(const std::string& featureKey) noexcept {
    std::future<bool> out;
    try {
        FacadeState& s = facade();
        std::unique_lock<std::timed_mutex> lock(s.mutex);
        std::vector<ImpressPlan> plans;
        try {
            std::shared_ptr<std::promise<bool>> promise(new std::promise<bool>());
            out = promise->get_future();
            const AbManager::DeliverAnswer deliver = [promise](const AbAnswer& resolved) {
                try {
                    promise->set_value(resolved.gateValue);
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Promise already delivered: idempotent no-op (exactly-once semantics)
                }
            };
            if (s.inited && !s.closed && !s.capturingDisabled && s.enableAB && s.ab) {
                s.ab->query(AbKind::kGate, featureKey, s.builder.anonId(), s.builder.loginId(),
                            abUserPropsSupplier(s), deliver, &plans);
            } else {
                deliver(AbAnswer());  // default false
            }
        } catch (...) {
            Logger::warn("checkFeatureGate failed");  // exactly-once is backstopped by deliver
        }
        lock.unlock();
        emitImpressPlans(s, plans);
    } catch (...) {
        // Fallback for the locking / impression re-emission paths; out stays
        // delivered or default-completed
        Logger::warn("checkFeatureGate failed");
    }
    return out;
}

std::future<std::string> getFeatureConfig(const std::string& configKey) noexcept {
    std::future<std::string> out;
    try {
        FacadeState& s = facade();
        std::unique_lock<std::timed_mutex> lock(s.mutex);
        std::vector<ImpressPlan> plans;
        try {
            std::shared_ptr<std::promise<std::string>> promise(new std::promise<std::string>());
            out = promise->get_future();
            const AbManager::DeliverAnswer deliver = [promise](const AbAnswer& resolved) {
                try {
                    promise->set_value(resolved.text);
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Promise already delivered: idempotent no-op (exactly-once semantics)
                }
            };
            if (s.inited && !s.closed && !s.capturingDisabled && s.enableAB && s.ab) {
                s.ab->query(AbKind::kConfig, configKey, s.builder.anonId(), s.builder.loginId(),
                            abUserPropsSupplier(s), deliver, &plans);
            } else {
                AbAnswer fallback;
                fallback.text = "{}";  // config defaults to an empty object
                deliver(fallback);
            }
        } catch (...) {
            Logger::warn("getFeatureConfig failed");
        }
        lock.unlock();
        emitImpressPlans(s, plans);
    } catch (...) {
        // Fallback for the locking / impression re-emission paths; out stays
        // delivered or default-completed
        Logger::warn("getFeatureConfig failed");
    }
    return out;
}

std::future<std::string> getExperiment(const std::string& expKey) noexcept {
    std::future<std::string> out;
    try {
        FacadeState& s = facade();
        std::unique_lock<std::timed_mutex> lock(s.mutex);
        std::vector<ImpressPlan> plans;
        try {
            std::shared_ptr<std::promise<std::string>> promise(new std::promise<std::string>());
            out = promise->get_future();
            const AbManager::DeliverAnswer deliver = [promise](const AbAnswer& resolved) {
                try {
                    promise->set_value(resolved.text);
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Promise already delivered: idempotent no-op (exactly-once semantics)
                }
            };
            if (s.inited && !s.closed && !s.capturingDisabled && s.enableAB && s.ab) {
                s.ab->query(AbKind::kExperiment, expKey, s.builder.anonId(), s.builder.loginId(),
                            abUserPropsSupplier(s), deliver, &plans);
            } else {
                AbAnswer fallback;
                fallback.text = "{}";  // experiment defaults to an empty object
                deliver(fallback);
            }
        } catch (...) {
            Logger::warn("getExperiment failed");
        }
        lock.unlock();
        emitImpressPlans(s, plans);
    } catch (...) {
        // Fallback for the locking / impression re-emission paths; out stays
        // delivered or default-completed
        Logger::warn("getExperiment failed");
    }
    return out;
}

// ── Compliance ──

void optOutCapturing() noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (s.closed) {
            // Called during / after destroy: the decision is still registered
            // (pre-init flag + observable state), and the Store is still in
            // place inside the destroy window (stage 3 and this call are
            // mutually exclusive on the facade lock) — the compliance decision
            // must be persisted and must not be lost to an exit race
            s.optOut.setPreInit(OptOutManager::PreInitState::kOptOut);
            s.capturingDisabled = true;
            OptOutManager::persistState(true, s.persistOptOut, s.store.get());  // "0"
            return;
        }
        if (!s.inited) {
            s.optOut.setPreInit(OptOutManager::PreInitState::kOptOut);  // kept for init
            return;
        }
        s.capturingDisabled = true;
        if (s.sender) {
            s.sender->setDispatchPaused(true);  // pause batch dispatch, queue untouched
        }
        // Crash capture is uninstalled together with the disable (Opt-Out
        // produces no collection at all, $Exception included): if it stayed
        // installed, a signal crash during Opt-Out would still write
        // sw_crash_pending, which a later enabled init would re-send — i.e.
        // collection while disabled. Uninstalled, crashes fall to the host's
        // default handling, semantically equal to a disabled init
        uninstallCrashHooks();
        OptOutManager::persistState(true, s.persistOptOut, s.store.get());  // "0"
    } catch (...) {
        Logger::warn("optOutCapturing failed");  // silent failure, never throws to the caller
    }
}

void optInCapturing() noexcept {
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (s.closed) {
            // Same as optOutCapturing: the decision is registered and
            // persisted as usual during / after destroy ("1")
            s.optOut.setPreInit(OptOutManager::PreInitState::kOptIn);
            s.capturingDisabled = false;
            OptOutManager::persistState(false, s.persistOptOut, s.store.get());  // "1"
            return;
        }
        if (!s.inited) {
            s.optOut.setPreInit(OptOutManager::PreInitState::kOptIn);
            return;
        }
        s.capturingDisabled = false;
        if (s.sender) {
            s.sender->setDispatchPaused(false);  // resume dispatch
        }
        if (!s.queue) {
            // Idempotent infrastructure rebuild after an Opt-Out-style init
            try {
                buildInfraLocked(s);
            } catch (...) {
                Logger::warn("opt-in infra rebuild failed");
            }
        }
        if (!crashHooksInstalled()) {
            // Reinstall after a runtime optOut uninstalled it, or first
            // install on the rebuild path; do not reinstall when already
            // registered — that would needlessly truncate the pending fd via
            // O_TRUNC and reopen the tiny signal-disposition restore window
            installCrashHooksIfEnabledLocked(s);
        }
        // Persist after the rebuild: a disabled init has no Store, so the
        // state change can only be persisted once rebuilt
        OptOutManager::persistState(false, s.persistOptOut, s.store.get());  // "1"
    } catch (...) {
        Logger::warn("optInCapturing failed");  // silent failure
    }
}

bool hasOptedOutCapturing() noexcept {
    bool out = false;
    try {
        FacadeState& s = facade();
        std::lock_guard<std::timed_mutex> lock(s.mutex);
        if (!s.inited) {
            // Pre-init (including after destroy): combine the pre-init flag
            // with the last known runtime state — after destroy, inited is
            // reset, and looking only at the pre-init flag would misreport
            // "disabled at runtime" as not disabled
            out = s.optOut.preInitOptedOut() || s.capturingDisabled;
        } else {
            out = s.capturingDisabled;
        }
    } catch (...) {
        Logger::warn("hasOptedOutCapturing failed");  // silent failure: defaults to not disabled
    }
    return out;
}

}  // namespace sensorswave
