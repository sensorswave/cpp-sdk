#include "queue/persistent_queue.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "constants.h"
#include "event/event_json.h"
#include "util/fs_util.h"
#include "util/logger.h"

namespace sensorswave {
namespace {

// One-line envelope → JSON string. dump uses the replace strategy: envelope
// content comes from an already-serialized event (valid UTF-8), so this is
// defense in depth — no string on the persistence path may ever make enqueue
// throw (otherwise the event is already in the in-memory queue while its send
// trigger loses it; see the enqueue ordering comment)
std::string envelopeToJson(const QueuedEvent& entry) {
    nlohmann::json root;
    root["time"] = entry.time;
    root["retry_count"] = entry.retryCount;
    root["event"] = nlohmann::json::parse(serializeEventJson(entry.event));
    return root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// One-line JSON → envelope; returns false on structural mismatch (bad line)
bool envelopeFromJson(const std::string& line, QueuedEvent* out) {
    try {
        nlohmann::json root = nlohmann::json::parse(line);
        if (!root.is_object() || !root.contains("time") || !root["time"].is_number() ||
            !root.contains("retry_count") || !root["retry_count"].is_number() ||
            !root.contains("event") || !root["event"].is_object()) {
            return false;
        }
        out->time = root["time"].get<int64_t>();
        out->retryCount = root["retry_count"].get<int>();
        return deserializeEventJson(root["event"].dump(), &out->event);
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace

PersistentQueue::PersistentQueue(std::string filePath) : filePath_(std::move(filePath)) {}

std::mutex& queueFileWriteMutex() {
    // Function-local static: C++11 magic static, process-wide singleton
    // (including across instances, see the header comment)
    static std::mutex mutex;
    return mutex;
}

std::vector<QueuedEvent> PersistentQueue::load(int64_t nowMs) {
    std::vector<QueuedEvent> survivors;
    bool truncated = false;

    // Write-order lock: load's clear / rewrite changes are likewise mutually
    // exclusive with concurrent appends (across instances, the EventQueue
    // constructor may race with a rewrite by a kept-alive old instance)
    std::unique_lock<std::mutex> ioLock(queueFileWriteMutex());

    // File-level defense: an oversize file is treated as corrupt — clear and
    // rewrite, without reading line by line. A legitimate queue holds at most
    // QUEUE_MAX_SIZE events, far below MAX_QUEUE_FILE_BYTES; exceeding the
    // cap can only mean disk anomalies / external tampering, and this also
    // avoids the memory risk of unbounded getline reads
    {
        std::ifstream sizeCheck(filePath_.c_str(), std::ios::binary);
        if (sizeCheck.is_open()) {
            sizeCheck.seekg(0, std::ios::end);
            const std::streamoff bytes = sizeCheck.tellg();
            sizeCheck.close();
            if (bytes > static_cast<std::streamoff>(MAX_QUEUE_FILE_BYTES)) {
                Logger::warn("queue file oversize, treated as corrupt: " + filePath_);
                writeContent("");
                return {};
            }
        }
    }

    // The read handle is closed explicitly before any rewrite: on Windows an
    // open file cannot be renamed/removed (CRT streams lack
    // FILE_SHARE_DELETE; POSIX has no such restriction), so a truncation /
    // expiry rewrite while in is still alive would silently fail with a
    // sharing violation.
    // binary mode matches sizeCheck / append / writeContent (the write side
    // is always binary)
    {
        std::ifstream in(filePath_.c_str(), std::ios::binary);
        if (in.is_open()) {
            std::string line;
            while (std::getline(in, line)) {
                QueuedEvent entry;
                // Empty lines / parse failures / structural mismatches are
                // all bad lines: stop at the first and discard the rest
                if (line.empty() || !envelopeFromJson(line, &entry)) {
                    truncated = true;
                    Logger::warn("queue file has invalid line, truncating: " + filePath_);
                    break;
                }
                survivors.push_back(std::move(entry));
            }
        }
    }

    // 7-day expiry filter
    const int64_t expiryMs = static_cast<int64_t>(EVENT_EXPIRY_DAYS) * 86400LL * 1000LL;
    std::vector<QueuedEvent> kept;
    kept.reserve(survivors.size());
    bool expiredDropped = false;
    for (QueuedEvent& entry : survivors) {
        if (nowMs - entry.time > expiryMs) {
            expiredDropped = true;
            continue;
        }
        kept.push_back(std::move(entry));
    }

    // Beyond 200 entries keep the newest 200
    bool cappedDropped = false;
    if (kept.size() > static_cast<std::size_t>(QUEUE_MAX_SIZE)) {
        const std::size_t dropCount = kept.size() - static_cast<std::size_t>(QUEUE_MAX_SIZE);
        kept.erase(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(dropCount));
        cappedDropped = true;
    }

    // Rewrite the whole file on any change: clears torn remnants and expired
    // entries (write-order lock already held, via rewriteLocked)
    if (truncated || expiredDropped || cappedDropped) {
        rewriteLocked(kept);
    }
    return kept;
}

bool PersistentQueue::append(const QueuedEvent& entry) {
    std::ofstream out(filePath_.c_str(), std::ios::app | std::ios::binary);
    if (!out.is_open()) {
        Logger::warn("queue append failed (cannot open): " + filePath_);
        return false;
    }
    out << envelopeToJson(entry) << "\n";
    out.flush();  // flush on every enqueue; no per-entry fsync (appends are frequent, the fsync
                  // cost does not match)
    if (!out.good()) {
        Logger::warn("queue append failed (write error): " + filePath_);
        return false;
    }
    if (!permsRestricted_) {
        // Tighten to 0600 after the first successful write (the queue holds
        // user events); older versions may have left a file with loose
        // permissions, corrected here as well. On failure only the tightening
        // is skipped; the persistence main path is unaffected
        permsRestricted_ = restrictPermsOwner(filePath_);
    }
    return true;
}

std::string PersistentQueue::serializeEntries(const std::vector<QueuedEvent>& entries) {
    std::string content;
    for (const QueuedEvent& entry : entries) {
        content += envelopeToJson(entry);
        content += '\n';
    }
    return content;
}

bool PersistentQueue::writeContent(const std::string& content) {
    const std::string tmpPath = filePath_ + ".tmp";
    std::FILE* out = std::fopen(tmpPath.c_str(), "wb");
    if (out == nullptr) {
        Logger::warn("queue rewrite failed (cannot open tmp): " + tmpPath);
        return false;
    }
    bool ok =
        content.empty() || std::fwrite(content.data(), 1, content.size(), out) == content.size();
    // fsync: the rewrite is the queue's only compaction / cleanup point, so
    // "successful return = data is durable" holds; losing one rewrite to a
    // power cut could be reconverged by the next load, but there is no need
    // to lose it
    ok = ok && std::fflush(out) == 0 && syncFile(out);
    if (std::fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        std::remove(tmpPath.c_str());
        Logger::warn("queue rewrite failed (write): " + tmpPath);
        return false;
    }
    if (!restrictPermsOwner(tmpPath)) {  // 0600: same policy as FileStore
        std::remove(tmpPath.c_str());
        Logger::warn("queue rewrite failed (chmod): " + tmpPath);
        return false;
    }
    if (!renameReplace(tmpPath,
                       filePath_)) {  // single convergence point for platform rename differences
        Logger::warn("queue rewrite failed (rename): " + filePath_);
        return false;
    }
    return true;
}

bool PersistentQueue::rewrite(const std::vector<QueuedEvent>& entries) {
    // The public entry takes the write-order lock itself: callers bypassing
    // EventQueue could otherwise recreate the append/rewrite interleaving;
    // must not be called while already holding queueFileWriteMutex()
    // (self-deadlock)
    std::unique_lock<std::mutex> ioLock(queueFileWriteMutex());
    return rewriteLocked(entries);
}

bool PersistentQueue::rewriteLocked(const std::vector<QueuedEvent>& entries) {
    return writeContent(serializeEntries(entries));
}

}  // namespace sensorswave
