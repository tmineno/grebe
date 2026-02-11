#pragma once

// DecimationStage — Decimator → IStage wrapper (Phase 12)
// Wraps the stateless Decimator as a ProcessingStage.

#include "grebe/stage.h"
#include "decimator.h"

#include <atomic>

namespace grebe {

class DecimationStage final : public IStage {
public:
    DecimationStage(DecimationMode mode, uint32_t target_points);

    StageResult process(const BatchView& in, BatchWriter& out,
                        ExecContext& ctx) override;

    std::string name() const override { return "DecimationStage"; }

    void set_mode(DecimationMode mode);
    void set_target_points(uint32_t n);
    void set_sample_rate(double rate);

    DecimationMode mode() const;
    DecimationMode effective_mode() const;
    uint32_t target_points() const;
    double sample_rate() const;

private:
    static constexpr double kLttbHighRateThreshold = 100e6;  // 100 MSPS

    std::atomic<DecimationMode> mode_;
    std::atomic<uint32_t> target_points_;
    std::atomic<double> sample_rate_{0.0};
};

} // namespace grebe
