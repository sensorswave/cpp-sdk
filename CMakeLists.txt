cmake_minimum_required(VERSION 3.16)

project(sensorswave
  VERSION 1.0.0
  DESCRIPTION "Sensors Wave C++ SDK - analytics / event tracking"
  LANGUAGES CXX)

# Language baseline: C++14 (backward compatibility first)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fallback when a single-config generator is used without an explicit build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(SW_WITH_CURL "Enable the built-in libcurl HTTP client" ON)
option(SW_BUILD_TESTS "Build unit tests (GoogleTest)" ON)
option(SW_BUILD_DEMO "Build the interactive verification demo (demo/)" ON)
# The published tree ships without tests / demo directories, so when the project is
# consumed via FetchContent / add_subdirectory those directories are absent and the
# options are forced off here — otherwise deps.cmake would still pull GoogleTest /
# GLFW / ImGui, and add_subdirectory would fail to configure over the missing dirs
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  set(SW_BUILD_TESTS OFF)
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/demo/CMakeLists.txt")
  set(SW_BUILD_DEMO OFF)
endif()
option(SW_DEMO_GUI "Build the ImGui desktop UI for the demo (OFF keeps only the terminal CLI mode)" ON)
option(SW_USE_SYSTEM_JSON "Use the system nlohmann/json instead of the FetchContent copy" OFF)
option(SW_DEMO_REGRESSION
  "Demo GUI automation hooks (command injection / framebuffer screenshots / timeline dump; local automated verification only)" OFF)

# Concurrency/memory-check build variants: -DSW_SANITIZE=tsan|asan (empty = off).
# Clang / GCC only (flag spelling normalized per compiler: GCC asan/tsan,
# Clang address/thread)
set(SW_SANITIZE "" CACHE STRING "sanitizer variant: tsan or asan")
if(SW_SANITIZE)
  if(NOT MSVC)
    if(NOT SW_SANITIZE STREQUAL "tsan" AND NOT SW_SANITIZE STREQUAL "asan")
      message(FATAL_ERROR "SW_SANITIZE only supports tsan / asan")
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
      set(SW_SAN_FLAG_${SW_SANITIZE} ${SW_SANITIZE})
    else()
      set(SW_SAN_FLAG_tsan thread)
      set(SW_SAN_FLAG_asan address)
    endif()
    add_compile_options(-fsanitize=${SW_SAN_FLAG_${SW_SANITIZE}} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${SW_SAN_FLAG_${SW_SANITIZE}})
  else()
    message(STATUS "SW_SANITIZE is not supported under MSVC; ignored")
  endif()
endif()

# Warnings-as-errors applies only to the SDK's own targets (GCC/Clang -Wall -Wextra
# -Werror, MSVC /W4 /WX); third-party targets pulled in via FetchContent are unaffected.
# MSVC additionally gets /utf-8: sources are UTF-8 (no BOM), while Chinese-language
# Windows defaults to ACP=936 (GBK) — decoding UTF-8 sources as GBK raises C4819,
# a hard error under /W4 /WX.
function(sw_enable_strict_warnings target)
  if(MSVC)
    # /Zc:preprocessor: the traditional preprocessor mis-stringifies raw string
    # literals passed as macro arguments (C2017/C3688 — feeding R"(...)" straight into
    # gtest EXPECT/ASSERT triggers it); the conforming preprocessor needs VS2019 16.5+,
    # while older compilers merely warn on the unknown option
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /Zc:preprocessor)
    # The SDK deliberately uses CRT APIs such as fopen/getenv/_open to stay portable
    # instead of adopting the MSVC-only _s family; silence the C4996 deprecation
    # warnings so /WX does not misfire on them
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

include(cmake/deps.cmake)

add_subdirectory(src)

if(SW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SW_BUILD_DEMO)
  add_subdirectory(demo)
endif()

# Install rules: library files (the public-header install lives in src/CMakeLists.txt)
install(TARGETS sensorswave
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib)
