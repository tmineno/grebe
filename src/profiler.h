#pragma once

#include <cstdint>
#include <string>
#include <vector>

class AppCommandQueue;
class Benchmark;

struct FrameSample {
    double frame_time_ms  = 0.0;
    double drain_ms       = 0.0;
    double decimate_ms    = 0.0;
    double upload_ms      = 0.0;
    double swap_ms        = 0.0;
    double render_ms      = 0.0;
    uint32_t samples      = 0;
    uint32_t vertex_count = 0;
    double decimate_ratio = 1.0;
    double data_rate      = 0.0;
    double ring_fill      = 0.0;
};

struct MetricStats {
    double avg = 0.0;
    double min = 0.0;
    double max = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

struct ScenarioConfig {
    std::string name;
    double sample_rate       = 1'000'000.0;
    int    warmup_frames     = 120;
    int    measure_frames    = 300;
    double min_fps_threshold = 30.0;
};

struct ScenarioResult {
    ScenarioConfig config;
    MetricStats fps;
    MetricStats frame_ms;
    MetricStats drain_ms;
    MetricStats decimate_ms;
    MetricStats upload_ms;
    MetricStats swap_ms;
    MetricStats render_ms;
    MetricStats samples_per_frame;
    MetricStats vertex_count;
    MetricStats data_rate;
    MetricStats ring_fill;
    uint64_t drop_total = 0;     // net viewer-side drops during measurement phase
    uint64_t sg_drop_total = 0;  // SG-side drops at end of measurement phase
    bool pass = false;
};

class ProfileRunner {
public:
    ProfileRunner();

    // Returns true if profiling is active and should continue
    bool should_continue() const;

    // Called each frame to manage scenario transitions.
    // Sets sample rate on data_gen at transitions.
    // Collects metrics during measurement phase.
    // Sets glfwSetWindowShouldClose when all done.
    void on_frame(const Benchmark& bench, uint32_t vertex_count,
                  double data_rate, double ring_fill,
                  uint64_t total_drops, uint64_t sg_drops,
                  AppCommandQueue& cmd_queue);

    void set_channel_count(uint32_t n) { channel_count_ = n; }

    // Generate report to stdout + JSON file.
    // Returns exit code: 0 = all pass, 1 = any fail.
    int generate_report() const;

private:
    static MetricStats compute_stats(const std::vector<double>& values);
    void build_scenarios();

    std::vector<ScenarioConfig> scenarios_;
    std::vector<ScenarioResult> results_;
    std::vector<FrameSample> current_samples_;

    int current_scenario_ = 0;
    int frame_in_scenario_ = 0;
    bool finished_ = false;
    bool scenario_started_ = false;
    bool scenarios_built_ = false;
    uint32_t channel_count_ = 1;
    uint64_t drops_at_start_ = 0;     // snapshot at scenario start
    uint64_t sg_drops_at_start_ = 0;  // snapshot at scenario start (SG-side)
};
