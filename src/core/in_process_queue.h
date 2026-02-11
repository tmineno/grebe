#pragma once

// InProcessQueue — Bounded in-process queue for move-only Frame objects
// Phase 11: IQueue<Frame> implementation with backpressure

#include "grebe/queue.h"
#include "grebe/frame.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace grebe {

/// In-process bounded queue backed by std::deque + mutex.
///
/// Thread-safe for concurrent enqueue/dequeue (MPSC or SPSC).
/// Backpressure policy is applied on the enqueue (producer) side.
/// Dequeue is always non-blocking (returns nullopt if empty).
class InProcessQueue final : public IQueue<Frame> {
public:
    /// @param capacity  Maximum number of frames the queue can hold.
    /// @param policy    Backpressure policy applied when queue is full.
    explicit InProcessQueue(size_t capacity,
                            BackpressurePolicy policy = BackpressurePolicy::DropLatest);

    ~InProcessQueue() override;

    // ---- IQueue<Frame> ----
    bool enqueue(Frame&& item) override;
    std::optional<Frame> dequeue() override;

    size_t   capacity()   const override;
    size_t   size()       const override;
    double   fill_ratio() const override;
    bool     empty()      const override;
    bool     full()       const override;

    uint64_t total_enqueued()   const override;
    uint64_t total_dropped()    const override;
    uint64_t total_blocked_ns() const override;

    /// Signal blocked producers to wake up (used during shutdown).
    void shutdown();

private:
    const size_t capacity_;
    const BackpressurePolicy policy_;

    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    bool shutdown_ = false;

    std::deque<Frame> queue_;

    // Telemetry (protected by mutex_)
    uint64_t total_enqueued_   = 0;
    uint64_t total_dropped_    = 0;
    uint64_t total_blocked_ns_ = 0;
};

} // namespace grebe
