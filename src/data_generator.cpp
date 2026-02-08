#include "data_generator.h"
#include "drop_counter.h"
#include "waveform_utils.h"

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <random>
#include <algorithm>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

DataGenerator::DataGenerator() {
    // Pre-compute sine LUT
    for (size_t i = 0; i < SINE_LUT_SIZE; i++) {
        double phase = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(SINE_LUT_SIZE);
        sine_lut_[i] = static_cast<int16_t>(std::sin(phase) * 32767.0);
    }
    // Initialize per-channel waveforms to Sine
    for (auto& cw : channel_waveforms_) {
        cw.store(WaveformType::Sine, std::memory_order_relaxed);
    }
    cached_types_.fill(WaveformType::Sine);
}

std::vector<int16_t> DataGenerator::generate_static(WaveformType type, uint32_t num_samples,
                                                     double frequency, double sample_rate) {
    std::vector<int16_t> data(num_samples);

    switch (type) {
    case WaveformType::Sine:
        for (uint32_t i = 0; i < num_samples; i++) {
            double t = static_cast<double>(i) / sample_rate;
            double val = std::sin(2.0 * M_PI * frequency * t);
            data[i] = static_cast<int16_t>(val * 32767.0);
        }
        break;

    case WaveformType::Square:
        for (uint32_t i = 0; i < num_samples; i++) {
            double t = static_cast<double>(i) / sample_rate;
            double val = std::sin(2.0 * M_PI * frequency * t);
            data[i] = val >= 0.0 ? 32767 : -32768;
        }
        break;

    case WaveformType::Sawtooth:
        for (uint32_t i = 0; i < num_samples; i++) {
            double t = static_cast<double>(i) / sample_rate;
            double phase = std::fmod(frequency * t, 1.0);
            double val = 2.0 * phase - 1.0;
            data[i] = static_cast<int16_t>(val * 32767.0);
        }
        break;

    case WaveformType::WhiteNoise: {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> dist(-32768, 32767);
        for (uint32_t i = 0; i < num_samples; i++) {
            data[i] = static_cast<int16_t>(dist(rng));
        }
        break;
    }

    case WaveformType::Chirp:
        for (uint32_t i = 0; i < num_samples; i++) {
            double t = static_cast<double>(i) / sample_rate;
            double duration = static_cast<double>(num_samples) / sample_rate;
            double f = frequency + (frequency * 10.0 - frequency) * (t / duration);
            double val = std::sin(2.0 * M_PI * f * t);
            data[i] = static_cast<int16_t>(val * 32767.0);
        }
        break;
    }

    return data;
}

DataGenerator::~DataGenerator() {
    stop();
}

void DataGenerator::start(RingBuffer<int16_t>& ring_buffer, double sample_rate, WaveformType type) {
    start(std::vector<RingBuffer<int16_t>*>{&ring_buffer}, sample_rate, type);
}

void DataGenerator::start(std::vector<RingBuffer<int16_t>*> ring_buffers, double sample_rate, WaveformType type) {
    stop();
    ring_buffers_ = std::move(ring_buffers);
    target_sample_rate_.store(sample_rate, std::memory_order_relaxed);
    waveform_type_.store(type, std::memory_order_relaxed);
    for (auto& cw : channel_waveforms_) {
        cw.store(type, std::memory_order_relaxed);
    }
    stop_requested_.store(false, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);
    total_samples_.store(0, std::memory_order_relaxed);
    actual_rate_.store(0.0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&DataGenerator::thread_func, this);
}

void DataGenerator::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void DataGenerator::set_drop_counters(std::vector<DropCounter*> counters) {
    drop_counters_ = std::move(counters);
}

void DataGenerator::set_sample_rate(double rate) {
    target_sample_rate_.store(rate, std::memory_order_relaxed);
}

void DataGenerator::set_waveform_type(WaveformType type) {
    waveform_type_.store(type, std::memory_order_relaxed);
    for (auto& cw : channel_waveforms_) {
        cw.store(type, std::memory_order_relaxed);
    }
}

void DataGenerator::set_channel_waveform(uint32_t ch, WaveformType type) {
    if (ch < MAX_CHANNELS) {
        channel_waveforms_[ch].store(type, std::memory_order_relaxed);
    }
}

WaveformType DataGenerator::get_channel_waveform(uint32_t ch) const {
    if (ch < MAX_CHANNELS) {
        return channel_waveforms_[ch].load(std::memory_order_relaxed);
    }
    return WaveformType::Sine;
}

void DataGenerator::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_relaxed);
}

void DataGenerator::rebuild_period_buffer(double sample_rate, double frequency) {
    size_t num_channels = ring_buffers_.size();
    channel_states_.resize(num_channels);

    size_t period_len = waveform_utils::compute_period_length(sample_rate, frequency);
    constexpr size_t NOISE_BUF_SIZE = 1'048'576;

    for (size_t ch = 0; ch < num_channels; ch++) {
        auto& cs = channel_states_[ch];
        WaveformType ch_type = channel_waveforms_[ch].load(std::memory_order_relaxed);
        cached_types_[ch] = ch_type;

        if (ch_type == WaveformType::WhiteNoise) {
            cs.period_len = NOISE_BUF_SIZE;
            cs.period_buf.resize(NOISE_BUF_SIZE);
            std::mt19937 rng(42 + static_cast<uint32_t>(ch));
            std::uniform_int_distribution<int> dist(-32768, 32767);
            for (size_t i = 0; i < NOISE_BUF_SIZE; i++) {
                cs.period_buf[i] = static_cast<int16_t>(dist(rng));
            }
        } else {
            cs.period_len = period_len;
            cs.period_buf.resize(period_len);

            double ch_phase_offset = M_PI * static_cast<double>(ch) / static_cast<double>(num_channels);

            for (size_t i = 0; i < period_len; i++) {
                double phase = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(period_len) + ch_phase_offset;
                switch (ch_type) {
                case WaveformType::Sine:
                    cs.period_buf[i] = static_cast<int16_t>(std::sin(phase) * 32767.0);
                    break;
                case WaveformType::Square:
                    cs.period_buf[i] = (std::sin(phase) >= 0.0) ? static_cast<int16_t>(32767) : static_cast<int16_t>(-32768);
                    break;
                case WaveformType::Sawtooth: {
                    double norm = std::fmod(static_cast<double>(i) / static_cast<double>(period_len)
                                            + 0.5 * static_cast<double>(ch) / static_cast<double>(num_channels), 1.0);
                    cs.period_buf[i] = static_cast<int16_t>((2.0 * norm - 1.0) * 32767.0);
                    break;
                }
                default:
                    cs.period_buf[i] = 0;
                    break;
                }
            }
        }
        cs.period_pos = 0;
    }

    cached_sample_rate_ = sample_rate;
    cached_frequency_ = frequency;
}

void DataGenerator::thread_func() {
    using Clock = std::chrono::steady_clock;

    // Batch sizes: larger batches at higher rates to reduce overhead
    constexpr size_t BATCH_SIZE_LOW  = 4096;   // <= 10 MSPS
    constexpr size_t BATCH_SIZE_HIGH = 65536;  // >= 100 MSPS

    std::vector<int16_t> batch(BATCH_SIZE_HIGH); // allocate max upfront

    double phase_acc = 0.0; // fixed-point phase accumulator [0, SINE_LUT_SIZE)
    uint64_t samples_generated = 0;
    auto rate_timer_start = Clock::now();
    uint64_t rate_sample_count = 0;

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> noise_dist(-32768, 32767);

    auto next_wake = Clock::now();

    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (paused_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            next_wake = Clock::now();
            rate_timer_start = Clock::now();
            rate_sample_count = 0;
            actual_rate_.store(0.0, std::memory_order_relaxed);
            continue;
        }

        double sample_rate = target_sample_rate_.load(std::memory_order_relaxed);
        bool high_rate = (sample_rate >= 100e6);
        size_t batch_size = high_rate ? BATCH_SIZE_HIGH : BATCH_SIZE_LOW;

        // Scale frequency so ~3 cycles are visible per frame at 60 FPS
        double frequency = waveform_utils::compute_frequency(sample_rate);

        // Read per-channel waveform types
        size_t num_channels = ring_buffers_.size();

        // Check if any channel has Chirp (cannot tile Chirp)
        bool any_chirp = false;
        for (size_t ch = 0; ch < num_channels; ch++) {
            if (channel_waveforms_[ch].load(std::memory_order_relaxed) == WaveformType::Chirp) {
                any_chirp = true;
                break;
            }
        }

        // Use period tiling for all periodic waveforms (not just high-rate).
        // This ensures exact periodicity for envelope verification at all rates.
        bool use_tiling = !any_chirp;

        // Maintain period buffer for tiling and envelope verification
        {
            bool need_rebuild = (sample_rate != cached_sample_rate_ || frequency != cached_frequency_);
            if (!need_rebuild) {
                for (size_t ch = 0; ch < num_channels; ch++) {
                    if (channel_waveforms_[ch].load(std::memory_order_relaxed) != cached_types_[ch]) {
                        need_rebuild = true;
                        break;
                    }
                }
            }
            if (need_rebuild) {
                rebuild_period_buffer(sample_rate, frequency);
            }
        }

        if (use_tiling) {
            // Fill batch by tiling per-channel period buffer
            for (size_t ch = 0; ch < num_channels; ch++) {
                auto& cs = channel_states_[ch];
                size_t remaining = batch_size;
                int16_t* dst = batch.data();
                while (remaining > 0) {
                    size_t chunk = std::min(remaining, cs.period_len - cs.period_pos);
                    std::memcpy(dst, &cs.period_buf[cs.period_pos], chunk * sizeof(int16_t));
                    dst += chunk;
                    remaining -= chunk;
                    cs.period_pos += chunk;
                    if (cs.period_pos >= cs.period_len) cs.period_pos = 0;
                }
                size_t pushed = ring_buffers_[ch]->push_bulk(batch.data(), batch_size);
                if (ch < drop_counters_.size() && drop_counters_[ch]) {
                    drop_counters_[ch]->record_push(batch_size, pushed);
                }
            }
        } else {
            // Low-rate or chirp: per-sample LUT generation
            double lut_increment = frequency * static_cast<double>(SINE_LUT_SIZE) / sample_rate;

            for (size_t ch = 0; ch < num_channels; ch++) {
                double ch_phase_offset = static_cast<double>(SINE_LUT_SIZE) * 0.5 * static_cast<double>(ch) / static_cast<double>(num_channels);
                double ch_phase = phase_acc + ch_phase_offset;
                WaveformType ch_type = channel_waveforms_[ch].load(std::memory_order_relaxed);

                switch (ch_type) {
                case WaveformType::Sine:
                    for (size_t i = 0; i < batch_size; i++) {
                        size_t idx = static_cast<size_t>(ch_phase) & (SINE_LUT_SIZE - 1);
                        batch[i] = sine_lut_[idx];
                        ch_phase += lut_increment;
                    }
                    break;

                case WaveformType::Square:
                    for (size_t i = 0; i < batch_size; i++) {
                        size_t idx = static_cast<size_t>(ch_phase) & (SINE_LUT_SIZE - 1);
                        batch[i] = sine_lut_[idx] >= 0 ? static_cast<int16_t>(32767) : static_cast<int16_t>(-32768);
                        ch_phase += lut_increment;
                    }
                    break;

                case WaveformType::Sawtooth:
                    for (size_t i = 0; i < batch_size; i++) {
                        double norm = ch_phase / static_cast<double>(SINE_LUT_SIZE);
                        norm = norm - std::floor(norm);
                        batch[i] = static_cast<int16_t>((2.0 * norm - 1.0) * 32767.0);
                        ch_phase += lut_increment;
                    }
                    break;

                case WaveformType::WhiteNoise:
                    for (size_t i = 0; i < batch_size; i++) {
                        batch[i] = static_cast<int16_t>(noise_dist(rng));
                    }
                    break;

                case WaveformType::Chirp:
                    for (size_t i = 0; i < batch_size; i++) {
                        size_t idx = static_cast<size_t>(ch_phase) & (SINE_LUT_SIZE - 1);
                        batch[i] = sine_lut_[idx];
                        double t = static_cast<double>(samples_generated + i) / sample_rate;
                        double sweep = std::fmod(t, 1.0);
                        double inst_freq = frequency * (1.0 + 9.0 * sweep);
                        ch_phase += inst_freq * static_cast<double>(SINE_LUT_SIZE) / sample_rate;
                    }
                    break;
                }

                size_t pushed = ring_buffers_[ch]->push_bulk(batch.data(), batch_size);
                if (ch < drop_counters_.size() && drop_counters_[ch]) {
                    drop_counters_[ch]->record_push(batch_size, pushed);
                }
            }

            // Advance the base phase accumulator (channel 0's amount)
            phase_acc += lut_increment * static_cast<double>(batch_size);

            // Keep phase accumulator from growing unbounded
            if (phase_acc > static_cast<double>(SINE_LUT_SIZE) * 1e6) {
                phase_acc = std::fmod(phase_acc, static_cast<double>(SINE_LUT_SIZE));
            }
        }

        // Track total samples (per-channel count)
        samples_generated += batch_size;
        total_samples_.store(samples_generated, std::memory_order_relaxed);

        // Timestamp for E2E latency measurement (Phase 12)
        last_push_ts_ns_.store(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch()).count()),
            std::memory_order_relaxed);

        // Rate measurement (update every 100ms) — must be before backpressure
        // check, otherwise backpressure `continue` skips counting generated batches
        rate_sample_count += batch_size;
        auto now = Clock::now();
        auto elapsed = std::chrono::duration<double>(now - rate_timer_start).count();
        if (elapsed >= 0.1) {
            actual_rate_.store(rate_sample_count / elapsed, std::memory_order_relaxed);
            rate_timer_start = now;
            rate_sample_count = 0;
        }

        // Backpressure: extra delay when any ring buffer is nearly full
        // (don't skip pacing — that causes uncontrolled generation rate)
        bool any_full = false;
        for (auto* rb : ring_buffers_) {
            if (rb->fill_ratio() > 0.9) { any_full = true; break; }
        }
        if (any_full && !high_rate) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        // Fall through to pacing unconditionally

        // Pacing: target the requested rate
        double batch_duration = static_cast<double>(batch_size) / sample_rate;
        next_wake += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(batch_duration));

        if (next_wake > Clock::now()) {
            if (high_rate) {
                // Busy-wait for high rates (sleep is too coarse for sub-ms timing)
                while (Clock::now() < next_wake) {
                    if (stop_requested_.load(std::memory_order_relaxed)) break;
                    std::this_thread::yield();
                }
            } else {
                std::this_thread::sleep_until(next_wake);
            }
        } else {
            // If more than 100ms behind, reset timer (CPU can't sustain target rate)
            if (Clock::now() - next_wake > std::chrono::milliseconds(100)) {
                next_wake = Clock::now();
            }
        }
    }
}
