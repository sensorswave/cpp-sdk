#pragma once

#include <string>

#include "storage/persistent_store.h"

namespace sensorswave {

// Default file-backed persistence:
// - One file per key: <dataDir>/<key>; mode 0600
// - Atomic writes: write <key>.tmp then rename-replace; the original file
//   survives a failed rename
// - If the directory cannot be created, valid() == false and all reads and
//   writes fail; the caller degrades to MemoryStore
class FileStore : public IPersistentStore {
   public:
    explicit FileStore(const std::string& directory);

    bool valid() const { return valid_; }
    const std::string& directory() const { return directory_; }

    bool read(const std::string& key, std::string* out) override;
    bool write(const std::string& key, const std::string& value) override;
    bool remove(const std::string& key) override;

   private:
    std::string pathFor(const std::string& key) const;

    std::string directory_;
    bool valid_;
};

}  // namespace sensorswave
