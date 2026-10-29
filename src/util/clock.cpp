#include "util/clock.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#endif

namespace sensorswave {

int64_t SystemClock::nowMs() const {
#ifdef _WIN32
    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER ticks;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    // FILETIME counts 100ns ticks since 1601-01-01; convert to Unix epoch
    // milliseconds
    return static_cast<int64_t>(ticks.QuadPart / 10000) - 11644473600000LL;
#else
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (static_cast<int64_t>(now.tv_sec) * 1000) + (now.tv_nsec / 1000000);
#endif
}

}  // namespace sensorswave
