#include "sender/batch_sender.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

#include "constants.h"
#include "event/event_json.h"
#include "util/logger.h"

namespace sensorswave {
namespace {

// Failure classification (aligned with the JS SDK batch-sender): success and
// retry are decided solely by HTTP status code; the response body (including
// its business code field) does not participate — 2xx → success; statusCode 0
// (network error/timeout), 3xx, 5xx, 408, 429 → retry; other 4xx → drop
// (resending will not succeed)
enum class SendOutcome : std::uint8_t { kSuccess, kRetry, kDrop };

SendOutcome classifyResponse(const HttpResponse& response) {
    if (!response.ok()) {
        if (response.statusCode >= 400 && response.statusCode < 500 && response.statusCode != 408 &&
            response.statusCode != 429) {
            return SendOutcome::kDrop;
        }
        return SendOutcome::kRetry;  // statusCode 0 (network error/timeout), 3xx, 5xx, 408, 429
    }
    return SendOutcome::kSuccess;
}

// The drain deadline uses the real wall clock (steady): IClock is an
// injectable mock business clock and must not carry blocking budgets
int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

BatchSender::BatchSender(EventQueue* queue, IHttpClient* client, const IClock& clock,
                         std::string apiHost, std::string sourceToken, bool batchSend,
                         int64_t sendIntervalMs, int batchMaxSize)
    : queue_(queue),
      client_(client),
      clock_(clock),
      apiHost_(std::move(apiHost)),
      sourceToken_(std::move(sourceToken)),
      batchSend_(batchSend),
      sendIntervalMs_(sendIntervalMs),
      batchMaxSize_(batchMaxSize) {}

BatchSender::~BatchSender() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            // Self-thread destruction (residual path of the host re-entering
            // destroy inside execute): join-self throws and an exception in a
            // destructor means terminate — detach instead; the thread's
            // epilogue is protected by the kept-alive references
            thread_.detach();
        } else {
            thread_.join();
        }
    }
}

void BatchSender::start() {
    lastSendMs_ = clock_.nowMs();
    thread_ = std::thread(&BatchSender::run, this);
}

void BatchSender::notifyEvent() {
    // Hold the lock briefly to avoid a lost wakeup racing with wait; the
    // trigger itself is the fact of "queue non-empty" (pending sends)
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
}

void BatchSender::setDispatchPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused;
    }
    cv_.notify_all();  // wake the loop to re-evaluate: pausing winds down, resuming continues
}

bool BatchSender::dispatchPaused() {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

void BatchSender::requestStop(int64_t drainBudgetMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_.joinable()) {
        return;  // never started: no stop flag to set
    }
    stop_ = true;
    // The drain budget is handed down with the stop flag: past the deadline
    // the drain loop stops starting new batches (otherwise with the network
    // down, 200 events ÷ batch 10 × HTTP_TIMEOUT_MS could drag to minutes,
    // far beyond the caller's promised blocking bound)
    drainDeadlineSteadyMs_ = steadyNowMs() + drainBudgetMs;
    cv_.notify_all();
}

bool BatchSender::awaitDrainFinished(int64_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!thread_.joinable()) {
        return queue_->size() == 0;  // never started: answer from the queue state alone
    }
    if (thread_.get_id() == std::this_thread::get_id()) {
        // Called from the sender thread itself (the host re-entering destroy
        // inside execute): drainFinished_ could only be set by this very
        // thread, which is blocked here — the wait would idle to timeout, so
        // answer from the queue state directly
        return queue_->size() == 0;
    }
    const bool finished = drainDoneCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                                [this] { return drainFinished_; });
    lock.unlock();
    return finished && queue_->size() == 0;
}

bool BatchSender::joinStopped() {
    if (!thread_.joinable()) {
        return true;
    }
    if (thread_.get_id() == std::this_thread::get_id()) {
        // A self-join (the host re-entering destroy inside execute) throws
        // resource_deadlock_would_occur and leaves the thread joinable — the
        // destructor path would then terminate on the joinable thread. Detach
        // instead and let the thread finish naturally; return false so the
        // caller keeps the queue / client alive until the thread exits
        // (run()'s epilogue still touches them)
        thread_.detach();
        return false;
    }
    thread_.join();  // after the deadline at most one in-flight HTTP (<= HTTP_TIMEOUT_MS) remains
    return true;
}

bool BatchSender::drainAndStop(int64_t timeoutMs) {
    requestStop(timeoutMs);
    const bool finished = awaitDrainFinished(timeoutMs);
    return joinStopped() && finished;
}

bool BatchSender::shouldSendNowLocked(int64_t nowMs) const {
    if (queue_->size() == 0) {
        return false;
    }
    if (!batchSend_) {
        return true;  // per-event trigger
    }
    if (static_cast<int>(queue_->size()) >= batchMaxSize_) {
        return true;  // size trigger
    }
    if (nowMs - lastSendMs_ >= sendIntervalMs_) {
        return true;  // timer trigger
    }
    return queue_->hasDueEvent(nowMs);  // opportunistic retry (non-blocking)
}

int BatchSender::nextBackoffMs(int retryCount) const {
    // Backoff = interval × 2^retryCount, capped at 30s; the loop avoids shift
    // overflow
    int64_t delay = sendIntervalMs_;
    for (int i = 0; i < retryCount && delay < BACKOFF_CAP_MS; ++i) {
        delay *= 2;
    }
    return static_cast<int>(std::min<int64_t>(delay, BACKOFF_CAP_MS));
}

void BatchSender::handleFailure(std::vector<QueuedEvent> batch, int64_t nowMs) {
    for (QueuedEvent& entry : batch) {
        entry.retryCount += 1;
        if (entry.retryCount > RETRY_MAX) {
            // Events past the retry cap stay queued and are only retried
            // opportunistically, until they expire or are evicted
            Logger::warn("event exceeded retry max, kept in queue: " + entry.event.name);
        }
        entry.backoffDueAtMs = nowMs + nextBackoffMs(entry.retryCount);
    }
    queue_->requeueBack(std::move(batch));  // failed events append to the tail
}

void BatchSender::sendBatch(std::vector<QueuedEvent> batch, int64_t attemptNowMs) {
    std::vector<Event> events;
    events.reserve(batch.size());
    for (const QueuedEvent& entry : batch) {
        events.push_back(entry.event);
    }

    isFlushing_.store(true, std::memory_order_release);
    // Last line of defense for serialization: the normal path is already
    // guaranteed not to throw by the dump replace policy (event_json.cpp);
    // even if that guarantee were ever relaxed, an exception must never
    // escape the sender thread (terminate would crash the host process).
    // A batch that fails serialization is treated as a drop — a retry would
    // fail serialization too
    std::string body;
    try {
        body = serializeEventsJson(events);
    } catch (...) {
        isFlushing_.store(false, std::memory_order_release);
        Logger::warn("event serialization failed, batch dropped, size: " +
                     std::to_string(events.size()));
        cv_.notify_all();
        return;
    }
    const HttpRequest request = buildTrackRequest(apiHost_, sourceToken_, body);
    if (Logger::debugEnabled()) {
        Logger::debug("track post body: " + request.body);
    }
    // No IHttpClient injected (SW_WITH_CURL=OFF facade build; initLocked
    // already warned): degrade as a transport failure (statusCode=0 →
    // retryable), the send is treated as a network failure and events stay
    // queued with backoff — never dereference the null pointer
    HttpResponse response;
    if (client_ == nullptr) {
        response.error = "no http client configured";
    } else {
        // A host-injected implementation violating its contract by throwing:
        // degrade as a transport failure (statusCode=0 → retryable), never
        // let it escape — an escaping exception terminates the sender thread
        // and crashes the host process; same degradation semantics as the AB
        // runFetch path
        try {
            response = client_->execute(request);
        } catch (const std::exception& e) {
            response.statusCode = 0;
            response.error = std::string("http client threw: ") + e.what();
            Logger::warn("http client raised, degraded to transport failure");
        } catch (...) {
            response.statusCode = 0;
            response.error = "http client threw unknown exception";
            Logger::warn("http client raised unknown, degraded to transport failure");
        }
    }
    const SendOutcome outcome = classifyResponse(response);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastSendMs_ = clock_.nowMs();  // timer baseline = time of each attempt
    }
    isFlushing_.store(false, std::memory_order_release);

    if (outcome == SendOutcome::kSuccess) {
        queue_->rewriteDisk();  // dequeue + whole-file rewrite
    } else if (outcome == SendOutcome::kDrop) {
        // 4xx (except 408/429): drop outright, no retry, not kept; already
        // removed from memory, the rewrite clears them from disk
        Logger::warn("track request dropped by server (http " +
                     std::to_string(response.statusCode) +
                     "), batch size: " + std::to_string(batch.size()));
        queue_->rewriteDisk();
    } else {
        handleFailure(std::move(batch), attemptNowMs);  // retry: append to tail + backoff
    }
    cv_.notify_all();  // re-run triggers that arrived during the send / wake destroy's evaluation
}

bool BatchSender::drainFinal(int64_t deadlineSteadyMs) {
    // Final drain: best-effort sends within the wall-clock deadline; failed
    // events are requeued with backoff, and the loop ends when drainDue yields
    // no due batch or the budget is exhausted — not a "send success guarantee".
    // The deadline is checked before taking a batch: past the deadline not
    // even a batch is taken, ruling out the unbounded blocking of "a full
    // serial round of failures with the network down"
    for (;;) {
        if (steadyNowMs() >= deadlineSteadyMs) {
            break;
        }
        const int64_t nowMs = clock_.nowMs();
        std::vector<QueuedEvent> batch =
            queue_->drainDue(static_cast<std::size_t>(batchMaxSize_), nowMs);
        if (batch.empty()) {
            break;
        }
        sendBatch(std::move(batch), nowMs);
    }
    return queue_->size() == 0;
}

void BatchSender::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (stop_) {
            const bool paused = paused_;  // disabled-mode destroy: respect the pause, send nothing
            const int64_t deadline = drainDeadlineSteadyMs_;
            lock.unlock();
            if (!paused) {
                drainFinal(deadline);  // destroy final drain (wall-clock deadline)
            }
            lock.lock();
            drainFinished_ = true;
            drainDoneCv_.notify_all();
            break;
        }

        if (paused_) {
            // Opt-Out pause: no trigger evaluation, wait for resume or stop;
            // the queue is untouched
            cv_.wait(lock, [this] { return stop_ || !paused_; });
            continue;
        }

        const int64_t nowMs = clock_.nowMs();
        if (queue_->size() == 0) {
            // Steady-state idle: wait with no deadline; enqueue's
            // notifyEvent() or stop wakes it — a timer deadline is meaningless
            // here (once the window passes, delta would be permanently 0 and
            // cause busy polling)
            cv_.wait(lock, [this] { return stop_ || paused_ || queue_->size() > 0; });
            continue;
        }

        if (!queue_->hasSendableEvent(nowMs)) {
            // All entries are in backoff: the timer window is irrelevant;
            // wait for the earliest backoff to expire or for a stop — bounded
            // deadline, no polling. Once due, the trigger evaluation below
            // performs the opportunistic retry
            const int64_t earliestMs = queue_->earliestBackoffDueMs();
            int64_t deltaMs = earliestMs - nowMs;
            deltaMs = std::max<int64_t>(deltaMs, 0);
            cv_.wait_until(
                lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(deltaMs),
                [this] { return stop_ || paused_ || queue_->hasSendableEvent(clock_.nowMs()); });
            continue;
        }

        if (!shouldSendNowLocked(clock_.nowMs())) {
            // Sendable entries exist but no trigger fired (size not reached /
            // timer not due): wait for the timer or a wakeup to re-evaluate
            const int64_t timerDueMs = lastSendMs_ + sendIntervalMs_;
            int64_t deltaMs = timerDueMs - clock_.nowMs();
            deltaMs = std::max<int64_t>(
                deltaMs, 0);  // timer window just passed: re-evaluate immediately instead of idling

            cv_.wait_until(
                lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(deltaMs),
                [this] { return stop_ || paused_ || shouldSendNowLocked(clock_.nowMs()); });
            continue;
        }

        lock.unlock();
        if (dispatchPaused()) {
            // Double-check gate: a pause landing between trigger evaluation
            // and batch taking takes effect immediately (when evaluation and
            // pause race, tolerate it as "this dispatch was already in
            // flight" — events are already dequeued and cannot be recalled)
            lock.lock();
            continue;
        }
        const int64_t sendNowMs = clock_.nowMs();
        std::vector<QueuedEvent> batch =
            queue_->drainDue(static_cast<std::size_t>(batchMaxSize_), sendNowMs);
        if (!batch.empty()) {
            sendBatch(std::move(batch), sendNowMs);
        }
        lock.lock();  // triggers that arrived during the send are recorded by the non-empty queue,
                      // re-run next round
    }
}

}  // namespace sensorswave
