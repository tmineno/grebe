#include "compute_decimator.h"
#include "vulkan_context.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

ComputeDecimator::~ComputeDecimator() {
    if (initialized_) destroy();
}

static std::vector<char> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader: " + path);
    }
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

void ComputeDecimator::init(VulkanContext& ctx, const std::string& shader_dir) {
    ctx_ = &ctx;
    VkDevice device = ctx.device();

    // --- Descriptor set layout: 2 storage buffers ---
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 2;
    layout_info.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &desc_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute descriptor set layout");
    }

    // --- Push constant range ---
    VkPushConstantRange pc_range = {};
    pc_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc_range.offset = 0;
    pc_range.size = 8; // 2 x uint32

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo pl_info = {};
    pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount = 1;
    pl_info.pSetLayouts = &desc_layout_;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges = &pc_range;

    if (vkCreatePipelineLayout(device, &pl_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute pipeline layout");
    }

    // --- Shader module ---
    std::string spv_path = (std::filesystem::path(shader_dir) / "minmax_decimate.comp.spv").string();
    auto code = read_file(spv_path);

    VkShaderModuleCreateInfo sm_info = {};
    sm_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_info.codeSize = code.size();
    sm_info.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &sm_info, nullptr, &shader_module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute shader module");
    }

    // --- Compute pipeline ---
    VkComputePipelineCreateInfo cp_info = {};
    cp_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp_info.stage.module = shader_module;
    cp_info.stage.pName = "main";
    cp_info.layout = pipeline_layout_;

    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp_info, nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device, shader_module, nullptr);
        throw std::runtime_error("Failed to create compute pipeline");
    }

    vkDestroyShaderModule(device, shader_module, nullptr);

    // --- Descriptor pool ---
    VkDescriptorPoolSize pool_size = {};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 2;

    VkDescriptorPoolCreateInfo dp_info = {};
    dp_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_info.maxSets = 1;
    dp_info.poolSizeCount = 1;
    dp_info.pPoolSizes = &pool_size;

    if (vkCreateDescriptorPool(device, &dp_info, nullptr, &desc_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute descriptor pool");
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo ds_alloc = {};
    ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool = desc_pool_;
    ds_alloc.descriptorSetCount = 1;
    ds_alloc.pSetLayouts = &desc_layout_;

    if (vkAllocateDescriptorSets(device, &ds_alloc, &desc_set_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate compute descriptor set");
    }

    // --- Command pool + buffer + fence ---
    VkCommandPoolCreateInfo cmd_pool_info = {};
    cmd_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmd_pool_info.queueFamilyIndex = ctx.graphics_queue_family();

    if (vkCreateCommandPool(device, &cmd_pool_info, nullptr, &cmd_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute command pool");
    }

    VkCommandBufferAllocateInfo cmd_alloc = {};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = cmd_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device, &cmd_alloc, &cmd_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate compute command buffer");
    }

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    if (vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute fence");
    }

    initialized_ = true;
    spdlog::info("ComputeDecimator initialized");
}

void ComputeDecimator::ensure_capacity(size_t num_samples, uint32_t num_buckets) {
    VkDevice device = ctx_->device();
    VmaAllocator alloc = ctx_->allocator();

    size_t input_elems = num_samples;
    size_t output_elems = num_buckets * 2;

    // Recreate input buffer if too small
    if (input_elems > input_capacity_) {
        if (input_buffer_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(alloc, input_buffer_, input_alloc_);
        }

        VkBufferCreateInfo buf_info = {};
        buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_info.size = input_elems * sizeof(int32_t); // int16 stored as int32 for shader
        buf_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;

        if (vmaCreateBuffer(alloc, &buf_info, &alloc_info,
                            &input_buffer_, &input_alloc_, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create compute input buffer");
        }
        input_capacity_ = input_elems;
    }

    // Recreate output buffer if too small
    if (output_elems > output_capacity_) {
        if (output_buffer_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(alloc, output_buffer_, output_alloc_);
        }
        if (readback_buffer_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(alloc, readback_buffer_, readback_alloc_);
        }

        VkDeviceSize out_size = output_elems * sizeof(int32_t);

        // Device-local output buffer
        VkBufferCreateInfo out_info = {};
        out_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        out_info.size = out_size;
        out_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo out_alloc_info = {};
        out_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        if (vmaCreateBuffer(alloc, &out_info, &out_alloc_info,
                            &output_buffer_, &output_alloc_, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create compute output buffer");
        }

        // Readback buffer (host-visible)
        VkBufferCreateInfo rb_info = {};
        rb_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        rb_info.size = out_size;
        rb_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo rb_alloc_info = {};
        rb_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        rb_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;

        if (vmaCreateBuffer(alloc, &rb_info, &rb_alloc_info,
                            &readback_buffer_, &readback_alloc_, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create compute readback buffer");
        }

        output_capacity_ = output_elems;
    }

    // Update descriptor set
    VkDescriptorBufferInfo input_desc = {};
    input_desc.buffer = input_buffer_;
    input_desc.offset = 0;
    input_desc.range = input_elems * sizeof(int32_t);

    VkDescriptorBufferInfo output_desc = {};
    output_desc.buffer = output_buffer_;
    output_desc.offset = 0;
    output_desc.range = output_elems * sizeof(int32_t);

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = desc_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &input_desc;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = desc_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &output_desc;

    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

std::vector<int16_t> ComputeDecimator::decimate(const std::vector<int16_t>& raw_samples, uint32_t num_buckets) {
    if (raw_samples.empty() || num_buckets == 0) return {};

    auto t0 = std::chrono::steady_clock::now();

    size_t n = raw_samples.size();
    ensure_capacity(n, num_buckets);

    // Upload int16 samples as int32 to the input buffer
    void* mapped = nullptr;
    vmaMapMemory(ctx_->allocator(), input_alloc_, &mapped);
    int32_t* dst = static_cast<int32_t*>(mapped);
    for (size_t i = 0; i < n; i++) {
        dst[i] = static_cast<int32_t>(raw_samples[i]);
    }
    vmaUnmapMemory(ctx_->allocator(), input_alloc_);

    // Record and execute compute dispatch
    vkResetCommandBuffer(cmd_, 0);

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);

    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
                            0, 1, &desc_set_, 0, nullptr);

    struct { uint32_t total_samples; uint32_t num_buckets; } pc;
    pc.total_samples = static_cast<uint32_t>(n);
    pc.num_buckets = num_buckets;
    vkCmdPushConstants(cmd_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    vkCmdDispatch(cmd_, num_buckets, 1, 1);

    // Memory barrier: compute → transfer
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Copy output → readback
    VkBufferCopy copy = {};
    copy.size = num_buckets * 2 * sizeof(int32_t);
    vkCmdCopyBuffer(cmd_, output_buffer_, readback_buffer_, 1, &copy);

    vkEndCommandBuffer(cmd_);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    vkQueueSubmit(ctx_->graphics_queue(), 1, &submit, fence_);
    vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx_->device(), 1, &fence_);

    // Read back results
    void* read_mapped = nullptr;
    vmaMapMemory(ctx_->allocator(), readback_alloc_, &read_mapped);
    const int32_t* src = static_cast<const int32_t*>(read_mapped);
    std::vector<int16_t> output(num_buckets * 2);
    for (uint32_t i = 0; i < num_buckets * 2; i++) {
        output[i] = static_cast<int16_t>(src[i]);
    }
    vmaUnmapMemory(ctx_->allocator(), readback_alloc_);

    auto t1 = std::chrono::steady_clock::now();
    last_compute_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();

    return output;
}

void ComputeDecimator::destroy() {
    if (!initialized_) return;

    VkDevice device = ctx_->device();
    VmaAllocator alloc = ctx_->allocator();

    vkDeviceWaitIdle(device);

    if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device, fence_, nullptr);
    if (cmd_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device, cmd_pool_, nullptr);
    if (desc_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, desc_pool_, nullptr);
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
    if (desc_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, desc_layout_, nullptr);

    if (input_buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(alloc, input_buffer_, input_alloc_);
    if (output_buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(alloc, output_buffer_, output_alloc_);
    if (readback_buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(alloc, readback_buffer_, readback_alloc_);

    initialized_ = false;
    spdlog::info("ComputeDecimator destroyed");
}
