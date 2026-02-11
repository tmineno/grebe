#include "vulkan_renderer.h"
#include "hud.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <vector>

VulkanRenderer::~VulkanRenderer() {
    if (initialized_) {
        shutdown();
    }
}

void VulkanRenderer::initialize(GLFWwindow* window, const std::string& shader_dir) {
    ctx_.init(window);

    int fb_width, fb_height;
    glfwGetFramebufferSize(window, &fb_width, &fb_height);

    swapchain_.init(ctx_, static_cast<uint32_t>(fb_width), static_cast<uint32_t>(fb_height));
    buf_mgr_.init(ctx_);
    renderer_.init(ctx_, swapchain_, shader_dir);

    initialized_ = true;
}

void VulkanRenderer::shutdown() {
    if (!initialized_) return;
    vkDeviceWaitIdle(ctx_.device());

    renderer_.destroy();
    buf_mgr_.destroy();
    swapchain_.destroy(ctx_.device());
    ctx_.destroy();
    initialized_ = false;
}

void VulkanRenderer::upload_vertices(const int16_t* data, size_t count) {
    if (count == 0) return;
    // Wrap raw pointer in temporary vector (data is not copied, just referenced)
    // BufferManager::upload_streaming takes const vector<int16_t>& so we must copy
    std::vector<int16_t> vec(data, data + count);
    buf_mgr_.upload_streaming(vec);
}

bool VulkanRenderer::swap_buffers() {
    return buf_mgr_.try_swap();
}

bool VulkanRenderer::draw_frame(const grebe::DrawCommand* channels, uint32_t num_channels,
                                const grebe::DrawRegion* region) {
    return draw_frame_with_hud(channels, num_channels, region, nullptr);
}

bool VulkanRenderer::draw_frame_with_hud(const grebe::DrawCommand* channels, uint32_t num_channels,
                                          const grebe::DrawRegion* region, Hud* hud) {
    // Convert DrawCommand[] → WaveformPushConstants[]
    std::vector<WaveformPushConstants> pcs(num_channels);
    for (uint32_t i = 0; i < num_channels; i++) {
        auto& pc = pcs[i];
        auto& dc = channels[i];
        pc.amplitude_scale   = dc.amplitude_scale;
        pc.vertical_offset   = dc.vertical_offset;
        pc.horizontal_scale  = dc.horizontal_scale;
        pc.horizontal_offset = dc.horizontal_offset;
        pc.vertex_count      = dc.vertex_count;
        pc.first_vertex      = dc.first_vertex;
        pc.color_r           = dc.color_r;
        pc.color_g           = dc.color_g;
        pc.color_b           = dc.color_b;
        pc.color_a           = dc.color_a;
    }

    // Convert DrawRegion → DrawRegionPx
    DrawRegionPx draw_region{};
    if (region) {
        draw_region.x      = region->x;
        draw_region.y      = region->y;
        draw_region.width  = region->width;
        draw_region.height = region->height;
    }

    return renderer_.draw_frame(ctx_, swapchain_, buf_mgr_,
                                pcs.data(), num_channels,
                                region ? &draw_region : nullptr, hud);
}

void VulkanRenderer::on_resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    swapchain_.recreate(ctx_, width, height);
    renderer_.on_swapchain_recreated(ctx_, swapchain_);
}

void VulkanRenderer::set_vsync(bool enabled) {
    swapchain_.set_vsync(enabled);
    auto extent = swapchain_.extent();
    if (extent.width > 0 && extent.height > 0) {
        swapchain_.recreate(ctx_, extent.width, extent.height);
        renderer_.on_swapchain_recreated(ctx_, swapchain_);
    }
}

bool VulkanRenderer::vsync() const {
    return swapchain_.vsync();
}

uint32_t VulkanRenderer::vertex_count() const {
    return buf_mgr_.vertex_count();
}
