#include "profiler.h"
#include "app_command.h"
#include "benchmark.h"
#include "synthetic_source.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>

ProfileRunner::ProfileRunner() {}

void ProfileRunner::build_scenarios() {
    if (scenarios_built_) return;
    scenarios_built_ = true;

    std::string prefix = channel_count_ > 1
        ? std::to_string(channel_count_) + "ch\xc3\x97" : "";  // UTF-8 ×

    scenarios_ = {
        {prefix + "1MSPS",   1'000'000.0,   120, 300, 30.0},
        {prefix + "10MSPS",  10'000'000.0,  120, 300, 30.0},
        {prefix + "100MSPS", 100'000'000.0, 120, 300, 30.0},
        {prefix + "1GSPS",   1'000'000'000.0, 120, 300, 30.0},
    };
}

bool ProfileRunner::should_continue() const {
    return !finished_;
}

void ProfileRunner::init_envelope_verifiers() {
    if (envelope_verifiers_initialized_) return;

    envelope_verifiers_.resize(channel_count_);

    if (synthetic_source_) {
        // Embedded mode: read period buffers from SyntheticSource
        for (uint32_t ch = 0; ch < channel_count_; ch++) {
            WaveformType wf = synthetic_source_->get_channel_waveform(ch);
            if (wf == WaveformType::WhiteNoise || wf == WaveformType::Chirp) {
                continue;
            }

            const int16_t* period_buf = synthetic_source_->period_buffer_ptr(ch);
            size_t period_len = synthetic_source_->period_length(ch);
            if (!period_buf || period_len == 0) continue;

            envelope_verifiers_[ch].set_period(period_buf, period_len);
        }
    } else {
        // IPC mode: generate period buffers locally (Sine waveform assumed for --profile)
        double sample_rate = scenarios_[current_scenario_].sample_rate;
        ipc_period_buffers_.resize(channel_count_);
        for (uint32_t ch = 0; ch < channel_count_; ch++) {
            ipc_period_buffers_[ch] = waveform_utils::generate_sine_period(
                sample_rate, ch, channel_count_);
            envelope_verifiers_[ch].set_period(
                ipc_period_buffers_[ch].data(),
                ipc_period_buffers_[ch].size());
        }
    }

    envelope_verifiers_initialized_ = true;
}

double ProfileRunner::run_envelope_verification(const int16_t* frame_data, uint32_t per_ch_vtx,
                                                  uint32_t raw_samples, grebe::DecimationAlgorithm dec_algo,
                                                  const std::vector<uint32_t>* per_ch_raw) {
    // Only verify MinMax mode with valid data
    if (dec_algo != grebe::DecimationAlgorithm::MinMax || per_ch_vtx == 0 || raw_samples == 0) {
        return -1.0;
    }
    if (!per_ch_raw || per_ch_raw->empty()) return -1.0;

    uint32_t num_buckets = per_ch_vtx / 2;
    if (num_buckets == 0) return -1.0;

    if (envelope_verifiers_.size() != channel_count_) {
        return -1.0;
    }

    double total_match = 0.0;
    uint32_t verified_channels = 0;

    for (uint32_t ch = 0; ch < channel_count_; ch++) {
        auto& verifier = envelope_verifiers_[ch];
        if (!verifier.is_ready()) continue;

        uint32_t ch_raw = (ch < per_ch_raw->size()) ? (*per_ch_raw)[ch] : 0;
        if (ch_raw == 0) continue;

        // Verify: lazily builds and caches window sets per bucket_size
        const int16_t* ch_decimated = frame_data + static_cast<size_t>(ch) * per_ch_vtx;
        EnvelopeResult er = verifier.verify(ch_decimated, num_buckets, ch_raw);

        if (er.match_rate >= 0.0) {
            total_match += er.match_rate;
            verified_channels++;
        }
    }

    if (verified_channels == 0) return -1.0;
    return total_match / static_cast<double>(verified_channels);
}

void ProfileRunner::on_frame(const Benchmark& bench, uint32_t vertex_count,
                             double data_rate,
                             uint64_t total_drops, uint64_t sg_drops,
                             uint32_t raw_samples,
                             AppCommandQueue& cmd_queue,
                             const int16_t* frame_data,
                             uint32_t per_ch_vtx,
                             grebe::DecimationAlgorithm dec_algo,
                             const std::vector<uint32_t>* per_ch_raw) {
    if (finished_) return;

    build_scenarios();

    const auto& scenario = scenarios_[current_scenario_];

    // First frame of scenario: set sample rate
    if (!scenario_started_) {
        scenario_started_ = true;
        frame_in_scenario_ = 0;
        current_samples_.clear();
        current_samples_.reserve(scenario.measure_frames);
        drops_at_start_ = total_drops;
        sg_drops_at_start_ = sg_drops;
        // Clear envelope verifier caches for new scenario (period may change)
        for (auto& v : envelope_verifiers_) v.clear();
        envelope_verifiers_initialized_ = false;
        cmd_queue.push(CmdSetSampleRate{scenario.sample_rate});
        spdlog::info("[profile] Starting scenario '{}' (rate={:.0f}, warmup={}, measure={})",
                     scenario.name, scenario.sample_rate,
                     scenario.warmup_frames, scenario.measure_frames);
    }

    int total_frames = scenario.warmup_frames + scenario.measure_frames;
    bool in_warmup = frame_in_scenario_ < scenario.warmup_frames;

    // Initialize envelope verifiers after rate change has settled.
    // CmdSetSampleRate is processed next frame, so wait a few frames for the
    // DataGenerator to regenerate period buffers and old ring data to drain.
    if (!envelope_verifiers_initialized_ && frame_in_scenario_ >= 10) {
        init_envelope_verifiers();
    }

    // Pre-warm envelope cache during warmup (discard results).
    // build_window_set() is expensive (~10ms at 1 GSPS period_len=333K);
    // running verification during warmup populates the cache so measurement
    // frames don't suffer cold-cache spikes.
    if (in_warmup && envelope_verifiers_initialized_ && frame_data && per_ch_vtx > 0) {
        run_envelope_verification(frame_data, per_ch_vtx, raw_samples, dec_algo, per_ch_raw);
    }

    // Collect metrics during measurement phase
    if (!in_warmup) {
        FrameSample sample;
        sample.frame_time_ms = bench.frame_time_ms();
        sample.drain_ms      = bench.drain_time_avg();
        sample.decimate_ms   = bench.decimation_time_avg();
        sample.upload_ms     = bench.upload_time_avg();
        sample.swap_ms       = bench.swap_time_avg();
        sample.render_ms     = bench.render_time_avg();
        sample.samples       = static_cast<uint32_t>(bench.samples_per_frame_avg());
        sample.vertex_count  = vertex_count;
        sample.decimate_ratio = bench.decimation_ratio();
        sample.data_rate     = data_rate;

        // Window coverage: raw_samples / expected_samples_per_frame
        double frame_ms = bench.frame_time_ms();
        double expected = (frame_ms > 0.0) ? (scenario.sample_rate * frame_ms / 1000.0) : 0.0;
        sample.window_coverage = (expected > 0.0) ? (static_cast<double>(raw_samples) / expected) : 0.0;

        // Envelope verification
        if (frame_data && per_ch_vtx > 0) {
            sample.envelope_match_rate = run_envelope_verification(
                frame_data, per_ch_vtx, raw_samples, dec_algo, per_ch_raw);
        }

        current_samples_.push_back(sample);
    }

    frame_in_scenario_++;

    // Scenario complete?
    if (frame_in_scenario_ >= total_frames) {
        // Compute results
        ScenarioResult result;
        result.config = scenario;

        // Extract per-metric vectors and compute stats
        std::vector<double> v_frame, v_drain, v_dec, v_upload, v_swap, v_render;
        std::vector<double> v_samples, v_vtx, v_rate, v_coverage;
        std::vector<double> v_envelope;

        for (const auto& s : current_samples_) {
            v_frame.push_back(s.frame_time_ms);
            v_drain.push_back(s.drain_ms);
            v_dec.push_back(s.decimate_ms);
            v_upload.push_back(s.upload_ms);
            v_swap.push_back(s.swap_ms);
            v_render.push_back(s.render_ms);
            v_samples.push_back(static_cast<double>(s.samples));
            v_vtx.push_back(static_cast<double>(s.vertex_count));
            v_rate.push_back(s.data_rate);
            v_coverage.push_back(s.window_coverage);
            if (s.envelope_match_rate >= 0.0) {
                v_envelope.push_back(s.envelope_match_rate);
            }
        }

        // Derive FPS from frame_ms using harmonic mean relationship.
        // avg(1000/x) is mathematically incorrect (Jensen's inequality);
        // correct FPS = 1000 / avg(frame_ms).
        result.frame_ms          = compute_stats(v_frame);
        result.fps               = derive_fps_stats(v_frame);
        result.drain_ms          = compute_stats(v_drain);
        result.decimate_ms       = compute_stats(v_dec);
        result.upload_ms         = compute_stats(v_upload);
        result.swap_ms           = compute_stats(v_swap);
        result.render_ms         = compute_stats(v_render);
        result.samples_per_frame = compute_stats(v_samples);
        result.vertex_count      = compute_stats(v_vtx);
        result.data_rate         = compute_stats(v_rate);
        result.window_coverage   = compute_stats(v_coverage);
        result.envelope_match_rate = compute_stats(v_envelope);

        result.drop_total = total_drops - drops_at_start_;
        result.sg_drop_total = sg_drops - sg_drops_at_start_;
        result.pass = result.fps.avg >= scenario.min_fps_threshold;

        spdlog::info("[profile] Scenario '{}' complete: FPS avg={:.1f} min={:.1f} max={:.1f} drops={} coverage={:.1f}% envelope={:.1f}% \xe2\x86\x92 {}",
                     scenario.name, result.fps.avg, result.fps.min, result.fps.max,
                     result.drop_total,
                     result.window_coverage.avg * 100.0,
                     v_envelope.empty() ? -1.0 : result.envelope_match_rate.avg * 100.0,
                     result.pass ? "PASS" : "FAIL");

        results_.push_back(result);

        // Next scenario
        current_scenario_++;
        scenario_started_ = false;

        if (current_scenario_ >= static_cast<int>(scenarios_.size())) {
            finished_ = true;
            cmd_queue.push(CmdQuit{});
        }
    }
}

MetricStats ProfileRunner::compute_stats(const std::vector<double>& values) {
    MetricStats stats;
    if (values.empty()) return stats;

    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());

    double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    stats.avg = sum / static_cast<double>(sorted.size());
    stats.min = sorted.front();
    stats.max = sorted.back();

    auto percentile = [&](double p) -> double {
        double idx = p * static_cast<double>(sorted.size() - 1);
        size_t lo = static_cast<size_t>(idx);
        size_t hi = std::min(lo + 1, sorted.size() - 1);
        double frac = idx - static_cast<double>(lo);
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    };

    stats.p50 = percentile(0.50);
    stats.p95 = percentile(0.95);
    stats.p99 = percentile(0.99);

    return stats;
}

MetricStats ProfileRunner::derive_fps_stats(const std::vector<double>& frame_ms_values) {
    MetricStats stats;
    if (frame_ms_values.empty()) return stats;

    std::vector<double> sorted = frame_ms_values;
    std::sort(sorted.begin(), sorted.end());

    // FPS avg = 1000 / avg(frame_ms) — harmonic mean
    double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    double avg_ms = sum / static_cast<double>(sorted.size());
    stats.avg = (avg_ms > 0.0) ? 1000.0 / avg_ms : 0.0;

    // min FPS from max frame_ms (slowest frame), max FPS from min frame_ms
    stats.min = (sorted.back() > 0.0) ? 1000.0 / sorted.back() : 0.0;
    stats.max = (sorted.front() > 0.0) ? 1000.0 / sorted.front() : 0.0;

    // FPS percentile p_k = 1000 / frame_ms percentile p_{1-k}
    // (monotonic decreasing transform inverts percentile rank)
    auto percentile_ms = [&](double p) -> double {
        double idx = p * static_cast<double>(sorted.size() - 1);
        size_t lo = static_cast<size_t>(idx);
        size_t hi = std::min(lo + 1, sorted.size() - 1);
        double frac = idx - static_cast<double>(lo);
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    };

    double ms_p50 = percentile_ms(0.50);
    double ms_p05 = percentile_ms(0.05);
    double ms_p01 = percentile_ms(0.01);

    stats.p50 = (ms_p50 > 0.0) ? 1000.0 / ms_p50 : 0.0;
    stats.p95 = (ms_p05 > 0.0) ? 1000.0 / ms_p05 : 0.0;
    stats.p99 = (ms_p01 > 0.0) ? 1000.0 / ms_p01 : 0.0;

    return stats;
}

static nlohmann::json stats_to_json(const MetricStats& s) {
    return {
        {"avg", s.avg}, {"min", s.min}, {"max", s.max},
        {"p50", s.p50}, {"p95", s.p95}, {"p99", s.p99}
    };
}

int ProfileRunner::generate_report() const {
    bool overall_pass = true;

    // Stdout report
    spdlog::info("========== PROFILE REPORT ==========");
    spdlog::info("{:<12} {:>8} {:>8} {:>8} {:>10} {:>10} {:>10} {:>10} {:>10} {:>8} {:>8} {:>8}",
                 "Scenario", "FPS avg", "FPS min", "FPS p95",
                 "Frame ms", "Render ms", "Vtx avg", "Smp/f", "Drops",
                 "WinCov%", "Env%", "Result");
    spdlog::info("{}", std::string(122, '-'));

    for (const auto& r : results_) {
        spdlog::info("{:<12} {:>8.1f} {:>8.1f} {:>8.1f} {:>10.2f} {:>10.2f} {:>10.0f} {:>10.0f} {:>10} {:>7.1f}% {:>7.1f}% {:>8}",
                     r.config.name,
                     r.fps.avg, r.fps.min, r.fps.p95,
                     r.frame_ms.avg, r.render_ms.avg,
                     r.vertex_count.avg, r.samples_per_frame.avg,
                     r.drop_total,
                     r.window_coverage.avg * 100.0,
                     r.envelope_match_rate.avg * 100.0,
                     r.pass ? "PASS" : "FAIL");
        if (!r.pass) overall_pass = false;
    }

    spdlog::info("{}", std::string(122, '='));
    spdlog::info("Overall: {}", overall_pass ? "PASS" : "FAIL");

    // JSON report
    nlohmann::json report;

    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    report["timestamp"] = ts;

    nlohmann::json scenarios_json = nlohmann::json::array();
    for (const auto& r : results_) {
        nlohmann::json s;
        s["name"]           = r.config.name;
        s["sample_rate"]    = r.config.sample_rate;
        s["warmup_frames"]  = r.config.warmup_frames;
        s["measure_frames"] = r.config.measure_frames;
        s["results"] = {
            {"fps",               stats_to_json(r.fps)},
            {"frame_ms",          stats_to_json(r.frame_ms)},
            {"drain_ms",          stats_to_json(r.drain_ms)},
            {"decimate_ms",       stats_to_json(r.decimate_ms)},
            {"upload_ms",         stats_to_json(r.upload_ms)},
            {"swap_ms",           stats_to_json(r.swap_ms)},
            {"render_ms",         stats_to_json(r.render_ms)},
            {"samples_per_frame", stats_to_json(r.samples_per_frame)},
            {"vertex_count",      stats_to_json(r.vertex_count)},
            {"data_rate",         stats_to_json(r.data_rate)},
            {"window_coverage",   stats_to_json(r.window_coverage)},
            {"envelope_match_rate", stats_to_json(r.envelope_match_rate)},
        };
        s["drop_total"] = r.drop_total;
        s["sg_drop_total"] = r.sg_drop_total;
        s["pass"] = r.pass;
        scenarios_json.push_back(s);
    }
    report["scenarios"] = scenarios_json;
    report["channel_count"] = channel_count_;
    report["overall_pass"] = overall_pass;

    // Write JSON file
    std::filesystem::create_directories("./tmp");
    char filename_ts[32];
    std::strftime(filename_ts, sizeof(filename_ts), "%Y%m%d_%H%M%S", std::localtime(&t));
    std::string json_path = std::string("./tmp/profile_") + filename_ts + ".json";

    std::ofstream out(json_path);
    if (out.is_open()) {
        out << report.dump(2) << "\n";
        out.close();
        spdlog::info("Profile report saved to: {}", json_path);
    } else {
        spdlog::error("Failed to write profile report to: {}", json_path);
    }

    return overall_pass ? 0 : 1;
}
