#include "swapchain.h"
#include "vulkan_context.h"

#include <VkBootstrap.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

Swapchain::~Swapchain() {
    if (initialized_ && device_cache_) {
        destroy(device_cache_);
    }
}

void Swapchain::init(VulkanContext& ctx, uint32_t width, uint32_t height, bool vsync) {
    vsync_ = vsync;
    device_cache_ = ctx.device();
    create(ctx, width, height);
}

void Swapchain::recreate(VulkanContext& ctx, uint32_t width, uint32_t height) {
    vkDeviceWaitIdle(ctx.device());
    destroy_views(ctx.device());
    create(ctx, width, height);
}

void Swapchain::create(VulkanContext& ctx, uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder builder{ctx.physical_device(), ctx.device(), ctx.surface()};

    auto present_mode = vsync_ ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR;

    auto swap_ret = builder
        .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(present_mode)
        .set_desired_extent(width, height)
        .set_desired_min_image_count(3)
        .set_old_swapchain(swapchain_)
        .build();

    if (!swap_ret) {
        throw std::runtime_error("Failed to create swapchain: " + swap_ret.error().message());
    }

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx.device(), swapchain_, nullptr);
    }

    auto vkb_swapchain = swap_ret.value();
    swapchain_ = vkb_swapchain.swapchain;
    image_format_ = vkb_swapchain.image_format;
    extent_ = vkb_swapchain.extent;
    images_ = vkb_swapchain.get_images().value();
    image_views_ = vkb_swapchain.get_image_views().value();

    initialized_ = true;
    spdlog::info("Swapchain created: {}x{}, {} images, format {}",
                 extent_.width, extent_.height, images_.size(), static_cast<int>(image_format_));
}

void Swapchain::destroy(VkDevice device) {
    if (!initialized_) return;
    destroy_views(device);
    if (swapchain_) {
        vkDestroySwapchainKHR(device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
}

void Swapchain::destroy_views(VkDevice device) {
    for (auto view : image_views_) {
        vkDestroyImageView(device, view, nullptr);
    }
    image_views_.clear();
    images_.clear();
}
