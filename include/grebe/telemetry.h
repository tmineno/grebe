#pragma once

// TelemetrySnapshot — Per-frame metrics API (FR-05)

#include <cstdint>

namespace grebe {

/// Snapshot of per-frame telemetry (rolling averages from Benchmark).
/// Used by HUD, logging, and external consumers.
struct TelemetrySnapshot {
    double fps = 0.0;
    double frame_time_ms = 0.0;       // rolling avg frame time

    // Per-phase pipeline timing (rolling averages)
    double drain_time_ms = 0.0;
    double upload_time_ms = 0.0;
    double swap_time_ms = 0.0;
    double render_time_ms = 0.0;
    double decimation_time_ms = 0.0;
    double decimation_ratio = 1.0;

    // Data flow
    double data_rate = 0.0;            // samples/sec
    double ring_fill_ratio = 0.0;      // [0, 1]
    double e2e_latency_ms = 0.0;

    // Counts
    uint32_t samples_per_frame = 0;
    uint32_t vertex_count = 0;
};

} // namespace grebe
