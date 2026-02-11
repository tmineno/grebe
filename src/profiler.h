#pragma once

#include "decimator.h"
#include "envelope_verifier.h"
#include "waveform_utils.h"

#include <cstdint>
#include <string>
#include <vector>

class AppCommandQueue;
class Benchmark;
class SyntheticSource;

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
    double window_coverage = 0.0;       // raw_samples / expected_samples_per_frame
    double envelope_match_rate = -1.0;  // -1.0 = skipped
    double e2e_latency_ms = 0.0;       // producer_ts → render_done
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
    MetricStats window_coverage;
    MetricStats envelope_match_rate;  // excludes skipped frames (-1)
    MetricStats e2e_latency_ms;
    uint64_t drop_total = 0;     // net viewer-side drops during measurement phase
    uint64_t sg_drop_total = 0;  // SG-side drops at end of measurement phase
    uint64_t seq_gaps = 0;       // IPC sequence gaps during measurement phase
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
                  uint64_t seq_gaps, uint32_t raw_samples,
                  double e2e_latency_ms,
                  AppCommandQueue& cmd_queue,
                  const int16_t* frame_data = nullptr,
                  uint32_t per_ch_vtx = 0,
                  DecimationMode dec_mode = DecimationMode::MinMax,
                  const std::vector<uint32_t>* per_ch_raw = nullptr);

    void set_channel_count(uint32_t n) { channel_count_ = n; }
    void set_synthetic_source(SyntheticSource* src) { synthetic_source_ = src; }

    // Generate report to stdout + JSON file.
    // Returns exit code: 0 = all pass, 1 = any fail.
    int generate_report() const;

private:
    static MetricStats compute_stats(const std::vector<double>& values);
    static MetricStats derive_fps_stats(const std::vector<double>& frame_ms_values);
    void build_scenarios();
    void init_envelope_verifiers();
    double run_envelope_verification(const int16_t* frame_data, uint32_t per_ch_vtx,
                                      uint32_t raw_samples, DecimationMode dec_mode,
                                      const std::vector<uint32_t>* per_ch_raw);

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
    uint64_t seq_gaps_at_start_ = 0;  // snapshot at scenario start (IPC seq gaps)

    SyntheticSource* synthetic_source_ = nullptr;
    std::vector<EnvelopeVerifier> envelope_verifiers_;
    std::vector<std::vector<int16_t>> ipc_period_buffers_;  // IPC mode: locally-generated period buffers
    bool envelope_verifiers_initialized_ = false;
};
