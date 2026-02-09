#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace waveform_utils {

// Compute waveform frequency for a given sample rate.
// ~3 cycles visible per frame at 60 FPS; minimum 180 Hz.
inline double compute_frequency(double sample_rate) {
    return std::max(180.0, 3.0 * sample_rate / 1'000'000.0);
}

// Compute period length in samples.
inline size_t compute_period_length(double sample_rate, double frequency) {
    return std::max(size_t(1), static_cast<size_t>(std::round(sample_rate / frequency)));
}

// Generate a sine period buffer for a given channel with per-channel phase offset.
inline std::vector<int16_t> generate_sine_period(double sample_rate,
                                                  uint32_t ch,
                                                  uint32_t num_channels) {
    double frequency = compute_frequency(sample_rate);
    size_t period_len = compute_period_length(sample_rate, frequency);
    double ch_phase_offset = M_PI * static_cast<double>(ch) / static_cast<double>(num_channels);

    std::vector<int16_t> buf(period_len);
    for (size_t i = 0; i < period_len; i++) {
        double phase = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(period_len)
                       + ch_phase_offset;
        buf[i] = static_cast<int16_t>(std::sin(phase) * 32767.0);
    }
    return buf;
}

} // namespace waveform_utils
