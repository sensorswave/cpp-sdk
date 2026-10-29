#include "exception/stack_trace.h"

#include <execinfo.h>

#include <dlfcn.h>
#include <cstdio>

namespace sensorswave {
namespace {

// Hex address (with 0x prefix); the signal path reuses this format (instruction_addr is a string)
std::string hexAddr(const void* address) {
    static const char* kDigits = "0123456789abcdef";
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    std::string out = "0x";
    if (value == 0) {
        out.push_back('0');
        return out;
    }
    bool started = false;
    // The shift loop variable is never negative; cast to unsigned at the point of
    // use so the shift count and mask are unsigned, avoiding signed bitwise ops
    // (bugprone-signed-bitwise)
    for (int shift = static_cast<int>((sizeof(uintptr_t) * 8U) - 4U); shift >= 0; shift -= 4) {
        const int digit = static_cast<int>((value >> static_cast<unsigned>(shift)) & 0xFU);
        if (digit != 0 || started) {
            out.push_back(kDigits[digit]);
            started = true;
        }
    }
    return out;
}

// POSIX implementation: backtrace for addresses, dladdr for the module path and
// symbol (left empty when unavailable; the frame still keeps its address)
class StackTracePosix : public IStackTrace {
   public:
    std::vector<StackFrame> capture(int maxFrames) override {
        std::vector<StackFrame> frames;
        if (maxFrames <= 0) {
            return frames;
        }
        void* addresses[64] = {nullptr};
        const int depth =
            ::backtrace(addresses, maxFrames < 64 ? maxFrames : 64);  // on failure -> 0 frames
        frames.reserve(static_cast<std::size_t>(depth > 0 ? depth : 0));
        for (int i = 0; i < depth; ++i) {
            StackFrame frame;
            frame.instructionAddr = hexAddr(addresses[i]);
            ::Dl_info info = {};
            if (::dladdr(addresses[i], &info) != 0) {
                if (info.dli_fname != nullptr) {
                    frame.modulePath = info.dli_fname;
                }
                if (info.dli_sname != nullptr) {
                    frame.function = info.dli_sname;  // not demangled: server-side symbolication
                }
            }
            frames.push_back(std::move(frame));
        }
        return frames;
    }
};

}  // namespace

std::unique_ptr<IStackTrace> createPlatformStackTrace() {
    return std::unique_ptr<IStackTrace>(new StackTracePosix());
}

}  // namespace sensorswave
