// Windows implementation (CaptureStackBackTrace + module paths). WIN32 builds only
#include "exception/stack_trace.h"

#ifndef _WIN32
#include "util/logger.h"
#else

#define WIN32_LEAN_AND_MEAN
// windows.h must come first: dbghelp.h depends on its basic types; the reverse
// order fails to compile.
// Own block: with IncludeBlocks: Preserve only within-block sorting applies, so
// SortIncludes cannot reorder it alphabetically
#include <windows.h>

#include <dbghelp.h>

#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace sensorswave {
namespace {

constexpr int kMaxRawFrames = 64;

std::string hexAddr(const void* address) {
    static const char* kDigits = "0123456789abcdef";
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    std::string out = "0x";
    if (value == 0) {
        out.push_back('0');
        return out;
    }
    bool started = false;
    for (int shift = (int)(sizeof(uintptr_t) * 8 - 4); shift >= 0; shift -= 4) {
        const int digit = (int)((value >> shift) & 0xF);
        if (digit != 0 || started) {
            out.push_back(kDigits[digit]);
            started = true;
        }
    }
    return out;
}

// CaptureStackBackTrace address frames + GetModuleHandleEx for the module path
// (symbol names are not expanded locally; the server symbolicates from debug info)
class StackTraceWindows : public IStackTrace {
   public:
    std::vector<StackFrame> capture(int maxFrames) override {
        std::vector<StackFrame> frames;
        if (maxFrames <= 0) {
            return frames;
        }
        void* addresses[kMaxRawFrames] = {nullptr};
        const USHORT depth = ::CaptureStackBackTrace(
            0, maxFrames < kMaxRawFrames ? (ULONG)maxFrames : (ULONG)kMaxRawFrames, addresses,
            nullptr);
        frames.reserve(depth);
        for (USHORT i = 0; i < depth; ++i) {
            StackFrame frame;
            frame.instructionAddr = hexAddr(addresses[i]);
            HMODULE module = nullptr;
            if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCSTR)addresses[i], &module) != 0 &&
                module != nullptr) {
                char path[MAX_PATH] = {0};
                if (::GetModuleFileNameA(module, path, MAX_PATH) > 0) {
                    frame.modulePath = path;
                }
            }
            frames.push_back(std::move(frame));
        }
        return frames;
    }
};

}  // namespace

std::unique_ptr<IStackTrace> createPlatformStackTrace() {
    return std::unique_ptr<IStackTrace>(new StackTraceWindows());
}

}  // namespace sensorswave

#endif  // !_WIN32
