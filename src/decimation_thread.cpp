#include "decimation_thread.h"

#include <spdlog/spdlog.h>
#include <algorithm>
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

    uint32_t num_ch = static_cast<uint32_t>(rings_.size());

    // Determine worker count: single-thread for 1ch, multi-thread for 2+ch.
    if (num_ch <= 1) {
        num_workers_ = 0;
    } else {
        uint32_t hw = std::max(1u, std::thread::hardware_concurrency() / 2);
        num_workers_ = std::min({num_ch, hw, 4u});
    }

    // Setup workers if multi-threaded
    if (num_workers_ > 0) {
        workers_.resize(num_workers_);

        // Assign channels round-robin: ch % num_workers
        for (uint32_t w = 0; w < num_workers_; w++) {
            workers_[w].assigned_channels.clear();
        }
        for (uint32_t ch = 0; ch < num_ch; ch++) {
            workers_[ch % num_workers_].assigned_channels.push_back(ch);
        }

        // Pre-allocate per-worker buffers
        for (uint32_t w = 0; w < num_workers_; w++) {
            size_t n = workers_[w].assigned_channels.size();
            workers_[w].drain_bufs.resize(n);
            workers_[w].dec_results.resize(n);
            workers_[w].raw_counts.resize(n, 0);
            for (size_t i = 0; i < n; i++) {
                uint32_t ch = workers_[w].assigned_channels[i];
                workers_[w].drain_bufs[i].reserve(rings_[ch]->capacity());
            }
        }

        // Initialize synchronization state
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            work_generation_ = 0;
            done_count_ = 0;
            workers_exit_ = false;
        }

        // Launch worker threads
        for (uint32_t w = 0; w < num_workers_; w++) {
            workers_[w].thread = std::thread(&DecimationThread::worker_func, this, w);
        }
    }

    thread_ = std::thread(&DecimationThread::thread_func, this);

    spdlog::info("DecimationThread started (channels={}, target={}, mode={}, workers={})",
                 rings_.size(), target_points, mode_name(mode), num_workers_);
}

void DecimationThread::stop() {
    if (!running_.load()) return;

    stop_requested_.store(true, std::memory_order_relaxed);

    // Wake workers so they can observe exit flag
    if (num_workers_ > 0) {
        std::lock_guard<std::mutex> lock(work_mutex_);
        workers_exit_ = true;
        work_cv_.notify_all();
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    for (auto& w : workers_) {
        if (w.thread.joinable()) {
            w.thread.join();
        }
    }

    workers_.clear();
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        workers_exit_ = false;
        work_generation_ = 0;
        done_count_ = 0;
    }
    num_workers_ = 0;
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

bool DecimationThread::try_get_frame(std::vector<int16_t>& output, uint32_t& raw_sample_count,
                                      std::vector<uint32_t>& per_ch_raw_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!new_data_) return false;

    output.swap(front_buffer_);
    raw_sample_count = front_raw_count_;
    per_ch_raw_out = front_per_ch_raw_;
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
    if (num_workers_ == 0) {
        thread_func_single();
    } else {
        thread_func_multi();
    }
}

// Single-channel optimized path (no worker threads)
void DecimationThread::thread_func_single() {
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
        std::vector<uint32_t> per_ch_raw(num_ch, 0);
        for (uint32_t ch = 0; ch < num_ch; ch++) {
            size_t avail = rings_[ch]->size();
            if (avail > 0) {
                drain_bufs[ch].resize(avail);
                size_t popped = rings_[ch]->pop_bulk(drain_bufs[ch].data(), avail);
                drain_bufs[ch].resize(popped);
                per_ch_raw[ch] = static_cast<uint32_t>(popped);
                total_raw += per_ch_raw[ch];
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
            front_per_ch_raw_ = std::move(per_ch_raw);
            new_data_ = true;
        }
    }
}

// Multi-channel path with worker threads using condition_variable sync.
// Uses CV-based signaling instead of std::barrier to avoid aggressive
// spin-waiting that causes CPU starvation on some platforms (Windows/MSVC).
void DecimationThread::thread_func_multi() {
    uint32_t num_ch = static_cast<uint32_t>(rings_.size());

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

        auto t0 = std::chrono::steady_clock::now();

        // Signal workers to start drain+decimate
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            work_generation_++;
            done_count_ = 0;
        }
        work_cv_.notify_all();

        // Wait for all workers to complete
        {
            std::unique_lock<std::mutex> lock(work_mutex_);
            done_cv_.wait(lock, [&] {
                return done_count_ >= num_workers_;
            });
        }

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Gather results: concatenate in channel order [ch0 | ch1 | ... | chN-1]
        std::vector<int16_t> concatenated;
        uint32_t total_raw = 0;
        std::vector<uint32_t> per_ch_raw(num_ch, 0);
        double max_fill = 0.0;
        uint32_t per_ch_vtx = 0;
        auto target = target_points_.load(std::memory_order_relaxed);

        for (uint32_t ch = 0; ch < num_ch; ch++) {
            // Find which worker owns this channel
            uint32_t w = ch % num_workers_;
            // Find the slot index within that worker
            auto& assigned = workers_[w].assigned_channels;
            size_t slot = 0;
            for (size_t i = 0; i < assigned.size(); i++) {
                if (assigned[i] == ch) { slot = i; break; }
            }

            per_ch_raw[ch] = static_cast<uint32_t>(workers_[w].raw_counts[slot]);
            total_raw += per_ch_raw[ch];

            auto& dec = workers_[w].dec_results[slot];
            if (dec.empty()) {
                concatenated.resize(concatenated.size() + target, 0);
                per_ch_vtx = target;
            } else {
                per_ch_vtx = static_cast<uint32_t>(dec.size());
                concatenated.insert(concatenated.end(), dec.begin(), dec.end());
            }

            if (workers_[w].max_fill > max_fill) {
                max_fill = workers_[w].max_fill;
            }
        }

        ring_fill_.store(max_fill, std::memory_order_relaxed);
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
            front_per_ch_raw_ = std::move(per_ch_raw);
            new_data_ = true;
        }

        // Pace at extreme sample rates (≥500 MSPS) to prevent the
        // DataGenerator busy-wait from starving the render thread.
        if (sample_rate_.load(std::memory_order_relaxed) >= 500e6) {
            constexpr auto MIN_FRAME_INTERVAL = std::chrono::milliseconds(2);
            auto frame_elapsed = std::chrono::steady_clock::now() - t0;
            if (frame_elapsed < MIN_FRAME_INTERVAL) {
                std::this_thread::sleep_for(MIN_FRAME_INTERVAL - frame_elapsed);
            }
        }
    }

    // Signal workers to exit
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        workers_exit_ = true;
    }
    work_cv_.notify_all();
}

void DecimationThread::worker_func(uint32_t worker_id) {
    auto& state = workers_[worker_id];
    uint32_t last_generation = 0;

    while (true) {
        // Wait for coordinator to signal new work (or exit)
        {
            std::unique_lock<std::mutex> lock(work_mutex_);
            work_cv_.wait(lock, [&] {
                return workers_exit_ || work_generation_ > last_generation;
            });

            if (workers_exit_) break;
            last_generation = work_generation_;
        }

        // Read current settings
        auto mode_val = mode_.load(std::memory_order_relaxed);
        auto target = target_points_.load(std::memory_order_relaxed);

        // LTTB high-rate guard
        if (mode_val == DecimationMode::LTTB && sample_rate_.load(std::memory_order_relaxed) >= 100e6) {
            mode_val = DecimationMode::MinMax;
        }
        effective_mode_.store(mode_val, std::memory_order_relaxed);

        // Drain and decimate assigned channels
        state.max_fill = 0.0;
        for (size_t i = 0; i < state.assigned_channels.size(); i++) {
            uint32_t ch = state.assigned_channels[i];

            // Drain
            size_t avail = rings_[ch]->size();
            if (avail > 0) {
                state.drain_bufs[i].resize(avail);
                size_t popped = rings_[ch]->pop_bulk(state.drain_bufs[i].data(), avail);
                state.drain_bufs[i].resize(popped);
                state.raw_counts[i] = popped;
            } else {
                state.drain_bufs[i].clear();
                state.raw_counts[i] = 0;
            }

            double fill = rings_[ch]->fill_ratio();
            if (fill > state.max_fill) state.max_fill = fill;

            // Decimate
            if (!state.drain_bufs[i].empty()) {
                state.dec_results[i] = Decimator::decimate(state.drain_bufs[i], mode_val, target);
            } else {
                state.dec_results[i].clear();
            }
        }

        // Signal completion
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            done_count_++;
        }
        done_cv_.notify_one();
    }
}
