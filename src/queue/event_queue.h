#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "queue/persistent_queue.h"
#include "util/clock.h"

namespace sensorswave {

// Thread-safe FIFO event queue (mutex + condvar) with JSONL persistence:
// - Enqueue appends to disk immediately; oldest entry evicted once full (200)
// - Hysteresis compaction: every QUEUE_MAX_SIZE further in-memory evictions
//   (disk lines reach ~2x capacity) triggers one whole-file rewrite, keeping
//   the on-disk line count bounded under high-frequency enqueue instead of
//   growing without bound with appends
// - drainUpTo only removes from memory, never touches disk: if the process is
//   killed mid-send the on-disk data survives and is re-sent after the next
//   startup recovery; callers invoke rewriteDisk() after a successful send
//   and requeueBack() on failure (both do a whole-file rewrite)
// - In-flight batch registration (between drainDue and rewriteDisk /
//   requeueBack): the compaction survivor set includes in-flight entries so
//   their on-disk copies are not wiped (at-least-once)
// - Write-order lock: the [snapshot + write] windows of append and whole-file
//   rewrites are mutually exclusive via the process-wide
//   queueFileWriteMutex(); mutex_ only guards in-memory state —
//   serialization runs under mutex_, the disk write outside it, so a
//   concurrent enqueue blocks on the write-order lock while a rewrite is
//   writing to disk (no events lost; disk writes are not fully decoupled
//   from enqueue)
// - On an empty queue waitForNonEmptyFor blocks on the condvar, so the sender
//   thread never polls
class EventQueue {
   public:
    // Does not own persistent / clock; the constructor recovers from disk
    // (filters expired entries, truncates at the first bad line)
    EventQueue(PersistentQueue* persistent, const IClock* clock);

    // Thread-safe; evicts the oldest entry first once full (200), then enqueues
    void enqueue(Event event);

    std::size_t size();

    // Removes at most maxCount entries from the front (memory only); the
    // caller must call rewriteDisk() after a successful send
    std::vector<QueuedEvent> drainUpTo(std::size_t maxCount);

    // Takes up to maxCount events from the front whose backoff has expired
    // (backoffDueAtMs <= nowMs); not-yet-due entries are skipped and stay in
    // the queue — a failed event must not block subsequent sends (the
    // non-blocking requirement).
    // Taken entries are registered as in-flight (protecting their on-disk
    // copies from compaction) until settled by rewriteDisk / requeueBack;
    // the caller disposes of them by result (success: rewriteDisk /
    // failure: requeueBack / drop: rewriteDisk)
    std::vector<QueuedEvent> drainDue(std::size_t maxCount, int64_t nowMs);

    // Requeues failed entries (retryCount / backoffDueAtMs already updated by
    // the caller) at the tail + whole-file rewrite (re-enqueued at the tail,
    // not the head); expired entries are dropped on save
    void requeueBack(std::vector<QueuedEvent> entries);

    // Rewrites the whole file from the current in-memory queue (call after a
    // successful send); expired entries are dropped on save
    void rewriteDisk();

    // Blocks until the queue is non-empty or the timeout elapses; returns
    // true if currently non-empty
    bool waitForNonEmptyFor(int64_t timeoutMs);

    // Whether any event's backoff has expired (backoffDueAtMs <= nowMs); used
    // by the sender thread to decide on opportunistic retries
    bool hasDueEvent(int64_t nowMs);
    // Whether any entry is immediately sendable (no backoff scheduled, or
    // backoff already expired)
    bool hasSendableEvent(int64_t nowMs);

    // Earliest backoff due time across the queue; returns INT64_MAX when
    // empty (used to compute wait deadlines)
    int64_t earliestBackoffDueMs();

   private:
    // Hysteresis compaction: whole-file rewrite with "in-flight batch ∪
    // in-memory queue" as the survivor set (serialized under mutex_, the
    // write-order lock spans [snapshot + write], see the class comment).
    // In-flight entries are not settled yet, so their on-disk copies must be
    // preserved
    void compactDiskPreservingInFlight();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedEvent> queue_;
    std::deque<QueuedEvent>
        inFlight_;  // entries taken by drainDue, not yet settled via rewriteDisk/requeueBack
    std::size_t evictedSinceCompaction_ =
        0;  // hysteresis counter: one full capacity of evictions triggers a compaction
    PersistentQueue* persistent_;
    const IClock* clock_;
};

}  // namespace sensorswave
