#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "event/event_builder.h"

namespace sensorswave {

// Property map → JSON object: serializes pair by pair; invalid values
// (non-finite floats / over-deep nesting / null) skip their key without
// interrupting the rest. Shared by event serialization and the AB evalall
// user.props channel
nlohmann::json propertiesToJson(const PropertyMap& properties);

// Single event → upload-protocol JSON object string (nlohmann is internal only)
std::string serializeEventJson(const Event& event);

// JSON → Event (queue restore / retransmission path); returns false on parse
// failure or unexpected structure
bool deserializeEventJson(const std::string& json, Event* out);

// Event list → JSON array string (a single event is still an array; the
// BatchSender upload body)
std::string serializeEventsJson(const std::vector<Event>& events);

}  // namespace sensorswave
