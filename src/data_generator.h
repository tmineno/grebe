#pragma once

#include <cstdint>
#include <vector>

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

    // Phase 1 stubs
    // void start(double sample_rate, WaveformType type);
    // void stop();
    // void set_sample_rate(double rate);
    // void set_waveform_type(WaveformType type);
};
