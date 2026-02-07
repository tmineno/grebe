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

    // Create command pool for transfer operations
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = ctx.graphics_queue_family();

    if (vkCreateCommandPool(ctx.device(), &pool_info, nullptr, &transfer_cmd_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create transfer command pool");
    }

    initialized_ = true;
    spdlog::info("BufferManager initialized");
}

void BufferManager::upload_vertex_data(const std::vector<int16_t>& data) {
    if (!initialized_ || data.empty()) return;

    VkDeviceSize buffer_size = data.size() * sizeof(int16_t);

    // Clean up previous buffers
    if (staging_buffer_) {
        vmaDestroyBuffer(ctx_->allocator(), staging_buffer_, staging_allocation_);
        staging_buffer_ = VK_NULL_HANDLE;
    }
    if (vertex_buffer_) {
        vmaDestroyBuffer(ctx_->allocator(), vertex_buffer_, vertex_allocation_);
        vertex_buffer_ = VK_NULL_HANDLE;
    }

    // Create staging buffer (CPU-visible)
    VkBufferCreateInfo staging_buf_info = {};
    staging_buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_buf_info.size = buffer_size;
    staging_buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc_info = {};
    staging_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    staging_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (vmaCreateBuffer(ctx_->allocator(), &staging_buf_info, &staging_alloc_info,
                        &staging_buffer_, &staging_allocation_, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create staging buffer");
    }

    // Copy data to staging buffer
    void* mapped = nullptr;
    vmaMapMemory(ctx_->allocator(), staging_allocation_, &mapped);
    std::memcpy(mapped, data.data(), buffer_size);
    vmaUnmapMemory(ctx_->allocator(), staging_allocation_);

    // Create device-local vertex buffer
    VkBufferCreateInfo vertex_buf_info = {};
    vertex_buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertex_buf_info.size = buffer_size;
    vertex_buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo vertex_alloc_info = {};
    vertex_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateBuffer(ctx_->allocator(), &vertex_buf_info, &vertex_alloc_info,
                        &vertex_buffer_, &vertex_allocation_, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex buffer");
    }

    // Record and submit transfer command
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = transfer_cmd_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(ctx_->device(), &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkBufferCopy copy_region = {};
    copy_region.size = buffer_size;
    vkCmdCopyBuffer(cmd, staging_buffer_, vertex_buffer_, 1, &copy_region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vkQueueSubmit(ctx_->graphics_queue(), 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx_->graphics_queue());

    vkFreeCommandBuffers(ctx_->device(), transfer_cmd_pool_, 1, &cmd);

    vertex_count_ = static_cast<uint32_t>(data.size());
    spdlog::info("Uploaded {} vertices ({} bytes) to GPU", vertex_count_, buffer_size);
}

void BufferManager::destroy() {
    if (!initialized_) return;

    vkDeviceWaitIdle(ctx_->device());

    if (staging_buffer_) {
        vmaDestroyBuffer(ctx_->allocator(), staging_buffer_, staging_allocation_);
        staging_buffer_ = VK_NULL_HANDLE;
    }
    if (vertex_buffer_) {
        vmaDestroyBuffer(ctx_->allocator(), vertex_buffer_, vertex_allocation_);
        vertex_buffer_ = VK_NULL_HANDLE;
    }
    if (transfer_cmd_pool_) {
        vkDestroyCommandPool(ctx_->device(), transfer_cmd_pool_, nullptr);
        transfer_cmd_pool_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
}
