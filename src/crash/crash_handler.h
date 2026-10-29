#pragma once

#include <string>
#include <vector>

#include "exception/stack_trace.h"

namespace sensorswave {

// Crash capture: three in-process layers —
//   std::set_terminate (uncaught C++ exceptions, normal context: full capture +
//   best-effort send with a short timeout);
//   POSIX sigaction (SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL, SA_ONSTACK alternate
//   stack; the handler only write()s a pre-serialized payload to a pre-opened
//   fd — async-signal-safe);
//   Windows SetUnhandledExceptionFilter (one platform file per build target).
// fatal semantics are uniform: best-effort send + pending-file fallback +
// resend on next startup.
//
// Known limitations (an honest statement of the best-effort semantics):
//   - backtrace() is not on the POSIX async-signal-safe whitelist; a minimal,
//     industry-common exception. Under extreme heap pressure the signal path
//     may produce only a 0-frame placeholder (the structure stays valid).
//   - The sigaltstack alternate stack is thread-local: only the installing
//     thread benefits. Crashes on other threads run the handler on the original
//     stack, so a stack overflow may prevent writing the pending file.
//   - If the host overrides a watched signal disposition / the terminate handler
//     after init, the SDK does not fight to reinstall (install runs once during
//     init); another init restores protection (idempotent reinstall).

// Pre-serialized payload split (the key to zero allocation in the signal
// context): during init a probe event produces the complete event JSON (with
// the required event fields and a level=fatal snapshot), split into four
// segments at markers; at crash time only two kinds of dynamic bytes are
// backfilled — hex frame addresses and the signal name — and every write()
// emits precomputed strings
struct CrashPayloadParts {
    std::string part1;     // event header; first frame {"platform":"native","instruction_addr":"
    std::string frameSep;  // between frames: ","in_app":true},{"platform":...,"instruction_addr":"
    std::string typePrefix;  // closes the last frame ] + ],"$exception_type":"
    std::string terminator;  // "}}
    // All four parts are required (empty on split failure: the caller abandons the signal path)
    bool valid() const {
        return !part1.empty() && !frameSep.empty() && !typePrefix.empty() && !terminator.empty();
    }
};

// Probe markers (buildCrashProbe event property placeholders; splitCrashPayload splits on them)
extern const char* const CRASH_ADDR_MARKER;
extern const char* const CRASH_ADDR_MARKER2;
extern const char* const CRASH_TYPE_MARKER;

// Splits the payload out of the probe event JSON (called in a normal context
// during init; missing / out-of-order markers yield empty segments)
CrashPayloadParts splitCrashPayload(const std::string& probeJson);

// terminate capture hook (normal context; the crash layer calls into the facade):
// type / message / frames are already captured by the crash layer; the facade
// handles enqueueing plus a short-timeout drain (TERMINATE_DRAIN_TIMEOUT_MS).
// Returns true = the facade captured and enqueued it; false = the facade could
// not capture (infrastructure missing / enqueue failed), letting the crash layer
// fall through to the signal path to write the pending file on the chained abort
using TerminateCaptureHook = bool (*)(const std::string& type, const std::string& message,
                                      const std::vector<StackFrame>& frames);

// Installed during init (requires enableCrashTrack=true): set_terminate -> signal
// registration (POSIX).
// pendingPath is the absolute path of sw_crash_pending (fd pre-opened with
// O_CREAT|O_WRONLY|O_TRUNC 0600; the only write target on the signal path);
// reinstalling uninstalls first (idempotent)
void installCrashHooks(const std::string& pendingPath, const CrashPayloadParts& parts,
                       TerminateCaptureHook onTerminate);

// Uninstall (destroy): restores the original handlers and closes the fd; the
// pending file is kept (crash fallback record)
void uninstallCrashHooks();

// Test observation: whether the capture layer is currently installed
bool crashHooksInstalled();

}  // namespace sensorswave
