#include "storage/memory_store.h"

namespace sensorswave {

bool MemoryStore::read(const std::string& key, std::string* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }
    if (out != nullptr) {
        *out = it->second;
    }
    return true;
}

bool MemoryStore::write(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_[key] = value;
    return true;
}

bool MemoryStore::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.erase(key);
    return true;
}

}  // namespace sensorswave
