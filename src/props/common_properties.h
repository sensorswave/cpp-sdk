#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "event/property_value.h"

namespace sensorswave {

// In-memory common-property registry: register merges (same-named keys
// overwritten), clearKeys removes selected keys, clearAll wipes everything.
// snapshot returns an unevaluated copy including dynamic getters — evaluation
// happens once per event in EventBuilder, and a throwing getter only skips
// that property. Purely in-memory, not persisted: empty after a process
// restart / re-init; the caller (sensors_wave_client) is responsible for
// rebuilding it.
class CommonProperties {
   public:
    // Merge-register: same-named keys take the new value
    void registerProperties(const PropertyMap& props);
    // Targeted removal: only the given keys are erased
    void clearKeys(const std::vector<std::string>& keys);
    // Clears everything
    void clearAll();
    // Copy of the registry (dynamic getters included, unevaluated)
    PropertyMap snapshot() const;

   private:
    mutable std::mutex mutex_;
    PropertyMap properties_;
};

}  // namespace sensorswave
