// Windows crash capture: SetUnhandledExceptionFilter + CaptureStackBackTrace.
// Compiled into the CMake build on WIN32 only.
// Mirrors the POSIX variant: pre-opened pending fd + pre-serialized payload
// segments backfilled at crash time; the callback does no complex work. The
// terminate layer reuses the same trampoline semantics
#include "crash/crash_handler.h"

#ifndef _WIN32
#include "util/logger.h"
#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>

#include <fcntl.h>
#include <io.h>

#include "constants.h"
#include "exception/exception_collector.h"

namespace sensorswave {
namespace {

std::mutex g_installMutex;
bool g_installed = false;
int g_pendingFd = -1;
CrashPayloadParts g_parts;
TerminateCaptureHook g_hook = nullptr;
std::terminate_handler g_prevTerminate = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
std::atomic<bool> g_fatalCaptured{false};  // as POSIX: no repeat write after terminate capture

void writeAll(const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const int n =
            ::_write(g_pendingFd, data + written, static_cast<unsigned int>(size - written));
        if (n <= 0) {
            return;
        }
        written += static_cast<std::size_t>(n);
    }
}

void writeHexAddr(const void* address) {
    static const char kDigits[] = "0123456789abcdef";
    char buf[2 + sizeof(uintptr_t) * 2];
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
            reversed[count++] = kDigits[rest & 0xF];
            rest >>= 4;
        }
        while (count != 0) {
            buf[len++] = reversed[--count];
        }
    }
    writeAll(buf, len);
}

// SEH unhandled-exception path: only writes the pending file (exception code as
// the type), then returns EXCEPTION_CONTINUE_SEARCH to default handling (WER /
// crash dialog); sending is left to the next-launch resend (same semantics as
// the POSIX signal path)
LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info) {
    if (g_pendingFd < 0 || !g_parts.valid()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Atomically claim the capture right (as in POSIX): of two concurrent fatals
    // only the first writes, avoiding interleaved writes that tear the JSON
    if (g_fatalCaptured.exchange(true, std::memory_order_relaxed)) {
        return EXCEPTION_CONTINUE_SEARCH;  // already captured: do not write again
    }
    void* addresses[FRAME_LIMIT] = {nullptr};
    const USHORT depth = ::CaptureStackBackTrace(0, FRAME_LIMIT, addresses, nullptr);
    writeAll(g_parts.part1.data(), g_parts.part1.size());
    if (depth == 0) {
        writeHexAddr(nullptr);
    } else {
        writeHexAddr(addresses[0]);
        for (USHORT i = 1; i < depth; ++i) {
            writeAll(g_parts.frameSep.data(), g_parts.frameSep.size());
            writeHexAddr(addresses[i]);
        }
    }
    writeAll(g_parts.typePrefix.data(), g_parts.typePrefix.size());
    char code[16];
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        // Exception code (decimal): formatted into a stack buffer, no heap allocation
        const DWORD value = info->ExceptionRecord->ExceptionCode;
        int pos = 0;
        if (value == 0) {
            code[pos++] = '0';
        } else {
            char reversed[12];
            int count = 0;
            DWORD rest = value;
            while (rest != 0) {
                reversed[count++] = static_cast<char>('0' + (rest % 10));
                rest /= 10;
            }
            while (count != 0) {
                code[pos++] = reversed[--count];
            }
        }
        writeAll(code, static_cast<std::size_t>(pos));
    }
    writeAll(g_parts.terminator.data(), g_parts.terminator.size());
    return EXCEPTION_CONTINUE_SEARCH;  // hand over to default handling (WER / crash dialog)
}

void crashTerminateHandler() {
    static std::atomic<bool> reentered(false);
    bool expected = false;
    if (!reentered.compare_exchange_strong(expected, true)) {
        std::abort();
    }
    g_fatalCaptured.store(true, std::memory_order_relaxed);  // as POSIX: no re-capture on abort

    std::string type = "terminate";
    std::string message;
    try {
        if (std::current_exception() != nullptr) {
            std::rethrow_exception(std::current_exception());
        }
    } catch (const std::exception& e) {
        type = exceptionTypeName(e);
        try {
            message = e.what();
        } catch (...) {
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
        frames.clear();
    }

    bool captured = false;
    if (g_hook != nullptr) {
        try {
            captured = g_hook(type, message, frames);
        } catch (...) {
        }
    }
    if (!captured) {
        // As in POSIX: the facade failed to capture -> clear the fatal flag so the
        // chained abort can reach the SEH path and write the pending fallback
        g_fatalCaptured.store(false, std::memory_order_relaxed);
    }

    if (g_prevTerminate != nullptr) {
        g_prevTerminate();
        return;
    }
    std::abort();
}

void uninstallLocked() {
    if (!g_installed) {
        return;
    }
    // Restore the filter before closing the pending fd (same order as POSIX):
    // after close the fd number can be reused by the host at once, and if the
    // still-installed filter catches a crash in that window, the payload would be
    // written via _write into an unrelated host file — remove the filter first to
    // close the write path, then release the fd
    ::SetUnhandledExceptionFilter(g_prevFilter);
    g_prevFilter = nullptr;
    if (g_pendingFd >= 0) {
        ::_close(g_pendingFd);  // pending file kept for the next-launch resend
        g_pendingFd = -1;
    }
    std::set_terminate(g_prevTerminate);
    g_prevTerminate = nullptr;
    g_hook = nullptr;
    g_installed = false;
}

}  // namespace

void installCrashHooks(const std::string& pendingPath, const CrashPayloadParts& parts,
                       TerminateCaptureHook onTerminate) {
    std::lock_guard<std::mutex> lock(g_installMutex);
    uninstallLocked();  // idempotent

    g_pendingFd = ::_open(pendingPath.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                          _S_IREAD | _S_IWRITE);
    g_parts = parts;
    g_hook = onTerminate;
    if (g_pendingFd >= 0 && parts.valid()) {
        g_prevFilter = ::SetUnhandledExceptionFilter(unhandledFilter);
    }
    g_prevTerminate = std::set_terminate(crashTerminateHandler);
    g_fatalCaptured.store(false, std::memory_order_relaxed);
    g_installed = true;
}

void uninstallCrashHooks() {
    std::lock_guard<std::mutex> lock(g_installMutex);
    uninstallLocked();
}

bool crashHooksInstalled() {
    std::lock_guard<std::mutex> lock(g_installMutex);
    return g_installed;
}

}  // namespace sensorswave

#endif  // !_WIN32
