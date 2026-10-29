#include "props/common_properties.h"

namespace sensorswave {

void CommonProperties::registerProperties(const PropertyMap& props) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : props) {
        properties_[entry.first] = entry.second;  // same-named keys overwritten
    }
}

void CommonProperties::clearKeys(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& key : keys) {
        properties_.erase(key);
    }
}

void CommonProperties::clearAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    properties_.clear();
}

PropertyMap CommonProperties::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return properties_;  // getters copied along with their values, unevaluated
                         // (evaluation happens in EventBuilder)
}

}  // namespace sensorswave
