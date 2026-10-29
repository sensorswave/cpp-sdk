#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "event/event_builder.h"
#include "event/property_value.h"

namespace sensorswave {

// $UserSet user-property operations ($delete / $unset carry no payload pairs
// and are expressed by dedicated constructors)
enum class ProfileOp : std::uint8_t {
    kSet,        // "$set"       overwrite
    kSetOnce,    // "$set_once"  set only once
    kIncrement,  // "$increment" numeric accumulation
    kAppend,     // "$append"    append (no dedup)
    kUnion,      // "$union"     append (dedup)
};

// Operator mapping: op → "$set" / "$set_once" / ...
const char* profileOperator(ProfileOp op);

// Validates and normalizes an operation payload:
// - kIncrement: values must be numeric (int64 / double); non-numeric pairs are
//   skipped with a warning, and if all are invalid an empty map is returned
//   (the caller sends no event)
// - Other operations: pairs are kept as-is (generic checks such as empty keys /
//   getter exceptions live in the EventBuilder merge layer)
// Returns the normalized payload; empty map = no valid pairs
PropertyMap sanitizeProfilePayload(ProfileOp op, const PropertyMap& props);

// Payload → UserOp: wrapped with the operator and serialized through the
// Event.userOps channel into the top-level user_properties nested form; a
// UserOp with an empty payload is skipped by the caller (no event sent)
UserOp buildUserOp(ProfileOp op, const PropertyMap& payload);

// profileUnset operation: key list → { key: null }; an empty list yields an
// empty payload.
// null values pass through the UserOp channel, bypassing the null filtering
// applied to common / event properties
UserOp buildUnsetOp(const std::vector<std::string>& keys);

// profileDelete operation: { "$delete": true }; the identity gate is checked
// by the facade via hasProfileIdentity
UserOp buildDeleteOp();

// profile identity gate: login_id or anon_id — either suffices; when not logged
// in, operations run on the anon dimension.
// If both are missing the facade drops the operation without sending an event
// (defensive: init always generates anon_id)
bool hasProfileIdentity(const std::string& loginId, const std::string& anonId);

}  // namespace sensorswave
