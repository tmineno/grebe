#include "profiler.h"
#include "benchmark.h"
#include "data_generator.h"

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>

ProfileRunner::ProfileRunner() {
    scenarios_ = {
        {"1 MSPS",   1'000'000.0,   120, 300, 30.0},
        {"10 MSPS",  10'000'000.0,  120, 300, 30.0},
        {"100 MSPS", 100'000'000.0, 120, 300, 30.0},
        {"1 GSPS",   1'000'000'000.0, 120, 300, 30.0},
    };
}

bool ProfileRunner::should_continue() const {
    return !finished_;
}

void ProfileRunner::on_frame(const Benchmark& bench, uint32_t vertex_count,
                             double data_rate, double ring_fill,
                             DataGenerator& data_gen, GLFWwindow* window) {
    if (finished_) return;

    const auto& scenario = scenarios_[current_scenario_];

    // First frame of scenario: set sample rate
    if (!scenario_started_) {
        scenario_started_ = true;
        frame_in_scenario_ = 0;
        current_samples_.clear();
        current_samples_.reserve(scenario.measure_frames);
        data_gen.set_sample_rate(scenario.sample_rate);
        spdlog::info("[profile] Starting scenario '{}' (rate={:.0f}, warmup={}, measure={})",
                     scenario.name, scenario.sample_rate,
                     scenario.warmup_frames, scenario.measure_frames);
    }

    int total_frames = scenario.warmup_frames + scenario.measure_frames;
    bool in_warmup = frame_in_scenario_ < scenario.warmup_frames;

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
        sample.ring_fill     = ring_fill;
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
        std::vector<double> v_samples, v_vtx, v_rate, v_ring, v_fps;

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
            v_ring.push_back(s.ring_fill);
            v_fps.push_back(s.frame_time_ms > 0.0 ? 1000.0 / s.frame_time_ms : 0.0);
        }

        result.fps               = compute_stats(v_fps);
        result.frame_ms          = compute_stats(v_frame);
        result.drain_ms          = compute_stats(v_drain);
        result.decimate_ms       = compute_stats(v_dec);
        result.upload_ms         = compute_stats(v_upload);
        result.swap_ms           = compute_stats(v_swap);
        result.render_ms         = compute_stats(v_render);
        result.samples_per_frame = compute_stats(v_samples);
        result.vertex_count      = compute_stats(v_vtx);
        result.data_rate         = compute_stats(v_rate);
        result.ring_fill         = compute_stats(v_ring);

        result.pass = result.fps.avg >= scenario.min_fps_threshold;

        spdlog::info("[profile] Scenario '{}' complete: FPS avg={:.1f} min={:.1f} max={:.1f} → {}",
                     scenario.name, result.fps.avg, result.fps.min, result.fps.max,
                     result.pass ? "PASS" : "FAIL");

        results_.push_back(result);

        // Next scenario
        current_scenario_++;
        scenario_started_ = false;

        if (current_scenario_ >= static_cast<int>(scenarios_.size())) {
            finished_ = true;
            glfwSetWindowShouldClose(window, GLFW_TRUE);
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
    spdlog::info("{:<12} {:>8} {:>8} {:>8} {:>10} {:>10} {:>10} {:>10} {:>8}",
                 "Scenario", "FPS avg", "FPS min", "FPS p95",
                 "Frame ms", "Render ms", "Vtx avg", "Smp/f", "Result");
    spdlog::info("{}", std::string(100, '-'));

    for (const auto& r : results_) {
        spdlog::info("{:<12} {:>8.1f} {:>8.1f} {:>8.1f} {:>10.2f} {:>10.2f} {:>10.0f} {:>10.0f} {:>8}",
                     r.config.name,
                     r.fps.avg, r.fps.min, r.fps.p95,
                     r.frame_ms.avg, r.render_ms.avg,
                     r.vertex_count.avg, r.samples_per_frame.avg,
                     r.pass ? "PASS" : "FAIL");
        if (!r.pass) overall_pass = false;
    }

    spdlog::info("{}", std::string(100, '='));
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
            {"ring_fill",         stats_to_json(r.ring_fill)},
        };
        s["pass"] = r.pass;
        scenarios_json.push_back(s);
    }
    report["scenarios"] = scenarios_json;
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
