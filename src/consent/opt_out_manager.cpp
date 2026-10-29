#include "consent/opt_out_manager.h"

#include "constants.h"
#include "util/logger.h"

namespace sensorswave {

void OptOutManager::setPreInit(PreInitState state) {
    preInit_ = state;
}

bool OptOutManager::preInitOptedOut() const {
    return preInit_ == PreInitState::kOptOut;
}

OptOutManager::PreInitState OptOutManager::consumePreInit() {
    const PreInitState state = preInit_;
    preInit_ = PreInitState::kUnset;
    return state;
}

bool OptOutManager::resolveInitDisabled(PreInitState preInit, bool configOptOut,
                                        bool persistEnabled, IPersistentStore* store) {
    if (preInit == PreInitState::kOptOut) {
        return true;  // priority 1: pre-init API setting
    }
    if (preInit == PreInitState::kOptIn) {
        return false;
    }
    if (configOptOut) {
        return true;  // priority 2: init config forces disable
    }
    if (persistEnabled && store != nullptr) {
        // priority 3: restore the persisted state; a persisted "disabled" beats
        // a want-to-capture config
        std::string value;
        if (store->read(SW_KEY_OPT_OUT, &value) && value == OPT_OUT_VALUE_DISABLED) {
            return true;
        }
    }
    return false;
}

void OptOutManager::persistState(bool disabled, bool persistEnabled, IPersistentStore* store) {
    if (!persistEnabled || store == nullptr) {
        return;  // persistence off by default: reads and writes are no-ops
    }
    const char* value = disabled ? OPT_OUT_VALUE_DISABLED : OPT_OUT_VALUE_ALLOWED;
    if (!store->write(SW_KEY_OPT_OUT, value)) {
        // storage unavailable: degrade to in-memory state, no error
        Logger::warn("persist sw_opt_out failed, state kept in memory");
    }
}

}  // namespace sensorswave
