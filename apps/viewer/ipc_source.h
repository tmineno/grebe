#pragma once

#include "grebe/data_source.h"

#include <atomic>
#include <cstdint>

class ITransportConsumer;
struct FrameHeaderV2;

/// IpcSource: IDataSource implementation wrapping IPC pipe transport.
/// Receives frames from grebe-sg via PipeConsumer.
class IpcSource : public grebe::IDataSource {
public:
    IpcSource(ITransportConsumer& transport, uint32_t num_channels);
    ~IpcSource() override = default;

    // IDataSource interface
    grebe::DataSourceInfo info() const override;
    grebe::ReadResult read_frame(grebe::FrameBuffer& frame) override;
    void start() override;
    void stop() override;

    // IPC-specific: access the underlying transport for command sending
    ITransportConsumer& transport() { return transport_; }

    // Telemetry propagated from SG headers
    uint64_t sg_drops_total() const { return sg_drops_total_.load(std::memory_order_relaxed); }

private:
    ITransportConsumer& transport_;
    uint32_t num_channels_;
    std::atomic<double> sample_rate_{0.0};
    std::atomic<uint64_t> sg_drops_total_{0};
    std::atomic<bool> started_{false};
};
