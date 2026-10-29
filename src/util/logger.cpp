#include "util/logger.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace sensorswave {
namespace {

std::atomic<bool> g_debugEnabled{false};
std::mutex g_writeMutex;

// Accepts C strings only: the caller (the public Logger interface) already
// guarantees no allocation / throw from message construction. A mutex lock
// failure (system-level pathology) is swallowed as well — a failed log write is
// never a reason to propagate anything upward
void writeLine(const char* level, const char* message) noexcept {
    if (!g_debugEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(g_writeMutex);
        std::fprintf(stderr, "[sensorswave][%s] %s\n", level, message);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // silently dropped: logging has no recovery action to take on failure —
        // best-effort is the design
    }
}

}  // namespace

void Logger::setDebug(bool enabled) {
    g_debugEnabled.store(enabled, std::memory_order_relaxed);
}

bool Logger::debugEnabled() {
    return g_debugEnabled.load(std::memory_order_relaxed);
}

void Logger::debug(const char* message) noexcept {
    writeLine("debug", message);
}

void Logger::warn(const char* message) noexcept {
    writeLine("warn", message);
}

// The std::string overloads only take a c_str() view (noexcept), no extra
// allocation
void Logger::debug(const std::string& message) noexcept {
    writeLine("debug", message.c_str());
}

void Logger::warn(const std::string& message) noexcept {
    writeLine("warn", message.c_str());
}

}  // namespace sensorswave
