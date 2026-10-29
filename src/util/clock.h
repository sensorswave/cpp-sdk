#pragma once

#include <cstdint>

namespace sensorswave {

// Clock abstraction: the session state machine / AB TTL / backoff waits all
// take it via injection, so tests can advance virtual time.
class IClock {
   public:
    virtual ~IClock() = default;

    // Unix epoch milliseconds
    virtual int64_t nowMs() const = 0;
};

// The real system clock (default implementation)
class SystemClock : public IClock {
   public:
    int64_t nowMs() const override;
};

}  // namespace sensorswave
