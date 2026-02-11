#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <string>
#include <vector>

class VulkanContext;

// GPU compute shader MinMax decimation (TI-03 experiment)
// Uploads raw int16 samples to a GPU storage buffer, dispatches compute shader
// to perform MinMax reduction, reads back decimated results.
class ComputeDecimator {
public:
    ComputeDecimator() = default;
    ~ComputeDecimator();

    ComputeDecimator(const ComputeDecimator&) = delete;
    ComputeDecimator& operator=(const ComputeDecimator&) = delete;

    void init(VulkanContext& ctx, const std::string& shader_dir);
    void destroy();

    // Upload raw samples, dispatch compute, read back decimated min/max pairs.
    // Returns the decimated output (num_buckets * 2 int16 values).
    // Also records timing in last_compute_ms().
    std::vector<int16_t> decimate(const std::vector<int16_t>& raw_samples, uint32_t num_buckets);

    double last_compute_ms() const { return last_compute_ms_; }

private:
    void ensure_capacity(size_t num_samples, uint32_t num_buckets);

    VulkanContext* ctx_ = nullptr;

    // Compute pipeline
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_  = VK_NULL_HANDLE;
    VkPipeline pipeline_               = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_        = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_          = VK_NULL_HANDLE;

    // Buffers
    VkBuffer input_buffer_              = VK_NULL_HANDLE;
    VmaAllocation input_alloc_          = VK_NULL_HANDLE;
    VkBuffer output_buffer_             = VK_NULL_HANDLE;
    VmaAllocation output_alloc_         = VK_NULL_HANDLE;
    VkBuffer readback_buffer_           = VK_NULL_HANDLE;
    VmaAllocation readback_alloc_       = VK_NULL_HANDLE;

    // Command infrastructure
    VkCommandPool cmd_pool_             = VK_NULL_HANDLE;
    VkCommandBuffer cmd_                = VK_NULL_HANDLE;
    VkFence fence_                      = VK_NULL_HANDLE;

    size_t input_capacity_  = 0;  // in elements (int32)
    size_t output_capacity_ = 0;  // in elements (int32)

    double last_compute_ms_ = 0.0;
    bool initialized_ = false;
};
