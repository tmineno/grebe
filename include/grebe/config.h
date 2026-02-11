#pragma once

// PipelineConfig — Configuration structures (FR-07)

#include "grebe/decimation_engine.h"

#include <cstddef>
#include <cstdint>

namespace grebe {

/// Pipeline configuration for the streaming visualization system.
/// Application constructs this from CLI/UI options and passes it to library components.
struct PipelineConfig {
    uint32_t channel_count = 1;           // number of channels (1-8)
    size_t ring_buffer_size = 67'108'864; // per-channel ring capacity (samples)
    DecimationConfig decimation;          // decimation engine settings
    bool vsync = true;                    // V-Sync on/off
};

} // namespace grebe
