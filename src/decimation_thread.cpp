#include "decimation_thread.h"

#include <spdlog/spdlog.h>
#include <chrono>

DecimationThread::~DecimationThread() {
    stop();
}

void DecimationThread::start(RingBuffer<int16_t>& ring, uint32_t target_points, DecimationMode mode) {
    start(std::vector<RingBuffer<int16_t>*>{&ring}, target_points, mode);
}

void DecimationThread::start(std::vector<RingBuffer<int16_t>*> rings, uint32_t target_points, DecimationMode mode) {
    if (running_.load()) return;

    rings_ = std::move(rings);
    channel_count_.store(static_cast<uint32_t>(rings_.size()), std::memory_order_relaxed);
    target_points_.store(target_points, std::memory_order_relaxed);
    mode_.store(mode, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);

    thread_ = std::thread(&DecimationThread::thread_func, this);

    spdlog::info("DecimationThread started (channels={}, target={}, mode={})",
                 rings_.size(), target_points, mode_name(mode));
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

void DecimationThread::set_sample_rate(double rate) {
    sample_rate_.store(rate, std::memory_order_relaxed);
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
    uint32_t num_ch = static_cast<uint32_t>(rings_.size());
    std::vector<std::vector<int16_t>> drain_bufs(num_ch);
    for (uint32_t ch = 0; ch < num_ch; ch++) {
        drain_bufs[ch].reserve(rings_[ch]->capacity());
    }

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // Check if any channel has data
        size_t total_avail = 0;
        for (uint32_t ch = 0; ch < num_ch; ch++) {
            total_avail += rings_[ch]->size();
        }
        if (total_avail == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // Drain all channels
        uint32_t total_raw = 0;
        double max_fill = 0.0;
        for (uint32_t ch = 0; ch < num_ch; ch++) {
            size_t avail = rings_[ch]->size();
            if (avail > 0) {
                drain_bufs[ch].resize(avail);
                size_t popped = rings_[ch]->pop_bulk(drain_bufs[ch].data(), avail);
                drain_bufs[ch].resize(popped);
                total_raw += static_cast<uint32_t>(popped);
            } else {
                drain_bufs[ch].clear();
            }
            double fill = rings_[ch]->fill_ratio();
            if (fill > max_fill) max_fill = fill;
        }

        if (total_raw == 0) continue;

        ring_fill_.store(max_fill, std::memory_order_relaxed);

        // Decimate all channels
        auto mode = mode_.load(std::memory_order_relaxed);
        auto target = target_points_.load(std::memory_order_relaxed);

        // LTTB high-rate guard: force MinMax at >= 100 MSPS
        if (mode == DecimationMode::LTTB && sample_rate_.load(std::memory_order_relaxed) >= 100e6) {
            mode = DecimationMode::MinMax;
        }
        effective_mode_.store(mode, std::memory_order_relaxed);

        auto t0 = std::chrono::steady_clock::now();

        // Concatenate decimated output: [ch0 | ch1 | ... | chN-1]
        std::vector<int16_t> concatenated;
        uint32_t per_ch_vtx = 0;
        for (uint32_t ch = 0; ch < num_ch; ch++) {
            if (drain_bufs[ch].empty()) {
                // No data for this channel, output zeros for target points
                concatenated.resize(concatenated.size() + target, 0);
                per_ch_vtx = target;
            } else {
                std::vector<int16_t> dec = Decimator::decimate(drain_bufs[ch], mode, target);
                per_ch_vtx = static_cast<uint32_t>(dec.size());
                concatenated.insert(concatenated.end(), dec.begin(), dec.end());
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        decimate_time_ms_.store(ms, std::memory_order_relaxed);
        per_ch_vtx_.store(per_ch_vtx, std::memory_order_relaxed);

        double ratio = (concatenated.empty()) ? 1.0
            : static_cast<double>(total_raw) / static_cast<double>(concatenated.size());
        decimate_ratio_.store(ratio, std::memory_order_relaxed);

        // Swap to front buffer
        {
            std::lock_guard<std::mutex> lock(mutex_);
            front_buffer_.swap(concatenated);
            front_raw_count_ = total_raw;
            new_data_ = true;
        }
    }
}
