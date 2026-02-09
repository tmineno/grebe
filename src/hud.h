#pragma once

#include "decimator.h"

#include <vulkan/vulkan.h>
#include <cstdint>

struct GLFWwindow;
class VulkanContext;
class Benchmark;

class Hud {
public:
    struct WaveformRegion {
        int32_t  x      = 0;
        int32_t  y      = 0;
        uint32_t width  = 1;
        uint32_t height = 1;
    };

    Hud() = default;
    ~Hud();

    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    void init(GLFWwindow* window, VulkanContext& ctx, VkRenderPass render_pass,
              uint32_t image_count);
    void destroy();

    void on_swapchain_recreated(uint32_t min_image_count);

    // Call each frame before draw_frame
    void new_frame();
    void build_status_bar(const Benchmark& bench, double data_rate,
                          double ring_fill, uint32_t vertex_count, bool paused,
                          DecimationMode dec_mode = DecimationMode::None,
                          uint32_t channel_count = 1,
                          uint64_t total_drops = 0,
                          uint64_t sg_drops = 0,
                          uint64_t seq_gaps = 0,
                          double window_coverage = 0.0,
                          double visible_time_span_s = 0.0,
                          double min_time_span_s = 0.0,
                          double max_time_span_s = 0.0,
                          double e2e_latency_ms = 0.0);

    // +1: up, -1: down, 0: no action
    int consume_time_span_step_request();
    WaveformRegion waveform_region() const { return waveform_region_; }

    // Call inside the active render pass
    void render(VkCommandBuffer cmd);

private:
    VkDevice device_cache_ = VK_NULL_HANDLE;
    bool     initialized_  = false;
    int      time_span_step_request_ = 0;
    WaveformRegion waveform_region_{};
};
