#pragma once

// BatchView / BatchWriter / ExecContext — Stage I/O types (RDD §5.2)
// Phase 10: IStage contract foundation

#include "grebe/frame.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace grebe {

/// Const view over a batch of frames (input side of Stage::process).
/// Runtime constructs this by moving frames drained from the upstream queue.
class BatchView {
public:
    BatchView() = default;

    explicit BatchView(std::vector<Frame>&& frames)
        : frames_(std::move(frames)) {}

    size_t size() const { return frames_.size(); }
    bool   empty() const { return frames_.empty(); }

    const Frame& operator[](size_t i) const { return frames_[i]; }

    // Range-for support
    auto begin() const { return frames_.cbegin(); }
    auto end()   const { return frames_.cend(); }

private:
    std::vector<Frame> frames_;
};

/// Frame accumulator (output side of Stage::process).
/// Stage pushes produced frames here; Runtime calls take() to drain.
class BatchWriter {
public:
    BatchWriter() = default;

    void push(Frame&& frame) { frames_.push_back(std::move(frame)); }

    size_t size()  const { return frames_.size(); }
    bool   empty() const { return frames_.empty(); }

    /// Drain all accumulated frames (called by Runtime).
    std::vector<Frame> take() { return std::move(frames_); }

private:
    std::vector<Frame> frames_;
};

/// Execution context passed to Stage::process() by the Runtime.
/// Minimal for Phase 10; extended with telemetry in Phase 13.
struct ExecContext {
    uint64_t iteration   = 0;   ///< Monotonic process() call counter
    uint32_t stage_id    = 0;   ///< Runtime-assigned stage identifier
    double   wall_time_s = 0.0; ///< Seconds since runtime start
};

} // namespace grebe
