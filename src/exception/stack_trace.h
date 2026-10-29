#pragma once

#include <memory>
#include <string>
#include <vector>

namespace sensorswave {

// A single stack frame (native address frame, platform="native"; symbolication needs debug info)
struct StackFrame {
    std::string modulePath;       // owning module path (in_app input; empty if unavailable)
    std::string function;         // symbol name (empty if unavailable; the frame omits the field)
    std::string instructionAddr;  // hex instruction address ("0x..."; event field instruction_addr)
};

// Injectable stack-capture abstraction (platform-specific code isolated behind
// an abstract interface; capture failure returns empty and never throws — with
// no frames the event carries no $exception_frames)
class IStackTrace {
   public:
    virtual ~IStackTrace() = default;

    // Captures the current call stack, at most maxFrames frames (capped by FRAME_LIMIT)
    virtual std::vector<StackFrame> capture(int maxFrames) = 0;
};

// Platform implementation factory: POSIX backtrace + dladdr / Windows CaptureStackBackTrace
std::unique_ptr<IStackTrace> createPlatformStackTrace();

}  // namespace sensorswave
