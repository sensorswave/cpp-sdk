#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "constants.h"
#include "http/http_client.h"
#include "queue/event_queue.h"
#include "sender/track_request.h"
#include "util/clock.h"

namespace sensorswave {

// Background single-threaded batch sender:
// - Triggers (batchSend=true): size-based (>= batchMaxSize) or timer-based
//   (sendIntervalMs since the last send); whichever fires first sends.
//   batchSend=false: every enqueue triggers immediately (both triggers
//   degenerate to per-event sends)
// - Failure classification (success/failure decided solely by HTTP status
//   code; the response body's business code does not participate):
//   network error / timeout (statusCode 0) / 3xx / 5xx / 408 / 429 →
//   retryCount+1, append to the queue tail + exponential backoff
//   (interval × 2^retryCount, capped at 30s); other 4xx → drop the batch +
//   warn; 2xx always succeeds
// - One in-flight send at any moment is guaranteed structurally by the single
//   sender thread; new triggers arriving during a send are recorded via the
//   condvar (a non-empty queue means pending) and re-run right after the
//   current round, so nothing is lost
// - Final drain in three stages: requestStop() (set under the lock,
//   instantaneous) → awaitDrainFinished() (bounded wait) → joinStopped()
//   (join). Facade destroy / terminate paths must call requestStop only under
//   the lock, moving the wait and join to after the outer lock is released —
//   if a host HTTP implementation re-enters SDK APIs from its callback, the
//   callback can take the facade lock and return as a no-op under the closed
//   guard, so the join finishes in bounded time (joining under the lock would
//   deadlock against it)
class BatchSender {
   public:
    // Owns none of its dependencies; sendIntervalMs / batchMaxSize are test
    // injection points for internal constants (not configurable by callers).
    // clock is injected by reference: the clock is a dependency guaranteed to
    // exist for the sender's lifetime (the facade holds it as a
    // FacadeState::clock member), ruling out null pointers at the type level
    BatchSender(EventQueue* queue, IHttpClient* client, const IClock& clock, std::string apiHost,
                std::string sourceToken, bool batchSend, int64_t sendIntervalMs = SEND_INTERVAL_MS,
                int batchMaxSize = BATCH_MAX_SIZE);
    ~BatchSender();

    BatchSender(const BatchSender&) = delete;
    BatchSender& operator=(const BatchSender&) = delete;

    void start();
    // Call after enqueue: wakes the sender thread (per-event trigger / batch
    // count and timer evaluation)
    void notifyEvent();
    bool isFlushing() const { return isFlushing_.load(std::memory_order_acquire); }

    // Opt-Out dispatch gate: while paused no new batches are started and the
    // queue is untouched; a batch already in flight on the network at the
    // moment of the call completes as usual (its events are already dequeued
    // and cannot be recalled). After resume, sending continues from the
    // current queue state. The destroy final drain respects the pause too
    // (a disabled-mode destroy sends nothing)
    void setDispatchPaused(bool paused);
    bool dispatchPaused();

    // Drain stage 1 (safe to call under the facade lock: only sets a flag +
    // notifies, never waits): sets the stop flag and hands down a drain
    // budget (steady wall-clock deadline). Past the deadline the drain loop
    // stops starting new batches — otherwise with the network down, 200
    // events ÷ batch 10 × HTTP_TIMEOUT_MS could drag to minutes, far beyond
    // the caller's blocking bound
    void requestStop(int64_t drainBudgetMs);

    // Drain stage 2: bounded wait for the sender thread to finish the final
    // drain (no join). Returns whether the queue was emptied within the
    // timeout; on timeout events remain queued (their on-disk copies are
    // safe) — this is not a "send success guarantee". Must be called with the
    // outer lock (facade lock) released (see the class comment on the final
    // drain).
    // When called from the sender thread itself (the host re-entering destroy
    // inside execute), only this thread could set the drain-finished flag, so
    // the wait would idle to timeout — return the queue state directly
    bool awaitDrainFinished(int64_t timeoutMs);

    // Drain stage 3: joins the sender thread. After the join, any in-flight
    // HTTP has certainly finished (<= one request timeout). Must be called
    // with the outer lock released; idempotent (no-op when never started /
    // already joined).
    // Returns false when a self-join is detected (the only scenario is the
    // host re-entering destroy inside execute): the thread is detached
    // instead, and the caller must switch to the "keep infrastructure alive"
    // path (keep the queue / client alive until the thread finishes;
    // otherwise run()'s epilogue touches freed objects — UAF)
    bool joinStopped();

    // Combined convenience entry (no outer lock held, e.g. unit tests):
    // requestStop + awaitDrainFinished + joinStopped. Blocking bound ≈
    // timeoutMs + one in-flight HTTP (HTTP_TIMEOUT_MS); returns false in the
    // self-thread re-entry scenario (see joinStopped)
    bool drainAndStop(int64_t timeoutMs);

   private:
    void run();
    bool shouldSendNowLocked(int64_t nowMs) const;
    // Performs one send and handles all three outcomes (success: rewrite /
    // drop: rewrite / retry: requeue at the tail)
    void sendBatch(std::vector<QueuedEvent> batch, int64_t attemptNowMs);
    void handleFailure(std::vector<QueuedEvent> batch, int64_t nowMs);
    // Final drain: deadlineSteadyMs is a steady wall-clock deadline; stop as
    // soon as it passes (no new batches started)
    bool drainFinal(int64_t deadlineSteadyMs);

    int nextBackoffMs(int retryCount) const;

    EventQueue* queue_;
    IHttpClient* client_;
    const IClock& clock_;
    std::string apiHost_;
    std::string sourceToken_;
    bool batchSend_;
    int64_t sendIntervalMs_;
    int batchMaxSize_;

    std::mutex mutex_;
    std::condition_variable cv_;           // trigger / stop wakeups
    std::condition_variable drainDoneCv_;  // destroy final-drain completion notification (distinct
                                           // from the trigger wakeup cv_)
    bool stop_ = false;
    bool drainFinished_ = false;
    // Final drain wall-clock deadline (steady ms): written by requestStop
    // from the budget, read by run()'s drain section; defaults to INT64_MAX
    // (the destructor path has no caller-imposed bound, meaning unlimited —
    // every facade path goes through requestStop first)
    int64_t drainDeadlineSteadyMs_ = INT64_MAX;
    bool paused_ = false;  // Opt-Out dispatch gate; guarded by mutex_
    // Timer baseline: the first timer window starts at start() (>= interval
    // since the "last send")
    int64_t lastSendMs_ = 0;
    std::thread thread_;
    std::atomic<bool> isFlushing_{false};
};

}  // namespace sensorswave
