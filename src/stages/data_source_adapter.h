#pragma once

// DataSourceAdapter — IDataSource → IStage adapter (Phase 12)
// Wraps any IDataSource (SyntheticSource, TransportSource, etc.) as a SourceStage.

#include "grebe/stage.h"
#include "grebe/data_source.h"

namespace grebe {

class DataSourceAdapter final : public IStage {
public:
    explicit DataSourceAdapter(IDataSource& source);

    StageResult process(const BatchView& in, BatchWriter& out,
                        ExecContext& ctx) override;

    std::string name() const override { return "DataSourceAdapter"; }

private:
    IDataSource& source_;
    FrameBuffer fb_;  // reusable buffer to avoid per-call allocation
};

} // namespace grebe
