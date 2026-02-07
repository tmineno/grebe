#include "data_generator.h"

#include <cmath>
#include <random>
#include <algorithm>

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
        std::uniform_int_distribution<int16_t> dist(-32768, 32767);
        for (uint32_t i = 0; i < num_samples; i++) {
            data[i] = dist(rng);
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
