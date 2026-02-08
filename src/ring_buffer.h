#pragma once

#include "ring_buffer_view.h"

#include <cassert>
#include <vector>

// Lock-free single-producer single-consumer ring buffer (owning storage).
// Delegates all operations to RingBufferView over its internal std::vector.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(capacity)
        , buffer_(capacity)
        , head_(0)
        , tail_(0)
        , view_(buffer_.data(), capacity, head_, tail_) {
        assert(capacity > 0);
    }

    bool push(const T& item)                  { return view_.push(item); }
    size_t push_bulk(const T* data, size_t n)  { return view_.push_bulk(data, n); }
    bool pop(T& item)                          { return view_.pop(item); }
    size_t pop_bulk(T* out, size_t max_count)  { return view_.pop_bulk(out, max_count); }
    size_t discard_bulk(size_t max_count)      { return view_.discard_bulk(max_count); }

    size_t size()       const { return view_.size(); }
    size_t capacity()   const { return view_.capacity(); }
    double fill_ratio() const { return view_.fill_ratio(); }
    bool empty()        const { return view_.empty(); }
    bool full()         const { return view_.full(); }

private:
    size_t                capacity_;
    std::vector<T>        buffer_;
    std::atomic<size_t>   head_;
    std::atomic<size_t>   tail_;
    RingBufferView<T>     view_;
};
