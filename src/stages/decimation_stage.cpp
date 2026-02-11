#include "stages/decimation_stage.h"

#include <cstring>

namespace grebe {

DecimationStage::DecimationStage(DecimationMode mode, uint32_t target_points)
    : mode_(mode)
    , target_points_(target_points) {}

StageResult DecimationStage::process(const BatchView& in, BatchWriter& out,
                                      ExecContext& /*ctx*/) {
    if (in.empty()) return StageResult::NoData;

    const auto cur_mode = effective_mode();
    const auto cur_target = target_points_.load(std::memory_order_relaxed);

    for (size_t i = 0; i < in.size(); ++i) {
        const Frame& src = in[i];
        const uint32_t ch_count = src.channel_count;
        const uint32_t spc = src.samples_per_channel;

        if (ch_count == 0 || spc == 0) continue;

        // Decimate each channel independently, then concatenate
        std::vector<std::vector<int16_t>> ch_results(ch_count);
        uint32_t decimated_spc = 0;

        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            // Extract single-channel data
            std::vector<int16_t> ch_data(spc);
            std::memcpy(ch_data.data(),
                        src.data() + static_cast<size_t>(ch) * spc,
                        spc * sizeof(int16_t));

            ch_results[ch] = Decimator::decimate(ch_data, cur_mode, cur_target);

            if (ch == 0) {
                decimated_spc = static_cast<uint32_t>(ch_results[ch].size());
            }
        }

        // Build output frame with concatenated decimated channels
        Frame dst = Frame::make_owned(ch_count, decimated_spc);

        // Copy metadata (adjust sample_rate_hz to preserve time span)
        // Use stored sample_rate_ as fallback when frame's rate is 0
        const double input_rate = (src.sample_rate_hz > 0.0)
            ? src.sample_rate_hz
            : sample_rate_.load(std::memory_order_relaxed);
        dst.sequence            = src.sequence;
        dst.producer_ts_ns      = src.producer_ts_ns;
        dst.sample_rate_hz      = (spc > 0 && input_rate > 0.0)
            ? input_rate * (static_cast<double>(decimated_spc) / static_cast<double>(spc))
            : input_rate;
        dst.first_sample_index  = src.first_sample_index;
        dst.flags               = src.flags;

        // Concatenate channel data
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            std::memcpy(dst.mutable_data() + static_cast<size_t>(ch) * decimated_spc,
                        ch_results[ch].data(),
                        ch_results[ch].size() * sizeof(int16_t));
        }

        out.push(std::move(dst));
    }

    return StageResult::Ok;
}

void DecimationStage::set_mode(DecimationMode mode) {
    mode_.store(mode, std::memory_order_relaxed);
}

void DecimationStage::set_target_points(uint32_t n) {
    target_points_.store(n, std::memory_order_relaxed);
}

DecimationMode DecimationStage::mode() const {
    return mode_.load(std::memory_order_relaxed);
}

uint32_t DecimationStage::target_points() const {
    return target_points_.load(std::memory_order_relaxed);
}

void DecimationStage::set_sample_rate(double rate) {
    sample_rate_.store(rate, std::memory_order_relaxed);
}

double DecimationStage::sample_rate() const {
    return sample_rate_.load(std::memory_order_relaxed);
}

DecimationMode DecimationStage::effective_mode() const {
    auto m = mode_.load(std::memory_order_relaxed);
    if (m == DecimationMode::LTTB &&
        sample_rate_.load(std::memory_order_relaxed) >= kLttbHighRateThreshold) {
        return DecimationMode::MinMax;
    }
    return m;
}

} // namespace grebe
