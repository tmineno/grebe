#pragma once

#include "ring_buffer.h"

#include <array>
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>

class DropCounter;

enum class WaveformType {
    Sine,
    Square,
    Sawtooth,
    WhiteNoise,
    Chirp
};

class DataGenerator {
public:
    static constexpr size_t SINE_LUT_SIZE = 4096;

    // Phase 0: generate a static waveform buffer
    static std::vector<int16_t> generate_static(WaveformType type, uint32_t num_samples,
                                                 double frequency = 1.0, double sample_rate = 1920.0);

    DataGenerator();
    ~DataGenerator();

    DataGenerator(const DataGenerator&) = delete;
    DataGenerator& operator=(const DataGenerator&) = delete;

    // Phase 1: threaded streaming generator (single channel)
    void start(RingBuffer<int16_t>& ring_buffer, double sample_rate, WaveformType type);
    // Phase 5: multi-channel streaming generator
    void start(std::vector<RingBuffer<int16_t>*> ring_buffers, double sample_rate, WaveformType type);
    void stop();

    void set_sample_rate(double rate);
    void set_waveform_type(WaveformType type);           // sets all channels
    void set_channel_waveform(uint32_t ch, WaveformType type);
    WaveformType get_channel_waveform(uint32_t ch) const;
    void set_paused(bool paused);

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    bool is_paused()  const { return paused_.load(std::memory_order_relaxed); }
    double target_sample_rate() const { return target_sample_rate_.load(std::memory_order_relaxed); }
    double actual_sample_rate() const { return actual_rate_.load(std::memory_order_relaxed); }
    uint64_t total_samples_generated() const { return total_samples_.load(std::memory_order_relaxed); }

    void set_drop_counters(std::vector<DropCounter*> counters);

    static constexpr uint32_t MAX_CHANNELS = 8;

private:
    void thread_func();
    void rebuild_period_buffer(double sample_rate, double frequency);

    // Pre-computed sine lookup table (int16 values)
    std::array<int16_t, SINE_LUT_SIZE> sine_lut_;

    // Period tiling: per-channel pre-computed waveform period for memcpy-based generation
    struct ChannelState {
        std::vector<int16_t> period_buf;
        size_t period_len = 0;
        size_t period_pos = 0;
    };
    std::vector<ChannelState> channel_states_;
    double cached_sample_rate_ = 0;
    double cached_frequency_ = 0;
    std::array<WaveformType, MAX_CHANNELS> cached_types_{};

    std::vector<RingBuffer<int16_t>*> ring_buffers_;
    std::vector<DropCounter*> drop_counters_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<double> target_sample_rate_{1'000'000.0};
    std::atomic<WaveformType> waveform_type_{WaveformType::Sine};
    std::array<std::atomic<WaveformType>, MAX_CHANNELS> channel_waveforms_;
    std::atomic<double> actual_rate_{0.0};
    std::atomic<uint64_t> total_samples_{0};
};
