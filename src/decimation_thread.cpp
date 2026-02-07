#include "decimation_thread.h"

#include <spdlog/spdlog.h>
#include <chrono>

DecimationThread::~DecimationThread() {
    stop();
}

void DecimationThread::start(RingBuffer<int16_t>& ring, uint32_t target_points, DecimationMode mode) {
    if (running_.load()) return;

    ring_ = &ring;
    target_points_.store(target_points, std::memory_order_relaxed);
    mode_.store(mode, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);

    thread_ = std::thread(&DecimationThread::thread_func, this);

    spdlog::info("DecimationThread started (target={}, mode={})",
                 target_points, mode_name(mode));
}

void DecimationThread::stop() {
    if (!running_.load()) return;

    stop_requested_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_relaxed);
    spdlog::info("DecimationThread stopped");
}

void DecimationThread::set_mode(DecimationMode mode) {
    mode_.store(mode, std::memory_order_relaxed);
}

void DecimationThread::set_target_points(uint32_t n) {
    target_points_.store(n, std::memory_order_relaxed);
}

void DecimationThread::cycle_mode() {
    auto m = mode_.load(std::memory_order_relaxed);
    DecimationMode next;
    switch (m) {
    case DecimationMode::None:   next = DecimationMode::MinMax; break;
    case DecimationMode::MinMax: next = DecimationMode::LTTB;   break;
    case DecimationMode::LTTB:   next = DecimationMode::None;   break;
    default:                     next = DecimationMode::None;   break;
    }
    mode_.store(next, std::memory_order_relaxed);
    spdlog::info("Decimation mode → {}", mode_name(next));
}

bool DecimationThread::try_get_frame(std::vector<int16_t>& output, uint32_t& raw_sample_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!new_data_) return false;

    output.swap(front_buffer_);
    raw_sample_count = front_raw_count_;
    new_data_ = false;
    return true;
}

const char* DecimationThread::mode_name(DecimationMode m) {
    switch (m) {
    case DecimationMode::None:   return "None";
    case DecimationMode::MinMax: return "MinMax";
    case DecimationMode::LTTB:   return "LTTB";
    default:                     return "Unknown";
    }
}

void DecimationThread::thread_func() {
    std::vector<int16_t> drain_buf;
    drain_buf.reserve(ring_->capacity()); // pre-allocate to match ring buffer size

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // Drain ring buffer
        size_t avail = ring_->size();
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        drain_buf.resize(avail);
        size_t popped = ring_->pop_bulk(drain_buf.data(), avail);
        drain_buf.resize(popped);

        if (popped == 0) continue;

        uint32_t raw_count = static_cast<uint32_t>(popped);

        // Record ring fill
        ring_fill_.store(ring_->fill_ratio(), std::memory_order_relaxed);

        // Decimate
        auto mode = mode_.load(std::memory_order_relaxed);
        auto target = target_points_.load(std::memory_order_relaxed);

        auto t0 = std::chrono::steady_clock::now();

        std::vector<int16_t> decimated = Decimator::decimate(drain_buf, mode, target);

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        decimate_time_ms_.store(ms, std::memory_order_relaxed);

        double ratio = (decimated.empty()) ? 1.0
            : static_cast<double>(raw_count) / static_cast<double>(decimated.size());
        decimate_ratio_.store(ratio, std::memory_order_relaxed);

        // Swap to front buffer
        {
            std::lock_guard<std::mutex> lock(mutex_);
            front_buffer_.swap(decimated);
            front_raw_count_ = raw_count;
            new_data_ = true;
        }
    }
}
