#pragma once

// LinearRuntime — Stage pipeline execution engine (RDD §4.2)
// Phase 13: Runtime foundation

#include "grebe/stage.h"
#include "grebe/queue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace grebe {

/// Per-stage telemetry snapshot.
struct StageTelemetry {
    std::string name;
    uint64_t frames_processed   = 0;
    double   avg_process_time_ms = 0.0;
    uint64_t queue_dropped      = 0;  ///< drops in this stage's input queue
};

/// Linear pipeline runtime engine.
///
/// Manages a linear sequence of Stages connected by InProcessQueues.
/// Each Stage runs in its own worker thread. The main thread polls
/// the output queue to consume processed frames.
///
/// Usage:
///   LinearRuntime rt;
///   rt.add_stage(make_unique<DataSourceAdapter>(source));    // SourceStage
///   rt.add_stage(make_unique<DecimationStage>(mode, 3840));  // ProcessingStage
///   rt.start();
///   while (auto frame = rt.poll_output()) { render(*frame); }
///   rt.stop();
class LinearRuntime {
public:
    LinearRuntime();
    ~LinearRuntime();

    LinearRuntime(const LinearRuntime&) = delete;
    LinearRuntime& operator=(const LinearRuntime&) = delete;

    /// Add a stage to the pipeline (call in order).
    /// The first stage is a SourceStage (no input queue).
    /// Subsequent stages get an input queue with the given capacity and policy.
    /// The last stage also gets an output queue for poll_output().
    void add_stage(std::unique_ptr<IStage> stage,
                   size_t queue_capacity = 64,
                   BackpressurePolicy policy = BackpressurePolicy::DropOldest);

    /// Start all worker threads.
    void start();

    /// Stop all workers (bounded time) and join threads.
    void stop();

    /// Whether the runtime is currently running.
    bool running() const;

    /// Poll the output queue (non-blocking). Returns the latest frame or nullopt.
    std::optional<Frame> poll_output();

    /// Drain all frames from output queue, return the latest (discard older ones).
    std::optional<Frame> poll_latest();

    /// Access a stage by index for runtime control (e.g., set_mode).
    IStage* stage(size_t index);
    size_t  stage_count() const;

    /// Snapshot of per-stage telemetry.
    std::vector<StageTelemetry> telemetry() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace grebe
