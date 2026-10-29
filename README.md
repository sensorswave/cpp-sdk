English | [简体中文](README.zh-CN.md)

# Sensors Wave C++ SDK

![C++14](https://img.shields.io/badge/C%2B%2B-14-00599C) ![CMake](https://img.shields.io/badge/CMake-%E2%89%A5%203.16-064F8C) ![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey) ![License](https://img.shields.io/badge/license-Apache--2.0-blue)

Sensors Wave C++ SDK is an analytics / event-tracking library for C++ applications (desktop clients, embedded hosts, etc.). It provides event collection, user identity management, user profiles, A/B testing, and exception & crash reporting. It ships as a **static library + a single header** that compiles alongside your project, with no mandatory third-party dependencies (the built-in libcurl sender can be disabled and replaced with a host-provided implementation; implementation details such as JSON never leak into the public header).

If you are new to Sensors Wave, visit [sensorswave.com](http://sensorswave.com/) to learn about the product and create an account. Current release: 1.0.0 — see [CHANGELOG.md](CHANGELOG.md).

## Features

- **Event tracking**: synchronous fire-and-forget `trackEvent`; events are persisted locally before being sent, retried with backoff on network failure, and re-sent after a crash on next launch
- **Exception & crash reporting**: manual `trackException`; with `enableCrashTrack` enabled, uncaught C++ exceptions, fatal signals (macOS / Linux), and unhandled Windows SEH exceptions are captured automatically
- **User identity**: anonymous ID + login ID attached to every event, with `identify` / `setLoginId` / `reset`
- **User profiles & common properties**: seven profile operations; global common properties with dynamic getters, merged with preset < common < event priority
- **A/B testing**: `checkFeatureGate` / `getFeatureConfig` / `getExperiment`, with fastFetch and local-cache semantics aligned with the JS SDK
- **Privacy compliance**: Opt-Out / Opt-In switchable at runtime, with optional cross-process persistence
- **Clean integration surface**: a single public header (`sdk.h`); every API is `noexcept` and fails silently, safe to call from multiple threads; calls in an uninitialized, invalid-argument, or opted-out state are safe no-ops

## Requirements

| Item | Requirement |
| --- | --- |
| Platforms | Windows / macOS / Linux |
| Compilers | GCC ≥ 5 / Clang ≥ 3.4 / MSVC 2017 15.7+ (C++14 baseline; nothing higher required) |
| Build | CMake ≥ 3.16 |
| Networking | libcurl development package (needed by the built-in sender; not needed with `SW_WITH_CURL=OFF` or an injected `httpClient`) |

## SDK Usage

The SDK is distributed as **source code**, hosted at <https://github.com/sensorswave/cpp-sdk> (pre-release development happens on an internal GitLab). It compiles together with your project and links statically — no prebuilt binaries, no runtime components. Your toolchain must support C++14.

### 1. Add the SDK

**Option A: FetchContent (recommended)** — the source is fetched and built together with your project at CMake configure time:

```cmake
include(FetchContent)
FetchContent_Declare(sensorswave
  GIT_REPOSITORY https://github.com/sensorswave/cpp-sdk.git
  GIT_TAG        v1.0.0)   # use the latest release tag
FetchContent_MakeAvailable(sensorswave)
```

**Option B: git clone / submodule + add_subdirectory**:

```bash
git clone https://github.com/sensorswave/cpp-sdk.git third_party/cpp-sdk
# or: git submodule add https://github.com/sensorswave/cpp-sdk.git third_party/cpp-sdk
```

```cmake
add_subdirectory(third_party/cpp-sdk)
```

**Option C: build and install locally, then link manually**:

```bash
git clone https://github.com/sensorswave/cpp-sdk.git
cd cpp-sdk
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix /opt/sensorswave
```

The install output is `include/sensorswave/sdk.h` (the only public header) and `lib/libsensorswave.a` (`lib/sensorswave.lib` on Windows).

### 2. Link

Options A / B need one line only (the static library's system dependencies are propagated to your target automatically):

```cmake
target_link_libraries(your_app PRIVATE sensorswave)
```

For option C, link the platform system dependencies yourself:

| Platform | Link against |
| --- | --- |
| macOS | `-lcurl -framework CoreFoundation -framework CoreGraphics -framework IOKit -framework SystemConfiguration` |
| Linux | `-lcurl -lpthread` |
| Windows | `user32 gdi32 version advapi32 iphlpapi bcrypt` + the libcurl import library |

> If your project implements its own networking (see `SensorsWaveConfig::httpClient`), you can build a libcurl-free library with `-DSW_WITH_CURL=OFF`. With `SW_WITH_CURL=OFF` and no `httpClient` injected, events are still queued and persisted and kept for retry (the sender degrades as if the network had failed) — nothing crashes.

### 3. Initialize

```cpp
#include <sensorswave/sdk.h>

sensorswave::SensorsWaveConfig config;
config.apiHost = "https://your-api-host.com";
config.dataDir = "/var/lib/your-app/sw";   // local persistence dir; leave empty for the platform default

sensorswave::init("your-source-token", config);
```

Call once per process; repeated calls are silently ignored.

### 4. Track events

```cpp
sensorswave::PropertyMap props;
props["order_id"] = sensorswave::PropertyValue::fromString("o-1001");
props["amount"]   = sensorswave::PropertyValue::fromDouble(99.5);
sensorswave::trackEvent("OrderPaid", props);
```

Events go into a local persistent queue first and are uploaded by a background thread: with `batchSend=true`, batches close at 10 events or every 5 s; with `false` (default), each event triggers a send as soon as it is enqueued. Network failures are retried once with exponential backoff (5 s base, 30 s cap), per-request timeout is 10 s; events that never made it are re-sent on next launch after a crash.

### 5. Destroy before exit

```cpp
sensorswave::destroy();
```

`destroy()` synchronously drains events that have not been uploaded yet (blocking up to 10 seconds; no new batches are started after the deadline) and releases all resources. Afterwards every API enters a safe no-op state. **Always call it before process exit.** It is idempotent and safe to call multiple times. In crash scenarios you do not need to call it — the SDK persists to disk automatically and re-sends on next launch.

## Configuration Options

Fields of `SensorsWaveConfig`:

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `sourceToken` | string | — (required) | Project token, sent via the `SourceToken` request header |
| `apiHost` | string | — (required) | Data ingestion server URL |
| `dataDir` | string | platform default | Local persistence directory (event queue, identity, AB cache, …); see Behavior Notes for defaults |
| `debug` | bool | `false` | Print debug logs (diagnostics are only written to stderr in this mode) |
| `batchSend` | bool | `false` | Batch sending; when `false`, each event triggers a send as soon as it is enqueued |
| `autoCapture` | bool | `true` | Auto-tracking master switch; in v1 this covers only the first-launch `$AppInstall` event |
| `enableAB` | bool | `false` | Enable A/B testing; when off, the three query APIs return defaults immediately |
| `abRefreshInterval` | int64_t | `600000` | AB data refresh interval (ms), minimum 30000; lower values are clamped |
| `enableCrashTrack` | bool | `false` | Enable automatic crash capture (see Error Tracking) |
| `optOutCapturing` | bool | `false` | Initialize in the opted-out state (no sender or persistence infrastructure is created) |
| `persistOptOut` | bool | `false` | Persist the opt-out decision across processes |
| `exceptionConfig` | `ExceptionConfig` | — | `in_app` attribution for exception frames: `inAppIncludes` / `inAppExcludes` (excludes win) |
| `httpClient` | `std::shared_ptr<IHttpClient>` | built-in libcurl | Custom HTTP implementation; see the `IHttpClient` interface in `sdk.h` |

Batch sizes, queue capacity, and timeouts are managed internally by the SDK and are not exposed as configuration.

## API Methods

Except for the three A/B query APIs that return `std::future`, every API returns synchronously and never throws (`noexcept`): calls with invalid arguments, before initialization, or while opted out are all safe no-ops. All public APIs are safe to call from multiple threads concurrently.

### Events

#### trackEvent

```cpp
void trackEvent(const std::string& eventName, const PropertyMap& properties = {});
void track(const SendEvent& eventData);   // advanced entry: event name + properties in one object
```

```cpp
sensorswave::trackEvent("ButtonClick", {{"button_name", sensorswave::PropertyValue::fromString("submit")}});
```

`PropertyValue` supports string / double / int64 / bool / string list / structured object list / dynamic getter (`fromString` / `fromDouble` / `fromInt64` / `fromBool` / `fromStringList` / `fromMapList` / `fromGetter`). Dynamic getters are re-evaluated for every event; if a getter throws, that property is skipped and the event is still sent. Getters must not call back into the SDK and should return quickly.

#### trackException

```cpp
void trackException(const std::exception& e, const PropertyMap& properties = {});
void trackException(const std::string& type, const std::string& message, const PropertyMap& properties = {});
```

Manually report an exception. Always recorded at `error` level and not subject to the `enableCrashTrack` switch:

```cpp
try {
    riskyOperation();
} catch (const std::exception& e) {
    sensorswave::trackException(e);
}
```

### Identity

Every event carries an anonymous ID (`anon_id`); once logged in, the login ID (`login_id`) is attached as well.

#### identify

```cpp
void identify(const std::string& loginId);
```

Bind the login ID: persists it and sends an `$Identify` event linking the anonymous identity with the login identity.

#### setLoginId

```cpp
void setLoginId(const std::string& loginId);
```

Persist the login ID without sending any event.

#### reset

```cpp
void reset(bool resetAnonId = false);
```

Log out: clears the login ID and starts a new session immediately; `reset(true)` also rotates the anonymous ID.

#### Identity queries

```cpp
std::string getAnonId();      // anonymous ID; "" when uninitialized / opted out
std::string getLoginId();     // login ID; "" when not logged in
std::string getSessionId();   // current session ID
```

### User profiles

| Method | Semantics |
| --- | --- |
| `profileSet(props)` | Overwrite |
| `profileSetOnce(props)` | Only the first write takes effect |
| `profileIncrement(props)` | Numeric increment (non-numeric values are skipped) |
| `profileAppend(props)` | Append to a list (duplicates allowed) |
| `profileUnion(props)` | Merge into a list (deduplicated) |
| `profileUnset(keys)` | Delete the given properties |
| `profileDelete()` | Clear all properties of the current user |

Profile operations are carried by the preset `$UserSet` event (the operation type is one of `$set` / `$set_once` / `$increment` / `$append` / `$union` / `$unset` / `$delete` in the event properties).

```cpp
sensorswave::PropertyMap profile;
profile["age"] = sensorswave::PropertyValue::fromInt64(25);
sensorswave::profileSet(profile);
```

### Common properties

```cpp
void registerCommonProperties(const PropertyMap& props);
void clearCommonProperties();
void clearCommonProperties(const std::vector<std::string>& keys);
```

Registered properties are attached to every event; values may be dynamic getters, re-evaluated per event. Merge priority: event properties > common properties > preset properties.

```cpp
sensorswave::PropertyMap common;
common["app_version"] = sensorswave::PropertyValue::fromString("1.2.0");
sensorswave::registerCommonProperties(common);
```

### A/B testing

Only these three APIs are asynchronous (`std::future`); everything else is synchronous:

```cpp
std::future<bool> checkFeatureGate(const std::string& featureKey);
std::future<std::string> getFeatureConfig(const std::string& configKey);
std::future<std::string> getExperiment(const std::string& expKey);
```

```cpp
if (sensorswave::checkFeatureGate("new-checkout").get()) {
    // feature gate hit
}
const std::string config = sensorswave::getFeatureConfig("search-config").get();
```

Return-value conventions:

- `checkFeatureGate`: `true` when hit and not judged as `fail`; `false` when AB is disabled, not hit, or the network failed
- `getFeatureConfig` / `getExperiment`: the value is returned as JSON text (objects / arrays as compact JSON you can parse again); `"{}"` when no value is available. This matches the JS SDK semantics of returning the parsed value — the C++ public header does not introduce a JSON type and carries the same information as text; string values that fail to parse are returned as-is
- On a hit where the server allows impression tracking, `$FeatureImpress` / `$ExpImpress` events are reported automatically — no manual tracking needed

### Privacy compliance

```cpp
void optOutCapturing();             // stop collecting and sending (safe before init too)
void optInCapturing();              // resume (idempotent)
bool hasOptedOutCapturing();        // query current state
```

Once opted out, collection stops immediately; already-queued events stay in the local queue and are sent after opting back in. Combined with the `persistOptOut` / `optOutCapturing` config options, the user's choice can survive process restarts.

### Destroy

```cpp
void destroy();
```

See "SDK Usage · 5. Destroy before exit". In crash scenarios (fatal signals / uncaught exceptions) you do not need to call it — the SDK persists automatically and re-sends on next launch.

## Error Tracking

### Manual reporting

See `trackException`. The event carries `$exception_level` (error), `$exception_type`, `$exception_message`, and `$exception_frames` (up to 30 frames, each with `platform` (always `"native"`) / `instruction_addr` / `in_app`).

### Automatic crash capture

```cpp
config.enableCrashTrack = true;
config.exceptionConfig.inAppExcludes = {"/usr/lib", "third-party/"};
```

When enabled, the SDK installs process-level hooks that catch uncaught C++ exceptions (`std::terminate`), fatal signals (macOS / Linux: SIGSEGV, SIGABRT, etc.), and unhandled exceptions (Windows SEH). Crash information is persisted as a `fatal`-level `$Exception` event and re-sent automatically on next launch — no extra code required.

`in_app` attribution: a frame address matching an `inAppExcludes` keyword is marked as not belonging to your app (excludes take precedence); addresses not excluded but matching `inAppIncludes` are marked as in-app.

## Supported Event Types

Events generated automatically by the SDK:

| Event | Trigger |
| --- | --- |
| `$AppInstall` | Reported automatically on first launch (`autoCapture` on by default), carries `$install_time` |
| `$Exception` | Manual `trackException` (error level); automatic crash capture (fatal level) |
| `$Identify` | `identify` binds the login identity |
| `$UserSet` | The seven profile operations (operation type distinguishes them) |
| `$FeatureImpress` / `$ExpImpress` | Feature flag / experiment hit where impressions are allowed |

Preset properties attached to every event: `$lib` (always `"cpp"`), `$lib_version`, `$os` / `$os_version`, `$language`, `$model`, `$screen_width` / `$screen_height`, `$timezone_offset`, `$network_type` / `$wifi`, `$session_id`, `$device_id`, `$region`, `$brand` / `$manufacturer`, plus app info `$app_id` / `$app_name` / `$app_version` (best effort, omitted when unavailable).

Do not use the `$` prefix for custom events or properties (it is reserved for preset naming; such events are sent as-is, with a warning logged only in debug mode).

## Behavior Notes

- Required fields on every event: `event` / `time` (ms timestamp) / `trace_id` (UUID) / `anon_id`; plus `login_id` once logged in
- Persist before send: local queue capacity is 200 events with 7-day retention; the oldest events are evicted when exceeded
- Sessions (`$session_id`): automatically rotated after 30 minutes idle or 12 hours total duration
- Default persistence directory (when `dataDir` is empty): Windows `%APPDATA%\SensorsWave`; macOS `~/Library/Application Support/SensorsWave`; Linux `$XDG_DATA_HOME/sensorswave` (falling back to `~/.local/share/sensorswave`). Server-style hosts (daemons / containers where environment variables may be stripped, degrading to a CWD-relative path) should set `dataDir` explicitly
- **Single-instance assumption**: one `dataDir` (including the default) belongs to one SDK instance; multiple processes on one machine or multiple instances in one process sharing a directory will clobber local state (last-writer-wins) — isolate with distinct `dataDir` values
- The SDK never throws into your code and never crashes due to being uninitialized; enable `debug: true` to see diagnostic logs when troubleshooting
- **fork restriction**: do not call `init()` in a forked child process (on macOS this crashes because system libraries are not fork-safe); exec a new program in the child before initializing
- **Sensitive data**: you are responsible for event property contents. Mask phone numbers, ID numbers, emails, etc. before reporting — the SDK performs no detection or filtering

## Building from Source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Available CMake options:

| Option | Default | Description |
| --- | --- | --- |
| `SW_WITH_CURL` | `ON` | Built-in libcurl HTTP client; when off, an `IHttpClient` implementation must be injected |
| `SW_USE_SYSTEM_JSON` | `OFF` | Use system nlohmann/json instead of the FetchContent-bundled version |
| `SW_SANITIZE` | `""` | `asan` / `tsan` sanitizer build variants (GCC / Clang only) |

## License

[Apache License 2.0](LICENSE)
