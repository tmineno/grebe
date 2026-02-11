#include "stages/transport_tx_stage.h"
#include "ipc/contracts.h"

TransportTxStage::TransportTxStage(ITransportProducer& producer)
    : producer_(producer) {}

grebe::StageResult TransportTxStage::process(const grebe::BatchView& in,
                                              grebe::BatchWriter& /*out*/,
                                              grebe::ExecContext& /*ctx*/) {
    if (in.empty()) return grebe::StageResult::NoData;

    for (size_t i = 0; i < in.size(); ++i) {
        const grebe::Frame& frame = in[i];

        // Build wire header from Frame metadata
        FrameHeaderV2 header{};
        header.sequence              = frame.sequence;
        header.producer_ts_ns        = frame.producer_ts_ns;
        header.channel_count         = frame.channel_count;
        header.block_length_samples  = frame.samples_per_channel;
        header.payload_bytes         = static_cast<uint32_t>(
            frame.data_count() * sizeof(int16_t));
        header.sample_rate_hz        = frame.sample_rate_hz;
        header.first_sample_index    = frame.first_sample_index;

        if (!producer_.send_frame(header, frame.data())) {
            return grebe::StageResult::Error;
        }
    }

    return grebe::StageResult::Ok;
}
