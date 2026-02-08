#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

class Benchmark {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    ~Benchmark();

    void frame_begin();
    void frame_end();

    // Frame timing
    double fps()            const { return fps_; }
    double frame_time_ms()  const { return frame_time_ms_; }
    double frame_time_avg() const { return frame_time_avg_; }

    // Per-phase telemetry (rolling averages)
    void set_drain_time(double ms);
    void set_upload_time(double ms);
    void set_swap_time(double ms);
    void set_render_time(double ms);
    void set_samples_per_frame(uint32_t n);
    void set_vertex_count(uint32_t n);
    void set_decimation_time(double ms);
    void set_decimation_ratio(double ratio);
    void set_data_rate(double samples_per_sec);
    void set_ring_fill(double ratio);

    double drain_time_avg()  const { return drain_avg_; }
    double upload_time_avg() const { return upload_avg_; }
    double swap_time_avg()   const { return swap_avg_; }
    double render_time_avg() const { return render_avg_; }
    double samples_per_frame_avg() const { return samples_avg_; }
    double vertex_count_avg()      const { return vtx_avg_; }
    double decimation_time_avg()   const { return decimate_avg_; }
    double decimation_ratio()      const { return decimate_ratio_; }

    // Telemetry CSV logging
    bool start_logging(const std::string& path);
    void stop_logging();
    bool is_logging() const { return log_file_.is_open(); }
    std::string log_path() const { return log_path_; }

    // Convenience: get current time for timing sections
    static TimePoint now() { return Clock::now(); }
    static double elapsed_ms(TimePoint start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

private:
    static constexpr int AVG_WINDOW = 60;

    // Rolling average helper
    struct RollingAvg {
        double values[AVG_WINDOW] = {};
        int    index = 0;
        int    count = 0;
        double avg   = 0.0;

        void push(double val);
    };

    void write_log_row();

    TimePoint frame_start_{};
    bool      frame_started_ = false;
    double    frame_time_ms_ = 0.0;
    double    frame_time_avg_ = 0.0;
    double    fps_ = 0.0;
    RollingAvg frame_rolling_{};

    // Per-phase telemetry
    RollingAvg drain_rolling_{};
    RollingAvg upload_rolling_{};
    RollingAvg swap_rolling_{};
    RollingAvg render_rolling_{};
    RollingAvg samples_rolling_{};
    RollingAvg vtx_rolling_{};
    RollingAvg decimate_rolling_{};

    double drain_avg_  = 0.0;
    double upload_avg_ = 0.0;
    double swap_avg_   = 0.0;
    double render_avg_ = 0.0;
    double samples_avg_ = 0.0;
    double vtx_avg_       = 0.0;
    double decimate_avg_  = 0.0;
    double decimate_ratio_ = 1.0;

    // Raw per-frame values (for logging)
    double drain_raw_   = 0.0;
    double upload_raw_  = 0.0;
    double swap_raw_    = 0.0;
    double render_raw_  = 0.0;
    uint32_t samples_raw_ = 0;
    uint32_t vtx_raw_       = 0;
    double decimate_raw_    = 0.0;
    double decimate_ratio_raw_ = 1.0;
    double data_rate_   = 0.0;
    double ring_fill_   = 0.0;

    // CSV log
    std::ofstream log_file_;
    std::string   log_path_;
    uint64_t      log_frame_  = 0;
    uint64_t      log_stdout_counter_ = 0;
    TimePoint     log_start_{};
};
