#pragma once

#include <cstddef>

namespace sensorswave {

// Random-source abstraction: used by UUID / trace_id generation; the injected
// implementation keeps tests deterministic.
class IRandom {
   public:
    virtual ~IRandom() = default;

    // Fills buffer with cryptographically secure random bytes
    virtual void fill(void* buffer, std::size_t size) = 0;
};

// Default platform secure random: Apple arc4random_buf / Linux getrandom
// (falling back to /dev/urandom) / Windows BCryptGenRandom.
class SecureRandom : public IRandom {
   public:
    void fill(void* buffer, std::size_t size) override;
};

}  // namespace sensorswave
