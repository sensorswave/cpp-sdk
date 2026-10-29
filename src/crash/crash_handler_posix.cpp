// POSIX crash capture: set_terminate + sigaction (SA_ONSTACK).
// Only async-signal-safe operations are allowed inside the signal handler:
// write() of a pre-serialized payload to a pre-opened fd, manual hex formatting
// into stack buffers, and raise/_exit; no malloc, no locks, no touching facade
// state. Known limitations (see the crash_handler.h header): backtrace is a
// minimal, industry-common exception to the safe-call whitelist and may degrade
// to a 0-frame placeholder under extreme conditions; sigaltstack only covers
// the installing thread.
// After capture, chain-forward (restore the original disposition + unblock +
// re-raise): the process dies from the real signal rather than a fake _exit
// code, preserving host crash-reporter / sanitizer semantics
#include "crash/crash_handler.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>

#include <fcntl.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <sys/ucontext.h>  // Apple: <ucontext.h> needs _XOPEN_SOURCE; structs taken from sys/
#else
#include <ucontext.h>
#endif
#include <unistd.h>

#include <execinfo.h>

#include "constants.h"
#include "exception/exception_collector.h"

namespace sensorswave {
namespace {

constexpr std::size_t kAltStackSize = 64UL * 1024UL;  // 64KB alt signal stack, preallocated

const int kWatchedSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
constexpr int kWatchedSignalCount = static_cast<int>(sizeof(kWatchedSignals) / sizeof(int));

// ── Registration state (written by install/uninstall; read-only in the handler) ──
std::mutex g_installMutex;  // serializes install/uninstall against init / destroy callers
bool g_installed = false;
int g_pendingFd = -1;
CrashPayloadParts g_parts;
TerminateCaptureHook g_hook = nullptr;
char* g_altStack = nullptr;                       // sigaltstack storage (process lifetime)
struct sigaction g_oldActs[kWatchedSignalCount];  // restored on uninstall
bool g_actRegistered[kWatchedSignalCount] = {};   // per-signal registration success flags
std::atomic<bool> g_fatalCaptured{false};         // fatal already captured in this process: the
// chained abort after the terminate path also raises SIGABRT — capture each
// crash only once (prevents duplicate pending records / duplicate reports)
std::terminate_handler g_prevTerminate = nullptr;

void writeAll(int fd, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);  // partial-write loop
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;  // write failed: give up the rest; pending is best-effort, process dying
        }
        written += static_cast<std::size_t>(n);
    }
}

// Signal-safe hex address (same style as trackException frames: "0x" prefix, leading zeros omitted)
void writeHexAddr(int fd, const void* address) {
    static const char kDigits[] = "0123456789abcdef";
    char buf[2 + (sizeof(uintptr_t) * 2)];
    buf[0] = '0';
    buf[1] = 'x';
    std::size_t len = 2;
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    if (value == 0) {
        buf[len++] = '0';
    } else {
        char reversed[sizeof(uintptr_t) * 2];
        std::size_t count = 0;
        uintptr_t rest = value;
        while (rest != 0) {
            reversed[count++] = kDigits[rest & 0xFU];
            rest >>= 4U;
        }
        while (count != 0) {
            buf[len++] = reversed[--count];
        }
    }
    writeAll(fd, buf, len);
}

// Static signal-name table (zero formatting inside the handler)
const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGABRT:
            return "SIGABRT";
        case SIGBUS:
            return "SIGBUS";
        case SIGFPE:
            return "SIGFPE";
        case SIGILL:
            return "SIGILL";
        default:
            return "SIG?";
    }
}

// Internal uninstall (the caller already holds g_installMutex; the public
// uninstall is a thin shell over uninstallCrashHooks).
// Order matters: restore the signal dispositions before closing the fd. If the
// fd were closed first, the fd number could be reused by another host open()
// while the handler is still registered, and a crash would then write the
// pending payload into an unrelated file
void uninstallCrashHooksLocked() {
    if (!g_installed) {
        return;
    }
    for (int i = 0; i < kWatchedSignalCount; ++i) {
        if (g_actRegistered[i]) {
            ::sigaction(kWatchedSignals[i], &g_oldActs[i], nullptr);
            g_actRegistered[i] = false;
        }
    }
    if (g_pendingFd >= 0) {
        ::close(g_pendingFd);  // pending file kept: crash fallback record, resent on next launch
        g_pendingFd = -1;
    }
    if (g_altStack != nullptr) {
        stack_t disable;
        std::memset(&disable, 0, sizeof(disable));
        disable.ss_flags = SS_DISABLE;
        ::sigaltstack(&disable, nullptr);
        ::free(g_altStack);
        g_altStack = nullptr;
    }
    std::set_terminate(g_prevTerminate);
    g_prevTerminate = nullptr;
    g_hook = nullptr;
    g_installed = false;
}

// Fault PC extraction (from the ucontext third handler argument): closer to the
// real faulting instruction than the first backtrace frame (backtrace's first
// frame often lands in the signal trampoline / kernel restoration stub).
// Adapted per target-platform macro; uncovered platforms return nullptr ->
// degrade to plain backtrace frames. Reads registers only, no allocation
const void* faultPcFromContext(void* ctx) {
    if (ctx == nullptr) {
        return nullptr;
    }
    const ucontext_t* uc = static_cast<const ucontext_t*>(ctx);
    // The kernel delivers the fault PC as an integer register value, so the
    // int-to-pointer conversion is inherent to the semantics, not removable
#if defined(__APPLE__) && defined(__aarch64__)
    return reinterpret_cast<const void*>(
        uc->uc_mcontext->__ss.__pc);  // NOLINT(performance-no-int-to-ptr)
#elif defined(__APPLE__) && defined(__x86_64__)
    return reinterpret_cast<const void*>(
        uc->uc_mcontext->__ss.__rip);  // NOLINT(performance-no-int-to-ptr)
#elif defined(__linux__) && defined(__x86_64__)
    return reinterpret_cast<const void*>(
        uc->uc_mcontext.gregs[REG_RIP]);  // NOLINT(performance-no-int-to-ptr)
#elif defined(__linux__) && defined(__aarch64__)
    return reinterpret_cast<const void*>(uc->uc_mcontext.pc);  // NOLINT(performance-no-int-to-ptr)
#else
    (void)uc;
    return nullptr;
#endif
}

// Signal path: only writes the pending file (sw_crash_pending), then
// chain-forwards to the original disposition (restore + re-raise, so the process
// dies from the real signal); no send attempt, never returns to the original flow
extern "C" void crashSignalHandler(int sig, siginfo_t* /*unused*/, void* ctx) {
    // Atomically claim the capture right: when signals arrive concurrently (e.g.
    // SIGABRT right on top of a segfault) only the first one writes, preventing
    // two handlers from interleaving writes to the same fd and tearing the JSON
    if (g_fatalCaptured.exchange(true, std::memory_order_relaxed)) {
        ::_exit(128 + sig);  // already captured by terminate / an earlier signal: no second write
    }
    const int fd = g_pendingFd;
    const CrashPayloadParts& parts = g_parts;
    if (fd < 0 || !parts.valid()) {
        ::_exit(128 + sig);  // payload not pre-serialized: abandon capture, default crash semantics
    }
    // Frame address capture (on the alternate signal stack; small stack buffers, no allocation)
    void* addresses[FRAME_LIMIT];
    int depth = ::backtrace(addresses, FRAME_LIMIT);  // failure -> 0 frames, placeholder written
    depth = std::max(depth, 0);
    const void* faultPc = faultPcFromContext(ctx);
    // With a fault PC available it takes the first frame; backtrace yields one slot
    // (total stays within FRAME_LIMIT)
    const int maxDepth = faultPc != nullptr ? FRAME_LIMIT - 1 : FRAME_LIMIT;
    depth = std::min(depth, maxDepth);
    writeAll(fd, parts.part1.data(), parts.part1.size());
    if (faultPc != nullptr) {
        writeHexAddr(fd, faultPc);
        for (int i = 0; i < depth; ++i) {
            writeAll(fd, parts.frameSep.data(), parts.frameSep.size());
            writeHexAddr(fd, addresses[i]);
        }
    } else if (depth == 0) {
        writeHexAddr(fd, nullptr);  // 0x0 placeholder: structure stays valid, not a real address
    } else {
        writeHexAddr(fd, addresses[0]);
        for (int i = 1; i < depth; ++i) {
            writeAll(fd, parts.frameSep.data(), parts.frameSep.size());
            writeHexAddr(fd, addresses[i]);
        }
    }
    writeAll(fd, parts.typePrefix.data(), parts.typePrefix.size());
    const char* name = signalName(sig);
    writeAll(fd, name, std::strlen(name));
    writeAll(fd, parts.terminator.data(), parts.terminator.size());

    // Chain-forward: restore the pre-init disposition + unblock this signal +
    // re-raise — the process dies from the real signal (default disposition /
    // the host's existing handler) instead of a fake _exit code; crash reporters,
    // sanitizers, and shell $? semantics are all preserved. The three steps
    // (sigaction / sigprocmask / raise) are all async-signal-safe
    int idx = -1;
    for (int i = 0; i < kWatchedSignalCount; ++i) {
        if (kWatchedSignals[i] == sig) {
            idx = i;
            break;
        }
    }
    if (idx >= 0 && g_actRegistered[idx]) {
        ::sigaction(sig, &g_oldActs[idx], nullptr);
        sigset_t mask;
        sigemptyset(&mask);  // a macro on Darwin; cannot take a :: prefix
        sigaddset(&mask, sig);
        ::sigprocmask(SIG_UNBLOCK, &mask, nullptr);  // blocked here: raise needs it unblocked
        ::raise(sig);
    }
    ::_exit(128 + sig);  // fallback: SIG_IGN / failed re-raise -> keep the exit-code semantics
}

// terminate path (normal context, full capture possible): the hook hands the
// capture to the facade for enqueue + short-timeout drain, then chains to the
// original terminate handler (preserving the process's existing abort behavior)
void crashTerminateHandler() {
    static std::atomic<bool> reentered(false);  // second terminate in hook: abort, no recursion
    bool expected = false;
    if (!reentered.compare_exchange_strong(expected, true)) {
        std::abort();
    }
    g_fatalCaptured.store(true, std::memory_order_relaxed);  // chained abort must not re-capture

    std::string type = "terminate";
    std::string message;
    try {
        if (std::current_exception() != nullptr) {
            std::rethrow_exception(std::current_exception());
        }
    } catch (const std::exception& e) {
        type = exceptionTypeName(e);  // uncaught exception: dynamic type name + what()
        try {
            message = e.what();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // what() failed: leave the message empty and continue the termination chain
        }
    } catch (...) {
        type = "unknown_exception";
    }

    std::vector<StackFrame> frames;
    try {
        std::unique_ptr<IStackTrace> tracer = createPlatformStackTrace();
        if (tracer != nullptr) {
            frames = tracer->capture(FRAME_LIMIT);
        }
    } catch (...) {
        frames.clear();  // capture failed: degrade, the event is still sent without frames
    }

    bool captured = false;
    if (g_hook != nullptr) {
        try {
            captured = g_hook(type, message, frames);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // a hook failure must not block the termination chain: captured stays
            // false and the signal path writes the pending file
        }
    }
    if (!captured) {
        // The facade failed to capture (hook missing / infrastructure unavailable /
        // enqueue failed): clear the fatal flag set above so the chained abort's
        // SIGABRT can still reach the signal path and write the pending file —
        // otherwise the flag would needlessly suppress the pending-file fallback
        g_fatalCaptured.store(false, std::memory_order_relaxed);
    }

    if (g_prevTerminate != nullptr) {
        g_prevTerminate();  // chain: preserve the host's / CRT termination behavior
        return;             // the original handler normally does not return
    }
    std::abort();
}

}  // namespace

void installCrashHooks(const std::string& pendingPath, const CrashPayloadParts& parts,
                       TerminateCaptureHook onTerminate) {
    std::lock_guard<std::mutex> lock(g_installMutex);
    uninstallCrashHooksLocked();  // idempotent: reinstall restores the previous state first

    const int fd = ::open(pendingPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    // fd failed to open -> no write target on the signal path; terminate can still
    // be registered (the normal context keeps the enqueue fallback)
    g_pendingFd = fd;
    g_parts = parts;
    g_hook = onTerminate;

    if (fd >= 0 && parts.valid()) {
        if (g_altStack == nullptr) {
            g_altStack = static_cast<char*>(::malloc(kAltStackSize));
        }
        stack_t alt;
        std::memset(&alt, 0, sizeof(alt));
        alt.ss_sp = g_altStack;
        alt.ss_size = kAltStackSize;
        alt.ss_flags = g_altStack != nullptr ? 0 : SS_DISABLE;
        ::sigaltstack(&alt, nullptr);  // disable if preallocation failed: not worth the risk
        for (int i = 0; i < kWatchedSignalCount; ++i) {
            struct sigaction act;
            std::memset(&act, 0, sizeof(act));
            act.sa_sigaction = crashSignalHandler;
            act.sa_flags = SA_SIGINFO | SA_ONSTACK;
            g_actRegistered[i] = ::sigaction(kWatchedSignals[i], &act, &g_oldActs[i]) == 0;
        }
    }

    g_prevTerminate = std::set_terminate(crashTerminateHandler);
    g_fatalCaptured.store(false, std::memory_order_relaxed);  // reset on reinstall
    g_installed = true;
}

void uninstallCrashHooks() {
    std::lock_guard<std::mutex> lock(g_installMutex);
    uninstallCrashHooksLocked();
}

bool crashHooksInstalled() {
    std::lock_guard<std::mutex> lock(g_installMutex);
    return g_installed;
}

}  // namespace sensorswave
