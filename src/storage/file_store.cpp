#include "storage/file_store.h"

#include <cstdio>
#include <string>

#include <cerrno>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

#include "util/fs_util.h"
#include "util/logger.h"

namespace sensorswave {
namespace {

// Keys come from SDK constants (constants.h); defensively reject path
// separators and relative path components here
bool isValidKey(const std::string& key) {
    return !key.empty() && key != "." && key != ".." && key.find('/') == std::string::npos &&
           key.find('\\') == std::string::npos;
}

// Recursively creates a directory (equivalent to mkdir -p); an existing
// directory counts as success
bool ensureDirectory(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    std::string current;
    std::size_t index = 0;
    if (path[0] == '/') {
        current = "/";
        index = 1;
    }

    while (index < path.size()) {
        const std::size_t next = path.find('/', index);
        const std::size_t end = (next == std::string::npos) ? path.size() : next;
        if (end > index) {
            current.append(path, index, end - index);
#ifdef _WIN32
            if (_mkdir(current.c_str()) != 0 && errno != EEXIST) {
                return false;
            }
#else
            if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
                return false;
            }
#endif
            current += '/';
        }
        index = end + 1;
    }

#ifdef _WIN32
    struct _stat st;
    return _stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

}  // namespace

FileStore::FileStore(const std::string& directory) : directory_(directory), valid_(false) {
    valid_ = ensureDirectory(directory_);
    if (!valid_) {
        // Warning log is debug-gated only (silent failure toward the caller)
        Logger::warn("FileStore: cannot create data dir: " + directory_);
    }
}

std::string FileStore::pathFor(const std::string& key) const {
    return directory_ + "/" + key;
}

bool FileStore::read(const std::string& key, std::string* out) {
    if (!valid_ || !isValidKey(key)) {
        return false;
    }

    std::FILE* file = std::fopen(pathFor(key).c_str(), "rb");
    if (file == nullptr) {
        return false;  // key missing (ENOENT etc.); the caller treats it as "no value"
    }

    std::string content;
    char buffer[4096];
    std::size_t chunk = 0;
    // The loop exits when fread returns 0; failure is decided uniformly via
    // ferror, with no reads past the EOF state; the analyzer's "call after
    // EOF / indeterminate position" warning on this standard read-loop idiom
    // is a false positive
    // NOLINTNEXTLINE(clang-analyzer-unix.Stream)
    while ((chunk = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        content.append(buffer, chunk);
        if (content.size() > MAX_STORE_VALUE_BYTES) {
            // Oversize is treated as a corrupt key: normal values are small
            // JSON, growth can only come from disk anomalies / external
            // tampering; this cuts off the memory risk of unbounded reads
            std::fclose(file);
            Logger::warn("FileStore: value oversize, treated as corrupt: " + key);
            return false;
        }
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);

    if (!ok) {
        Logger::warn("FileStore: read failed: " + key);
        return false;
    }

    if (out != nullptr) {
        *out = content;
    }
    return true;
}

bool FileStore::write(const std::string& key, const std::string& value) {
    if (!valid_ || !isValidKey(key)) {
        return false;
    }

    // Atomic write: tmp file + rename
    const std::string finalPath = pathFor(key);
    const std::string tmpPath = finalPath + ".tmp";
    std::FILE* file = std::fopen(tmpPath.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    const std::size_t written = std::fwrite(value.data(), 1, value.size(), file);
    // fsync: values are critical small data (identity / compliance flags) and
    // writes are infrequent — "successful return = survives power loss" is
    // worth the cost
    const bool ok = written == value.size() && std::fflush(file) == 0 && syncFile(file);
    std::fclose(file);

    if (!ok) {
        std::remove(tmpPath.c_str());
        Logger::warn("FileStore: write failed: " + key);
        return false;
    }

    // fopen defaults to 0666 & ~umask; tighten explicitly to 0600 (Windows
    // has no POSIX permission model, always true there)
    if (!restrictPermsOwner(tmpPath)) {
        std::remove(tmpPath.c_str());
        Logger::warn("FileStore: chmod failed: " + key);
        return false;
    }

    // Convergence point for platform rename semantics: POSIX atomic replace;
    // Windows UCRT replace + remove-and-retry fallback on failure
    if (!renameReplace(tmpPath, finalPath)) {
        Logger::warn("FileStore: rename failed: " + key);
        return false;
    }
    return true;
}

bool FileStore::remove(const std::string& key) {
    if (!valid_ || !isValidKey(key)) {
        return false;
    }
    std::remove(pathFor(key).c_str());  // a missing key also counts as success (idempotent)
    return true;
}

}  // namespace sensorswave
