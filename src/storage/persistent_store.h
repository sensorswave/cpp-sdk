#pragma once

#include <string>

namespace sensorswave {

// Single-key persistence abstraction: one file per key, mode 0600, atomic
// writes.
// Note: PersistentQueue's sw_queue JSONL appends do not go through this
// interface (it manages its own file I/O).
// Failure semantics: no exceptions, expressed via return values; the caller
// degrades to in-memory state when persistence fails.
class IPersistentStore {
   public:
    virtual ~IPersistentStore() = default;

    // Reads a key; returns false if the key does not exist (*out is left
    // unchanged)
    virtual bool read(const std::string& key, std::string* out) = 0;

    // Atomic write (tmp file + rename)
    virtual bool write(const std::string& key, const std::string& value) = 0;

    // Deletes a key; a missing key also counts as success (idempotent)
    virtual bool remove(const std::string& key) = 0;
};

}  // namespace sensorswave
