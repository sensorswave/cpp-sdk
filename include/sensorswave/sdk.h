// Sensors Wave C++ SDK — the single public header.
//
// The install artifact is this header plus the library binary. Design boundaries:
// - Zero third-party includes: standard library only; implementation details such as
//   JSON / networking never leak into the public surface
// - The facade is namespace-level functions over global internal state; the public
//   header exposes no internal types or instance handles
// - Every API is noexcept and fails silently toward the caller; all APIs return
//   synchronously except the three async AB interfaces.
//   Thread safety: all public APIs may be called concurrently from multiple threads.
//   Single initialization: repeated init() calls are silently ignored; after
//   destroy() the semantics equal the uninitialized state — every API goes through
//   guarded no-ops / safe defaults.
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sensorswave {

// ── Property values ──

class PropertyValue;
// Property table: an ordered map so serialization and merging are deterministic.
// Declared before PropertyValue so structured properties ($exception_frames) can
// nest; the map / vector members are only instantiated inside member function bodies
// (complete-class context), which sidesteps incomplete-type instantiation issues
// under C++14
using PropertyMap = std::map<std::string, PropertyValue>;

// Property value: the SDK's own lightweight variant (string / double / int64 / bool /
// vector<string> / vector<map> (structured properties) / dynamic getter). An
// SDK-owned type keeps third-party types out of the public header; a
// default-constructed instance is null (an invalid value, dropped during the merge
// phase).
//
// Dynamic getter: common properties may carry an evaluation function; EventBuilder
// calls it once per event to fetch the latest value. An exception thrown by a getter
// is caught by the merge layer and that property is skipped; the event is still sent.
// Note: getters run synchronously on the event-building path while the SDK's internal
// lock is held — they must not call any SDK API (reentrant deadlock) and should
// return quickly.
class PropertyValue {
   public:
    enum class Type {
        kNull,
        kString,
        kDouble,
        kInt64,
        kBool,
        kStringList,
        kMapList,
        kGetter,
    };

    PropertyValue() : type_(Type::kNull) {}

    static PropertyValue fromString(std::string value) {
        PropertyValue v;
        v.type_ = Type::kString;
        v.stringValue_ = std::move(value);
        return v;
    }

    static PropertyValue fromDouble(double value) {
        PropertyValue v;
        v.type_ = Type::kDouble;
        v.doubleValue_ = value;
        return v;
    }

    static PropertyValue fromInt64(int64_t value) {
        PropertyValue v;
        v.type_ = Type::kInt64;
        v.int64Value_ = value;
        return v;
    }

    static PropertyValue fromBool(bool value) {
        PropertyValue v;
        v.type_ = Type::kBool;
        v.boolValue_ = value;
        return v;
    }

    static PropertyValue fromStringList(std::vector<std::string> value) {
        PropertyValue v;
        v.type_ = Type::kStringList;
        v.listValue_ = std::move(value);
        return v;
    }

    // Structured property ($exception_frames: object frames in an array). The
    // constructor and accessor are defined out of line (complete-type context)
    static PropertyValue fromMapList(std::vector<PropertyMap> value);

    // Constructs a dynamic getter. See the class comment for the constraints: the
    // getter must not call any SDK API and should return quickly
    static PropertyValue fromGetter(std::function<PropertyValue()> getter) {
        PropertyValue v;
        v.type_ = Type::kGetter;
        v.getter_ = std::move(getter);
        return v;
    }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::kNull; }

    // The accessors below are only valid for the matching type() (callers branch on type)
    const std::string& asString() const { return stringValue_; }
    double asDouble() const { return doubleValue_; }
    int64_t asInt64() const { return int64Value_; }
    bool asBool() const { return boolValue_; }
    const std::vector<std::string>& asStringList() const { return listValue_; }
    const std::vector<PropertyMap>& asMapList() const;

    // Evaluates: in getter state, invokes the getter (may throw; the caller catches);
    // in every other state, returns a copy of itself
    PropertyValue resolve() const {
        if (type_ != Type::kGetter) {
            return *this;
        }
        if (getter_) {
            return getter_();
        }
        return PropertyValue();
    }

   private:
    Type type_;
    std::string stringValue_;
    double doubleValue_ = 0.0;
    int64_t int64Value_ = 0;
    bool boolValue_ = false;
    std::vector<std::string> listValue_;
    std::vector<PropertyMap> mapListValue_;
    std::function<PropertyValue()> getter_;
};

inline PropertyValue PropertyValue::fromMapList(std::vector<PropertyMap> value) {
    PropertyValue v;
    v.type_ = Type::kMapList;
    v.mapListValue_ = std::move(value);
    return v;
}

inline const std::vector<PropertyMap>& PropertyValue::asMapList() const {
    return mapListValue_;
}

// ── HTTP injection interface (host-provided; SDK-owned abstraction, not third-party) ──

// A single outbound HTTP request (value object)
struct HttpRequest {
    std::string url;
    std::string method = "POST";  // outbound tracking always uses POST
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // Per-request timeout in milliseconds; the SDK's sender always passes 10000, but
    // implementations may handle the value according to their own policy
    int timeoutMs = 10000;
};

// HTTP response (value object). statusCode == 0 means a transport-level failure
// (network error / timeout / DNS); in that case error is non-empty and body is empty.
// HTTP-level failures (4xx/5xx) do not count as transport failures.
struct HttpResponse {
    long statusCode = 0;
    std::string body;
    std::string error;

    bool ok() const { return error.empty() && statusCode >= 200 && statusCode < 300; }
};

// Injectable HTTP abstraction: the host supplies the implementation, tests substitute
// a mock, and no real network is ever touched. Implementations must be thread-safe on
// their own: the SDK only calls this from its background sender thread.
class IHttpClient {
   public:
    virtual ~IHttpClient() = default;

    // Executes the request synchronously. Any failure is expressed through the return
    // value; never throws.
    // Implementation constraints: must not block for long (the SDK performs its drain
    // wait and thread join outside the facade lock, so re-entering other SDK APIs
    // cannot deadlock — during destroy/terminate they return as guarded no-ops in the
    // final state). Implementations should still stay one-directional: calling back
    // into the SDK inside execute markedly prolongs draining and widens the failure
    // surface.
    virtual HttpResponse execute(const HttpRequest& request) = 0;
};

// ── Initialization configuration ──

// Fine-grained exception-capture settings (in_app classification)
struct ExceptionConfig {
    std::vector<std::string> inAppIncludes;  // keywords for the in_app include check
    std::vector<std::string> inAppExcludes;  // exclude keywords (take precedence over includes)
};

// Advanced event entry point: the host supplies the event name and properties;
// mandatory fields such as time / trace_id / anon_id / login_id are still filled in
// by the SDK — the host cannot forge identity fields
struct SendEvent {
    std::string eventName;
    PropertyMap properties;
};

// SDK initialization configuration. Anything not listed here (batching parameters,
// queue capacity, timeouts, ...) is an internal constant and not exposed.
struct SensorsWaveConfig {
    // ── Required ──
    std::string sourceToken;  // unique project ID; sent as the SourceToken request header
    std::string apiHost;      // data-receiving server URL
    // ── Optional ──
    bool debug = false;             // debug logging switch
    bool batchSend = false;         // batched sending; false = trigger a send after every enqueue
    bool enableAB = false;          // master switch for AB queries (off by default)
    bool enableCrashTrack = false;  // automatic crash capture (off by default)
    bool autoCapture = true;        // auto-capture switch (v1: $AppInstall; on by default)
    int64_t abRefreshInterval = 600000;  // AB data refresh interval ms (clamped to ≥ 30000)
    bool optOutCapturing = false;        // initialize in the opted-out state
    bool persistOptOut = false;          // persist the opt-out decision across runs
    ExceptionConfig exceptionConfig;     // in_app classification settings for exceptions
    // ── C++ platform-specific ──
    std::string dataDir;                      // persistence dir; empty = platform default path
    std::shared_ptr<IHttpClient> httpClient;  // injected HTTP client; empty = built-in curl sender
};

// ── Public API ──

// Initializes the SDK; takes effect once, repeated calls are silently ignored.
// Never throws: any failing step follows its own degradation path (in-memory mode /
// deferred initialization).
void init(const std::string& sourceToken, const SensorsWaveConfig& config) noexcept;

// Destroys the SDK and drains before exit: idempotent. First drains synchronously
// (blocking bounded by the internal constant DESTROY_DRAIN_TIMEOUT_MS; no new batch
// is started past the deadline) → stops the sender thread → performs the final
// persist → releases infrastructure.
// The real blocking upper bound ≈ DESTROY_DRAIN_TIMEOUT_MS + one in-flight HTTP
// request (≤ HTTP_TIMEOUT_MS): a batch already on the wire at the deadline must be
// waited out and cannot be interrupted. If the timeout is hit, events remain queued
// (already persisted) — this is not a "send succeeded" guarantee. The destructor
// performs no blocking work.
void destroy() noexcept;

// ── Events (fire-and-forget) ──

void trackEvent(const std::string& eventName,
                const PropertyMap& properties = PropertyMap()) noexcept;
void track(const SendEvent& eventData) noexcept;  // advanced entry; the host builds the event
void trackException(const std::exception& e,
                    const PropertyMap& properties = PropertyMap()) noexcept;
void trackException(const std::string& type, const std::string& message,
                    const PropertyMap& properties = PropertyMap()) noexcept;

// ── Identity ──

void identify(const std::string& loginId) noexcept;    // persists + sends the $Identify event
void setLoginId(const std::string& loginId) noexcept;  // persists only, sends no event
void reset(bool resetAnonId = false) noexcept;         // logout; reset(true) also rotates anon ID
std::string getAnonId() noexcept;                      // returns "" when opted out / uninitialized
std::string getLoginId() noexcept;
std::string getSessionId() noexcept;

// ── User properties ($UserSet carriers) ──

void profileSet(const PropertyMap& props) noexcept;
void profileSetOnce(const PropertyMap& props) noexcept;
void profileIncrement(const PropertyMap& props) noexcept;  // values must be numeric
void profileAppend(const PropertyMap& props) noexcept;
void profileUnion(const PropertyMap& props) noexcept;
void profileUnset(const std::vector<std::string>& keys) noexcept;
void profileDelete() noexcept;  // needs login_id or anon_id; silently dropped if both are missing

// ── Common properties ──

void registerCommonProperties(const PropertyMap& props) noexcept;  // values may be dynamic getters
void clearCommonProperties() noexcept;
void clearCommonProperties(const std::vector<std::string>& keys) noexcept;

// ── AB (the only three async APIs: std::future + detached threads) ──

std::future<bool> checkFeatureGate(const std::string& featureKey) noexcept;
std::future<std::string> getFeatureConfig(const std::string& configKey) noexcept;  // value as text
std::future<std::string> getExperiment(const std::string& expKey) noexcept;  // value as JSON text

// ── Compliance ──

void optOutCapturing() noexcept;       // safe to call pre-init (state survives the first init)
void optInCapturing() noexcept;        // idempotent after opted-out init; rebuilds infrastructure
bool hasOptedOutCapturing() noexcept;  // also safe to call before init

}  // namespace sensorswave
