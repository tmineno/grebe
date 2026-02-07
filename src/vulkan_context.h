#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

struct GLFWwindow;

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    void init(GLFWwindow* window);
    void destroy();

    VkInstance       instance()        const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkDevice         device()          const { return device_; }
    VkSurfaceKHR     surface()         const { return surface_; }
    VmaAllocator     allocator()       const { return allocator_; }

    VkQueue          graphics_queue()        const { return graphics_queue_; }
    uint32_t         graphics_queue_family() const { return graphics_queue_family_; }
    VkQueue          present_queue()         const { return present_queue_; }
    uint32_t         present_queue_family()  const { return present_queue_family_; }

private:
    VkInstance       instance_        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice         device_          = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_         = VK_NULL_HANDLE;
    VmaAllocator     allocator_       = VK_NULL_HANDLE;

    VkQueue          graphics_queue_        = VK_NULL_HANDLE;
    uint32_t         graphics_queue_family_ = 0;
    VkQueue          present_queue_         = VK_NULL_HANDLE;
    uint32_t         present_queue_family_  = 0;

    bool initialized_ = false;
};
