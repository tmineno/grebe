#pragma once

// VisualizationStage — Display windowing + decimation (Phase 13)
// Accumulates pipeline-decimated samples, windows to visible_time_span,
// and re-decimates to display target points (MinMax) for rendering.

#include "grebe/stage.h"
#include "decimator.h"

#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace grebe {

class VisualizationStage final : public IStage {
public:
    explicit VisualizationStage(uint32_t display_target_points = 3840);

    StageResult process(const BatchView& in, BatchWriter& out,
                        ExecContext& ctx) override;

    std::string name() const override { return "VisualizationStage"; }

    void set_visible_time_span(double seconds);
    double visible_time_span() const;

    void set_display_target_points(uint32_t n);
    uint32_t display_target_points() const;

    /// Fraction of visible window covered by available data [0, 1].
    double window_coverage() const;

    /// Request a one-shot debug CSV dump (next process() call).
    /// Files written to dir/viz_debug_windowed_ch0.csv and viz_debug_decimated_ch0.csv.
    void request_debug_dump(const std::string& dir = "./tmp");

private:
    void dump_debug_csv(const std::vector<int16_t>& windowed,
                        const std::vector<int16_t>& decimated,
                        const std::vector<size_t>& boundary_offsets);

    uint32_t display_target_points_;
    std::atomic<double> visible_time_span_s_{0.010};  // 10ms default

    // Per-channel sample history (accumulates pipeline-decimated data)
    std::vector<std::deque<int16_t>> channel_history_;
    double last_sample_rate_hz_ = 0.0;
    uint32_t last_channel_count_ = 0;
    double last_coverage_ = 0.0;

    // Frame boundary tracking for ch0 (diagnostic)
    std::deque<size_t> ch0_frame_ends_;   // absolute sample index where each frame ends
    size_t ch0_total_appended_ = 0;       // total samples ever appended to ch0

    // Debug dump state
    std::atomic<bool> debug_dump_requested_{false};
    std::string debug_dump_dir_{"./tmp"};
};

} // namespace grebe
