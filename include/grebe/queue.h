#pragma once

// IQueue — Bounded queue contract with backpressure policy (RDD §5.3)
// Phase 11: Queue contract foundation

#include <cstdint>
#include <optional>

namespace grebe {

/// Backpressure policy applied when the queue is full on enqueue.
enum class BackpressurePolicy {
    DropLatest,  ///< Discard the incoming (newest) frame
    DropOldest,  ///< Discard the oldest frame in the queue, then enqueue
    Block,       ///< Block the producer until space is available
};

/// Abstract bounded queue interface.
///
/// Implementations:
///   - InProcessQueue  (Phase 11, in-process std::deque + mutex)
///   - ShmQueue        (Phase 14, SharedMemory backing)
template <typename T>
class IQueue {
public:
    virtual ~IQueue() = default;

    /// Enqueue an item. Behavior on full queue depends on BackpressurePolicy:
    ///   - DropLatest: returns false (item discarded)
    ///   - DropOldest: drops oldest, enqueues new, returns true
    ///   - Block: blocks until space available, returns true
    virtual bool enqueue(T&& item) = 0;

    /// Dequeue an item (non-blocking). Returns std::nullopt if empty.
    virtual std::optional<T> dequeue() = 0;

    /// Maximum number of items the queue can hold.
    virtual size_t capacity() const = 0;

    /// Current number of items in the queue.
    virtual size_t size() const = 0;

    /// Fill ratio: size() / capacity(), in [0.0, 1.0].
    virtual double fill_ratio() const = 0;

    virtual bool empty() const = 0;
    virtual bool full() const = 0;

    // ---- Telemetry ----

    /// Total items successfully enqueued.
    virtual uint64_t total_enqueued() const = 0;

    /// Total items dropped (DropLatest or DropOldest policy).
    virtual uint64_t total_dropped() const = 0;

    /// Total time spent blocking in enqueue (nanoseconds, Block policy).
    virtual uint64_t total_blocked_ns() const = 0;
};

} // namespace grebe
