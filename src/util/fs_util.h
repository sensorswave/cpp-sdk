#pragma once

#include <cstdio>
#include <string>

namespace sensorswave {

// Small filesystem helpers (the convergence point for POSIX / Windows
// differences): shared by FileStore and PersistentQueue so rename / fsync /
// permission semantics cannot drift apart on either side

// Atomic replace: tmp → final.
// - POSIX rename atomically overwrites an existing target by design;
// - Windows UCRT rename also overwrites (old MSVCRT does not): on failure the
//   target is removed and the rename retried once as a compatibility fallback
//   (the non-atomic window exists only on the retry path — a platform
//   limitation).
// On failure any leftover tmp is cleaned up and false is returned
bool renameReplace(const std::string& tmpPath, const std::string& finalPath);

// Flushes the kernel page cache (the caller must have fflushed first):
// POSIX fsync / Windows _commit. Used for power-loss-durable writes of critical
// small data (identity / compliance flag / queue rewrites); returns false on
// failure
bool syncFile(std::FILE* file);

// Tightens permissions to owner read/write (0600). Effective on POSIX; Windows
// has no POSIX permission model, always true
bool restrictPermsOwner(const std::string& path);

// Defensive size caps for persisted files: legitimate content is far smaller
// (the queue holds at most QUEUE_MAX_SIZE events; a single stored value is a
// small JSON). Anything larger is treated as corrupt — a guard against runaway
// reads and allocations driven by corrupt / abnormally bloated files
constexpr std::size_t MAX_QUEUE_FILE_BYTES = 16UL * 1024UL * 1024UL;   // whole sw_queue file
constexpr std::size_t MAX_STORE_VALUE_BYTES = 16UL * 1024UL * 1024UL;  // FileStore single value

}  // namespace sensorswave
