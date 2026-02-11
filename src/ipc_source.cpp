#include "ipc_source.h"
#include "transport.h"
#include "contracts.h"

#include <spdlog/spdlog.h>

IpcSource::IpcSource(ITransportConsumer& transport, uint32_t num_channels)
    : transport_(transport)
    , num_channels_(num_channels) {}

grebe::DataSourceInfo IpcSource::info() const {
    grebe::DataSourceInfo si;
    si.channel_count = num_channels_;
    si.sample_rate_hz = sample_rate_.load(std::memory_order_relaxed);
    si.is_realtime = true;
    return si;
}

void IpcSource::start() {
    started_.store(true, std::memory_order_release);
}

void IpcSource::stop() {
    started_.store(false, std::memory_order_release);
}

grebe::ReadResult IpcSource::read_frame(grebe::FrameBuffer& frame) {
    if (!started_.load(std::memory_order_acquire)) {
        return grebe::ReadResult::EndOfStream;
    }

    FrameHeaderV2 hdr{};
    std::vector<int16_t> payload;

    if (!transport_.receive_frame(hdr, payload)) {
        spdlog::info("IpcSource: pipe closed");
        return grebe::ReadResult::EndOfStream;
    }

    // Update sample rate from header
    if (hdr.sample_rate_hz > 0.0) {
        sample_rate_.store(hdr.sample_rate_hz, std::memory_order_relaxed);
    }

    // Track SG-side drops
    sg_drops_total_.store(hdr.sg_drops_total, std::memory_order_relaxed);

    // Convert to FrameBuffer
    frame.sequence = hdr.sequence;
    frame.producer_ts_ns = hdr.producer_ts_ns;
    frame.channel_count = hdr.channel_count;
    frame.samples_per_channel = hdr.block_length_samples;
    frame.data = std::move(payload);

    return grebe::ReadResult::Ok;
}
