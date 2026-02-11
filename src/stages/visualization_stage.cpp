#include "stages/visualization_stage.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace grebe {

VisualizationStage::VisualizationStage(uint32_t display_target_points)
    : display_target_points_(display_target_points) {}

StageResult VisualizationStage::process(const BatchView& in, BatchWriter& out,
                                         ExecContext& /*ctx*/) {
    // 1. Accumulate input frames (may be empty — still produce output below)
    for (size_t i = 0; i < in.size(); ++i) {
        const Frame& frame = in[i];
        const uint32_t ch_count = frame.channel_count;
        const uint32_t spc = frame.samples_per_channel;

        if (ch_count == 0 || spc == 0) continue;

        if (frame.sample_rate_hz > 0.0) {
            // Clear history when sample rate changes (different time density)
            if (last_sample_rate_hz_ > 0.0
                && frame.sample_rate_hz != last_sample_rate_hz_) {
                for (auto& hist : channel_history_) {
                    hist.clear();
                }
                ch0_frame_ends_.clear();
                ch0_total_appended_ = 0;
            }
            last_sample_rate_hz_ = frame.sample_rate_hz;
        }

        // Resize channel history if channel count changed
        if (ch_count != last_channel_count_) {
            channel_history_.resize(ch_count);
            for (uint32_t ch = last_channel_count_; ch < ch_count; ++ch) {
                channel_history_[ch].clear();
            }
            last_channel_count_ = ch_count;
        }

        // Append per-channel samples to history
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            const int16_t* ch_data = frame.data()
                + static_cast<size_t>(ch) * spc;
            channel_history_[ch].insert(
                channel_history_[ch].end(), ch_data, ch_data + spc);
        }

        // Track frame boundary for ch0
        ch0_total_appended_ += spc;
        ch0_frame_ends_.push_back(ch0_total_appended_);
    }

    // 2. Produce display output from accumulated history
    if (last_sample_rate_hz_ <= 0.0 || last_channel_count_ == 0) {
        last_coverage_ = 0.0;
        return StageResult::NoData;
    }

    const double time_span = visible_time_span_s_.load(std::memory_order_relaxed);
    if (time_span <= 0.0) {
        last_coverage_ = 0.0;
        return StageResult::NoData;
    }

    const size_t window_samples = static_cast<size_t>(
        last_sample_rate_hz_ * time_span);
    if (window_samples == 0) {
        last_coverage_ = 0.0;
        return StageResult::NoData;
    }

    // Trim history: keep at most 2x window to limit memory
    const size_t max_history = window_samples * 2;
    for (auto& hist : channel_history_) {
        if (hist.size() > max_history) {
            hist.erase(hist.begin(),
                       hist.begin()
                           + static_cast<ptrdiff_t>(hist.size() - max_history));
        }
    }

    // Prune stale frame boundaries (before current deque start)
    {
        const size_t deque_abs_start = ch0_total_appended_
            - (channel_history_.empty() ? 0 : channel_history_[0].size());
        while (!ch0_frame_ends_.empty() && ch0_frame_ends_.front() <= deque_abs_start) {
            ch0_frame_ends_.pop_front();
        }
    }

    // Check if any channel has data
    bool has_data = false;
    for (const auto& hist : channel_history_) {
        if (!hist.empty()) { has_data = true; break; }
    }
    if (!has_data) {
        last_coverage_ = 0.0;
        return StageResult::NoData;
    }

    // Compute coverage from channel with least data
    size_t min_available = channel_history_[0].size();
    for (uint32_t ch = 1; ch < last_channel_count_; ++ch) {
        min_available = std::min(min_available, channel_history_[ch].size());
    }
    last_coverage_ = std::min(
        1.0,
        static_cast<double>(min_available)
            / static_cast<double>(window_samples));

    // 3. Window + decimate per channel
    const size_t take = std::min(min_available, window_samples);

    std::vector<std::vector<int16_t>> ch_decimated(last_channel_count_);
    uint32_t decimated_spc = 0;
    std::vector<int16_t> ch0_windowed;  // saved for debug dump

    for (uint32_t ch = 0; ch < last_channel_count_; ++ch) {
        auto& hist = channel_history_[ch];
        const size_t start = hist.size() - take;

        // Copy windowed data from deque to contiguous vector
        std::vector<int16_t> windowed(take);
        auto it = hist.begin() + static_cast<ptrdiff_t>(start);
        std::copy(it, hist.end(), windowed.begin());

        // Save ch0 windowed data for potential debug dump
        if (ch == 0 && debug_dump_requested_.load(std::memory_order_relaxed)) {
            ch0_windowed = windowed;
        }

        // Decimate to display target (always MinMax for visual fidelity)
        if (windowed.size() > display_target_points_) {
            ch_decimated[ch] = Decimator::decimate(
                windowed, DecimationMode::MinMax, display_target_points_);
        } else {
            ch_decimated[ch] = std::move(windowed);
        }

        if (ch == 0) {
            decimated_spc = static_cast<uint32_t>(ch_decimated[ch].size());
        }
    }

    // Debug dump (one-shot, ch0 only)
    if (debug_dump_requested_.exchange(false, std::memory_order_relaxed)
        && !ch0_windowed.empty()) {
        // Compute boundary offsets within windowed region
        const size_t win_abs_start = ch0_total_appended_ - take;
        std::vector<size_t> boundary_offsets;
        for (auto b : ch0_frame_ends_) {
            if (b > win_abs_start && b < ch0_total_appended_) {
                boundary_offsets.push_back(b - win_abs_start);
            }
        }
        dump_debug_csv(ch0_windowed, ch_decimated[0], boundary_offsets);
    }

    if (decimated_spc == 0) {
        return StageResult::NoData;
    }

    // 4. Build output frame
    Frame dst = Frame::make_owned(last_channel_count_, decimated_spc);
    dst.sample_rate_hz = last_sample_rate_hz_;

    for (uint32_t ch = 0; ch < last_channel_count_; ++ch) {
        std::memcpy(dst.mutable_data()
                        + static_cast<size_t>(ch) * decimated_spc,
                    ch_decimated[ch].data(),
                    ch_decimated[ch].size() * sizeof(int16_t));
    }

    out.push(std::move(dst));
    return StageResult::Ok;
}

void VisualizationStage::set_visible_time_span(double seconds) {
    visible_time_span_s_.store(seconds, std::memory_order_relaxed);
}

double VisualizationStage::visible_time_span() const {
    return visible_time_span_s_.load(std::memory_order_relaxed);
}

void VisualizationStage::set_display_target_points(uint32_t n) {
    display_target_points_ = n;
}

uint32_t VisualizationStage::display_target_points() const {
    return display_target_points_;
}

double VisualizationStage::window_coverage() const {
    return last_coverage_;
}

void VisualizationStage::request_debug_dump(const std::string& dir) {
    debug_dump_dir_ = dir;
    debug_dump_requested_.store(true, std::memory_order_relaxed);
}

void VisualizationStage::dump_debug_csv(
    const std::vector<int16_t>& windowed,
    const std::vector<int16_t>& decimated,
    const std::vector<size_t>& boundary_offsets) {

    // Build a set of boundary positions for O(1) lookup
    std::vector<bool> is_boundary(windowed.size(), false);
    for (auto off : boundary_offsets) {
        if (off < windowed.size()) {
            is_boundary[off] = true;
        }
    }

    // Windowed data (pre re-decimation)
    {
        std::string path = debug_dump_dir_ + "/viz_debug_windowed_ch0.csv";
        std::ofstream ofs(path);
        if (ofs) {
            ofs << "index,value,frame_boundary\n";
            for (size_t i = 0; i < windowed.size(); ++i) {
                ofs << i << ',' << windowed[i] << ','
                    << (is_boundary[i] ? 1 : 0) << '\n';
            }
            spdlog::info("Debug dump: {} ({} samples, {} boundaries)",
                         path, windowed.size(), boundary_offsets.size());
        }
    }

    // Decimated data (post re-decimation)
    {
        std::string path = debug_dump_dir_ + "/viz_debug_decimated_ch0.csv";
        std::ofstream ofs(path);
        if (ofs) {
            ofs << "index,value\n";
            for (size_t i = 0; i < decimated.size(); ++i) {
                ofs << i << ',' << decimated[i] << '\n';
            }
            spdlog::info("Debug dump: {} ({} samples)", path, decimated.size());
        }
    }
}

} // namespace grebe
