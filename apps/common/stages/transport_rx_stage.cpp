#include "stages/transport_rx_stage.h"
#include "ipc/contracts.h"

#include <cstring>

TransportRxStage::TransportRxStage(ITransportConsumer& consumer)
    : consumer_(consumer) {}

grebe::StageResult TransportRxStage::process(const grebe::BatchView& /*in*/,
                                              grebe::BatchWriter& out,
                                              grebe::ExecContext& /*ctx*/) {
    FrameHeaderV2 header{};

    if (!consumer_.receive_frame(header, payload_)) {
        return grebe::StageResult::EOS;
    }

    // Build Frame from wire format
    const uint32_t ch = header.channel_count;
    const uint32_t spc = header.block_length_samples;
    grebe::Frame frame = grebe::Frame::make_owned(ch, spc);

    // Copy metadata (superset: includes fields not in FrameBuffer)
    frame.sequence           = header.sequence;
    frame.producer_ts_ns     = header.producer_ts_ns;
    frame.channel_count      = ch;
    frame.samples_per_channel = spc;
    frame.sample_rate_hz     = header.sample_rate_hz;
    frame.first_sample_index = header.first_sample_index;

    // Copy payload
    const size_t count = static_cast<size_t>(ch) * spc;
    if (count > 0 && payload_.size() >= count) {
        std::memcpy(frame.mutable_data(), payload_.data(),
                    count * sizeof(int16_t));
    }

    out.push(std::move(frame));
    return grebe::StageResult::Ok;
}
