#include "exception/exception_collector.h"

#include <algorithm>
#include <iterator>
#include <typeinfo>
#include <utility>

#include "constants.h"

#ifdef __GNUG__
#include <cxxabi.h>
#include <cstdlib>
#endif

namespace sensorswave {
namespace {

bool containsKeyword(const std::string& path, const std::vector<std::string>& keywords) {
    return std::any_of(keywords.begin(), keywords.end(), [&path](const std::string& keyword) {
        return !keyword.empty() && path.find(keyword) != std::string::npos;
    });
}

// Vendor system directories (default table per platform family): a modulePath
// hit means a third-party frame.
// Windows matches by substring (with case normalization): module paths carry a
// drive-letter prefix (C:\Windows\...), so prefix matching would never hit and
// every system frame would be classified in_app; POSIX paths are always
// absolute, so prefix matching suffices and avoids false hits on deep paths
bool isVendorPath(const std::string& path) {
#ifdef _WIN32
    static const char* const kVendorDirs[] = {"\\windows\\", "/windows/", "program files"};
    std::string lower;
    lower.reserve(path.size());
    for (char c : path) {
        lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    for (const char* dir : kVendorDirs) {
        if (lower.find(dir) != std::string::npos) {
            return true;
        }
    }
    return false;
#elif defined(__APPLE__)
    static const char* const kVendorDirs[] = {"/System/", "/usr/lib/"};
    return std::any_of(std::begin(kVendorDirs), std::end(kVendorDirs),
                       [&path](const char* dir) { return path.find(dir) == 0; });
#else
    // /lib64 and /usr/lib64: the actual glibc install paths on RHEL-family systems
    static const char* const kVendorDirs[] = {"/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/"};
    return std::any_of(std::begin(kVendorDirs), std::end(kVendorDirs),
                       [&path](const char* dir) { return path.find(dir) == 0; });
#endif
}

}  // namespace

bool resolveInApp(const std::string& modulePath, const ExceptionConfig& config) {
    if (containsKeyword(modulePath, config.inAppExcludes)) {
        return false;  // excludes take precedence over includes
    }
    if (containsKeyword(modulePath, config.inAppIncludes)) {
        return true;
    }
    if (modulePath.empty()) {
        return true;  // no module info (statically linked, missing symbols, etc.): treat as in-app
    }
    return !isVendorPath(modulePath);
}

void buildExceptionProperties(PropertyMap* out, const std::string& type, const std::string& message,
                              const std::vector<StackFrame>& frames, const ExceptionConfig& config,
                              const std::string& level) {
    (*out)["$exception_level"] = PropertyValue::fromString(level);  // error / fatal for crashes
    (*out)["$exception_type"] = PropertyValue::fromString(type);
    (*out)["$exception_message"] = PropertyValue::fromString(message);

    if (frames.empty()) {
        return;  // no stack or capture failed: omit $exception_frames, the event is still sent
    }

    // Truncate the frame count (FRAME_LIMIT = 30, keeping the top of the stack)
    const std::size_t count = frames.size() > static_cast<std::size_t>(FRAME_LIMIT)
                                  ? static_cast<std::size_t>(FRAME_LIMIT)
                                  : frames.size();
    std::vector<PropertyMap> frameMaps;
    frameMaps.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        PropertyMap frame;
        // platform is mandatory (the only required frame field); C++ uses "native"
        frame["platform"] = PropertyValue::fromString(EXCEPTION_PLATFORM);
        if (!frames[i].modulePath.empty()) {
            frame["filename"] = PropertyValue::fromString(frames[i].modulePath);
        }
        if (!frames[i].function.empty()) {
            frame["function"] = PropertyValue::fromString(frames[i].function);
        }
        frame["in_app"] = PropertyValue::fromBool(resolveInApp(frames[i].modulePath, config));
        frame["instruction_addr"] = PropertyValue::fromString(frames[i].instructionAddr);
        frameMaps.push_back(std::move(frame));
    }
    (*out)["$exception_frames"] = PropertyValue::fromMapList(std::move(frameMaps));
}

std::string exceptionTypeName(const std::exception& e) {
    try {
        const char* raw = typeid(e).name();
#ifdef __GNUG__
        int status = 0;
        char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
        if (status == 0 && demangled != nullptr) {
            std::string name(demangled);
            std::free(demangled);
            return name;
        }
        if (demangled != nullptr) {
            std::free(demangled);
        }
#endif
        std::string name(raw);
#ifdef _MSC_VER
        // MSVC typeid().name() is already human-readable, but class types carry a
        // class / struct qualification prefix (e.g. "class std::runtime_error") —
        // strip it to match the GNU demangle output and the cross-platform
        // $exception_type shape
        static const char* const kMsvcPrefixes[] = {"class ", "struct "};
        for (const char* prefix : kMsvcPrefixes) {
            const std::string p(prefix);
            if (name.compare(0, p.size(), p) == 0) {
                name.erase(0, p.size());
                break;
            }
        }
#endif
        return name;
    } catch (...) {
        return "unknown";  // defensive catch around typeid: an exception never escapes
    }
}

}  // namespace sensorswave
