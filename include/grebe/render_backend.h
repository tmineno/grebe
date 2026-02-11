#pragma once

// IRenderBackend — Abstract rendering backend interface (FR-04)
// Implemented in Phase 3

#include <cstddef>
#include <cstdint>

namespace grebe {

/// Per-channel draw parameters (backend-agnostic equivalent of WaveformPushConstants).
struct DrawCommand {
    float amplitude_scale   = 1.0f;
    float vertical_offset   = 0.0f;
    float horizontal_scale  = 1.0f;
    float horizontal_offset = 0.0f;
    int   vertex_count      = 0;
    int   first_vertex      = 0;
    float color_r           = 0.0f;
    float color_g           = 1.0f;
    float color_b           = 0.0f;
    float color_a           = 1.0f;
};

/// Pixel region for waveform drawing area.
struct DrawRegion {
    int32_t  x      = 0;
    int32_t  y      = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

/// Abstract rendering backend interface.
/// Implementations: VulkanRenderer (Vulkan), future: OpenGLRenderer, SoftwareRenderer.
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    /// Upload vertex data for streaming display.
    virtual void upload_vertices(const int16_t* data, size_t count) = 0;

    /// Promote completed transfers to draw slot. Returns true if new data is ready.
    virtual bool swap_buffers() = 0;

    /// Draw a complete frame with multi-channel waveforms.
    /// Returns false if the display surface needs recreation (e.g., window resize).
    virtual bool draw_frame(const DrawCommand* channels, uint32_t num_channels,
                            const DrawRegion* region) = 0;

    /// Handle window/surface resize.
    virtual void on_resize(uint32_t width, uint32_t height) = 0;

    /// V-Sync control.
    virtual void set_vsync(bool enabled) = 0;
    virtual bool vsync() const = 0;

    /// Current number of vertices in the draw buffer.
    virtual uint32_t vertex_count() const = 0;
};

} // namespace grebe
