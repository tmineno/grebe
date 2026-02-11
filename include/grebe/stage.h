#pragma once

// IStage — Unified processing contract (RDD §5.2)
// Phase 10: IStage contract foundation

#include "grebe/batch.h"

#include <string>

namespace grebe {

/// Result of IStage::process() invocation.
enum class StageResult {
    Ok,     ///< Produced output successfully
    NoData, ///< No input available (source stages, try again later)
    EOS,    ///< End of stream — no more data will be produced
    Retry,  ///< Temporary failure, caller should retry
    Error,  ///< Unrecoverable error
};

/// Abstract Stage interface (RDD §5.2).
///
/// All processing units — Source, Transform, Processing, Visualization,
/// Sink — implement this common contract.
///
/// Lifecycle: construct → process()* → destroy
/// A Stage must respond to the Runtime's stop request within bounded time.
class IStage {
public:
    virtual ~IStage() = default;

    /// Process a batch of input frames and produce output frames.
    ///
    /// - SourceStage: `in` is empty; produce frames into `out`.
    /// - ProcessingStage: consume `in`, produce transformed frames into `out`.
    /// - SinkStage: consume `in`; `out` typically empty.
    ///
    /// Borrowed frames received in `in` must be either:
    ///   (a) released after processing, or
    ///   (b) converted to Owned via to_owned() before passing downstream.
    virtual StageResult process(const BatchView& in, BatchWriter& out,
                                ExecContext& ctx) = 0;

    /// Human-readable stage name for telemetry and logging.
    virtual std::string name() const = 0;
};

} // namespace grebe
