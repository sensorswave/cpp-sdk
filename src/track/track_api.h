#pragma once

#include <string>

#include "event/property_value.h"
#include "platform/system_info.h"

namespace sensorswave {

// Preset property set (built once at init): the automatic set $lib /
// $lib_version / $os / $os_version / $timezone_offset / $language is always
// present (a failed per-key query skips that key and the event still goes
// out); the best-effort set $model / $screen_width / $screen_height / $app_id /
// $app_name / $app_version depends on availability. $session_id is injected
// last by EventBuilder, not here.
// Presets sit at the bottom of the merge order: the host may override them via
// same-named common / event properties.
PropertyMap buildPresetProperties(ISystemInfo* systemInfo);

// Event-name validity: only an empty name is invalid; the tracking entry point
// silently drops such calls. A '$' prefix is not rejected — hosts may report
// preset-shaped events on demand; the reserved prefix is flagged by
// isReservedEventName and only warns before being accepted
bool isValidEventName(const std::string& eventName);

// Reserved '$' prefix marker: a hit means the caller used preset-event naming;
// the tracking entry point still sends but emits a warning (nothing is dropped)
bool isReservedEventName(const std::string& eventName);

// Reserved '$' prefix warning for property keys: custom property keys are not
// blocked — they are merged and sent as usual, with a per-key warning only
// (same policy as event names). Placed at the caller entry point rather than
// the merge layer: internally injected keys such as $lib / $exception_* flow
// through the merge layer, so warning at the boundary avoids flagging them
void warnReservedPropertyKeys(const PropertyMap& props, const char* api);

}  // namespace sensorswave
