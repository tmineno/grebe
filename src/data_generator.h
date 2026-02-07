#pragma once

#include "ring_buffer.h"

#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>

enum class WaveformType {
    Sine,
    Square,
    Sawtooth,
    WhiteNoise,
    Chirp
};

class DataGenerator {
public:
    // Phase 0: generate a static waveform buffer
    static std::vector<int16_t> generate_static(WaveformType type, uint32_t num_samples,
                                                 double frequency = 1.0, double sample_rate = 1920.0);

    DataGenerator() = default;
    ~DataGenerator();

    DataGenerator(const DataGenerator&) = delete;
    DataGenerator& operator=(const DataGenerator&) = delete;

    // Phase 1: threaded streaming generator
    void start(RingBuffer<int16_t>& ring_buffer, double sample_rate, WaveformType type);
    void stop();

    void set_sample_rate(double rate);
    void set_waveform_type(WaveformType type);
    void set_paused(bool paused);

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    bool is_paused()  const { return paused_.load(std::memory_order_relaxed); }
    double target_sample_rate() const { return target_sample_rate_.load(std::memory_order_relaxed); }
    double actual_sample_rate() const { return actual_rate_.load(std::memory_order_relaxed); }
    uint64_t total_samples_generated() const { return total_samples_.load(std::memory_order_relaxed); }

private:
    void thread_func();

    RingBuffer<int16_t>* ring_buffer_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<double> target_sample_rate_{1'000'000.0};
    std::atomic<WaveformType> waveform_type_{WaveformType::Sine};
    std::atomic<double> actual_rate_{0.0};
    std::atomic<uint64_t> total_samples_{0};
};
