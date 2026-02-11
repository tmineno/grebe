#pragma once

// IDataSource — Public data ingestion API (FR-01)
// Implemented in Phase 2

#include <cstdint>
#include <vector>

namespace grebe {

/// Metadata about a data source.
struct DataSourceInfo {
    uint32_t channel_count = 1;
    double sample_rate_hz = 1e6;
    bool is_realtime = true;  // false for file playback
};

/// A block of sample data returned by IDataSource::read_frame().
/// Layout: channel-major [ch0_samples][ch1_samples]...
struct FrameBuffer {
    uint64_t sequence = 0;
    uint64_t producer_ts_ns = 0;
    uint32_t channel_count = 0;
    uint32_t samples_per_channel = 0;
    std::vector<int16_t> data;
};

/// Result of IDataSource::read_frame().
enum class ReadResult {
    Ok,           // Frame read successfully
    NoData,       // No data available right now (try again)
    EndOfStream,  // Source exhausted (e.g., file finished)
    Error         // Unrecoverable error
};

/// Abstract data source interface.
/// Implementations: SyntheticSource (embedded waveform generator),
///                  TransportSource (IPC pipe from grebe-sg),
///                  FileSource (binary file playback).
class IDataSource {
public:
    virtual ~IDataSource() = default;

    /// Source metadata (channel count, sample rate, etc.).
    virtual DataSourceInfo info() const = 0;

    /// Read one block of samples. Blocking behavior is implementation-defined:
    /// - SyntheticSource: blocks for pacing (rate limiting)
    /// - TransportSource: blocks on pipe read
    /// - FileSource: blocks for rate pacing
    virtual ReadResult read_frame(FrameBuffer& frame) = 0;

    /// Prepare the source for reading (e.g., open file, init state).
    virtual void start() = 0;

    /// Stop the source and release resources.
    virtual void stop() = 0;
};

} // namespace grebe
