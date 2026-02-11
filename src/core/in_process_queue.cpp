#include "core/in_process_queue.h"

#include <chrono>

namespace grebe {

InProcessQueue::InProcessQueue(size_t capacity, BackpressurePolicy policy)
    : capacity_(capacity > 0 ? capacity : 1)
    , policy_(policy) {}

InProcessQueue::~InProcessQueue() {
    shutdown();
}

bool InProcessQueue::enqueue(Frame&& item) {
    std::unique_lock lock(mutex_);

    if (queue_.size() >= capacity_) {
        switch (policy_) {
        case BackpressurePolicy::DropLatest:
            ++total_dropped_;
            return false;

        case BackpressurePolicy::DropOldest:
            queue_.pop_front();  // discard oldest
            ++total_dropped_;
            queue_.push_back(std::move(item));
            ++total_enqueued_;
            return true;

        case BackpressurePolicy::Block: {
            auto t0 = std::chrono::steady_clock::now();
            not_full_.wait(lock, [this] {
                return queue_.size() < capacity_ || shutdown_;
            });
            auto t1 = std::chrono::steady_clock::now();
            total_blocked_ns_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            if (shutdown_) return false;
            break;  // fall through to enqueue below
        }
        }
    }

    queue_.push_back(std::move(item));
    ++total_enqueued_;
    return true;
}

std::optional<Frame> InProcessQueue::dequeue() {
    std::lock_guard lock(mutex_);

    if (queue_.empty()) return std::nullopt;

    Frame f = std::move(queue_.front());
    queue_.pop_front();

    // Wake blocked producer if Block policy
    if (policy_ == BackpressurePolicy::Block) {
        not_full_.notify_one();
    }

    return f;
}

size_t InProcessQueue::capacity() const {
    return capacity_;
}

size_t InProcessQueue::size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

double InProcessQueue::fill_ratio() const {
    std::lock_guard lock(mutex_);
    return static_cast<double>(queue_.size()) / static_cast<double>(capacity_);
}

bool InProcessQueue::empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
}

bool InProcessQueue::full() const {
    std::lock_guard lock(mutex_);
    return queue_.size() >= capacity_;
}

uint64_t InProcessQueue::total_enqueued() const {
    std::lock_guard lock(mutex_);
    return total_enqueued_;
}

uint64_t InProcessQueue::total_dropped() const {
    std::lock_guard lock(mutex_);
    return total_dropped_;
}

uint64_t InProcessQueue::total_blocked_ns() const {
    std::lock_guard lock(mutex_);
    return total_blocked_ns_;
}

void InProcessQueue::shutdown() {
    {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
    }
    not_full_.notify_all();
}

} // namespace grebe
