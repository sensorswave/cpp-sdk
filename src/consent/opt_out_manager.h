#pragma once

#include <cstdint>
#include <string>

#include "storage/persistent_store.h"

namespace sensorswave {

// Opt-Out compliance state: pre-init settings, the three-source priority
// resolution at init, sw_opt_out persistence encoding, and write-failure
// degradation. A small object of pure state and pure functions — no threading
// or infrastructure orchestration; dispatch pausing / deferred-init catch-up
// stay in the facade.
class OptOutManager {
   public:
    // Pre-init API setting (highest priority): consumed and cleared by the
    // first init
    enum class PreInitState : std::uint8_t { kUnset = 0, kOptOut = 1, kOptIn = 2 };

    OptOutManager() = default;

    // State recorded by pre-init optOutCapturing() / optInCapturing()
    void setPreInit(PreInitState state);
    // Observation for pre-init hasOptedOutCapturing() (safe to call before init)
    bool preInitOptedOut() const;
    // init consumes the pre-init state and clears it (the state survives
    // exactly one init)
    PreInitState consumePreInit();

    // Final disabled-state resolution at init (priority: API setting > config >
    // persisted value).
    // When persistEnabled, sw_opt_out is read via store; a persisted "disabled"
    // wins over a "want to capture" config.
    // store may be null (unusable directory etc.); the persisted source is then
    // skipped
    static bool resolveInitDisabled(PreInitState preInit, bool configOptOut, bool persistEnabled,
                                    IPersistentStore* store);

    // Runtime state persistence: "0" = disabled, "1" = allowed;
    // persistEnabled=false or a missing store makes this a no-op (no write is
    // produced); write failure only warns — the state already stands in memory
    static void persistState(bool disabled, bool persistEnabled, IPersistentStore* store);

   private:
    PreInitState preInit_ = PreInitState::kUnset;
};

}  // namespace sensorswave
