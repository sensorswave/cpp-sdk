#pragma once

#include <string>

namespace sensorswave {

// Debug logging (emitted only when debug: true; always silent to the outside).
// Writes to stderr with a [sensorswave] prefix; both debug and warn are gated
// by the debug switch. Thread-safe (a single fprintf under a lock).
//
// Every API is noexcept and catches internally: logging is a best-effort
// component, and any failure (including allocation failure under OOM or a
// mutex error) silently drops that one message without ever affecting the
// caller — the catch-all path of the public APIs itself relies on Logger never
// rethrowing.
// The const char* overloads serve literal call sites, avoiding a std::string
// temporary allocation on the caller side.
class Logger {
   public:
    static void setDebug(bool enabled);
    static bool debugEnabled();

    static void debug(const char* message) noexcept;
    static void warn(const char* message) noexcept;
    static void debug(const std::string& message) noexcept;
    static void warn(const std::string& message) noexcept;
};

}  // namespace sensorswave
