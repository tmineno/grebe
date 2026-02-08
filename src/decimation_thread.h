#pragma once

#include "decimator.h"
#include "ring_buffer.h"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <memory>
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
    void start(std::vector<RingBuffer<int16_t>*> rings, uint32_t target_points, DecimationMode mode);
    void stop();

    uint32_t channel_count() const { return channel_count_.load(std::memory_order_relaxed); }
    uint32_t per_channel_vertex_count() const { return per_ch_vtx_.load(std::memory_order_relaxed); }

    void set_mode(DecimationMode mode);
    void set_target_points(uint32_t n);
    void set_sample_rate(double rate);
    void cycle_mode(); // None → MinMax → LTTB → None

    // Main thread: get latest decimated frame.
    // Returns true if new data was available; fills output and raw_sample_count.
    bool try_get_frame(std::vector<int16_t>& output, uint32_t& raw_sample_count);

    // Telemetry (thread-safe, called from main thread)
    double decimation_time_ms() const { return decimate_time_ms_.load(std::memory_order_relaxed); }
    double decimation_ratio()   const { return decimate_ratio_.load(std::memory_order_relaxed); }
    double ring_fill_ratio()    const { return ring_fill_.load(std::memory_order_relaxed); }
    DecimationMode current_mode() const { return mode_.load(std::memory_order_relaxed); }
    DecimationMode effective_mode() const { return effective_mode_.load(std::memory_order_relaxed); }

    static const char* mode_name(DecimationMode m);

private:
    // Per-worker state for multi-threaded decimation
    struct WorkerState {
        std::thread thread;
        std::vector<uint32_t> assigned_channels;
        std::vector<std::vector<int16_t>> drain_bufs;    // indexed by assigned channel slot
        std::vector<std::vector<int16_t>> dec_results;   // indexed by assigned channel slot
        std::vector<size_t> raw_counts;                   // indexed by assigned channel slot
        double max_fill = 0.0;
    };

    void thread_func();
    void thread_func_single();      // 1ch optimized path (no workers)
    void thread_func_multi();       // multi-ch with worker threads
    void worker_func(uint32_t worker_id);

    std::vector<RingBuffer<int16_t>*> rings_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint32_t> channel_count_{1};
    std::atomic<uint32_t> per_ch_vtx_{0};

    // Settings
    std::atomic<DecimationMode> mode_{DecimationMode::None};
    std::atomic<DecimationMode> effective_mode_{DecimationMode::None};
    std::atomic<uint32_t> target_points_{3840};
    std::atomic<double> sample_rate_{0.0};

    // Double-buffered output
    std::mutex mutex_;
    std::vector<int16_t> front_buffer_;
    uint32_t front_raw_count_ = 0;
    bool new_data_ = false;

    // Telemetry
    std::atomic<double> decimate_time_ms_{0.0};
    std::atomic<double> decimate_ratio_{1.0};
    std::atomic<double> ring_fill_{0.0};

    // Multi-threaded worker state
    std::vector<WorkerState> workers_;
    std::unique_ptr<std::barrier<>> start_barrier_;
    std::unique_ptr<std::barrier<>> done_barrier_;
    uint32_t num_workers_ = 0;
};
