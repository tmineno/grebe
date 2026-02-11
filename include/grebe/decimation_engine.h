#pragma once

// DecimationEngine — Public decimation pipeline API (FR-02, FR-03)
// Implemented in Phase 4

#include <cstdint>
#include <memory>
#include <vector>

template <typename T> class RingBuffer;

namespace grebe {

/// Decimation algorithm selection.
enum class DecimationAlgorithm {
    None,    // Pass-through (no decimation)
    MinMax,  // Min-max envelope (preserves peaks)
    LTTB     // Largest-Triangle-Three-Buckets (visually optimal)
};

/// Configuration for the decimation engine.
struct DecimationConfig {
    uint32_t target_points = 3840;      // output vertices per frame (1920*2 for MinMax)
    DecimationAlgorithm algorithm = DecimationAlgorithm::MinMax;
    double sample_rate = 1e6;           // current input sample rate
    double visible_time_span_s = 0.010; // visible time window (seconds)
};

/// Decimated frame output.
struct DecimationOutput {
    std::vector<int16_t> data;                     // concatenated per-channel decimated vertices
    uint32_t per_channel_vertex_count = 0;         // vertices per channel
    uint32_t raw_sample_count = 0;                 // total raw samples consumed
    std::vector<uint32_t> per_channel_raw_counts;  // raw samples consumed per channel
};

/// Decimation telemetry.
struct DecimationMetrics {
    double decimation_time_ms = 0.0;     // time spent in decimation
    double decimation_ratio = 1.0;       // input:output ratio
    double ring_fill_ratio = 0.0;        // ring buffer fill level [0,1]
    DecimationAlgorithm effective_algorithm = DecimationAlgorithm::None;
};

/// DecimationEngine: public facade over the internal DecimationThread.
/// Manages the ring-buffer-to-decimated-output pipeline.
class DecimationEngine {
public:
    DecimationEngine();
    ~DecimationEngine();

    DecimationEngine(const DecimationEngine&) = delete;
    DecimationEngine& operator=(const DecimationEngine&) = delete;

    /// Start the decimation pipeline reading from the given ring buffers.
    void start(std::vector<RingBuffer<int16_t>*> rings, const DecimationConfig& config);
    void stop();

    /// Update configuration at runtime.
    void set_algorithm(DecimationAlgorithm algo);
    void set_sample_rate(double rate);
    void set_visible_time_span(double seconds);
    void set_target_points(uint32_t n);
    void cycle_algorithm();  // None -> MinMax -> LTTB -> None

    /// Try to get the latest decimated frame. Returns true if new data was available.
    bool try_get_frame(DecimationOutput& output);

    /// Current telemetry.
    DecimationMetrics metrics() const;

    /// Number of channels being processed.
    uint32_t channel_count() const;

    /// Algorithm name for display.
    static const char* algorithm_name(DecimationAlgorithm algo);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace grebe
