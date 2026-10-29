#include "util/time_format.h"

#include <cstdio>
#include <ctime>

namespace sensorswave {

std::string formatIso8601UtcMs(int64_t epochMs) {
    // Negative values (pre-1970, defensive): floor division keeps the ms part
    // non-negative and corrects the seconds accordingly
    int64_t seconds = epochMs / 1000;
    int ms = static_cast<int>(epochMs % 1000);
    if (ms < 0) {
        ms += 1000;
        seconds -= 1;
    }
    const std::time_t timeVal = static_cast<std::time_t>(seconds);
    std::tm tmUtc = {};
#ifdef _WIN32
    gmtime_s(&tmUtc, &timeVal);
#else
    gmtime_r(&timeVal, &tmUtc);
#endif
    char buffer[32];  // the format is a fixed 24 characters + '\0'
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday, tmUtc.tm_hour,
                  tmUtc.tm_min, tmUtc.tm_sec, ms);
    return std::string(buffer);
}

}  // namespace sensorswave
