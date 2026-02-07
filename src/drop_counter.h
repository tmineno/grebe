#pragma once

#include <atomic>
#include <cstdint>

class DropCounter {
public:
    void record_push(uint64_t attempted, uint64_t pushed) {
        total_pushed_.fetch_add(pushed, std::memory_order_relaxed);
        if (pushed < attempted) {
            total_dropped_.fetch_add(attempted - pushed, std::memory_order_relaxed);
        }
    }

    uint64_t total_pushed()  const { return total_pushed_.load(std::memory_order_relaxed); }
    uint64_t total_dropped() const { return total_dropped_.load(std::memory_order_relaxed); }

    void reset() {
        total_pushed_.store(0, std::memory_order_relaxed);
        total_dropped_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> total_pushed_{0};
    std::atomic<uint64_t> total_dropped_{0};
};
