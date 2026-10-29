# Third-party dependencies (brought in via FetchContent or vcpkg; this build uses
# FetchContent throughout)
include(FetchContent)

# nlohmann/json: internal JSON serialization, never exposed through the public
# surface; it only requires C++11, so it does not raise the baseline. Use the official
# release archive (~1.8MB) rather than the git repository (which carries a large
# test-data tree and clones slowly)
if(SW_USE_SYSTEM_JSON)
  find_package(nlohmann_json 3.11 REQUIRED)
else()
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# GoogleTest + GoogleMock (test stack; v1.14.0 requires C++14, matching the baseline)
if(SW_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
endif()

# Dear ImGui + GLFW (demo desktop GUI only, SW_DEMO_GUI): neither raises the C++
# baseline (imgui v1.90.9 requires C++11, GLFW 3.4 is C99). The imgui repository has
# no top-level CMakeLists.txt, so Populate is used purely to fetch the sources and
# demo/CMakeLists.txt assembles the compile list itself; third-party sources must not
# inherit the repo's -Werror (demo/CMakeLists.txt builds them as a separate static
# library and includes it as SYSTEM). imgui stays on 1.90.9 for now: the dynamic font
# system rewrite in 1.92.x brings no benefit to this demo
if(POLICY CMP0169)
  # imgui goes through Populate rather than MakeAvailable; newer CMake
  # deprecation-warns on that pattern, so keep the old behavior
  cmake_policy(SET CMP0169 OLD)
endif()
if(SW_BUILD_DEMO AND SW_DEMO_GUI)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.9
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()
endif()
