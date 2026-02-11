#include "vulkan_context.h"

#include <VkBootstrap.h>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

VulkanContext::~VulkanContext() {
    if (initialized_) {
        destroy();
    }
}

void VulkanContext::init(GLFWwindow* window) {
    // Instance
    vkb::InstanceBuilder instance_builder;
    auto inst_ret = instance_builder
        .set_app_name("vulkan-stream-poc")
        .require_api_version(1, 2, 0)
#ifndef NDEBUG
        .request_validation_layers(true)
        .use_default_debug_messenger()
#endif
        .build();

    if (!inst_ret) {
        throw std::runtime_error("Failed to create Vulkan instance: " + inst_ret.error().message());
    }

    auto vkb_inst = inst_ret.value();
    instance_ = vkb_inst.instance;
    debug_messenger_ = vkb_inst.debug_messenger;

    // Surface
    if (glfwCreateWindowSurface(instance_, window, nullptr, &surface_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create window surface");
    }

    // Physical device
    vkb::PhysicalDeviceSelector selector{vkb_inst};
    auto phys_ret = selector
        .set_minimum_version(1, 2)
        .set_surface(surface_)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .select();

    if (!phys_ret) {
        throw std::runtime_error("Failed to select physical device: " + phys_ret.error().message());
    }

    auto vkb_phys = phys_ret.value();
    physical_device_ = vkb_phys.physical_device;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    spdlog::info("Selected GPU: {}", props.deviceName);

    // Logical device
    vkb::DeviceBuilder device_builder{vkb_phys};
    auto dev_ret = device_builder.build();

    if (!dev_ret) {
        throw std::runtime_error("Failed to create logical device: " + dev_ret.error().message());
    }

    auto vkb_device = dev_ret.value();
    device_ = vkb_device.device;

    // Queues
    auto gq = vkb_device.get_queue(vkb::QueueType::graphics);
    if (!gq) {
        throw std::runtime_error("Failed to get graphics queue");
    }
    graphics_queue_ = gq.value();
    graphics_queue_family_ = vkb_device.get_queue_index(vkb::QueueType::graphics).value();

    auto pq = vkb_device.get_queue(vkb::QueueType::present);
    if (!pq) {
        throw std::runtime_error("Failed to get present queue");
    }
    present_queue_ = pq.value();
    present_queue_family_ = vkb_device.get_queue_index(vkb::QueueType::present).value();

    // VMA allocator
    VmaAllocatorCreateInfo alloc_info = {};
    alloc_info.physicalDevice = physical_device_;
    alloc_info.device = device_;
    alloc_info.instance = instance_;
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_2;

    if (vmaCreateAllocator(&alloc_info, &allocator_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create VMA allocator");
    }

    initialized_ = true;
    spdlog::info("Vulkan context initialized");
}

void VulkanContext::destroy() {
    if (!initialized_) return;

    vkDeviceWaitIdle(device_);

    if (allocator_) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }

    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (debug_messenger_) {
        vkb::destroy_debug_utils_messenger(instance_, debug_messenger_);
        debug_messenger_ = VK_NULL_HANDLE;
    }

    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
    spdlog::info("Vulkan context destroyed");
}
