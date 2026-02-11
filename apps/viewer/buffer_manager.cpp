#include "buffer_manager.h"
#include "vulkan_context.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <stdexcept>

BufferManager::~BufferManager() {
    if (initialized_) {
        destroy();
    }
}

void BufferManager::init(VulkanContext& ctx) {
    ctx_ = &ctx;

    // Create command pool (resettable, for per-slot command buffers)
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = ctx.graphics_queue_family();

    if (vkCreateCommandPool(ctx.device(), &pool_info, nullptr, &transfer_cmd_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create transfer command pool");
    }

    // Create per-slot fences (start signaled) and command buffers
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = transfer_cmd_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    for (int i = 0; i < SLOT_COUNT; i++) {
        if (vkCreateFence(ctx.device(), &fence_info, nullptr, &slots_[i].transfer_fence) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create transfer fence");
        }
        if (vkAllocateCommandBuffers(ctx.device(), &alloc_info, &slots_[i].transfer_cmd) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate transfer command buffer");
        }
    }

    // Pre-allocate buffers for all slots
    for (int i = 0; i < SLOT_COUNT; i++) {
        ensure_slot_capacity(i, INITIAL_CAPACITY);
    }

    initialized_ = true;
    spdlog::info("BufferManager initialized ({} triple-buffer slots, {} MB each)",
                 SLOT_COUNT, INITIAL_CAPACITY / (1024 * 1024));
}

void BufferManager::ensure_slot_capacity(int slot, VkDeviceSize required_size) {
    auto& s = slots_[slot];
    if (s.capacity >= required_size) return;

    // Destroy old buffers
    if (s.staging != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx_->allocator(), s.staging, s.staging_alloc);
        s.staging = VK_NULL_HANDLE;
    }
    if (s.device != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx_->allocator(), s.device, s.device_alloc);
        s.device = VK_NULL_HANDLE;
    }

    // Create staging buffer (CPU-visible)
    VkBufferCreateInfo staging_info = {};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = required_size;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc_info = {};
    staging_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    staging_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (vmaCreateBuffer(ctx_->allocator(), &staging_info, &staging_alloc_info,
                        &s.staging, &s.staging_alloc, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create staging buffer");
    }

    // Create device-local vertex buffer
    VkBufferCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    device_info.size = required_size;
    device_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo device_alloc_info = {};
    device_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateBuffer(ctx_->allocator(), &device_info, &device_alloc_info,
                        &s.device, &s.device_alloc, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create device buffer");
    }

    s.capacity = required_size;
}

int BufferManager::find_free_slot() {
    // Prefer a slot that's idle (not draw, not transferring)
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (i == draw_slot_) continue;
        if (!slots_[i].transfer_pending) return i;
    }
    // All non-draw slots have pending transfers; wait for one
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (i == draw_slot_) continue;
        vkWaitForFences(ctx_->device(), 1, &slots_[i].transfer_fence, VK_TRUE, UINT64_MAX);
        slots_[i].transfer_pending = false;
        return i;
    }
    return 0; // fallback
}

void BufferManager::upload_vertex_data(const std::vector<int16_t>& data) {
    if (!initialized_ || data.empty()) return;

    VkDeviceSize data_size = data.size() * sizeof(int16_t);
    ensure_slot_capacity(0, data_size);

    auto& s = slots_[0];

    vkWaitForFences(ctx_->device(), 1, &s.transfer_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx_->device(), 1, &s.transfer_fence);

    void* mapped = nullptr;
    vmaMapMemory(ctx_->allocator(), s.staging_alloc, &mapped);
    std::memcpy(mapped, data.data(), data_size);
    vmaUnmapMemory(ctx_->allocator(), s.staging_alloc);

    vkResetCommandBuffer(s.transfer_cmd, 0);
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.transfer_cmd, &begin_info);

    VkBufferCopy copy_region = {};
    copy_region.size = data_size;
    vkCmdCopyBuffer(s.transfer_cmd, s.staging, s.device, 1, &copy_region);

    vkEndCommandBuffer(s.transfer_cmd);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &s.transfer_cmd;

    vkQueueSubmit(ctx_->graphics_queue(), 1, &submit_info, s.transfer_fence);
    vkWaitForFences(ctx_->device(), 1, &s.transfer_fence, VK_TRUE, UINT64_MAX);

    s.vertex_count = static_cast<uint32_t>(data.size());
    s.transfer_pending = false;
    draw_slot_ = 0;

    spdlog::info("Uploaded {} vertices ({} bytes) to GPU [sync]", s.vertex_count, data_size);
}

void BufferManager::upload_streaming(const std::vector<int16_t>& data) {
    if (!initialized_ || data.empty()) return;

    VkDeviceSize data_size = data.size() * sizeof(int16_t);
    int slot = find_free_slot();

    ensure_slot_capacity(slot, data_size);

    auto& s = slots_[slot];

    // Wait for any previous transfer on this slot
    vkWaitForFences(ctx_->device(), 1, &s.transfer_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx_->device(), 1, &s.transfer_fence);

    // Copy data to staging buffer
    void* mapped = nullptr;
    vmaMapMemory(ctx_->allocator(), s.staging_alloc, &mapped);
    std::memcpy(mapped, data.data(), data_size);
    vmaUnmapMemory(ctx_->allocator(), s.staging_alloc);

    // Record transfer command
    vkResetCommandBuffer(s.transfer_cmd, 0);

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.transfer_cmd, &begin_info);

    VkBufferCopy copy_region = {};
    copy_region.size = data_size;
    vkCmdCopyBuffer(s.transfer_cmd, s.staging, s.device, 1, &copy_region);

    vkEndCommandBuffer(s.transfer_cmd);

    // Submit async transfer
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &s.transfer_cmd;

    vkQueueSubmit(ctx_->graphics_queue(), 1, &submit_info, s.transfer_fence);

    s.vertex_count = static_cast<uint32_t>(data.size());
    s.transfer_pending = true;
}

bool BufferManager::try_swap() {
    int best_slot = -1;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!slots_[i].transfer_pending) continue;
        VkResult result = vkGetFenceStatus(ctx_->device(), slots_[i].transfer_fence);
        if (result == VK_SUCCESS) {
            slots_[i].transfer_pending = false;
            best_slot = i;
        }
    }

    if (best_slot >= 0 && best_slot != draw_slot_) {
        draw_slot_ = best_slot;
        return true;
    }
    return false;
}

VkBuffer BufferManager::vertex_buffer() const {
    if (draw_slot_ < 0) return VK_NULL_HANDLE;
    return slots_[draw_slot_].device;
}

uint32_t BufferManager::vertex_count() const {
    if (draw_slot_ < 0) return 0;
    return slots_[draw_slot_].vertex_count;
}

void BufferManager::destroy() {
    if (!initialized_) return;

    vkDeviceWaitIdle(ctx_->device());

    for (int i = 0; i < SLOT_COUNT; i++) {
        auto& s = slots_[i];
        if (s.staging != VK_NULL_HANDLE) {
            vmaDestroyBuffer(ctx_->allocator(), s.staging, s.staging_alloc);
            s.staging = VK_NULL_HANDLE;
        }
        if (s.device != VK_NULL_HANDLE) {
            vmaDestroyBuffer(ctx_->allocator(), s.device, s.device_alloc);
            s.device = VK_NULL_HANDLE;
        }
        if (s.transfer_fence != VK_NULL_HANDLE) {
            vkDestroyFence(ctx_->device(), s.transfer_fence, nullptr);
            s.transfer_fence = VK_NULL_HANDLE;
        }
    }

    if (transfer_cmd_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(ctx_->device(), transfer_cmd_pool_, nullptr);
        transfer_cmd_pool_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
}
