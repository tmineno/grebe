#pragma once

#include "ring_buffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

class DropCounter;

// GRB binary file format header (32 bytes, little-endian)
struct GrbFileHeader {
    uint32_t magic;              // 'GRB1' = 0x31425247
    uint32_t version;            // 1
    uint32_t channel_count;      // 1–8
    uint32_t reserved;           // 0
    double   sample_rate_hz;     // e.g. 100e6
    uint64_t total_samples;      // per channel
};

static constexpr uint32_t GRB_MAGIC   = 0x31425247;  // 'GRB1' little-endian
static constexpr uint32_t GRB_VERSION = 1;

/// FileReader: reads a .grb binary file and pushes samples to ring buffers
/// with rate pacing. Same thread model as DataGenerator.
class FileReader {
public:
    /// Opens and validates the file, mmaps it. Throws on error.
    explicit FileReader(const std::string& path);
    ~FileReader();

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    /// Start the reader thread (pushes to ring buffers with pacing).
    void start(std::vector<RingBuffer<int16_t>*> rings,
               std::vector<DropCounter*> drop_counters);
    void stop();

    void set_paused(bool paused);
    bool is_paused() const { return paused_.load(std::memory_order_relaxed); }

    double target_sample_rate() const { return header_.sample_rate_hz; }
    double actual_sample_rate() const { return actual_rate_.load(std::memory_order_relaxed); }
    uint64_t total_samples_read() const { return total_samples_read_.load(std::memory_order_relaxed); }
    uint32_t channel_count() const { return header_.channel_count; }
    uint64_t total_file_samples() const { return header_.total_samples; }
    const std::string& path() const { return path_; }

    void set_looping(bool loop) { looping_.store(loop, std::memory_order_relaxed); }
    bool is_looping() const { return looping_.load(std::memory_order_relaxed); }

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

private:
    void thread_func();
    void cleanup_mmap();

    std::string path_;
    int fd_ = -1;
    void* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    const int16_t* sample_data_ = nullptr;  // pointer past header into mmap
    GrbFileHeader header_{};

    // Ring buffers (set by start())
    std::vector<RingBuffer<int16_t>*> rings_;
    std::vector<DropCounter*> drop_counters_;

    // Thread state
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> looping_{true};

    // Telemetry
    std::atomic<double> actual_rate_{0.0};
    std::atomic<uint64_t> total_samples_read_{0};
};
