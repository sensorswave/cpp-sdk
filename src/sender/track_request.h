#pragma once

#include <string>

#include "http/http_client.h"

namespace sensorswave {

// Builds the /in/track ingest request:
// POST {apiHost}/in/track; headers SourceToken + Content-Type:
// application/json; charset=utf-8; the body is a JSON array string (a single
// event is still an array). Redundant trailing '/' in apiHost are collapsed.
HttpRequest buildTrackRequest(const std::string& apiHost, const std::string& sourceToken,
                              const std::string& body);

}  // namespace sensorswave
