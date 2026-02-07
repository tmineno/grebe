#include "benchmark.h"
#include <spdlog/spdlog.h>
#include <cstdio>

Benchmark::~Benchmark() {
    stop_logging();
}

void Benchmark::RollingAvg::push(double val) {
    values[index] = val;
    index = (index + 1) % AVG_WINDOW;
    if (count < AVG_WINDOW) count++;

    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += values[i];
    }
    avg = sum / count;
}

void Benchmark::frame_begin() {
    frame_start_ = Clock::now();
}

void Benchmark::frame_end() {
    auto now = Clock::now();
    frame_time_ms_ = std::chrono::duration<double, std::milli>(now - frame_start_).count();

    frame_rolling_.push(frame_time_ms_);
    frame_time_avg_ = frame_rolling_.avg;
    fps_ = (frame_time_avg_ > 0.0) ? 1000.0 / frame_time_avg_ : 0.0;

    if (log_file_.is_open()) {
        write_log_row();
    }
}

void Benchmark::set_drain_time(double ms) {
    drain_raw_ = ms;
    drain_rolling_.push(ms);
    drain_avg_ = drain_rolling_.avg;
}

void Benchmark::set_upload_time(double ms) {
    upload_raw_ = ms;
    upload_rolling_.push(ms);
    upload_avg_ = upload_rolling_.avg;
}

void Benchmark::set_swap_time(double ms) {
    swap_raw_ = ms;
    swap_rolling_.push(ms);
    swap_avg_ = swap_rolling_.avg;
}

void Benchmark::set_render_time(double ms) {
    render_raw_ = ms;
    render_rolling_.push(ms);
    render_avg_ = render_rolling_.avg;
}

void Benchmark::set_samples_per_frame(uint32_t n) {
    samples_raw_ = n;
    samples_rolling_.push(static_cast<double>(n));
    samples_avg_ = samples_rolling_.avg;
}

void Benchmark::set_vertex_count(uint32_t n) {
    vtx_raw_ = n;
    vtx_rolling_.push(static_cast<double>(n));
    vtx_avg_ = vtx_rolling_.avg;
}

void Benchmark::set_decimation_time(double ms) {
    decimate_raw_ = ms;
    decimate_rolling_.push(ms);
    decimate_avg_ = decimate_rolling_.avg;
}

void Benchmark::set_decimation_ratio(double ratio) {
    decimate_ratio_raw_ = ratio;
    decimate_ratio_ = ratio;
}

void Benchmark::set_data_rate(double samples_per_sec) {
    data_rate_ = samples_per_sec;
}

void Benchmark::set_ring_fill(double ratio) {
    ring_fill_ = ratio;
}

bool Benchmark::start_logging(const std::string& path) {
    log_file_.open(path, std::ios::out | std::ios::trunc);
    if (!log_file_.is_open()) {
        spdlog::error("Failed to open telemetry log: {}", path);
        return false;
    }
    log_path_ = path;
    log_frame_ = 0;
    log_stdout_counter_ = 0;
    log_start_ = Clock::now();

    // CSV header
    log_file_ << "frame,time_s,frame_ms,fps,drain_ms,decimate_ms,upload_ms,swap_ms,render_ms,"
                 "samples,vtx,decimate_ratio,data_rate,ring_fill\n";

    spdlog::info("Telemetry logging started: {}", path);
    return true;
}

void Benchmark::stop_logging() {
    if (!log_file_.is_open()) return;
    log_file_.close();
    spdlog::info("Telemetry logging stopped: {} frames recorded to {}", log_frame_, log_path_);
}

void Benchmark::write_log_row() {
    double time_s = std::chrono::duration<double>(Clock::now() - log_start_).count();

    // CSV row (every frame)
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%lu,%.4f,%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%.1f,%.0f,%.3f\n",
                  static_cast<unsigned long>(log_frame_), time_s,
                  frame_time_ms_, fps_,
                  drain_raw_, decimate_raw_, upload_raw_, swap_raw_, render_raw_,
                  samples_raw_, vtx_raw_, decimate_ratio_raw_, data_rate_, ring_fill_);
    log_file_ << buf;

    // Stdout summary (throttled to ~1 Hz: every 60 frames)
    if (log_stdout_counter_ % 60 == 0) {
        spdlog::info("[telemetry] frame={} fps={:.1f} frame={:.2f}ms "
                     "drain={:.2f} dec={:.2f}({:.0f}:1) upload={:.2f} swap={:.2f} render={:.2f} "
                     "smp={} vtx={} rate={:.0f} ring={:.1f}%",
                     log_frame_, fps_, frame_time_ms_,
                     drain_raw_, decimate_raw_, decimate_ratio_raw_,
                     upload_raw_, swap_raw_, render_raw_,
                     samples_raw_, vtx_raw_, data_rate_, ring_fill_ * 100.0);
    }

    log_frame_++;
    log_stdout_counter_++;
}
