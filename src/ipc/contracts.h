#pragma once

#include <cstdint>

// =========================================================================
// IPC Frame Header (v2)
// =========================================================================
// Sent by grebe-sg (producer) for each block of sample data.
// Layout: [FrameHeaderV2][ch0 int16_t[block_length_samples]][ch1 ...][...]
// Payload is channel-major: all samples for ch0, then ch1, etc.

constexpr uint32_t FRAME_HEADER_MAGIC = 0x32484647;  // 'GFH2' little-endian

struct FrameHeaderV2 {
    uint32_t magic              = FRAME_HEADER_MAGIC;
    uint32_t header_bytes       = sizeof(FrameHeaderV2);
    uint64_t sequence           = 0;
    uint64_t producer_ts_ns     = 0;
    uint32_t channel_count      = 0;
    uint32_t block_length_samples = 0;  // samples per channel
    uint32_t payload_bytes      = 0;    // = channel_count * block_length_samples * sizeof(int16_t)
    uint32_t header_crc32c      = 0;    // placeholder for Phase 8; real CRC in Phase 10
    double   sample_rate_hz     = 0.0;  // current sample rate (grebe-sg authoritative)
    uint64_t sg_drops_total     = 0;    // cumulative SG-side ring buffer drops
};

// =========================================================================
// IPC Command (grebe → grebe-sg)
// =========================================================================
// Sent by grebe (consumer) to control grebe-sg (producer).

constexpr uint32_t IPC_COMMAND_MAGIC = 0x32434947;  // 'GIC2' little-endian

struct IpcCommand {
    enum Type : uint32_t {
        SET_SAMPLE_RATE = 1,
        TOGGLE_PAUSED   = 2,
        QUIT            = 3,
    };

    uint32_t magic  = IPC_COMMAND_MAGIC;
    uint32_t type   = 0;
    double   value  = 0.0;  // used by SET_SAMPLE_RATE; 0 otherwise
};
