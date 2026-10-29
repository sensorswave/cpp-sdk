// Platform system info implementation: Windows (WIN32 builds only).
// Per-key best-effort: any failed system call returns false and the key is left
// out of the preset properties.

#include "platform/system_info.h"

#ifdef _WIN32

// winsock2.h must precede windows.h: IP_ADAPTER_ADDRESSES / GAA_FLAG_* live in
// the #ifdef _WINSOCK2API_ section of IPTypes.h, and only winsock2.h defines that
// macro. Normally windows.h includes winsock2.h itself, but with some broken
// SDKs the selection block degrades to winsock.h and the declarations disappear
// entirely. winsock2.h defines both _WINSOCK2API_ and _WINSOCKAPI_, so once it
// comes first nothing depends on windows.h's selection block; in the other order
// winsock.h first trips "#error WinSock.h has already been included" in ws2def.h.
// Each header in its own block: with IncludeBlocks: Preserve only within-block
// sorting applies, so SortIncludes cannot restore alphabetical order
#include <winsock2.h>

#include <windows.h>

#include <iphlpapi.h>
// iprtrmfc.h ships only with MinGW-w64; the MSVC SDK lacks it (C1083). This file
// only uses GetAdaptersAddresses and gets IP_ADAPTER_ADDRESSES from iphlpapi.h;
// this include is kept solely for MinGW compatibility
#if defined(__MINGW32__)
#include <iprtrmfc.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

// iphlpapi.lib is linked by the platform library section of src/CMakeLists.txt
// (no #pragma comment: -Werror rejects unknown pragmas under MinGW / Clang-Windows)

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

class WindowsSystemInfo : public ISystemInfo {
   public:
    bool osName(std::string* out) override {
        *out = "Windows";
        return true;
    }

    bool osVersion(std::string* out) override {
        try {
            // Resolve RtlGetVersion dynamically: VerifyVersionInfo-family APIs lie
            // about the version depending on the manifest compatibility declaration;
            // querying ntdll directly is unaffected
            using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (ntdll == nullptr) {
                return false;
            }
            const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
            if (rtlGetVersion == nullptr) {
                return false;
            }
            RTL_OSVERSIONINFOW info = {};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtlGetVersion(&info) != 0) {
                return false;
            }
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%lu.%lu.%lu",
                          static_cast<unsigned long>(info.dwMajorVersion),
                          static_cast<unsigned long>(info.dwMinorVersion),
                          static_cast<unsigned long>(info.dwBuildNumber));
            *out = buffer;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool model(std::string* out) override {
        try {
            // Machine product name from the registry SystemInformation key
            // (present on OEM and self-built machines alike)
            return regString("SYSTEM\\CurrentControlSet\\Control\\SystemInformation",
                             "SystemProductName", out);
        } catch (...) {
            return false;
        }
    }

    bool screenWidth(int* out) override {
        try {
            // Primary display resolution (session 0 / service processes return 0 -> unavailable)
            const int width = ::GetSystemMetrics(SM_CXSCREEN);
            if (width <= 0) {
                return false;
            }
            *out = width;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool screenHeight(int* out) override {
        try {
            const int height = ::GetSystemMetrics(SM_CYSCREEN);
            if (height <= 0) {
                return false;
            }
            *out = height;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool timezoneOffsetSeconds(int64_t* out) override {
        try {
            // _get_timezone returns west-positive seconds (UTC-8 -> +28800), so negate
            // for the east-positive convention.
            // Note: it returns the standard-time offset and does not follow DST
            // (an acceptable difference from POSIX tm_gmtoff)
            long offsetSeconds = 0;
            if (_get_timezone(&offsetSeconds) != 0) {
                return false;
            }
            *out = static_cast<int64_t>(-offsetSeconds);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool language(std::string* out) override {
        try {
            // User default locale (e.g. "zh-CN", RFC 4646 form; no encoding suffix by nature)
            wchar_t wide[LOCALE_NAME_MAX_LENGTH] = {0};
            const int length = ::GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH);
            if (length <= 0) {
                return false;
            }
            std::string value;
            if (!wideToUtf8(wide, &value)) {
                return false;
            }
            value = trimCopy(value);
            if (value.empty()) {
                return false;
            }
            *out = value;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool appId(std::string* out) override {
        (void)out;
        // Windows has no bundle-identifier equivalent; hosts can override it via a common property
        return false;
    }

    bool appName(std::string* out) override { return versionResourceString("ProductName", out); }

    bool appVersion(std::string* out) override { return versionResourceString("FileVersion", out); }

    bool brand(std::string* out) override {
        // At the desktop-machine dimension, same source as manufacturer (SystemManufacturer)
        return manufacturer(out);
    }

    bool manufacturer(std::string* out) override {
        try {
            return regString("SYSTEM\\CurrentControlSet\\Control\\SystemInformation",
                             "SystemManufacturer", out);
        } catch (...) {
            return false;
        }
    }

    bool deviceId(std::string* out) override {
        try {
            // MachineGuid (generated at system install, read-only; the SDK neither
            // generates nor persists it)
            return regString("SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", out);
        } catch (...) {
            return false;
        }
    }

    bool networkType(std::string* out) override {
        try {
            // Classify by GetAdaptersAddresses IfType: active non-loopback adapters
            // only; 71 IEEE80211 -> wifi, 6 ETHERNET_CSMACD -> ethernet,
            // 243/244 WWAN -> cellular; no 2g~5g breakdown on desktops, so none is
            // produced.
            // Collect the three flags first: adapter enumeration order is not
            // deterministic, so gather all flags and only then apply the
            // wifi > ethernet > cellular priority, preventing ethernet from
            // preempting wifi merely by enumerating earlier
            const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
            ULONG size = 0;
            if (::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size) !=
                    ERROR_BUFFER_OVERFLOW ||
                size == 0) {
                return false;
            }
            std::vector<char> buffer(size, '\0');
            auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            if (::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) != NO_ERROR) {
                return false;
            }
            bool hasWifi = false;
            bool hasWired = false;
            bool hasCellular = false;
            for (const IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr;
                 adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp ||
                    adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                    continue;
                }
                if (adapter->IfType == IF_TYPE_IEEE80211) {
                    hasWifi = true;
                } else if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) {
                    hasWired = true;
                } else if (adapter->IfType == 243 || adapter->IfType == 244) {  // WWANPP/WWANPP2P
                    hasCellular = true;
                }
            }
            if (hasWifi) {
                *out = "wifi";
                return true;
            }
            if (hasWired) {
                *out = "ethernet";
                return true;
            }
            if (hasCellular) {
                *out = "cellular";
                return true;
            }
            return false;
        } catch (...) {
            return false;
        }
    }

    bool region(std::string* out) override {
        try {
            // User geo country code (ISO 3166-1 alpha-2, e.g. "CN") -> uppercase normalization
            wchar_t wide[8] = {0};
            const int length = ::GetUserDefaultGeoName(wide, 8);
            if (length <= 1) {  // the count includes the trailing NUL; only a NUL means unavailable
                return false;
            }
            std::string value;
            if (!wideToUtf8(wide, &value)) {
                return false;
            }
            value = upperAsciiCopy(trimCopy(value));
            if (value.empty()) {
                return false;
            }
            *out = value;
            return true;
        } catch (...) {
            return false;
        }
    }

   private:
    // Single-key HKLM registry string read (source for $model / $manufacturer /
    // $device_id): missing / empty -> false (best-effort semantics)
    static bool regString(const char* subKey, const char* valueName, std::string* out) {
        char buffer[512];
        DWORD size = sizeof(buffer);
        const LSTATUS status = ::RegGetValueA(HKEY_LOCAL_MACHINE, subKey, valueName, RRF_RT_REG_SZ,
                                              nullptr, buffer, &size);
        if (status != ERROR_SUCCESS) {
            return false;
        }
        buffer[sizeof(buffer) - 1] = '\0';
        const std::string value = trimCopy(buffer);
        if (value.empty()) {
            return false;
        }
        *out = value;
        return true;
    }

    // UTF-16 -> UTF-8 (the single conversion exit for Windows API strings; -1
    // counts the trailing NUL, so the result is naturally terminated)
    static bool wideToUtf8(const wchar_t* wide, std::string* out) {
        const int sizeBytes =
            ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (sizeBytes <= 0) {
            return false;
        }
        std::vector<char> buffer(static_cast<std::size_t>(sizeBytes), '\0');
        if (::WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer.data(), sizeBytes, nullptr,
                                  nullptr) <= 0) {
            return false;
        }
        *out = buffer.data();
        return true;
    }

    // Executable version-resource string table (locates the actual language/code
    // page via VarFileInfo\\Translation)
    static bool versionResourceString(const char* field, std::string* out) {
        try {
            char modulePath[MAX_PATH] = {0};
            if (::GetModuleFileNameA(nullptr, modulePath, MAX_PATH) == 0) {
                return false;
            }
            const DWORD size = ::GetFileVersionInfoSizeA(modulePath, nullptr);
            if (size == 0) {
                return false;
            }
            std::vector<char> data(size, '\0');
            if (!::GetFileVersionInfoA(modulePath, 0, size, data.data())) {
                return false;
            }
            struct LangCodePage {
                WORD language;
                WORD codePage;
            };
            LangCodePage* translation = nullptr;
            UINT translationLen = 0;
            if (!::VerQueryValueA(data.data(), "\\VarFileInfo\\Translation",
                                  reinterpret_cast<LPVOID*>(&translation), &translationLen) ||
                translation == nullptr || translationLen < sizeof(LangCodePage)) {
                return false;
            }
            char query[256];
            std::snprintf(query, sizeof(query), "\\StringFileInfo\\%04x%04x\\%s",
                          translation[0].language, translation[0].codePage, field);
            char* value = nullptr;
            UINT valueLen = 0;
            if (!::VerQueryValueA(data.data(), query, reinterpret_cast<LPVOID*>(&value),
                                  &valueLen) ||
                value == nullptr || valueLen == 0) {
                return false;
            }
            const std::string text = trimCopy(value);
            if (text.empty()) {
                return false;
            }
            *out = text;
            return true;
        } catch (...) {
            return false;
        }
    }
};

}  // namespace

std::unique_ptr<ISystemInfo> createPlatformSystemInfo() {
    return std::unique_ptr<ISystemInfo>(new WindowsSystemInfo());
}

}  // namespace sensorswave

#endif  // _WIN32
