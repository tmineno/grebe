#pragma once

#include "decimator.h"
#include "ring_buffer.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class DecimationThread {
public:
    DecimationThread() = default;
    ~DecimationThread();

    DecimationThread(const DecimationThread&) = delete;
    DecimationThread& operator=(const DecimationThread&) = delete;

    void start(RingBuffer<int16_t>& ring, uint32_t target_points, DecimationMode mode);
    void stop();

    void set_mode(DecimationMode mode);
    void set_target_points(uint32_t n);
    void cycle_mode(); // None → MinMax → LTTB → None

    // Main thread: get latest decimated frame.
    // Returns true if new data was available; fills output and raw_sample_count.
    bool try_get_frame(std::vector<int16_t>& output, uint32_t& raw_sample_count);

    // Telemetry (thread-safe, called from main thread)
    double decimation_time_ms() const { return decimate_time_ms_.load(std::memory_order_relaxed); }
    double decimation_ratio()   const { return decimate_ratio_.load(std::memory_order_relaxed); }
    double ring_fill_ratio()    const { return ring_fill_.load(std::memory_order_relaxed); }
    DecimationMode current_mode() const { return mode_.load(std::memory_order_relaxed); }

    static const char* mode_name(DecimationMode m);

private:
    void thread_func();

    RingBuffer<int16_t>* ring_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Settings
    std::atomic<DecimationMode> mode_{DecimationMode::None};
    std::atomic<uint32_t> target_points_{3840};

    // Double-buffered output
    std::mutex mutex_;
    std::vector<int16_t> front_buffer_;
    uint32_t front_raw_count_ = 0;
    bool new_data_ = false;

    // Telemetry
    std::atomic<double> decimate_time_ms_{0.0};
    std::atomic<double> decimate_ratio_{1.0};
    std::atomic<double> ring_fill_{0.0};
};
