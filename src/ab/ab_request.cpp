#include "ab/ab_request.h"

#include "constants.h"

namespace sensorswave {
namespace {

// Same rule as track_request: strip trailing '/' to avoid //ab/evalall
std::string joinUrl(const std::string& apiHost, const std::string& path) {
    std::string base = apiHost;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + path;
}

// Single result item -> JSON (cache entry; keeps only the fields the SDK consumes)
nlohmann::json abItemToJson(const AbDataItem& item) {
    nlohmann::json node;
    node["key"] = item.key;
    node["id"] = item.id;  // Cached: lets impressions build $feature_<id> keys after a restart
    node["typ"] = item.typ;
    if (item.hasVid) {
        node["vid"] = item.vid;
    }
    node["disable_impress"] = item.disableImpress;
    node["value"] = item.value;
    return node;
}

// JSON -> result item (tolerant): non-string key -> invalid; missing fields take safe defaults
bool abItemFromJson(const nlohmann::json& node, AbDataItem* out) {
    if (!node.is_object() || !node.contains("key") || !node["key"].is_string()) {
        return false;
    }
    out->key = node["key"].get<std::string>();
    out->hasVid = node.contains("vid") && node["vid"].is_string();
    if (out->hasVid) {
        out->vid = node["vid"].get<std::string>();
    }
    if (node.contains("typ") && node["typ"].is_number_integer()) {
        out->typ = node["typ"].get<int>();
    }
    if (node.contains("id") && node["id"].is_number_integer()) {
        out->id =
            node["id"].get<int64_t>();  // Defaults to 0 when missing (not part of validity checks)
    }
    out->disableImpress = node.contains("disable_impress") &&
                          node["disable_impress"].is_boolean() &&
                          node["disable_impress"].get<bool>();
    out->value = node.contains("value") ? node["value"] : nlohmann::json();
    return true;
}

}  // namespace

HttpRequest buildAbRequest(const std::string& apiHost, const std::string& sourceToken,
                           const std::string& anonId, const std::string& loginId,
                           const nlohmann::json& userProps) {
    HttpRequest request;
    request.url = joinUrl(apiHost, "/ab/evalall");
    request.method = "POST";
    request.headers.push_back(std::make_pair("SourceToken", sourceToken));
    request.headers.push_back(std::make_pair("Content-Type", "application/json; charset=utf-8"));
    nlohmann::json body;
    nlohmann::json user;
    user["anon_id"] = anonId;
    if (!loginId.empty()) {
        user["login_id"] = loginId;  // Omitted when logged out
    }
    // user.props (matching the JS fetchABData: preset + resolved public
    // properties) — the server's segmentation evaluation input; non-object
    // shapes (theoretically unreachable) are defensively ignored
    user["props"] = userProps.is_object() ? userProps : nlohmann::json::object();
    body["user"] = user;
    body["sdk"] = LIB_NAME;
    body["sdk_version"] = LIB_VERSION;
    request.body = body.dump();
    return request;
}

bool parseAbResults(const std::string& body, std::vector<AbDataItem>* out) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    try {
        const nlohmann::json root = nlohmann::json::parse(body);
        if (!root.is_object()) {
            return false;
        }
        // Top-level {"results":[...]}; also accepts a {code,data:{results}} wrapper
        const nlohmann::json* results = nullptr;
        if (root.contains("results") && root["results"].is_array()) {
            results = &root["results"];
        } else if (root.contains("data") && root["data"].is_object() &&
                   root["data"].contains("results") && root["data"]["results"].is_array()) {
            results = &root["data"]["results"];
        }
        if (results == nullptr) {
            return false;
        }
        for (const nlohmann::json& node : *results) {
            AbDataItem item;
            if (abItemFromJson(node, &item)) {
                out->push_back(
                    item);  // Mismatched entries are skipped, the rest unaffected (tolerant)
            }
        }
        return true;  // Empty results is valid too (see adoptCache: treated as "not produced")
    } catch (...) {
        out->clear();
        return false;
    }
}

std::string buildAbCacheJson(const std::vector<AbDataItem>& results, int64_t fetchedAtMs,
                             const std::string& anonId, const std::string& loginId) {
    try {
        nlohmann::json root;
        root["results"] = nlohmann::json::array();
        for (const AbDataItem& item : results) {
            root["results"].push_back(abItemToJson(item));
        }
        root["fetched_at"] = fetchedAtMs;
        root["anon_id"] = anonId;
        root["login_id"] = loginId;
        return root.dump();
    } catch (...) {
        return "";  // Serialization failure: the caller degrades as a persist failure
    }
}

bool parseAbCache(const std::string& raw, std::vector<AbDataItem>* out, int64_t* fetchedAtMs,
                  std::string* anonId, std::string* loginId) {
    if (out == nullptr || fetchedAtMs == nullptr || anonId == nullptr || loginId == nullptr) {
        return false;
    }
    out->clear();
    try {
        const nlohmann::json root = nlohmann::json::parse(raw);
        if (!root.is_object() || !root.contains("results") || !root["results"].is_array() ||
            !root.contains("fetched_at") || !root["fetched_at"].is_number() ||
            !root.contains("anon_id") || !root["anon_id"].is_string() ||
            !root.contains("login_id") || !root["login_id"].is_string()) {
            return false;  // Structural mismatch -> invalid as a whole (discard the cache)
        }
        for (const nlohmann::json& node : root["results"]) {
            AbDataItem item;
            if (abItemFromJson(node, &item)) {
                out->push_back(item);
            }
        }
        *fetchedAtMs = root["fetched_at"].get<int64_t>();
        *anonId = root["anon_id"].get<std::string>();
        *loginId = root["login_id"].get<std::string>();
        return true;
    } catch (...) {
        out->clear();
        return false;
    }
}

}  // namespace sensorswave
