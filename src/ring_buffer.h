#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cassert>
#include <algorithm>

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

    // Producer: bulk push with batched memcpy. Returns number of items pushed.
    size_t push_bulk(const T* data, size_t count) {
        if (count == 0) return 0;

        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);

        // Compute free space (one slot reserved as sentinel)
        size_t free;
        if (head >= tail) {
            free = capacity_ - 1 - head + tail;
        } else {
            free = tail - head - 1;
        }

        size_t to_push = std::min(count, free);
        if (to_push == 0) return 0;

        // Copy in up to 2 chunks (handle wraparound)
        size_t first_chunk = std::min(to_push, capacity_ - head);
        std::memcpy(&buffer_[head], data, first_chunk * sizeof(T));

        if (to_push > first_chunk) {
            size_t second_chunk = to_push - first_chunk;
            std::memcpy(&buffer_[0], data + first_chunk, second_chunk * sizeof(T));
        }

        size_t new_head = (head + to_push) % capacity_;
        head_.store(new_head, std::memory_order_release);
        return to_push;
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

    // Consumer: bulk pop with batched memcpy. Returns number of items popped.
    size_t pop_bulk(T* out, size_t max_count) {
        if (max_count == 0) return 0;

        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);

        // Compute available items
        size_t avail;
        if (head >= tail) {
            avail = head - tail;
        } else {
            avail = capacity_ - tail + head;
        }

        size_t to_pop = std::min(max_count, avail);
        if (to_pop == 0) return 0;

        // Copy in up to 2 chunks (handle wraparound)
        size_t first_chunk = std::min(to_pop, capacity_ - tail);
        std::memcpy(out, &buffer_[tail], first_chunk * sizeof(T));

        if (to_pop > first_chunk) {
            size_t second_chunk = to_pop - first_chunk;
            std::memcpy(out + first_chunk, &buffer_[0], second_chunk * sizeof(T));
        }

        size_t new_tail = (tail + to_pop) % capacity_;
        tail_.store(new_tail, std::memory_order_release);
        return to_pop;
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
