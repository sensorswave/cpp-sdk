// Platform system info implementation: macOS / Linux (Windows in
// system_info_windows.cpp).
// Per-key best-effort: any failed system call returns false and the key is left
// out of the preset properties; each method body catches all exceptions so the
// interface contract of never throwing holds.

#include "platform/system_info.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <map>
#include <string>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <SystemConfiguration/SCNetworkConfiguration.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/sysctl.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <cstdlib>
#include <fstream>
#endif

namespace sensorswave {
namespace {

std::string trimCopy(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Shared helpers upperAsciiCopy / normalizeRegionCode live in system_info.h ($region validation)

class PosixSystemInfo : public ISystemInfo {
   public:
    bool osName(std::string* out) override {
        // Desktop conventions: macOS / Linux; Windows lives in system_info_windows.cpp
#ifdef __APPLE__
        *out = "macOS";
#else
        *out = "Linux";
#endif
        return true;
    }

    bool osVersion(std::string* out) override {
        try {
#ifdef __APPLE__
            // kern.osproductversion (e.g. "15.6", product version only); the sysctl
            // result is not guaranteed NUL-terminated
            char buffer[32] = {0};
            std::size_t size = sizeof(buffer) - 1;
            if (::sysctlbyname("kern.osproductversion", buffer, &size, nullptr, 0) != 0 ||
                size == 0) {
                return false;
            }
            buffer[size] = '\0';
            const std::string version = trimCopy(buffer);
            if (version.empty()) {
                return false;
            }
            *out = version;
            return true;
#else
            utsname names = {};
            if (::uname(&names) != 0) {
                return false;
            }
            const std::string version = trimCopy(names.release);  // kernel version
            if (version.empty()) {
                return false;
            }
            *out = version;
            return true;
#endif
        } catch (...) {
            return false;
        }
    }

    bool model(std::string* out) override {
        try {
#ifdef __APPLE__
            // hw.model (e.g. "MacBookPro18,3")
            char buffer[128] = {0};
            std::size_t size = sizeof(buffer) - 1;
            if (::sysctlbyname("hw.model", buffer, &size, nullptr, 0) != 0 || size == 0) {
                return false;
            }
            buffer[size] = '\0';
            const std::string model = trimCopy(buffer);
#else
            // Linux: DMI product name (common on desktops) or device-tree model (ARM / embedded)
            std::string raw = readFile("/sys/devices/virtual/dmi/id/product_name");
            if (raw.empty()) {
                raw = readFile("/proc/device-tree/model");
            }
            const std::string model = trimCopy(raw);
#endif
            if (model.empty()) {
                return false;
            }
            *out = model;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool screenWidth(int* out) override {
#ifdef __APPLE__
        try {
            const CGDirectDisplayID display = primaryDisplay();
            if (display == kCGNullDirectDisplay) {
                return false;
            }
            const int width = static_cast<int>(CGDisplayPixelsWide(display));
            if (width <= 0) {
                return false;
            }
            *out = width;
            return true;
        } catch (...) {
            return false;
        }
#else
        (void)out;
        // Screen size on Linux needs X11 / Wayland, which v1 does not add: key omitted
        return false;
#endif
    }

    bool screenHeight(int* out) override {
#ifdef __APPLE__
        try {
            const CGDirectDisplayID display = primaryDisplay();
            if (display == kCGNullDirectDisplay) {
                return false;
            }
            const int height = static_cast<int>(CGDisplayPixelsHigh(display));
            if (height <= 0) {
                return false;
            }
            *out = height;
            return true;
        } catch (...) {
            return false;
        }
#else
        (void)out;
        return false;  // same as screenWidth: no X11 dependency
#endif
    }

    bool timezoneOffsetSeconds(int64_t* out) override {
        try {
            // East-positive convention: tm_gmtoff is + east / - west (UTC+8 -> +28800)
            const std::time_t now = std::time(nullptr);
            std::tm local = {};
            if (::localtime_r(&now, &local) == nullptr) {
                return false;
            }
            *out = static_cast<int64_t>(local.tm_gmtoff);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool language(std::string* out) override {
        try {
#ifdef __APPLE__
            // Note: CFLocale identifiers use '_' separators (e.g. "zh_Hans_CN")
            // instead of the BCP 47 '-', so they must go through normalizeLanguageTag
            // and must not be sent as-is
            CFLocaleRef locale = ::CFLocaleCopyCurrent();
            if (locale == nullptr) {
                return false;
            }
            std::string identifier;
            const CFTypeRef value = ::CFLocaleGetValue(locale, kCFLocaleIdentifier);
            if (value != nullptr && ::CFGetTypeID(value) == ::CFStringGetTypeID()) {
                char buffer[256] = {0};
                if (::CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer),
                                         kCFStringEncodingUTF8) != 0U) {
                    identifier = buffer;
                }
            }
            ::CFRelease(locale);
            // '@keyword' extension suffix truncated + '_' -> '-' (shared normalization)
            const std::string language = trimCopy(normalizeLanguageTag(identifier));
            if (language.empty()) {
                return false;
            }
            *out = language;
            return true;
#else
            // LANG (e.g. "zh_CN.UTF-8") -> strip the encoding suffix, then apply the
            // shared normalization (BCP 47 approximation).
            // "C" / "POSIX" are not user languages: treat as unavailable
            const char* raw = std::getenv("LANG");
            if (raw == nullptr || raw[0] == '\0') {
                return false;
            }
            std::string value = normalizeLanguageTag(std::string(raw));
            const std::size_t dot = value.find('.');
            if (dot != std::string::npos) {
                value.resize(dot);
            }
            value = trimCopy(value);
            if (value.empty() || value == "C" || value == "POSIX") {
                return false;
            }
            *out = value;
            return true;
#endif
        } catch (...) {
            return false;
        }
    }

    bool appId(std::string* out) override {
#ifdef __APPLE__
        // CLI processes / non-bundle hosts have no main bundle: key omitted (best-effort)
        return bundleInfoString(kCFBundleIdentifierKey, out);
#else
        (void)out;
        return false;  // Linux has no unified app identity source; omitted in v1
#endif
    }

    bool appName(std::string* out) override {
#ifdef __APPLE__
        return bundleInfoString(kCFBundleNameKey, out);
#else
        (void)out;
        return false;
#endif
    }

    bool appVersion(std::string* out) override {
#ifdef __APPLE__
        // CF exports no constant for this key, so use the literal (Info.plist key names are fixed)
        return bundleInfoString(CFSTR("CFBundleShortVersionString"), out);
#else
        (void)out;
        return false;
#endif
    }

    bool brand(std::string* out) override {
#ifdef __APPLE__
        // The de facto sole vendor on desktop Macs (sysctl has no hw.vendor key)
        *out = "Apple";
        return true;
#else
        // DMI board vendor (often missing on self-built machines / cloud hosts; best-effort)
        return dmiString("/sys/devices/virtual/dmi/id/board_vendor", out);
#endif
    }

    bool manufacturer(std::string* out) override {
#ifdef __APPLE__
        *out = "Apple";
        return true;
#else
        return dmiString("/sys/devices/virtual/dmi/id/sys_vendor", out);
#endif
    }

    bool deviceId(std::string* out) override {
        try {
#ifdef __APPLE__
            // IOPlatformUUID (IOKit, read-only platform machine id; the SDK neither
            // generates nor persists it)
            const io_service_t platform = ::IOServiceGetMatchingService(
                kIOMainPortDefault, ::IOServiceMatching("IOPlatformExpertDevice"));
            if (platform == 0) {
                return false;
            }
            const CFTypeRef value = ::IORegistryEntrySearchCFProperty(
                platform, kIOServicePlane, CFSTR("IOPlatformUUID"), kCFAllocatorDefault,
                kIORegistryIterateRecursively | kIORegistryIterateParents);
            ::IOObjectRelease(platform);
            std::string uuid;
            if (value != nullptr) {
                if (::CFGetTypeID(value) == ::CFStringGetTypeID()) {
                    char buffer[128] = {0};
                    if (::CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                                             sizeof(buffer), kCFStringEncodingUTF8) != 0U) {
                        uuid = trimCopy(buffer);
                    }
                }
                ::CFRelease(value);
            }
            if (uuid.empty()) {
                return false;
            }
            *out = uuid;
            return true;
#else
            // /etc/machine-id (mainstream systemd); fall back to the dbus copy when missing
            std::string id = trimCopy(readFile("/etc/machine-id"));
            if (id.empty()) {
                id = trimCopy(readFile("/var/lib/dbus/machine-id"));
            }
            if (id.empty()) {
                return false;
            }
            *out = id;
            return true;
#endif
        } catch (...) {
            return false;
        }
    }

    bool networkType(std::string* out) override {
        try {
#ifdef __APPLE__
            // Classify getifaddrs active non-loopback / non-tunnel interface names
            // against the full SCNetworkInterface table (IEEE80211 -> wifi /
            // Ethernet -> ethernet; desktops have no 2g~5g source).
            // With several interfaces up, wifi wins (aligned with the $wifi semantics)
            struct ifaddrs* list = nullptr;
            if (::getifaddrs(&list) != 0) {
                return false;
            }
            std::map<std::string, bool> active;
            for (struct ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
                if (it->ifa_addr == nullptr) {
                    continue;
                }
                const unsigned int flags = it->ifa_flags;
                // IFF_RUNNING must not be skipped: IFF_UP only means administratively
                // enabled; with no link (cable unplugged / no AP association) the
                // running bit is 0 and the interface is not active
                if ((flags & IFF_UP) == 0 || (flags & IFF_RUNNING) == 0 ||
                    (flags & IFF_LOOPBACK) != 0) {
                    continue;
                }
                const std::string name = it->ifa_name != nullptr ? it->ifa_name : "";
                if (name.empty() || isVirtualMacInterfaceName(name)) {
                    continue;
                }
                active[name] = true;
            }
            ::freeifaddrs(list);
            std::map<std::string, std::string> types;
            scInterfaceTypes(&types);
            bool hasWifi = false;
            bool hasWired = false;
            for (const auto& entry : active) {
                const auto type = types.find(entry.first);
                if (type == types.end()) {
                    continue;
                }
                if (type->second == "wifi") {
                    hasWifi = true;
                } else if (type->second == "ethernet") {
                    hasWired = true;
                }
            }
            if (hasWifi || hasWired) {
                *out = hasWifi ? "wifi" : "ethernet";
                return true;
            }
            return false;
#else
            // Active physical interfaces from /sys/class/net: a wireless/ subdirectory
            // means wifi, otherwise ethernet (best-effort).
            // Virtual / tunnel interfaces (VPN / docker / bridges etc.) are excluded —
            // if only those remain, treat as unavailable
            std::string wired;
            if (DIR* dir = ::opendir("/sys/class/net")) {
                while (const dirent* entry = ::readdir(dir)) {
                    const std::string name = entry->d_name;
                    if (name == "." || name == ".." || name == "lo" ||
                        isVirtualLinuxInterfaceName(name)) {
                        continue;
                    }
                    const std::string base = "/sys/class/net/" + name;
                    if (trimCopy(readFile((base + "/operstate").c_str())) == "down") {
                        continue;
                    }
                    struct stat st = {};
                    if (::stat((base + "/wireless").c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                        ::closedir(dir);
                        *out = "wifi";  // wifi wins (consistent with the macOS decision)
                        return true;
                    }
                    wired = "ethernet";
                }
                ::closedir(dir);
            }
            if (wired.empty()) {
                return false;
            }
            *out = wired;
            return true;
#endif
        } catch (...) {
            return false;
        }
    }

    bool region(std::string* out) override {
        try {
#ifdef __APPLE__
            // CFLocale country code (e.g. "CN") -> uppercase normalization (avoids mixing cn/CN)
            CFLocaleRef locale = ::CFLocaleCopyCurrent();
            if (locale == nullptr) {
                return false;
            }
            std::string region;
            const CFTypeRef value = ::CFLocaleGetValue(locale, kCFLocaleCountryCode);
            if (value != nullptr && ::CFGetTypeID(value) == ::CFStringGetTypeID()) {
                char buffer[16] = {0};
                if (::CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer),
                                         kCFStringEncodingUTF8) != 0U) {
                    region = buffer;
                }
            }
            ::CFRelease(locale);
            region = upperAsciiCopy(trimCopy(region));
            if (region.empty()) {
                return false;
            }
            *out = region;
            return true;
#else
            // LANG region segment: "zh_CN.UTF-8" -> "CN"; no region segment means unavailable
            const char* raw = std::getenv("LANG");
            if (raw == nullptr || raw[0] == '\0') {
                return false;
            }
            std::string value(raw);
            const std::size_t dot = value.find('.');
            if (dot != std::string::npos) {
                value.resize(dot);
            }
            const std::size_t underscore = value.find('_');
            if (underscore == std::string::npos || underscore + 1 >= value.size()) {
                return false;
            }
            const std::string region = upperAsciiCopy(trimCopy(value.substr(underscore + 1)));
            if (region.empty()) {
                return false;
            }
            *out = region;
            return true;
#endif
        } catch (...) {
            return false;
        }
    }

   private:
#ifdef __APPLE__
    // Tunnel / virtual interface prefixes: excluded from network type detection
    static bool isVirtualMacInterfaceName(const std::string& name) {
        static const char* const kPrefixes[] = {
            "utun", "awdl", "llw", "bridge", "demux", "pktap",
            "gif",  "stf",  "ap",  "vlan",   "vmnet", "lo",
        };
        return std::any_of(std::begin(kPrefixes), std::end(kPrefixes),
                           [&name](const char* prefix) { return name.rfind(prefix, 0) == 0; });
    }

    // BSD interface name -> network type table (via SCNetworkInterfaceCopyAll;
    // only wifi / ethernet are mapped — desktops have no 2g~5g value source)
    static void scInterfaceTypes(std::map<std::string, std::string>* types) {
        const CFArrayRef all = ::SCNetworkInterfaceCopyAll();
        if (all == nullptr) {
            return;
        }
        const CFIndex count = ::CFArrayGetCount(all);
        for (CFIndex i = 0; i < count; ++i) {
            const SCNetworkInterfaceRef ref =
                static_cast<SCNetworkInterfaceRef>(::CFArrayGetValueAtIndex(all, i));
            const CFStringRef bsd = ::SCNetworkInterfaceGetBSDName(ref);
            const CFStringRef type = ::SCNetworkInterfaceGetInterfaceType(ref);
            if (bsd == nullptr || type == nullptr) {
                continue;
            }
            char buffer[64] = {0};
            if (::CFStringGetCString(bsd, buffer, sizeof(buffer), kCFStringEncodingUTF8) == 0U) {
                continue;
            }
            if (::CFStringCompare(type, kSCNetworkInterfaceTypeIEEE80211, 0) == kCFCompareEqualTo) {
                (*types)[buffer] = "wifi";
            } else if (::CFStringCompare(type, kSCNetworkInterfaceTypeEthernet, 0) ==
                       kCFCompareEqualTo) {
                (*types)[buffer] = "ethernet";
            }
        }
        ::CFRelease(all);
    }

    // The global-coordinate origin is the main display (per CG global coordinate
    // definition); CGMainDisplayID is deprecated since macOS 12 and unusable under
    // -Werror, so derive it from the online display list instead
    static CGDirectDisplayID primaryDisplay() {
        CGDirectDisplayID displays[8];
        uint32_t count = 0;
        if (::CGGetOnlineDisplayList(static_cast<uint32_t>(sizeof(displays) / sizeof(displays[0])),
                                     displays, &count) != kCGErrorSuccess ||
            count == 0) {
            return kCGNullDirectDisplay;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const CGRect bounds = ::CGDisplayBounds(displays[i]);
            if (bounds.origin.x == 0.0 && bounds.origin.y == 0.0) {
                return displays[i];
            }
        }
        return displays[0];
    }

    static bool bundleInfoString(CFStringRef key, std::string* out) {
        try {
            CFBundleRef bundle = ::CFBundleGetMainBundle();
            if (bundle == nullptr) {
                return false;
            }
            const CFTypeRef value = ::CFBundleGetValueForInfoDictionaryKey(bundle, key);
            if (value == nullptr || ::CFGetTypeID(value) != ::CFStringGetTypeID()) {
                return false;
            }
            char buffer[512] = {0};
            if (::CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer),
                                     kCFStringEncodingUTF8) == 0U) {
                return false;
            }
            const std::string text = trimCopy(buffer);
            if (text.empty()) {
                return false;
            }
            *out = text;
            return true;
        } catch (...) {
            return false;
        }
    }
#else
    // Virtual / tunnel interface prefixes: VPN (tun/tap/wg/ppp), containers and
    // virtualization (docker/veth/virbr/vnet/br-/vmnet), and aggregates
    // (bond/team/dummy) are not classified, avoiding a false "ethernet" when only
    // such interfaces remain
    static bool isVirtualLinuxInterfaceName(const std::string& name) {
        static const char* const kPrefixes[] = {"tun",  "tap", "docker", "veth", "virbr",
                                                "vnet", "br-", "vmnet",  "wg",   "bond",
                                                "team", "ppp", "dummy"};
        for (const char* prefix : kPrefixes) {
            if (name.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    }

    static std::string readFile(const char* path) {
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            return std::string();
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // device-tree files often end with a NUL; treat it as string termination
        const std::size_t nul = content.find('\0');
        if (nul != std::string::npos) {
            content.resize(nul);
        }
        return content;
    }

    // Single-key DMI read ($brand / $manufacturer source): missing / empty -> false (best-effort)
    static bool dmiString(const char* path, std::string* out) {
        const std::string value = trimCopy(readFile(path));
        if (value.empty()) {
            return false;
        }
        *out = value;
        return true;
    }
#endif
};

}  // namespace

std::unique_ptr<ISystemInfo> createPlatformSystemInfo() {
    return std::unique_ptr<ISystemInfo>(new PosixSystemInfo());
}

}  // namespace sensorswave
