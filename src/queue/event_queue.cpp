#include "queue/event_queue.h"

#include <algorithm>
#include <utility>

#include "constants.h"
#include "util/logger.h"

namespace sensorswave {
namespace {

// 7-day expiry check: same formula as PersistentQueue::load
bool entryExpired(const QueuedEvent& entry, int64_t nowMs) {
    const int64_t expiryMs = static_cast<int64_t>(EVENT_EXPIRY_DAYS) * 86400LL * 1000LL;
    return nowMs - entry.time > expiryMs;
}

}  // namespace

EventQueue::EventQueue(PersistentQueue* persistent, const IClock* clock)
    : persistent_(persistent), clock_(clock) {
    // Startup recovery: bad-line truncation / expiry cleanup / cap trimming
    // all happen inside load
    const int64_t nowMs = clock_ != nullptr ? clock_->nowMs() : 0;
    const std::vector<QueuedEvent> recovered = persistent_->load(nowMs);
    queue_.assign(recovered.begin(), recovered.end());
}

void EventQueue::enqueue(Event event) {
    QueuedEvent entry;
    entry.time = clock_ != nullptr ? clock_->nowMs() : 0;
    entry.retryCount = 0;
    entry.event = std::move(event);

    // Persist to disk before inserting into memory (append runs under the
    // write-order lock): append must be mutually exclusive with the
    // [snapshot + write] window of whole-file rewrites, otherwise an event
    // landing in the "after snapshot, before rename" window is already
    // confirmed on disk yet gets overwritten by the stale snapshot
    // (breaking at-least-once).
    // append must not run while holding mutex_: the rewrite path's lock order
    // is write-order lock → mutex_, so nesting [mutex_ → write-order lock]
    // would form a cycle. Swapping the order does not change semantics — an
    // append failure still degrades to memory-only; if the process crashes
    // after a successful append, the event is on disk but not in memory and
    // is re-sent by the next load (acceptable).
    // Accepted trade-off: append does not hold mutex_, so the on-disk order
    // of concurrent enqueues can interleave with the in-memory enqueue order
    // — this only affects replay order after crash recovery; both events are
    // still re-sent (at-least-once holds; analytics events carry no
    // cross-event ordering requirement), and preserving order would mean
    // doing file IO under mutex_, which is not worth the cost
    {
        std::unique_lock<std::mutex> ioLock(queueFileWriteMutex());
        // An append exception (e.g. allocation failure while serializing the
        // envelope) is handled the same as a false return: degrade to
        // memory-only — an exception must never lose an event from both
        // memory and disk
        try {
            persistent_->append(entry);
        } catch (...) {
            Logger::warn("queue append threw; event degraded to memory-only");
        }
    }

    bool compact = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= static_cast<std::size_t>(QUEUE_MAX_SIZE)) {
            queue_.pop_front();  // full at 200: evict oldest
            // Hysteresis compaction counter: disk lines ≈ capacity +
            // evictions; every further eviction of one capacity (lines ≈ 2x
            // capacity) triggers one compaction, run under the write-order
            // lock (without mutex_)
            if (++evictedSinceCompaction_ >= static_cast<std::size_t>(QUEUE_MAX_SIZE)) {
                evictedSinceCompaction_ = 0;
                compact = true;
            }
        }
        queue_.push_back(std::move(entry));
        cv_.notify_all();
    }
    if (compact) {
        compactDiskPreservingInFlight();
    }
}

std::size_t EventQueue::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::vector<QueuedEvent> EventQueue::drainUpTo(std::size_t maxCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QueuedEvent> batch;
    batch.reserve(maxCount < queue_.size() ? maxCount : queue_.size());
    while (!queue_.empty() && batch.size() < maxCount) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return batch;
}

void EventQueue::requeueBack(std::vector<QueuedEvent> entries) {
    std::string content;
    // Write-order lock spans [snapshot + write]: closes the append insertion
    // window; disk IO still happens without mutex_ (concurrent enqueues /
    // drains are not blocked by the write). Lock order is always
    // write-order lock → mutex_
    std::unique_lock<std::mutex> ioLock(queueFileWriteMutex());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The batch is back in the in-memory queue: release the in-flight
        // registration
        inFlight_.clear();
        const int64_t nowMs = clock_ != nullptr ? clock_->nowMs() : 0;
        for (QueuedEvent& entry : entries) {
            if (entryExpired(entry, nowMs)) {
                continue;  // drop expired entries on save (cleaned on load and on save)
            }
            queue_.push_back(std::move(entry));  // failed events append to the tail, not the head
        }
        // Only serialization under the lock; the disk write happens without
        // mutex_
        const std::vector<QueuedEvent> snapshot(queue_.begin(), queue_.end());
        content = persistent_->serializeEntries(snapshot);
    }
    persistent_->writeContent(content);  // whole-file rewrite for requeued retries
}

std::vector<QueuedEvent> EventQueue::drainDue(std::size_t maxCount, int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QueuedEvent> due;
    std::deque<std::size_t>
        skip;  // indices of entries kept in the queue (original order preserved)
    // Every entry must be visited: sendable entries past maxCount must also
    // be marked as kept, otherwise an early loop exit would silently drop
    // unvisited events
    for (std::size_t i = 0; i < queue_.size(); ++i) {
        const bool sendable = queue_[i].backoffDueAtMs == 0 || queue_[i].backoffDueAtMs <= nowMs;
        if (sendable && due.size() < maxCount) {
            inFlight_.push_back(queue_[i]);  // in-flight registration: compaction must keep the
                                             // on-disk copy until settled
            due.push_back(std::move(queue_[i]));
        } else {
            skip.push_back(i);
        }
    }
    std::deque<QueuedEvent> remaining;
    for (std::size_t i = 0; i < queue_.size(); ++i) {
        if (!skip.empty() && skip.front() == i) {
            remaining.push_back(std::move(queue_[i]));
            skip.pop_front();
        }
    }
    queue_.swap(remaining);
    return due;
}

void EventQueue::rewriteDisk() {
    std::string content;
    // Write-order lock spans [snapshot + write], same as requeueBack
    std::unique_lock<std::mutex> ioLock(queueFileWriteMutex());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The batch is settled (success / drop): release the in-flight
        // registration
        inFlight_.clear();
        // Drop expired entries on save: memory and disk are purged in sync —
        // events accumulated by a long-running process must not get written
        // back to disk by a rewrite (expiry filtering on load alone) and then
        // be sent anyway
        const int64_t nowMs = clock_ != nullptr ? clock_->nowMs() : 0;
        std::deque<QueuedEvent> kept;
        for (QueuedEvent& entry : queue_) {
            if (!entryExpired(entry, nowMs)) {
                kept.push_back(std::move(entry));
            }
        }
        queue_.swap(kept);
        // Only serialization under the lock; the disk write happens outside
        // it and does not block concurrent enqueues
        const std::vector<QueuedEvent> snapshot(queue_.begin(), queue_.end());
        content = persistent_->serializeEntries(snapshot);
    }
    persistent_->writeContent(content);  // whole-file rewrite after a successful send
}

void EventQueue::compactDiskPreservingInFlight() {
    std::string content;
    // Write-order lock spans [snapshot + write], same as requeueBack
    std::unique_lock<std::mutex> ioLock(queueFileWriteMutex());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t nowMs = clock_ != nullptr ? clock_->nowMs() : 0;
        // Survivor set = in-flight batch ∪ in-memory queue (entries are
        // copied, no real state is mutated — compaction only converges the
        // on-disk view). The in-flight batch has been taken but not settled,
        // so its on-disk copy must be preserved; otherwise a crash after the
        // compaction loses that batch (breaking at-least-once)
        std::vector<QueuedEvent> snapshot;
        snapshot.reserve(inFlight_.size() + queue_.size());
        for (const QueuedEvent& entry : inFlight_) {
            if (!entryExpired(entry, nowMs)) {
                snapshot.push_back(entry);
            }
        }
        for (const QueuedEvent& entry : queue_) {
            if (!entryExpired(entry, nowMs)) {
                snapshot.push_back(entry);
            }
        }
        content = persistent_->serializeEntries(snapshot);
    }
    persistent_->writeContent(content);
}

bool EventQueue::waitForNonEmptyFor(int64_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !queue_.empty(); });
    return !queue_.empty();
}

bool EventQueue::hasDueEvent(int64_t nowMs) {
    // Only count events with scheduled backoff (backoffDueAtMs != 0):
    // ordinary events are decided by the size / timer triggers and must not
    // bypass the batching condition just because they carry no backoff
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(queue_.begin(), queue_.end(), [nowMs](const QueuedEvent& entry) {
        return entry.backoffDueAtMs != 0 && entry.backoffDueAtMs <= nowMs;
    });
}

bool EventQueue::hasSendableEvent(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(queue_.begin(), queue_.end(), [nowMs](const QueuedEvent& entry) {
        return entry.backoffDueAtMs == 0 || entry.backoffDueAtMs <= nowMs;
    });
}

int64_t EventQueue::earliestBackoffDueMs() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t earliest = INT64_MAX;
    for (const QueuedEvent& entry : queue_) {
        if (entry.backoffDueAtMs != 0 && entry.backoffDueAtMs < earliest) {
            earliest = entry.backoffDueAtMs;
        }
    }
    return earliest;
}

}  // namespace sensorswave
