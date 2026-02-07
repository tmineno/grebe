#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <cassert>

// Lock-free single-producer single-consumer ring buffer
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(capacity)
        , buffer_(capacity)
        , head_(0)
        , tail_(0) {
        assert(capacity > 0);
    }

    // Producer: push a single element. Returns false if full.
    bool push(const T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % capacity_;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Producer: bulk push. Returns number of items actually pushed.
    size_t push_bulk(const T* data, size_t count) {
        size_t pushed = 0;
        for (size_t i = 0; i < count; i++) {
            if (!push(data[i])) break;
            pushed++;
        }
        return pushed;
    }

    // Consumer: pop a single element. Returns false if empty.
    bool pop(T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        item = buffer_[tail];
        tail_.store((tail + 1) % capacity_, std::memory_order_release);
        return true;
    }

    // Consumer: bulk pop into buffer. Returns number of items popped.
    size_t pop_bulk(T* out, size_t max_count) {
        size_t popped = 0;
        for (size_t i = 0; i < max_count; i++) {
            if (!pop(out[i])) break;
            popped++;
        }
        return popped;
    }

    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        if (head >= tail) return head - tail;
        return capacity_ - tail + head;
    }

    size_t capacity() const { return capacity_ - 1; } // one slot reserved

    double fill_ratio() const {
        return static_cast<double>(size()) / static_cast<double>(capacity());
    }

    bool empty() const { return size() == 0; }
    bool full()  const { return size() == capacity(); }

private:
    size_t                capacity_;
    std::vector<T>        buffer_;
    std::atomic<size_t>   head_;
    std::atomic<size_t>   tail_;
};
