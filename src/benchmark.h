#pragma once

#include <chrono>
#include <cstdint>

class Benchmark {
public:
    void frame_begin();
    void frame_end();

    double fps()            const { return fps_; }
    double frame_time_ms()  const { return frame_time_ms_; }
    double frame_time_avg() const { return frame_time_avg_; }

    // Phase 1+ stubs
    // void reset_stats();
    // double gpu_transfer_rate() const;
    // double decimation_rate() const;
    // void export_csv(const std::string& path) const;
    // void export_json(const std::string& path) const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    TimePoint frame_start_{};
    double    frame_time_ms_ = 0.0;
    double    frame_time_avg_ = 0.0;
    double    fps_ = 0.0;

    // Rolling average
    static constexpr int AVG_WINDOW = 60;
    double frame_times_[AVG_WINDOW] = {};
    int    frame_index_ = 0;
    int    frame_count_ = 0;
};
