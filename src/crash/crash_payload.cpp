// Pre-serialized payload split (platform-independent)
#include "crash/crash_handler.h"

#include "constants.h"

namespace sensorswave {

const char* const CRASH_ADDR_MARKER = "@@SW_ADDR@@";
const char* const CRASH_ADDR_MARKER2 = "@@SW_ADDR2@@";
const char* const CRASH_TYPE_MARKER = "@@SW_TYPE@@";

namespace {
constexpr std::size_t markerLen(const char* marker) {
    std::size_t len = 0;
    while (marker[len] != '\0') {
        ++len;
    }
    return len;
}
}  // namespace

CrashPayloadParts splitCrashPayload(const std::string& probeJson) {
    CrashPayloadParts parts;
    // The probe event carries two marker frames, giving the four segment boundaries
    // (part1 / frameSep / typePrefix / terminator); out-of-order markers are
    // treated as a template anomaly and empty segments are returned so the caller
    // abandons the signal path (defensive; normal construction cannot hit this)
    const std::size_t addr1 = probeJson.find(CRASH_ADDR_MARKER);
    const std::size_t addr2 = probeJson.find(CRASH_ADDR_MARKER2);
    const std::size_t type = probeJson.find(CRASH_TYPE_MARKER);
    const std::size_t kNotFound = std::string::npos;
    if (addr1 == kNotFound || addr2 == kNotFound || type == kNotFound || addr1 > addr2 ||
        addr2 > type) {
        return parts;
    }
    parts.part1 = probeJson.substr(0, addr1);
    parts.frameSep = probeJson.substr(addr1 + markerLen(CRASH_ADDR_MARKER),
                                      addr2 - (addr1 + markerLen(CRASH_ADDR_MARKER)));
    parts.typePrefix = probeJson.substr(addr2 + markerLen(CRASH_ADDR_MARKER2),
                                        type - (addr2 + markerLen(CRASH_ADDR_MARKER2)));
    parts.terminator = probeJson.substr(type + markerLen(CRASH_TYPE_MARKER));
    return parts;
}

}  // namespace sensorswave
