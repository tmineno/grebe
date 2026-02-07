#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <cstdint>

class VulkanContext;
class Swapchain;
class BufferManager;
class Hud;

struct WaveformPushConstants {
    float amplitude_scale   = 1.0f;
    float vertical_offset   = 0.0f;
    float horizontal_scale  = 1.0f;
    float horizontal_offset = 0.0f;
    int   vertex_count      = 0;
};

class Renderer {
public:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void init(VulkanContext& ctx, Swapchain& swapchain, const std::string& shader_dir);
    void destroy();

    // Returns false if swapchain needs recreation
    bool draw_frame(VulkanContext& ctx, Swapchain& swapchain, BufferManager& buf_mgr,
                    const WaveformPushConstants& push_constants, Hud* hud = nullptr);

    void on_swapchain_recreated(VulkanContext& ctx, Swapchain& swapchain);

    VkRenderPass render_pass() const { return render_pass_; }

private:
    void create_render_pass(VkDevice device, VkFormat format);
    void create_framebuffers(VkDevice device, Swapchain& swapchain);
    void create_pipeline(VkDevice device, const std::string& shader_dir);
    void create_sync_objects(VkDevice device);
    void create_command_pool_and_buffers(VulkanContext& ctx);

    VkShaderModule load_shader_module(VkDevice device, const std::string& path);
    void destroy_framebuffers(VkDevice device);

    VkRenderPass     render_pass_     = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_        = VK_NULL_HANDLE;

    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool                   cmd_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer>    cmd_buffers_;

    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence>     in_flight_fences_;

    uint32_t current_frame_ = 0;
    bool initialized_ = false;
    VkDevice device_cache_ = VK_NULL_HANDLE;
};
