# SDK static library: builds the static lib + public header; no shared library for now
set(SW_SOURCES
  util/clock.cpp
  util/fs_util.cpp
  util/logger.cpp
  util/random.cpp
  util/time_format.cpp
  util/uuid.cpp
  storage/file_store.cpp
  storage/memory_store.cpp
  consent/opt_out_manager.cpp
  ab/ab_request.cpp
  ab/ab_manager.cpp
  crash/crash_payload.cpp
  exception/exception_collector.cpp
  identity/identity_manager.cpp
  event/event_builder.cpp
  event/event_json.cpp
  queue/event_queue.cpp
  queue/persistent_queue.cpp
  sender/batch_sender.cpp
  sender/track_request.cpp
  session/session_manager.cpp
  track/track_api.cpp
  props/common_properties.cpp
  profile/profile_api.cpp
  sensors_wave_client.cpp
)

# Platform system-info implementation: one of the two per target platform
if(WIN32)
  list(APPEND SW_SOURCES platform/system_info_windows.cpp)
else()
  list(APPEND SW_SOURCES platform/system_info_posix.cpp)
endif()

# Stack-trace capture implementation: one of the two per target platform
if(WIN32)
  list(APPEND SW_SOURCES exception/stack_trace_windows.cpp)
else()
  list(APPEND SW_SOURCES exception/stack_trace_posix.cpp)
endif()

# Crash handler implementation: one of the two per target platform
if(WIN32)
  list(APPEND SW_SOURCES crash/crash_handler_windows.cpp)
else()
  list(APPEND SW_SOURCES crash/crash_handler_posix.cpp)
endif()

if(SW_WITH_CURL)
  list(APPEND SW_SOURCES http/curl_http_client.cpp)
endif()

add_library(sensorswave STATIC ${SW_SOURCES})

target_include_directories(sensorswave
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

# JSON serialization: used internally by src/ only, never leaked to consumers
target_link_libraries(sensorswave PRIVATE nlohmann_json::nlohmann_json)

# Optional libcurl HTTP client; when disabled, the host must inject an IHttpClient implementation
if(SW_WITH_CURL)
  find_package(CURL REQUIRED)
  target_compile_definitions(sensorswave PRIVATE SW_WITH_CURL=1)
  target_link_libraries(sensorswave PRIVATE CURL::libcurl)
endif()

# The static lib may be linked into a host shared library, so keep it position independent
set_target_properties(sensorswave PROPERTIES POSITION_INDEPENDENT_CODE ON)

# System framework / system-library dependencies for platform system info.
# App environment properties: macOS additionally links IOKit (IOPlatformUUID) and
# SystemConfiguration (SCNetworkInterface, interface type); Windows additionally links
# iphlpapi (GetAdaptersAddresses) and bcrypt (BCryptGenRandom, used by SecureRandom)
if(APPLE)
  target_link_libraries(sensorswave PRIVATE "-framework CoreFoundation" "-framework CoreGraphics"
                                            "-framework IOKit" "-framework SystemConfiguration")
elseif(WIN32)
  target_link_libraries(sensorswave PRIVATE user32 gdi32 version advapi32 iphlpapi bcrypt)
endif()

sw_enable_strict_warnings(sensorswave)

# Public header install: artifacts are only include/sensorswave/sdk.h + the library file.
# Headers under src/ are internal implementation and are not installed (public-header hygiene)
install(FILES ${CMAKE_SOURCE_DIR}/include/sensorswave/sdk.h
  DESTINATION include/sensorswave)
