#pragma once

#include <vulkan/vulkan.h>
#include <vector>

class VulkanContext;

class Swapchain {
public:
    Swapchain() = default;
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void init(VulkanContext& ctx, uint32_t width, uint32_t height, bool vsync = true);
    void recreate(VulkanContext& ctx, uint32_t width, uint32_t height);
    void destroy(VkDevice device);

    void set_vsync(bool vsync) { vsync_ = vsync; }
    bool vsync() const { return vsync_; }

    VkSwapchainKHR           handle()       const { return swapchain_; }
    VkFormat                 image_format()  const { return image_format_; }
    VkExtent2D               extent()        const { return extent_; }
    const std::vector<VkImageView>& image_views() const { return image_views_; }
    uint32_t                 image_count()   const { return static_cast<uint32_t>(image_views_.size()); }

private:
    void create(VulkanContext& ctx, uint32_t width, uint32_t height);
    void destroy_views(VkDevice device);

    VkSwapchainKHR           swapchain_    = VK_NULL_HANDLE;
    VkFormat                 image_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D               extent_       = {0, 0};
    std::vector<VkImage>     images_;
    std::vector<VkImageView> image_views_;
    bool                     vsync_        = true;
    bool                     initialized_  = false;
    VkDevice                 device_cache_ = VK_NULL_HANDLE;
};
