#pragma once

#include "grebe/data_source.h"
#include "waveform_type.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

/// SyntheticSource: IDataSource implementation for embedded waveform generation.
/// Uses period tiling (same algorithm as DataGenerator) but produces FrameBuffers
/// instead of pushing directly to ring buffers.
/// The IngestionThread drives read_frame() calls.
class SyntheticSource : public grebe::IDataSource {
public:
    static constexpr size_t SINE_LUT_SIZE = 4096;
    static constexpr uint32_t MAX_CHANNELS = 8;

    SyntheticSource(uint32_t num_channels, double sample_rate, WaveformType type);
    ~SyntheticSource() override = default;

    // IDataSource interface
    grebe::DataSourceInfo info() const override;
    grebe::ReadResult read_frame(grebe::FrameBuffer& frame) override;
    void start() override;
    void stop() override;

    // Runtime control (called from main thread)
    void set_sample_rate(double rate);
    void set_frequency(double hz);
    void set_waveform_type(WaveformType type);
    void set_channel_waveform(uint32_t ch, WaveformType type);
    WaveformType get_channel_waveform(uint32_t ch) const;
    void set_paused(bool paused);

    bool is_paused() const { return paused_.load(std::memory_order_relaxed); }
    double target_sample_rate() const { return target_sample_rate_.load(std::memory_order_relaxed); }
    double actual_sample_rate() const { return actual_rate_.load(std::memory_order_relaxed); }

    // Envelope verification access (safe when sample rate is stable)
    const int16_t* period_buffer_ptr(uint32_t ch) const;
    size_t period_length(uint32_t ch) const;

private:
    void rebuild_period_buffer(double sample_rate, double frequency);

    uint32_t num_channels_;

    // Sine lookup table
    std::array<int16_t, SINE_LUT_SIZE> sine_lut_;

    // Period tiling state (per-channel)
    struct ChannelState {
        std::vector<int16_t> period_buf;
        size_t period_len = 0;
        size_t period_pos = 0;
    };
    std::vector<ChannelState> channel_states_;
    double cached_sample_rate_ = 0;
    double cached_frequency_ = 0;
    std::array<WaveformType, MAX_CHANNELS> cached_types_{};

    // Atomic settings (written from main thread, read from ingestion thread)
    std::atomic<double> target_sample_rate_;
    std::atomic<double> target_frequency_{1'000.0};
    std::atomic<WaveformType> waveform_type_;
    std::array<std::atomic<WaveformType>, MAX_CHANNELS> channel_waveforms_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> started_{false};

    // Rate measurement
    std::atomic<double> actual_rate_{0.0};

    // Frame generation state (used only from ingestion thread)
    using Clock = std::chrono::steady_clock;
    Clock::time_point next_wake_;
    Clock::time_point rate_timer_start_;
    uint64_t rate_sample_count_ = 0;
    uint64_t sequence_ = 0;
    uint64_t total_samples_ = 0;
    double phase_acc_ = 0.0;
    std::mt19937 rng_{42};
    std::uniform_int_distribution<int> noise_dist_{-32768, 32767};
};
