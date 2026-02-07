#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <algorithm>

// Lock-free SPSC ring buffer view over raw memory.
// Does not own the storage — suitable for shared memory regions.
template <typename T>
class RingBufferView {
public:
    RingBufferView(T* data, size_t capacity,
                   std::atomic<size_t>& head, std::atomic<size_t>& tail)
        : data_(data), capacity_(capacity), head_(head), tail_(tail) {}

    bool push(const T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % capacity_;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        data_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    size_t push_bulk(const T* src, size_t count) {
        if (count == 0) return 0;

        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);

        size_t free = (head >= tail)
            ? (capacity_ - 1 - head + tail)
            : (tail - head - 1);

        size_t to_push = std::min(count, free);
        if (to_push == 0) return 0;

        size_t first_chunk = std::min(to_push, capacity_ - head);
        std::memcpy(&data_[head], src, first_chunk * sizeof(T));

        if (to_push > first_chunk) {
            std::memcpy(&data_[0], src + first_chunk,
                        (to_push - first_chunk) * sizeof(T));
        }

        head_.store((head + to_push) % capacity_, std::memory_order_release);
        return to_push;
    }

    bool pop(T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        item = data_[tail];
        tail_.store((tail + 1) % capacity_, std::memory_order_release);
        return true;
    }

    size_t pop_bulk(T* out, size_t max_count) {
        if (max_count == 0) return 0;

        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);

        size_t avail = (head >= tail)
            ? (head - tail)
            : (capacity_ - tail + head);

        size_t to_pop = std::min(max_count, avail);
        if (to_pop == 0) return 0;

        size_t first_chunk = std::min(to_pop, capacity_ - tail);
        std::memcpy(out, &data_[tail], first_chunk * sizeof(T));

        if (to_pop > first_chunk) {
            std::memcpy(out + first_chunk, &data_[0],
                        (to_pop - first_chunk) * sizeof(T));
        }

        tail_.store((tail + to_pop) % capacity_, std::memory_order_release);
        return to_pop;
    }

    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (capacity_ - tail + head);
    }

    size_t capacity() const { return capacity_ - 1; }

    double fill_ratio() const {
        return static_cast<double>(size()) / static_cast<double>(capacity());
    }

    bool empty() const { return size() == 0; }
    bool full()  const { return size() == capacity(); }

private:
    T* data_;
    size_t capacity_;
    std::atomic<size_t>& head_;
    std::atomic<size_t>& tail_;
};
