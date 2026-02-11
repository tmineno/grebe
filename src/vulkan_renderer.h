#pragma once

#include "grebe/render_backend.h"
#include "vulkan_context.h"
#include "swapchain.h"
#include "renderer.h"
#include "buffer_manager.h"

#include <functional>
#include <string>

struct GLFWwindow;

/// VulkanRenderer: IRenderBackend implementation wrapping the Vulkan pipeline.
/// Owns VulkanContext, Swapchain, Renderer, and BufferManager.
class VulkanRenderer : public grebe::IRenderBackend {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer() override;

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    /// Initialize the Vulkan pipeline with a GLFW window.
    void initialize(GLFWwindow* window, const std::string& shader_dir);
    void shutdown();

    // IRenderBackend interface
    void upload_vertices(const int16_t* data, size_t count) override;
    bool swap_buffers() override;
    bool draw_frame(const grebe::DrawCommand* channels, uint32_t num_channels,
                    const grebe::DrawRegion* region) override;
    void on_resize(uint32_t width, uint32_t height) override;
    void set_vsync(bool enabled) override;
    bool vsync() const override;
    uint32_t vertex_count() const override;

    // Vulkan-specific: draw frame with optional overlay (e.g. ImGui HUD)
    using OverlayCallback = std::function<void(VkCommandBuffer)>;
    bool draw_frame_with_overlay(const grebe::DrawCommand* channels, uint32_t num_channels,
                                 const grebe::DrawRegion* region, OverlayCallback overlay_cb = {});

    // Vulkan-specific accessors (needed by HUD init, compute decimator, etc.)
    VulkanContext& vulkan_context() { return ctx_; }
    Swapchain& swapchain_obj() { return swapchain_; }
    Renderer& renderer_obj() { return renderer_; }
    BufferManager& buffer_manager() { return buf_mgr_; }
    VkRenderPass render_pass() const { return renderer_.render_pass(); }
    uint32_t image_count() const { return swapchain_.image_count(); }

private:
    VulkanContext ctx_;
    Swapchain swapchain_;
    Renderer renderer_;
    BufferManager buf_mgr_;
    bool initialized_ = false;
};
