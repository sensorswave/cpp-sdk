#pragma once

// Compiled only when SW_WITH_CURL=1 (toggled in src/CMakeLists.txt)

#ifdef SW_WITH_CURL

#include "http/http_client.h"

namespace sensorswave {

// Default libcurl HTTP implementation (used when the host does not inject an
// IHttpClient). Process-wide curl_global_init is guaranteed via std::once_flag.
// Thread safety: each execute uses its own easy handle (created/destroyed
// within the call) — the facade's internal sender thread and the AB fetch
// thread share one injected instance, and concurrent use of a shared handle is
// a data race libcurl explicitly forbids; with per-call handles, concurrent
// execute is safe, and at typical request rates the per-request init overhead
// is negligible
class CurlHttpClient : public IHttpClient {
   public:
    CurlHttpClient();
    ~CurlHttpClient() override;

    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;

    HttpResponse execute(const HttpRequest& request) override;
};

}  // namespace sensorswave

#endif  // SW_WITH_CURL
