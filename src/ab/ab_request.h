#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/http_client.h"

namespace sensorswave {

// A single evalall result item (shared by response and cache entries)
struct AbDataItem {
    std::string key;  // feature / experiment key
    int64_t id = 0;   // Server numeric ID (JS ABData.id): the basis for building
                      // the impression user_properties keys $feature_<id> /
                      // $exp_<id>; falls back to 0 when absent from the response
    std::string vid;  // Hit variant; hasVid=false = not delivered by the server (miss)
    bool hasVid = false;
    int typ = 0;  // 1=feature gate / 2=feature config / 3=experiment
    bool disableImpress = false;
    nlohmann::json value;  // Config value (any JSON; missing / null counts as invalid)
};

// AB query request: POST {apiHost}/ab/evalall, headers identical to track;
// user carries anon_id (login_id omitted when logged out) + props (preset +
// public property snapshot, matching the JS evalall user.props segmentation
// channel) + sdk / sdk_version
HttpRequest buildAbRequest(const std::string& apiHost, const std::string& sourceToken,
                           const std::string& anonId, const std::string& loginId,
                           const nlohmann::json& userProps);

// Parses an evalall response ({"results":[...]}; also accepts a
// {"data":{"results":[...]}} wrapper). Missing results container / non-JSON ->
// false; a single entry with mismatched types is skipped (the rest are
// unaffected); on success out receives all valid entries (may be an empty
// array; empty results still counts as a valid response)
bool parseAbResults(const std::string& body, std::vector<AbDataItem>* out);

// sw_ab_data cache document: results + fetched_at + fetch-time identity (ownership check)
std::string buildAbCacheJson(const std::vector<AbDataItem>& results, int64_t fetchedAtMs,
                             const std::string& anonId, const std::string& loginId);
// Cache restore: any missing or mistyped field -> false (the caller discards the cache)
bool parseAbCache(const std::string& raw, std::vector<AbDataItem>* out, int64_t* fetchedAtMs,
                  std::string* anonId, std::string* loginId);

}  // namespace sensorswave
