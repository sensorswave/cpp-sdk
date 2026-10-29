#pragma once

#include <map>
#include <mutex>
#include <string>

#include "storage/persistent_store.h"

namespace sensorswave {

// In-memory persistence implementation: used as the degraded mode when the
// directory is unavailable and by unit tests; semantics match FileStore.
// Thread safety: init and tracking threads may access it concurrently, so it
// locks internally.
class MemoryStore : public IPersistentStore {
   public:
    MemoryStore() = default;

    bool read(const std::string& key, std::string* out) override;
    bool write(const std::string& key, const std::string& value) override;
    bool remove(const std::string& key) override;

   private:
    std::mutex mutex_;
    std::map<std::string, std::string> data_;
};

}  // namespace sensorswave
