#include "synthetic_source.h"
#include "waveform_utils.h"

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SyntheticSource::SyntheticSource(uint32_t num_channels, double sample_rate, WaveformType type)
    : num_channels_(num_channels)
    , target_sample_rate_(sample_rate)
    , waveform_type_(type) {
    // Pre-compute sine LUT
    for (size_t i = 0; i < SINE_LUT_SIZE; i++) {
        double phase = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(SINE_LUT_SIZE);
        sine_lut_[i] = static_cast<int16_t>(std::sin(phase) * 32767.0);
    }
    // Initialize per-channel waveforms
    for (auto& cw : channel_waveforms_) {
        cw.store(type, std::memory_order_relaxed);
    }
    cached_types_.fill(type);
}

grebe::DataSourceInfo SyntheticSource::info() const {
    grebe::DataSourceInfo si;
    si.channel_count = num_channels_;
    si.sample_rate_hz = target_sample_rate_.load(std::memory_order_relaxed);
    si.is_realtime = true;
    return si;
}

void SyntheticSource::start() {
    next_wake_ = Clock::now();
    rate_timer_start_ = Clock::now();
    rate_sample_count_ = 0;
    sequence_ = 0;
    total_samples_ = 0;
    phase_acc_ = 0.0;
    started_.store(true, std::memory_order_release);
}

void SyntheticSource::stop() {
    started_.store(false, std::memory_order_release);
}

void SyntheticSource::set_sample_rate(double rate) {
    target_sample_rate_.store(rate, std::memory_order_relaxed);
}

void SyntheticSource::set_frequency(double hz) {
    target_frequency_.store(std::max(1.0, hz), std::memory_order_relaxed);
}

void SyntheticSource::set_waveform_type(WaveformType type) {
    waveform_type_.store(type, std::memory_order_relaxed);
    for (auto& cw : channel_waveforms_) {
        cw.store(type, std::memory_order_relaxed);
    }
}

void SyntheticSource::set_channel_waveform(uint32_t ch, WaveformType type) {
    if (ch < MAX_CHANNELS) {
        channel_waveforms_[ch].store(type, std::memory_order_relaxed);
    }
}

WaveformType SyntheticSource::get_channel_waveform(uint32_t ch) const {
    if (ch < MAX_CHANNELS) {
        return channel_waveforms_[ch].load(std::memory_order_relaxed);
    }
    return WaveformType::Sine;
}

void SyntheticSource::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_relaxed);
}

const int16_t* SyntheticSource::period_buffer_ptr(uint32_t ch) const {
    if (ch < channel_states_.size()) return channel_states_[ch].period_buf.data();
    return nullptr;
}

size_t SyntheticSource::period_length(uint32_t ch) const {
    if (ch < channel_states_.size()) return channel_states_[ch].period_len;
    return 0;
}

void SyntheticSource::rebuild_period_buffer(double sample_rate, double frequency) {
    channel_states_.resize(num_channels_);

    size_t period_len = waveform_utils::compute_period_length(sample_rate, frequency);
    constexpr size_t NOISE_BUF_SIZE = 1'048'576;

    for (size_t ch = 0; ch < num_channels_; ch++) {
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

            double ch_phase_offset = M_PI * static_cast<double>(ch) / static_cast<double>(num_channels_);

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
                                            + 0.5 * static_cast<double>(ch) / static_cast<double>(num_channels_), 1.0);
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

grebe::ReadResult SyntheticSource::read_frame(grebe::FrameBuffer& frame) {
    if (!started_.load(std::memory_order_acquire)) {
        return grebe::ReadResult::EndOfStream;
    }

    // Handle pause
    if (paused_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        next_wake_ = Clock::now();
        rate_timer_start_ = Clock::now();
        rate_sample_count_ = 0;
        actual_rate_.store(0.0, std::memory_order_relaxed);
        return grebe::ReadResult::NoData;
    }

    double sample_rate = target_sample_rate_.load(std::memory_order_relaxed);
    bool high_rate = (sample_rate >= 100e6);

    constexpr size_t BATCH_SIZE_LOW = 4096;
    constexpr size_t BATCH_SIZE_HIGH = 65536;
    size_t batch_size = high_rate ? BATCH_SIZE_HIGH : BATCH_SIZE_LOW;

    double frequency = target_frequency_.load(std::memory_order_relaxed);
    if (frequency < 1.0) frequency = 1.0;

    // Check for chirp (cannot tile)
    bool any_chirp = false;
    for (uint32_t ch = 0; ch < num_channels_; ch++) {
        if (channel_waveforms_[ch].load(std::memory_order_relaxed) == WaveformType::Chirp) {
            any_chirp = true;
            break;
        }
    }
    bool use_tiling = !any_chirp;

    // Rebuild period buffer if needed
    {
        bool need_rebuild = (sample_rate != cached_sample_rate_ || frequency != cached_frequency_);
        if (!need_rebuild) {
            for (uint32_t ch = 0; ch < num_channels_; ch++) {
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

    // Prepare frame
    frame.sequence = sequence_++;
    frame.channel_count = num_channels_;
    frame.samples_per_channel = static_cast<uint32_t>(batch_size);
    frame.data.resize(num_channels_ * batch_size);

    if (use_tiling) {
        for (uint32_t ch = 0; ch < num_channels_; ch++) {
            auto& cs = channel_states_[ch];
            int16_t* dst = frame.data.data() + static_cast<size_t>(ch) * batch_size;
            size_t remaining = batch_size;
            while (remaining > 0) {
                size_t chunk = std::min(remaining, cs.period_len - cs.period_pos);
                std::memcpy(dst, &cs.period_buf[cs.period_pos], chunk * sizeof(int16_t));
                dst += chunk;
                remaining -= chunk;
                cs.period_pos += chunk;
                if (cs.period_pos >= cs.period_len) cs.period_pos = 0;
            }
        }
    } else {
        // LUT-based generation (chirp / low-rate fallback)
        double lut_increment = frequency * static_cast<double>(SINE_LUT_SIZE) / sample_rate;

        for (uint32_t ch = 0; ch < num_channels_; ch++) {
            double ch_phase_offset = static_cast<double>(SINE_LUT_SIZE) * 0.5
                * static_cast<double>(ch) / static_cast<double>(num_channels_);
            double ch_phase = phase_acc_ + ch_phase_offset;
            WaveformType ch_type = channel_waveforms_[ch].load(std::memory_order_relaxed);
            int16_t* dst = frame.data.data() + static_cast<size_t>(ch) * batch_size;

            switch (ch_type) {
            case WaveformType::Sine:
                for (size_t i = 0; i < batch_size; i++) {
                    size_t idx = static_cast<size_t>(ch_phase) & (SINE_LUT_SIZE - 1);
                    dst[i] = sine_lut_[idx];
                    ch_phase += lut_increment;
                }
                break;
            case WaveformType::Square:
                for (size_t i = 0; i < batch_size; i++) {
                    size_t idx = static_cast<size_t>(ch_phase) & (SINE_LUT_SIZE - 1);
                    dst[i] = sine_lut_[idx] >= 0 ? static_cast<int16_t>(32767) : static_cast<int16_t>(-32768);
                    ch_phase += lut_increment;
                }
                break;
            case WaveformType::Sawtooth:
                for (size_t i = 0; i < batch_size; i++) {
                    double norm = ch_phase / static_cast<double>(SINE_LUT_SIZE);
                    norm = norm - std::floor(norm);
                    dst[i] = static_cast<int16_t>((2.0 * norm - 1.0) * 32767.0);
                    ch_phase += lut_increment;
                }
                break;
            case WaveformType::WhiteNoise:
                for (size_t i = 0; i < batch_size; i++) {
                    dst[i] = static_cast<int16_t>(noise_dist_(rng_));
                }
                break;
            case WaveformType::Chirp:
                for (size_t i = 0; i < batch_size; i++) {
                    size_t idx = static_cast<size_t>(ch_phase) & (SINE_LUT_SIZE - 1);
                    dst[i] = sine_lut_[idx];
                    double t = static_cast<double>(total_samples_ + i) / sample_rate;
                    double sweep = std::fmod(t, 1.0);
                    double inst_freq = frequency * (1.0 + 9.0 * sweep);
                    ch_phase += inst_freq * static_cast<double>(SINE_LUT_SIZE) / sample_rate;
                }
                break;
            }
        }
        phase_acc_ += lut_increment * static_cast<double>(batch_size);
        if (phase_acc_ > static_cast<double>(SINE_LUT_SIZE) * 1e6) {
            phase_acc_ = std::fmod(phase_acc_, static_cast<double>(SINE_LUT_SIZE));
        }
    }

    total_samples_ += batch_size;

    // Timestamp
    frame.producer_ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());

    // Rate measurement (update every 100ms)
    rate_sample_count_ += batch_size;
    auto now = Clock::now();
    auto elapsed = std::chrono::duration<double>(now - rate_timer_start_).count();
    if (elapsed >= 0.1) {
        actual_rate_.store(static_cast<double>(rate_sample_count_) / elapsed, std::memory_order_relaxed);
        rate_timer_start_ = now;
        rate_sample_count_ = 0;
    }

    // Pacing: target the requested rate
    double batch_duration = static_cast<double>(batch_size) / sample_rate;
    next_wake_ += std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(batch_duration));

    if (next_wake_ > Clock::now()) {
        if (high_rate) {
            while (Clock::now() < next_wake_) {
                if (!started_.load(std::memory_order_relaxed)) break;
                std::this_thread::yield();
            }
        } else {
            std::this_thread::sleep_until(next_wake_);
        }
    } else {
        if (Clock::now() - next_wake_ > std::chrono::milliseconds(100)) {
            next_wake_ = Clock::now();
        }
    }

    return grebe::ReadResult::Ok;
}
