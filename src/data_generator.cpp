#include "data_generator.h"

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
    stop();
    ring_buffer_ = &ring_buffer;
    target_sample_rate_.store(sample_rate, std::memory_order_relaxed);
    waveform_type_.store(type, std::memory_order_relaxed);
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

void DataGenerator::set_sample_rate(double rate) {
    target_sample_rate_.store(rate, std::memory_order_relaxed);
}

void DataGenerator::set_waveform_type(WaveformType type) {
    waveform_type_.store(type, std::memory_order_relaxed);
}

void DataGenerator::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_relaxed);
}

void DataGenerator::rebuild_period_buffer(double sample_rate, double frequency, WaveformType type) {
    if (type == WaveformType::WhiteNoise) {
        // Noise: pre-generate a large random buffer (~1M samples)
        constexpr size_t NOISE_BUF_SIZE = 1'048'576;
        period_len_ = NOISE_BUF_SIZE;
        period_buf_.resize(NOISE_BUF_SIZE);
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> dist(-32768, 32767);
        for (size_t i = 0; i < NOISE_BUF_SIZE; i++) {
            period_buf_[i] = static_cast<int16_t>(dist(rng));
        }
    } else {
        // Periodic waveforms: compute one exact period
        period_len_ = std::max(size_t(1), static_cast<size_t>(std::round(sample_rate / frequency)));
        period_buf_.resize(period_len_);

        for (size_t i = 0; i < period_len_; i++) {
            double phase = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(period_len_);
            switch (type) {
            case WaveformType::Sine:
                period_buf_[i] = static_cast<int16_t>(std::sin(phase) * 32767.0);
                break;
            case WaveformType::Square:
                period_buf_[i] = (std::sin(phase) >= 0.0) ? static_cast<int16_t>(32767) : static_cast<int16_t>(-32768);
                break;
            case WaveformType::Sawtooth: {
                double norm = static_cast<double>(i) / static_cast<double>(period_len_);
                period_buf_[i] = static_cast<int16_t>((2.0 * norm - 1.0) * 32767.0);
                break;
            }
            default:
                period_buf_[i] = 0;
                break;
            }
        }
    }

    period_pos_ = 0;
    cached_sample_rate_ = sample_rate;
    cached_frequency_ = frequency;
    cached_type_ = type;
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
        WaveformType type = waveform_type_.load(std::memory_order_relaxed);
        bool high_rate = (sample_rate >= 100e6);
        size_t batch_size = high_rate ? BATCH_SIZE_HIGH : BATCH_SIZE_LOW;

        // Scale frequency so ~3 cycles are visible per frame at 60 FPS
        double frequency = std::max(180.0, 3.0 * sample_rate / 1'000'000.0);

        // High-rate periodic waveforms: use memcpy period tiling
        bool use_tiling = high_rate && type != WaveformType::Chirp;

        if (use_tiling) {
            // Rebuild period buffer if parameters changed
            if (sample_rate != cached_sample_rate_ || frequency != cached_frequency_ || type != cached_type_) {
                rebuild_period_buffer(sample_rate, frequency, type);
            }

            // Fill batch by tiling the pre-computed period buffer
            size_t remaining = batch_size;
            int16_t* dst = batch.data();
            while (remaining > 0) {
                size_t chunk = std::min(remaining, period_len_ - period_pos_);
                std::memcpy(dst, &period_buf_[period_pos_], chunk * sizeof(int16_t));
                dst += chunk;
                remaining -= chunk;
                period_pos_ += chunk;
                if (period_pos_ >= period_len_) period_pos_ = 0;
            }
        } else {
            // Low-rate or chirp: per-sample LUT generation
            double lut_increment = frequency * static_cast<double>(SINE_LUT_SIZE) / sample_rate;

            switch (type) {
            case WaveformType::Sine:
                for (size_t i = 0; i < batch_size; i++) {
                    size_t idx = static_cast<size_t>(phase_acc) & (SINE_LUT_SIZE - 1);
                    batch[i] = sine_lut_[idx];
                    phase_acc += lut_increment;
                }
                break;

            case WaveformType::Square:
                for (size_t i = 0; i < batch_size; i++) {
                    size_t idx = static_cast<size_t>(phase_acc) & (SINE_LUT_SIZE - 1);
                    batch[i] = sine_lut_[idx] >= 0 ? static_cast<int16_t>(32767) : static_cast<int16_t>(-32768);
                    phase_acc += lut_increment;
                }
                break;

            case WaveformType::Sawtooth:
                for (size_t i = 0; i < batch_size; i++) {
                    double norm = phase_acc / static_cast<double>(SINE_LUT_SIZE);
                    norm = norm - std::floor(norm); // [0, 1)
                    batch[i] = static_cast<int16_t>((2.0 * norm - 1.0) * 32767.0);
                    phase_acc += lut_increment;
                }
                break;

            case WaveformType::WhiteNoise:
                for (size_t i = 0; i < batch_size; i++) {
                    batch[i] = static_cast<int16_t>(noise_dist(rng));
                }
                phase_acc += lut_increment * static_cast<double>(batch_size);
                break;

            case WaveformType::Chirp:
                for (size_t i = 0; i < batch_size; i++) {
                    size_t idx = static_cast<size_t>(phase_acc) & (SINE_LUT_SIZE - 1);
                    batch[i] = sine_lut_[idx];
                    double t = static_cast<double>(samples_generated + i) / sample_rate;
                    double sweep = std::fmod(t, 1.0);
                    double inst_freq = frequency * (1.0 + 9.0 * sweep);
                    phase_acc += inst_freq * static_cast<double>(SINE_LUT_SIZE) / sample_rate;
                }
                break;
            }

            // Keep phase accumulator from growing unbounded
            if (phase_acc > static_cast<double>(SINE_LUT_SIZE) * 1e6) {
                phase_acc = std::fmod(phase_acc, static_cast<double>(SINE_LUT_SIZE));
            }
        }

        // Push to ring buffer
        size_t pushed = ring_buffer_->push_bulk(batch.data(), batch_size);
        samples_generated += pushed;
        total_samples_.store(samples_generated, std::memory_order_relaxed);

        // Backpressure: yield when ring buffer is full
        if (pushed < batch_size) {
            if (high_rate) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            next_wake = Clock::now();
            continue;
        }

        // Rate measurement (update every 100ms)
        rate_sample_count += pushed;
        auto now = Clock::now();
        auto elapsed = std::chrono::duration<double>(now - rate_timer_start).count();
        if (elapsed >= 0.1) {
            actual_rate_.store(rate_sample_count / elapsed, std::memory_order_relaxed);
            rate_timer_start = now;
            rate_sample_count = 0;
        }

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
