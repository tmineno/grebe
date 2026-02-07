#pragma once

#include "decimator.h"

#include <vulkan/vulkan.h>
#include <cstdint>

struct GLFWwindow;
class VulkanContext;
class Benchmark;

class Hud {
public:
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
                          DecimationMode dec_mode = DecimationMode::None);

    // Call inside the active render pass
    void render(VkCommandBuffer cmd);

private:
    VkDevice device_cache_ = VK_NULL_HANDLE;
    bool     initialized_  = false;
};
