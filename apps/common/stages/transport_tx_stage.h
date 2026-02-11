#pragma once

// TransportTxStage — ITransportProducer → IStage wrapper (Phase 12)
// Wraps Pipe/UDP producer as a SinkStage.

#include "grebe/stage.h"
#include "ipc/transport.h"

class TransportTxStage final : public grebe::IStage {
public:
    explicit TransportTxStage(ITransportProducer& producer);

    grebe::StageResult process(const grebe::BatchView& in, grebe::BatchWriter& out,
                               grebe::ExecContext& ctx) override;

    std::string name() const override { return "TransportTxStage"; }

private:
    ITransportProducer& producer_;
};
