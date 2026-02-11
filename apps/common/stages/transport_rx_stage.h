#pragma once

// TransportRxStage — ITransportConsumer → IStage wrapper (Phase 12)
// Wraps Pipe/UDP consumer as a SourceStage.

#include "grebe/stage.h"
#include "ipc/transport.h"

class TransportRxStage final : public grebe::IStage {
public:
    explicit TransportRxStage(ITransportConsumer& consumer);

    grebe::StageResult process(const grebe::BatchView& in, grebe::BatchWriter& out,
                               grebe::ExecContext& ctx) override;

    std::string name() const override { return "TransportRxStage"; }

private:
    ITransportConsumer& consumer_;
    std::vector<int16_t> payload_;  // reusable buffer
};
