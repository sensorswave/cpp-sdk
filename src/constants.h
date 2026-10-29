#pragma once

#include <cstddef>
#include <cstdint>

namespace sensorswave {

// Centralizes internal constants; none of them are exposed as configuration.
// Names use UPPER_SNAKE_CASE; constexpr namespace-scope constants have internal
// linkage, so there are no ODR issues.

// ── Library identity (if these ever change, adjust them only here) ──
constexpr const char* LIB_NAME = "cpp";  // $lib
// $lib_version; kept in sync with the project VERSION in CMakeLists.txt
// (scripts/release.sh bumps both together).
constexpr const char* LIB_VERSION = "1.0.0";
constexpr const char* EXCEPTION_PLATFORM = "native";  // $exception_frames.platform

// ── Storage keys (sw_ prefix; one file per key) ──
constexpr const char* SW_KEY_ANON_ID = "sw_anon_id";
constexpr const char* SW_KEY_LOGIN_ID = "sw_login_id";
constexpr const char* SW_KEY_OPT_OUT = "sw_opt_out";
constexpr const char* SW_KEY_SESSION = "sw_session";
constexpr const char* SW_KEY_AB_DATA = "sw_ab_data";
constexpr const char* SW_KEY_QUEUE = "sw_queue";
constexpr const char* SW_KEY_CRASH_PENDING = "sw_crash_pending";
constexpr const char* SW_KEY_LAUNCHED =
    "sw_launched";  // first-launch marker (criterion for $AppInstall)

// ── Batched sending (internal constants, not exposed) ──
constexpr std::size_t BATCH_MAX_SIZE = 10;  // sending fires once the queue holds ≥ 10 events
constexpr int64_t SEND_INTERVAL_MS = 5000;  // fixed timer interval

// ── Queue ──
constexpr std::size_t QUEUE_MAX_SIZE = 200;  // queue capacity cap; the oldest events are evicted
constexpr int EVENT_EXPIRY_DAYS = 7;         // event lifetime, purged on load and rewrite

// ── Retry and backoff (backoff = baseDelay × 2^retryCount, 30s cap; baseDelay = send interval) ──
constexpr int RETRY_MAX = 1;               // max retries for network-class failures
constexpr int64_t BACKOFF_BASE_MS = 5000;  // backoff base delay
constexpr int64_t BACKOFF_CAP_MS = 30000;  // backoff upper bound

// ── Timeouts (C++-specific choices, not exposed) ──
constexpr int HTTP_TIMEOUT_MS = 10000;                // per-request timeout, passed to each execute
constexpr int64_t DESTROY_DRAIN_TIMEOUT_MS = 10000;   // blocking cap for the final destroy drain
constexpr int64_t TERMINATE_DRAIN_TIMEOUT_MS = 2000;  // short-timeout drain on the terminate path

// ── AB (minimum refresh interval; also the lower-bound clamp for config.abRefreshInterval) ──
constexpr int64_t AB_MIN_REFRESH_MS =
    30000;  // values < 30000 (including ≤ 0) are clamped to 30000 (matches the JS SDK's Math.max)
constexpr int64_t AB_DEFAULT_REFRESH_MS =
    600000;  // FacadeState default (pre-clamp; invalid values are clamped, no default fallback)

// ── Session (internal constants, not configurable) ──
constexpr int64_t SESSION_IDLE_MS = 30LL * 60 * 1000;      // idle-rotation threshold: 30 min
constexpr int64_t SESSION_MAX_MS = 12LL * 60 * 60 * 1000;  // maximum session duration: 12 h

// ── Opt-Out persisted values (note: "0" = disabled (declined), "1" = allowed (consented)) ──
constexpr const char* OPT_OUT_VALUE_DISABLED = "0";
constexpr const char* OPT_OUT_VALUE_ALLOWED = "1";

// ── Exception capture ──
constexpr int FRAME_LIMIT = 30;  // maximum number of $exception_frames entries

}  // namespace sensorswave
