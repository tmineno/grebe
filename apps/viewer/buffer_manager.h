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

    // Phase 0: one-shot synchronous upload
    void upload_vertex_data(const std::vector<int16_t>& data);

    // Phase 1: async streaming upload (triple-buffered)
    void upload_streaming(const std::vector<int16_t>& data);
    bool try_swap(); // check completed transfers, promote to draw slot

    VkBuffer vertex_buffer() const;
    uint32_t vertex_count()  const;

private:
    static constexpr int SLOT_COUNT = 3;
    static constexpr VkDeviceSize INITIAL_CAPACITY = 2 * 1024 * 1024; // 2 MB

    struct BufferSlot {
        VkBuffer      staging       = VK_NULL_HANDLE;
        VmaAllocation staging_alloc = VK_NULL_HANDLE;
        VkBuffer      device        = VK_NULL_HANDLE;
        VmaAllocation device_alloc  = VK_NULL_HANDLE;
        VkFence       transfer_fence = VK_NULL_HANDLE;
        VkCommandBuffer transfer_cmd = VK_NULL_HANDLE;
        uint32_t      vertex_count  = 0;
        VkDeviceSize  capacity      = 0;
        bool          transfer_pending = false;
    };

    void ensure_slot_capacity(int slot, VkDeviceSize required_size);
    int  find_free_slot();

    VulkanContext* ctx_ = nullptr;
    BufferSlot    slots_[SLOT_COUNT] = {};
    int           draw_slot_ = -1;
    VkCommandPool transfer_cmd_pool_ = VK_NULL_HANDLE;
    bool          initialized_ = false;
};
