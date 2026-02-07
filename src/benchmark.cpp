#include "benchmark.h"
#include <algorithm>
#include <numeric>

void Benchmark::frame_begin() {
    frame_start_ = Clock::now();
}

void Benchmark::frame_end() {
    auto now = Clock::now();
    auto duration = std::chrono::duration<double, std::milli>(now - frame_start_);
    frame_time_ms_ = duration.count();

    // Rolling average
    frame_times_[frame_index_] = frame_time_ms_;
    frame_index_ = (frame_index_ + 1) % AVG_WINDOW;
    if (frame_count_ < AVG_WINDOW) frame_count_++;

    double sum = 0.0;
    for (int i = 0; i < frame_count_; i++) {
        sum += frame_times_[i];
    }
    frame_time_avg_ = sum / frame_count_;
    fps_ = (frame_time_avg_ > 0.0) ? 1000.0 / frame_time_avg_ : 0.0;
}
