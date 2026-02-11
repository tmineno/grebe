#include "file_reader.h"
#include "drop_counter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

FileReader::FileReader(const std::string& path) : path_(path) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw std::runtime_error("FileReader: cannot open " + path);
    }

    struct stat st{};
    if (fstat(fd_, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(GrbFileHeader))) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("FileReader: file too small or stat failed: " + path);
    }

    mapped_size_ = static_cast<size_t>(st.st_size);
    mapped_ = mmap(nullptr, mapped_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("FileReader: mmap failed: " + path);
    }

    madvise(mapped_, mapped_size_, MADV_SEQUENTIAL);

    // Read and validate header
    std::memcpy(&header_, mapped_, sizeof(GrbFileHeader));

    if (header_.magic != GRB_MAGIC) {
        cleanup_mmap();
        throw std::runtime_error("FileReader: invalid magic (expected GRB1): " + path);
    }
    if (header_.version != GRB_VERSION) {
        cleanup_mmap();
        throw std::runtime_error("FileReader: unsupported version " +
                                 std::to_string(header_.version) + ": " + path);
    }
    if (header_.channel_count < 1 || header_.channel_count > 8) {
        cleanup_mmap();
        throw std::runtime_error("FileReader: invalid channel_count " +
                                 std::to_string(header_.channel_count) + ": " + path);
    }
    if (header_.sample_rate_hz <= 0) {
        cleanup_mmap();
        throw std::runtime_error("FileReader: invalid sample_rate_hz: " + path);
    }

    // Verify file size matches header
    size_t expected_payload = static_cast<size_t>(header_.channel_count) *
                              header_.total_samples * sizeof(int16_t);
    size_t expected_total = sizeof(GrbFileHeader) + expected_payload;
    if (mapped_size_ < expected_total) {
        cleanup_mmap();
        throw std::runtime_error("FileReader: file truncated (expected " +
                                 std::to_string(expected_total) + " bytes, got " +
                                 std::to_string(mapped_size_) + "): " + path);
    }

    sample_data_ = reinterpret_cast<const int16_t*>(
        static_cast<const char*>(mapped_) + sizeof(GrbFileHeader));

    spdlog::info("FileReader: opened {} ({}ch, {:.0f} SPS, {} samples/ch, {:.1f} MB)",
                 path, header_.channel_count, header_.sample_rate_hz,
                 header_.total_samples,
                 static_cast<double>(mapped_size_) / (1024.0 * 1024.0));
}

FileReader::~FileReader() {
    stop();
    cleanup_mmap();
}

void FileReader::cleanup_mmap() {
    if (mapped_) {
        munmap(mapped_, mapped_size_);
        mapped_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void FileReader::start(std::vector<RingBuffer<int16_t>*> rings,
                       std::vector<DropCounter*> drop_counters) {
    stop();
    rings_ = std::move(rings);
    drop_counters_ = std::move(drop_counters);
    stop_requested_.store(false, std::memory_order_relaxed);
    total_samples_read_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&FileReader::thread_func, this);
}

void FileReader::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void FileReader::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_relaxed);
}

void FileReader::thread_func() {
    using Clock = std::chrono::steady_clock;

    constexpr size_t BATCH_SIZE_LOW  = 4096;
    constexpr size_t BATCH_SIZE_HIGH = 65536;

    double sample_rate = header_.sample_rate_hz;
    bool high_rate = (sample_rate >= 100e6);
    size_t batch_size = high_rate ? BATCH_SIZE_HIGH : BATCH_SIZE_LOW;

    uint32_t num_ch = header_.channel_count;
    uint64_t total_samples = header_.total_samples;
    // Per-channel sample data pointers
    // File layout: [ch0_all_samples][ch1_all_samples]...
    std::vector<const int16_t*> ch_base(num_ch);
    for (uint32_t ch = 0; ch < num_ch; ch++) {
        ch_base[ch] = sample_data_ + static_cast<size_t>(ch) * total_samples;
    }

    size_t read_pos = 0;  // current sample position within each channel
    uint64_t cumulative_samples = 0;

    auto next_wake = Clock::now();
    auto rate_timer_start = Clock::now();
    uint64_t rate_sample_count = 0;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        // Handle pause
        if (paused_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            next_wake = Clock::now();
            rate_timer_start = Clock::now();
            rate_sample_count = 0;
            actual_rate_.store(0.0, std::memory_order_relaxed);
            continue;
        }

        // Determine how many samples to read this batch
        size_t remaining_in_file = total_samples - read_pos;
        if (remaining_in_file == 0) {
            if (looping_.load(std::memory_order_relaxed)) {
                read_pos = 0;
                remaining_in_file = total_samples;
                spdlog::debug("FileReader: looping back to start");
            } else {
                spdlog::info("FileReader: reached end of file");
                break;
            }
        }

        size_t this_batch = std::min(batch_size, remaining_in_file);

        // Push samples from mmap to ring buffers per channel
        for (uint32_t ch = 0; ch < num_ch && ch < rings_.size(); ch++) {
            const int16_t* src = ch_base[ch] + read_pos;
            size_t pushed = rings_[ch]->push_bulk(src, this_batch);
            if (ch < drop_counters_.size() && drop_counters_[ch]) {
                drop_counters_[ch]->record_push(this_batch, pushed);
            }
        }

        read_pos += this_batch;
        cumulative_samples += this_batch;
        total_samples_read_.store(cumulative_samples, std::memory_order_relaxed);

        // Rate measurement (update every 100ms)
        rate_sample_count += this_batch;
        auto now = Clock::now();
        auto elapsed = std::chrono::duration<double>(now - rate_timer_start).count();
        if (elapsed >= 0.1) {
            actual_rate_.store(static_cast<double>(rate_sample_count) / elapsed,
                               std::memory_order_relaxed);
            rate_timer_start = now;
            rate_sample_count = 0;
        }

        // Backpressure: extra delay when ring nearly full
        bool any_full = false;
        for (auto* rb : rings_) {
            if (rb->fill_ratio() > 0.9) { any_full = true; break; }
        }
        if (any_full && !high_rate) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // Pacing: target the file's sample rate
        double batch_duration = static_cast<double>(this_batch) / sample_rate;
        next_wake += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(batch_duration));

        if (next_wake > Clock::now()) {
            if (high_rate) {
                while (Clock::now() < next_wake) {
                    if (stop_requested_.load(std::memory_order_relaxed)) break;
                    std::this_thread::yield();
                }
            } else {
                std::this_thread::sleep_until(next_wake);
            }
        } else {
            if (Clock::now() - next_wake > std::chrono::milliseconds(100)) {
                next_wake = Clock::now();
            }
        }
    }
}
