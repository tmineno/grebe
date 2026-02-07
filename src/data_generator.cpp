#include "data_generator.h"

#include <cmath>
#include <random>
#include <algorithm>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

void DataGenerator::thread_func() {
    using Clock = std::chrono::steady_clock;

    constexpr size_t BATCH_SIZE = 4096;
    std::vector<int16_t> batch(BATCH_SIZE);

    double phase = 0.0;
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

        // Scale frequency so ~3 cycles are visible per frame at 60 FPS
        double frequency = std::max(180.0, 3.0 * sample_rate / 1'000'000.0);
        double phase_increment = 2.0 * M_PI * frequency / sample_rate;

        // Generate batch
        for (size_t i = 0; i < BATCH_SIZE; i++) {
            switch (type) {
            case WaveformType::Sine:
                batch[i] = static_cast<int16_t>(std::sin(phase) * 32767.0);
                phase += phase_increment;
                break;
            case WaveformType::Square:
                batch[i] = std::sin(phase) >= 0.0 ? static_cast<int16_t>(32767) : static_cast<int16_t>(-32768);
                phase += phase_increment;
                break;
            case WaveformType::Sawtooth: {
                double norm = std::fmod(phase / (2.0 * M_PI), 1.0);
                if (norm < 0.0) norm += 1.0;
                batch[i] = static_cast<int16_t>((2.0 * norm - 1.0) * 32767.0);
                phase += phase_increment;
                break;
            }
            case WaveformType::WhiteNoise:
                batch[i] = static_cast<int16_t>(noise_dist(rng));
                phase += phase_increment;
                break;
            case WaveformType::Chirp: {
                batch[i] = static_cast<int16_t>(std::sin(phase) * 32767.0);
                double t = static_cast<double>(samples_generated + i) / sample_rate;
                double sweep = std::fmod(t, 1.0);
                double inst_freq = frequency * (1.0 + 9.0 * sweep);
                phase += 2.0 * M_PI * inst_freq / sample_rate;
                break;
            }
            }
        }

        // Keep phase in [0, 2*PI) to avoid precision loss
        phase = std::fmod(phase, 2.0 * M_PI);
        if (phase < 0.0) phase += 2.0 * M_PI;

        // Push to ring buffer
        size_t pushed = ring_buffer_->push_bulk(batch.data(), BATCH_SIZE);
        samples_generated += pushed;
        total_samples_.store(samples_generated, std::memory_order_relaxed);

        // Backpressure: yield when ring buffer is full
        if (pushed < BATCH_SIZE) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
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

        // Pace generation to target sample rate
        double batch_duration = static_cast<double>(BATCH_SIZE) / sample_rate;
        next_wake += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(batch_duration));

        if (next_wake > Clock::now()) {
            std::this_thread::sleep_until(next_wake);
        } else {
            // If more than 100ms behind, reset timer to avoid permanent catch-up
            if (Clock::now() - next_wake > std::chrono::milliseconds(100)) {
                next_wake = Clock::now();
            }
        }
    }
}
