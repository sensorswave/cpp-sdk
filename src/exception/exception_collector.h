#pragma once

#include <exception>
#include <string>
#include <vector>

#include "config.h"
#include "event/property_value.h"
#include "exception/stack_trace.h"

namespace sensorswave {

// in_app decision order (excludes take precedence over includes):
//   1. an inAppExcludes keyword matches modulePath (substring) -> false;
//   2. an inAppIncludes keyword matches -> true;
//   3. a vendor system directory prefix (macOS /System /usr/lib; Linux /lib
//      /usr/lib; Windows %WINDIR% / Program Files) -> false;
//   otherwise (including an empty modulePath) -> true.
bool resolveInApp(const std::string& modulePath, const ExceptionConfig& config);

// $Exception preset properties: $exception_level / $exception_type /
// $exception_message are always written; $exception_frames is appended when
// frames is non-empty (frame objects carry only the fields that could be
// captured; platform is mandatory = EXCEPTION_PLATFORM; empty frames omit the
// property).
// Writes into out (append semantics, existing keys are not cleared; the SDK's
// authoritative $ keys are guaranteed to be overwritten by the caller later).
void buildExceptionProperties(PropertyMap* out, const std::string& type, const std::string& message,
                              const std::vector<StackFrame>& frames, const ExceptionConfig& config,
                              const std::string& level);

// Exception type name: the typeid dynamic type name, demangled under the GNU ABI
// (e.g. "std::runtime_error"); on MSVC the class / struct prefix is stripped to
// match the same shape; falls back to the raw name on failure; never throws
// (defensive catch for bad_typeid)
std::string exceptionTypeName(const std::exception& e);

}  // namespace sensorswave
