#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <vector>

class VulkanContext;

class BufferManager {
public:
    BufferManager() = default;
    ~BufferManager();

    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;

    void init(VulkanContext& ctx);
    void destroy();

    // Phase 0: one-shot upload of int16 data to device-local vertex buffer
    void upload_vertex_data(const std::vector<int16_t>& data);

    VkBuffer vertex_buffer() const { return vertex_buffer_; }
    uint32_t vertex_count()  const { return vertex_count_; }

    // Phase 1 stubs
    // void begin_triple_buffer_cycle();
    // void swap_buffers();
    // VkBuffer current_draw_buffer() const;

private:
    VulkanContext* ctx_ = nullptr;

    VkBuffer       staging_buffer_      = VK_NULL_HANDLE;
    VmaAllocation  staging_allocation_  = VK_NULL_HANDLE;

    VkBuffer       vertex_buffer_       = VK_NULL_HANDLE;
    VmaAllocation  vertex_allocation_   = VK_NULL_HANDLE;
    uint32_t       vertex_count_        = 0;

    VkCommandPool   transfer_cmd_pool_  = VK_NULL_HANDLE;
    VkCommandBuffer transfer_cmd_buf_   = VK_NULL_HANDLE;

    bool initialized_ = false;
};
