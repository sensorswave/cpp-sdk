#include "sender/track_request.h"

namespace sensorswave {
namespace {

std::string joinUrl(const std::string& apiHost, const std::string& path) {
    std::string base = apiHost;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();  // strip trailing '/' to avoid //in/track
    }
    return base + path;
}

}  // namespace

HttpRequest buildTrackRequest(const std::string& apiHost, const std::string& sourceToken,
                              const std::string& body) {
    HttpRequest request;
    request.url = joinUrl(apiHost, "/in/track");
    request.method = "POST";
    request.headers.push_back(std::make_pair("SourceToken", sourceToken));
    request.headers.push_back(std::make_pair("Content-Type", "application/json; charset=utf-8"));
    request.body = body;
    return request;
}

}  // namespace sensorswave
