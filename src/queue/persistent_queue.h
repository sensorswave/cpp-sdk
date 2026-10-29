#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "event/event_builder.h"

namespace sensorswave {

// Queue envelope: one sw_queue JSONL line, format {time, retry_count, event}.
// time is the enqueue timestamp in ms (basis for the 7-day expiry); retryCount
// is the cumulative retry count for the entry; backoffDueAtMs is the runtime
// backoff due time and is not persisted — 0 means no backoff scheduled
// (ordinary events / entries recovered after a restart are treated as
// sendable, decided by the size / timer triggers).
struct QueuedEvent {
    int64_t time = 0;
    int retryCount = 0;
    int64_t backoffDueAtMs = 0;
    Event event;
};

// JSONL persistent queue storage:
// - Enqueue appends one line + flush, without a per-entry fsync (appends are
//   frequent; durability is guaranteed by the whole-file rewrite's fsync and
//   the enqueue ordering); crash recovery trusts whatever reached disk
// - Load: parse line by line, stop at the first bad line — truncate that line
//   and everything after it, with a warning (a torn final line is the only
//   common corruption shape, so the prefix is kept); a file over
//   MAX_QUEUE_FILE_BYTES is treated as corrupt and cleared; expired entries
//   are filtered; beyond QUEUE_MAX_SIZE entries the newest are kept
// - Whole-file rewrite of the survivor set (atomic: tmp file + rename, fsync
//   before the rename, mode 0600), clearing expired entries and torn
//   remnants
// - serializeEntries / writeContent split pair: supports "serialize under the
//   lock, write to disk outside it", so callers never do file IO while
//   holding the lock
class PersistentQueue {
   public:
    virtual ~PersistentQueue() =
        default;  // base-class convention required by the virtual writeContent (test injection)
    explicit PersistentQueue(std::string filePath);

    // Recovers the queue content; returns surviving entries (expired filtered,
    // bad lines truncated)
    std::vector<QueuedEvent> load(int64_t nowMs);

    // Appends one entry to the end of the file; a false return means
    // persisting failed (the in-memory queue is unaffected; the event
    // degrades to memory-only); warns internally. Virtual only for test
    // injection (same base-class convention as writeContent)
    virtual bool append(const QueuedEvent& entry);

    // Serializes entries to JSONL text (pure in-memory, safe to call under a
    // lock; touches no members, hence static)
    static std::string serializeEntries(const std::vector<QueuedEvent>& entries);

    // Atomically rewrites the whole file with content (tmp + fsync + rename,
    // mode 0600). Usually paired with serializeEntries: serialize under the
    // lock, write to disk outside it.
    // The caller must hold queueFileWriteMutex() (virtual only as a
    // test-injection blocking point)
    virtual bool writeContent(const std::string& content);

    // Rewrites the whole file with entries (atomic replace); called after a
    // successful send / when failed entries are requeued. Takes
    // queueFileWriteMutex() internally — must not be called while already
    // holding it (self-deadlock); lock-holding callers (load) go through
    // rewriteLocked
    bool rewrite(const std::vector<QueuedEvent>& entries);

   private:
    // Whole-file rewrite assuming the write-order lock is already held
    // (load's truncation / expiry convergence path)
    bool rewriteLocked(const std::vector<QueuedEvent>& entries);

    std::string filePath_;
    bool permsRestricted_ = false;  // one-shot flag: file permissions tightened to 0600 after the
                                    // first successful append
};

// Queue file write-order lock (process-wide): serializes every change to the
// same sw_queue file — append and the [snapshot + write] window of whole-file
// rewrites are mutually exclusive, otherwise an append landing in the "after
// rewrite snapshot, before rename" window loses an already-persisted event to
// the stale snapshot (breaking at-least-once). Process-wide rather than
// per-instance: the old instance kept alive across destroy and the instance
// created by a new init point at the same path (cross-instance writes must
// also be mutually exclusive, to prevent concurrent tmp truncation /
// out-of-order renames). Lock order convention: write-order lock →
// EventQueue::mutex_ (append runs outside the mutex_ scope, no reverse nesting)
std::mutex& queueFileWriteMutex();

}  // namespace sensorswave
