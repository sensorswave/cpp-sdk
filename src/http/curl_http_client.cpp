#include "http/curl_http_client.h"

#ifdef SW_WITH_CURL

#include <curl/curl.h>

#include <memory>
#include <mutex>
#include <string>

namespace sensorswave {
namespace {

// curl_global_init is not thread-safe and must run once (libcurl: before the first easy_init)
void ensureGlobalInit() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Response body cap: normal responses (track ack / evalall) are far smaller;
// guards against anomalous / hijacked endpoints exhausting host memory with a
// huge body
constexpr std::size_t kMaxBodyBytes = 1024UL * 1024UL;

// Body callback: appends to HttpResponse::body. Returns 0 past the cap to abort
// the transfer (CURLE_WRITE_ERROR -> perform fails -> statusCode=0, retried as
// a transport failure)
std::size_t writeBodyCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    std::string* body = static_cast<std::string*>(userdata);
    if (body->size() + bytes > kMaxBodyBytes) {
        return 0;
    }
    body->append(ptr, bytes);
    return bytes;
}

// Per-request easy handle guard: RAII cleanup, prevents leaks on early returns
struct CurlHandleGuard {
    CURL* handle = nullptr;
    ~CurlHandleGuard() {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }
};

}  // namespace

CurlHttpClient::CurlHttpClient() {
    ensureGlobalInit();
}

CurlHttpClient::~CurlHttpClient() = default;

HttpResponse CurlHttpClient::execute(const HttpRequest& request) {
    HttpResponse response;
    ensureGlobalInit();  // once_flag idempotent: direct construction also inits before easy_init

    // One handle per request (thread-safety precondition, see the header); never shared
    CurlHandleGuard guard;
    guard.handle = curl_easy_init();
    CURL* handle = guard.handle;
    if (handle == nullptr) {
        response.error = "curl handle not initialized";
        return response;
    }

    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    // Timeout comes from the caller (fixed to the internal constant HTTP_TIMEOUT_MS)
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeoutMs));
    // Thread safety: disable signal-driven DNS resolution (libcurl multithread requirement)
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    // Do not follow redirects: uploads carry the X-Source-Token credential
    // header, and following a cross-origin redirect would leak the credential to
    // the target host; 3xx is treated as a retryable failure with backoff in the
    // sending layer

    // Uploads always use POST
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));

    curl_slist* headerList = nullptr;
    for (const auto& header : request.headers) {
        headerList = curl_slist_append(headerList, (header.first + ": " + header.second).c_str());
    }
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList);

    const CURLcode result = curl_easy_perform(handle);

    if (headerList != nullptr) {
        curl_slist_free_all(headerList);
    }

    if (result != CURLE_OK) {
        // Transport failure (network / timeout / DNS): statusCode=0; the error
        // text is for internal logs only (silent to callers)
        response.statusCode = 0;
        response.error = curl_easy_strerror(result);
        response.body.clear();
        return response;
    }

    long statusCode = 0;  // NOLINT(google-runtime-int): libcurl's CURLINFO output requires long
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &statusCode);
    response.statusCode = statusCode;
    return response;
}

}  // namespace sensorswave

#endif  // SW_WITH_CURL
