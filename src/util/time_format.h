#pragma once

#include <cstdint>
#include <string>

namespace sensorswave {

// epoch milliseconds → fixed-width ISO 8601 UTC form (with milliseconds, e.g.
// "2024-08-04T09:31:44.037Z").
// Dedicated to DATETIME-typed preset properties ($install_time of $AppInstall)
std::string formatIso8601UtcMs(int64_t epochMs);

}  // namespace sensorswave
