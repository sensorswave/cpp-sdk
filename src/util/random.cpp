#include "util/random.h"

#include <cstdio>
#include <stdexcept>

#ifdef _WIN32
// windows.h must come first: bcrypt.h depends on its base types (PVOID/ULONG);
// the reverse order fails to compile.
// Kept in a separate block: with IncludeBlocks: Preserve, sorting happens
// within a block only, so SortIncludes cannot reorder them alphabetically again
#include <windows.h>

#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <unistd.h>
#include <cerrno>
#if defined(__has_include)
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#define SW_HAS_GETRANDOM 1
#endif
#endif
#endif

namespace sensorswave {

// Throws when every source fails (rather than silently producing predictable
// pseudo-randomness): colliding or predictable UUIDs would clash event
// trace_ids and cross-wire identities — a "better nothing than bad" trade-off.
// The caller (facade / builder) catches and drops that capture attempt; the
// failure stays silent to the outside. The three platform sources (BCrypt /
// arc4random / getrandom + urandom) do not all fail in a normal process
// environment
void SecureRandom::fill(void* buffer, std::size_t size) {
    if (size == 0) {
        return;
    }
    auto* out = static_cast<unsigned char*>(buffer);

#ifdef _WIN32
    // BCryptGenRandom handle-less mode (the MSDN-recommended usage). The
    // NTSTATUS success code is simply 0: STATUS_SUCCESS lives in ntstatus.h,
    // which under MSVC requires the WIN32_NO_STATUS macro dance, so comparing
    // against 0 is simpler
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) !=
        0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
#elif defined(__APPLE__)
    arc4random_buf(out, size);
#else
#if defined(SW_HAS_GETRANDOM)
    std::size_t filled = 0;
    while (filled < size) {
        const ssize_t got = ::getrandom(out + filled, size - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            filled = 0;  // on failure fall back to a full /dev/urandom fill
            break;
        }
        filled += static_cast<std::size_t>(got);
    }
    if (filled == size) {
        return;
    }
#endif
    FILE* urandom = std::fopen("/dev/urandom", "rb");
    if (urandom == nullptr) {
        throw std::runtime_error("cannot open /dev/urandom");
    }
    const std::size_t got = std::fread(out, 1, size, urandom);
    std::fclose(urandom);
    if (got != size) {
        throw std::runtime_error("short read from /dev/urandom");
    }
#endif
}

}  // namespace sensorswave
