#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "util/random.h"

namespace sensorswave {

// UUID generator: v4 (trace_id / anon_id, RFC 4122) and v7 (session, RFC 9562:
// 48-bit Unix milliseconds + randomness). The random source is injected via
// IRandom and the v7 timestamp is passed in by the caller (paired with IClock)
// so tests stay deterministic.
class Uuid {
   public:
    // Does not own random; its lifetime must cover this object
    explicit Uuid(IRandom* random);

    // RFC 4122 v4: random bits + version 4 / variant 10xx
    std::string v4();

    // RFC 9562 v7: top 48 bits = big-endian unixMs, version 7 / variant 10xx
    std::string v7(int64_t unixMs);

    // Parses the timestamp (ms) from a v7 string; returns 0 for invalid formats
    // (internal and test use)
    static int64_t v7TimestampMs(const std::string& uuid);

   private:
    void fillRandom(unsigned char* buffer, std::size_t size);

    IRandom* random_;
};

}  // namespace sensorswave
