#pragma once

#include "grebe/data_source.h"
#include "ring_buffer.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

class DropCounter;

/// IngestionThread: drives an IDataSource, pushing samples to ring buffers.
/// Replaces the per-source threading (DataGenerator thread, ipc_receiver_func).
class IngestionThread {
public:
    IngestionThread() = default;
    ~IngestionThread();

    IngestionThread(const IngestionThread&) = delete;
    IngestionThread& operator=(const IngestionThread&) = delete;

    /// Start the ingestion loop. The source must already be started.
    void start(grebe::IDataSource& source,
               std::vector<RingBuffer<int16_t>*> rings,
               std::vector<DropCounter*> drop_counters);
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // Telemetry (thread-safe reads from main thread)
    uint64_t last_producer_ts_ns() const { return last_producer_ts_ns_.load(std::memory_order_relaxed); }
    double sample_rate() const { return sample_rate_.load(std::memory_order_relaxed); }
    uint64_t source_drops() const { return source_drops_.load(std::memory_order_relaxed); }
    uint64_t sequence_gaps() const { return sequence_gaps_.load(std::memory_order_relaxed); }

private:
    void thread_func();

    grebe::IDataSource* source_ = nullptr;
    std::vector<RingBuffer<int16_t>*> rings_;
    std::vector<DropCounter*> drop_counters_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Telemetry atomics
    std::atomic<uint64_t> last_producer_ts_ns_{0};
    std::atomic<double> sample_rate_{0.0};
    std::atomic<uint64_t> source_drops_{0};
    std::atomic<uint64_t> sequence_gaps_{0};
    uint64_t expected_seq_ = 0;
};
