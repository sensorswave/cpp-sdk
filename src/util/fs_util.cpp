#include "util/fs_util.h"

#include <cstdio>
#include <string>

#include <cerrno>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "util/logger.h"

namespace sensorswave {

bool renameReplace(const std::string& tmpPath, const std::string& finalPath) {
    if (std::rename(tmpPath.c_str(), finalPath.c_str()) == 0) {
        return true;
    }
#ifdef _WIN32
    // Compatibility fallback: failure because the target exists (old MSVCRT
    // semantics / the file being held by a scanner) → remove and retry once.
    // UCRT rename can overwrite the target itself, so the normal path never
    // enters this branch
    if (std::remove(finalPath.c_str()) == 0 &&
        std::rename(tmpPath.c_str(), finalPath.c_str()) == 0) {
        return true;
    }
#endif
    const int renameErrno = errno;  // remove() overwrites errno, so the rename
                                    // failure cause is saved first
    std::remove(tmpPath.c_str());   // clean up the tmp leftover so it does not
                                    // pollute the next write
    Logger::warn("rename failed: " + tmpPath + " -> " + finalPath +
                 ", errno: " + std::to_string(renameErrno));
    return false;
}

bool syncFile(std::FILE* file) {
    if (file == nullptr) {
        return false;
    }
#ifdef _WIN32
    return ::_commit(::_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

bool restrictPermsOwner(const std::string& path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    return ::chmod(path.c_str(), 0600) == 0;
#endif
}

}  // namespace sensorswave
