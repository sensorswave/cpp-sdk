#include "profile/profile_api.h"

#include "track/track_api.h"
#include "util/logger.h"

namespace sensorswave {

const char* profileOperator(ProfileOp op) {
    switch (op) {
        case ProfileOp::kSet:
            return "$set";
        case ProfileOp::kSetOnce:
            return "$set_once";
        case ProfileOp::kIncrement:
            return "$increment";
        case ProfileOp::kAppend:
            return "$append";
        case ProfileOp::kUnion:
            return "$union";
    }
    return "$set";  // unreachable (all enumerators covered); defensive return
}

PropertyMap sanitizeProfilePayload(ProfileOp op, const PropertyMap& props) {
    PropertyMap payload;
    warnReservedPropertyKeys(props, "profile");  // '$'-prefixed keys pass with a warning only
    for (const auto& entry : props) {
        if (entry.first.empty()) {
            continue;  // drop empty keys
        }
        if (op == ProfileOp::kIncrement) {
            // values must be numeric: non-numeric pairs are skipped with a
            // warning, the rest proceed
            const PropertyValue::Type type = entry.second.type();
            if (type != PropertyValue::Type::kInt64 && type != PropertyValue::Type::kDouble) {
                Logger::warn("profileIncrement skipped non-numeric value: " + entry.first);
                continue;
            }
        }
        payload[entry.first] = entry.second;
    }
    if (payload.empty() && !props.empty()) {
        Logger::warn("profile payload has no valid pairs after sanitization");
    }
    return payload;
}

UserOp buildUserOp(ProfileOp op, const PropertyMap& payload) {
    UserOp userOp;
    userOp.op = profileOperator(op);
    userOp.payload = payload;
    return userOp;
}

UserOp buildUnsetOp(const std::vector<std::string>& keys) {
    UserOp userOp;
    userOp.op = "$unset";
    for (const std::string& key : keys) {
        if (key.empty()) {
            continue;
        }
        if (key.front() == '$') {
            // property keys are not blocked — unset proceeds, warning only
            Logger::warn("profileUnset: '$'-prefixed key is reserved for preset properties: " +
                         key);
        }
        userOp.payload[key] = PropertyValue();  // default null state → key: null
    }
    return userOp;
}

UserOp buildDeleteOp() {
    UserOp userOp;
    userOp.op = "$delete";
    userOp.deleteFlag = true;  // "$delete": true
    return userOp;
}

bool hasProfileIdentity(const std::string& loginId, const std::string& anonId) {
    // Login is not required: login_id / anon_id — either suffices; when not
    // logged in, operations run on the anon dimension
    return !loginId.empty() || !anonId.empty();
}

}  // namespace sensorswave
