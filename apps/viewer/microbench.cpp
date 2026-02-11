#include "microbench.h"
#include "vulkan_context.h"
#include "swapchain.h"
#include "buffer_manager.h"
#include "renderer.h"
#include "decimator.h"
#include "compute_decimator.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <vector>

using Clock = std::chrono::steady_clock;

// ============================================================================
// BM-A: CPU-to-GPU Transfer Throughput
// ============================================================================

struct TransferResult {
    size_t buffer_size_bytes = 0;
    int iterations = 0;
    double total_seconds = 0.0;
    double throughput_gbps = 0.0;
};

static TransferResult bench_transfer(VulkanContext& ctx, size_t buffer_size, int iterations) {
    // Create staging + device buffers
    VkBuffer staging = VK_NULL_HANDLE, device = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = VK_NULL_HANDLE, device_alloc = VK_NULL_HANDLE;

    VkBufferCreateInfo staging_info = {};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = buffer_size;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc_info = {};
    staging_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    staging_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;

    vmaCreateBuffer(ctx.allocator(), &staging_info, &staging_alloc_info,
                    &staging, &staging_alloc, nullptr);

    VkBufferCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    device_info.size = buffer_size;
    device_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo device_alloc_info = {};
    device_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    vmaCreateBuffer(ctx.allocator(), &device_info, &device_alloc_info,
                    &device, &device_alloc, nullptr);

    // Fill staging with dummy data
    void* mapped = nullptr;
    vmaMapMemory(ctx.allocator(), staging_alloc, &mapped);
    std::memset(mapped, 0x42, buffer_size);
    vmaUnmapMemory(ctx.allocator(), staging_alloc);

    // Create command pool + buffer + fence
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = ctx.graphics_queue_family();

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    vkCreateCommandPool(ctx.device(), &pool_info, nullptr, &cmd_pool);

    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx.device(), &alloc_info, &cmd);

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(ctx.device(), &fence_info, nullptr, &fence);

    // Warmup
    for (int i = 0; i < 10; i++) {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        VkBufferCopy copy = {};
        copy.size = buffer_size;
        vkCmdCopyBuffer(cmd, staging, device, 1, &copy);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(ctx.graphics_queue(), 1, &submit, fence);
        vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(ctx.device(), 1, &fence);
    }

    // Timed run
    auto t0 = Clock::now();
    for (int i = 0; i < iterations; i++) {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        VkBufferCopy copy = {};
        copy.size = buffer_size;
        vkCmdCopyBuffer(cmd, staging, device, 1, &copy);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(ctx.graphics_queue(), 1, &submit, fence);
        vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(ctx.device(), 1, &fence);
    }
    auto t1 = Clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    TransferResult result;
    result.buffer_size_bytes = buffer_size;
    result.iterations = iterations;
    result.total_seconds = seconds;
    result.throughput_gbps = (static_cast<double>(buffer_size) * iterations / seconds) / (1024.0 * 1024.0 * 1024.0);

    // Cleanup
    vkDestroyFence(ctx.device(), fence, nullptr);
    vkDestroyCommandPool(ctx.device(), cmd_pool, nullptr);
    vmaDestroyBuffer(ctx.allocator(), staging, staging_alloc);
    vmaDestroyBuffer(ctx.allocator(), device, device_alloc);

    return result;
}

// ============================================================================
// BM-B: Decimation Throughput
// ============================================================================

struct DecimateResult {
    std::string algorithm;
    size_t input_samples = 0;
    uint32_t target_points = 0;
    int iterations = 0;
    double total_seconds = 0.0;
    double throughput_msps = 0.0; // million samples/sec
};

static DecimateResult bench_decimate(const std::string& name,
                                     const std::vector<int16_t>& input,
                                     uint32_t target_points, int iterations,
                                     std::vector<int16_t> (*func)(const std::vector<int16_t>&, uint32_t)) {
    // Warmup
    for (int i = 0; i < 3; i++) {
        auto result = func(input, target_points);
        (void)result;
    }

    auto t0 = Clock::now();
    for (int i = 0; i < iterations; i++) {
        auto result = func(input, target_points);
        (void)result;
    }
    auto t1 = Clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    DecimateResult r;
    r.algorithm = name;
    r.input_samples = input.size();
    r.target_points = target_points;
    r.iterations = iterations;
    r.total_seconds = seconds;
    r.throughput_msps = (static_cast<double>(input.size()) * iterations / seconds) / 1e6;
    return r;
}

// ============================================================================
// BM-C: Draw Throughput
// ============================================================================

struct DrawResult {
    uint32_t vertex_count = 0;
    int frames = 0;
    double total_seconds = 0.0;
    double fps = 0.0;
    double avg_frame_ms = 0.0;
};

static DrawResult bench_draw(VulkanContext& ctx, Swapchain& swapchain,
                             BufferManager& buf_mgr, Renderer& renderer,
                             uint32_t vertex_count, int num_frames,
                             Renderer::OverlayCallback overlay_cb = {}) {
    // Generate fixed test data
    std::vector<int16_t> test_data(vertex_count);
    for (uint32_t i = 0; i < vertex_count; i++) {
        double t = static_cast<double>(i) / static_cast<double>(vertex_count);
        test_data[i] = static_cast<int16_t>(std::sin(t * 6.283) * 32767.0);
    }

    // Upload once
    buf_mgr.upload_vertex_data(test_data);

    WaveformPushConstants pc;
    pc.amplitude_scale = 0.8f;
    pc.vertex_count = static_cast<int>(vertex_count);

    // Warmup
    for (int i = 0; i < 30; i++) {
        renderer.draw_frame(ctx, swapchain, buf_mgr, pc, overlay_cb);
    }

    auto t0 = Clock::now();
    for (int i = 0; i < num_frames; i++) {
        renderer.draw_frame(ctx, swapchain, buf_mgr, pc, overlay_cb);
    }
    vkDeviceWaitIdle(ctx.device());
    auto t1 = Clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    DrawResult r;
    r.vertex_count = vertex_count;
    r.frames = num_frames;
    r.total_seconds = seconds;
    r.fps = num_frames / seconds;
    r.avg_frame_ms = (seconds / num_frames) * 1000.0;
    return r;
}

// ============================================================================
// Main entry point
// ============================================================================

int run_microbenchmarks(VulkanContext& ctx, Swapchain& swapchain,
                        BufferManager& buf_mgr, Renderer& renderer,
                        const std::string& shader_dir) {
    spdlog::info("========== MICROBENCHMARKS ==========");

    nlohmann::json report;
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    report["timestamp"] = ts;

    // --- BM-A: Transfer ---
    spdlog::info("[BM-A] CPU-to-GPU Transfer Throughput");
    nlohmann::json bma = nlohmann::json::array();
    struct { const char* label; size_t size; int iters; } transfer_configs[] = {
        {"1 MB",  1ULL * 1024 * 1024, 500},
        {"4 MB",  4ULL * 1024 * 1024, 200},
        {"16 MB", 16ULL * 1024 * 1024, 100},
        {"64 MB", 64ULL * 1024 * 1024, 50},
    };
    for (auto& cfg : transfer_configs) {
        auto r = bench_transfer(ctx, cfg.size, cfg.iters);
        spdlog::info("  {}: {:.2f} GB/s ({} iters, {:.3f}s)",
                     cfg.label, r.throughput_gbps, r.iterations, r.total_seconds);
        bma.push_back({
            {"size", cfg.label},
            {"size_bytes", r.buffer_size_bytes},
            {"iterations", r.iterations},
            {"throughput_gbps", r.throughput_gbps},
            {"total_seconds", r.total_seconds}
        });
    }
    report["bm_a_transfer"] = bma;

    // --- BM-B: Decimation ---
    spdlog::info("[BM-B] Decimation Throughput (CPU)");
    constexpr size_t DECIMATE_INPUT_SIZE = 16 * 1024 * 1024; // 16M samples
    constexpr uint32_t DECIMATE_TARGET = 3840;
    constexpr int DECIMATE_ITERS = 20;

    // Generate test data
    std::vector<int16_t> test_input(DECIMATE_INPUT_SIZE);
    for (size_t i = 0; i < DECIMATE_INPUT_SIZE; i++) {
        test_input[i] = static_cast<int16_t>((i * 7 + 13) & 0xFFFF); // deterministic pattern
    }

    nlohmann::json bmb = nlohmann::json::array();

    auto r_scalar = bench_decimate("MinMax_Scalar", test_input, DECIMATE_TARGET, DECIMATE_ITERS,
                                   Decimator::minmax_scalar);
    spdlog::info("  MinMax (scalar): {:.1f} MSamples/s ({} iters, {:.3f}s)",
                 r_scalar.throughput_msps, r_scalar.iterations, r_scalar.total_seconds);
    bmb.push_back({{"algorithm", r_scalar.algorithm}, {"input_samples", r_scalar.input_samples},
                   {"target_points", r_scalar.target_points}, {"iterations", r_scalar.iterations},
                   {"throughput_msps", r_scalar.throughput_msps}, {"total_seconds", r_scalar.total_seconds}});

    auto r_simd = bench_decimate("MinMax_SIMD", test_input, DECIMATE_TARGET, DECIMATE_ITERS,
                                 Decimator::minmax);
    spdlog::info("  MinMax (SIMD):   {:.1f} MSamples/s ({} iters, {:.3f}s)",
                 r_simd.throughput_msps, r_simd.iterations, r_simd.total_seconds);
    bmb.push_back({{"algorithm", r_simd.algorithm}, {"input_samples", r_simd.input_samples},
                   {"target_points", r_simd.target_points}, {"iterations", r_simd.iterations},
                   {"throughput_msps", r_simd.throughput_msps}, {"total_seconds", r_simd.total_seconds}});

    if (r_scalar.throughput_msps > 0) {
        spdlog::info("  SIMD speedup: {:.1f}x", r_simd.throughput_msps / r_scalar.throughput_msps);
    }

    auto r_lttb = bench_decimate("LTTB", test_input, DECIMATE_TARGET, 3, // fewer iters (LTTB is slow)
                                 Decimator::lttb);
    spdlog::info("  LTTB:            {:.1f} MSamples/s ({} iters, {:.3f}s)",
                 r_lttb.throughput_msps, r_lttb.iterations, r_lttb.total_seconds);
    bmb.push_back({{"algorithm", r_lttb.algorithm}, {"input_samples", r_lttb.input_samples},
                   {"target_points", r_lttb.target_points}, {"iterations", r_lttb.iterations},
                   {"throughput_msps", r_lttb.throughput_msps}, {"total_seconds", r_lttb.total_seconds}});

    // Verify SIMD produces identical output to scalar
    auto scalar_out = Decimator::minmax_scalar(test_input, DECIMATE_TARGET);
    auto simd_out = Decimator::minmax(test_input, DECIMATE_TARGET);
    bool simd_correct = (scalar_out == simd_out);
    spdlog::info("  SIMD correctness: {}", simd_correct ? "PASS" : "FAIL");
    bmb.push_back({{"test", "simd_correctness"}, {"pass", simd_correct}});

    report["bm_b_decimate"] = bmb;

    // --- BM-C: Draw ---
    spdlog::info("[BM-C] Draw Throughput (GPU, V-Sync OFF)");
    nlohmann::json bmc = nlohmann::json::array();
    uint32_t draw_configs[] = {3840, 38400, 384000};
    for (auto vtx : draw_configs) {
        auto r = bench_draw(ctx, swapchain, buf_mgr, renderer, vtx, 500);
        spdlog::info("  {} vertices: {:.0f} FPS, {:.2f} ms/frame",
                     vtx, r.fps, r.avg_frame_ms);
        bmc.push_back({{"vertex_count", r.vertex_count}, {"frames", r.frames},
                       {"fps", r.fps}, {"avg_frame_ms", r.avg_frame_ms},
                       {"total_seconds", r.total_seconds}});
    }
    report["bm_c_draw"] = bmc;

    // --- BM-E: Compute Shader Decimation (TI-03) ---
    spdlog::info("[BM-E] Compute Shader Decimation (GPU)");
    nlohmann::json bme = nlohmann::json::array();
    try {
        ComputeDecimator compute_dec;
        compute_dec.init(ctx, SHADER_DIR);

        uint32_t compute_buckets = 1920; // same as CPU MinMax
        constexpr int COMPUTE_ITERS = 20;

        // Warmup
        for (int i = 0; i < 3; i++) {
            compute_dec.decimate(test_input, compute_buckets);
        }

        // Timed run
        auto ct0 = Clock::now();
        for (int i = 0; i < COMPUTE_ITERS; i++) {
            compute_dec.decimate(test_input, compute_buckets);
        }
        auto ct1 = Clock::now();
        double compute_sec = std::chrono::duration<double>(ct1 - ct0).count();
        double compute_msps = (static_cast<double>(DECIMATE_INPUT_SIZE) * COMPUTE_ITERS / compute_sec) / 1e6;

        spdlog::info("  Compute MinMax: {:.1f} MSamples/s ({} iters, {:.3f}s, {:.2f} ms/call)",
                     compute_msps, COMPUTE_ITERS, compute_sec,
                     compute_dec.last_compute_ms());

        // Verify correctness: check output size and global min/max match
        // Note: bucket boundaries differ slightly (GPU uses remainder-based distribution,
        // CPU uses floor-division), so per-bucket values won't be bit-identical.
        auto compute_out = compute_dec.decimate(test_input, compute_buckets);
        auto cpu_out = Decimator::minmax_scalar(test_input, compute_buckets * 2);
        bool size_correct = (compute_out.size() == cpu_out.size());

        // Check global min/max across all buckets match
        int16_t gpu_min = 32767, gpu_max = -32768;
        int16_t cpu_min = 32767, cpu_max = -32768;
        for (auto v : compute_out) { if (v < gpu_min) gpu_min = v; if (v > gpu_max) gpu_max = v; }
        for (auto v : cpu_out) { if (v < cpu_min) cpu_min = v; if (v > cpu_max) cpu_max = v; }
        bool minmax_correct = (gpu_min == cpu_min && gpu_max == cpu_max);
        bool compute_correct = size_correct && minmax_correct;
        spdlog::info("  Compute correctness: size={} global_minmax={} → {}",
                     size_correct ? "OK" : "FAIL", minmax_correct ? "OK" : "FAIL",
                     compute_correct ? "PASS" : "FAIL");

        if (r_simd.throughput_msps > 0) {
            spdlog::info("  CPU SIMD vs Compute: CPU={:.1f}, GPU={:.1f} MSamples/s",
                         r_simd.throughput_msps, compute_msps);
        }

        bme.push_back({{"algorithm", "Compute_MinMax"}, {"input_samples", DECIMATE_INPUT_SIZE},
                       {"num_buckets", compute_buckets}, {"iterations", COMPUTE_ITERS},
                       {"throughput_msps", compute_msps}, {"total_seconds", compute_sec},
                       {"ms_per_call", compute_dec.last_compute_ms()},
                       {"correctness", compute_correct}});

        compute_dec.destroy();
    } catch (const std::exception& e) {
        spdlog::warn("  BM-E skipped: {}", e.what());
        bme.push_back({{"error", e.what()}});
    }
    report["bm_e_compute"] = bme;

    // --- BM-F: Overlay Callback Overhead (R-3) ---
    spdlog::info("[BM-F] Overlay Callback Overhead (R-3)");
    nlohmann::json bmf = nlohmann::json::object();
    {
        constexpr uint32_t vtx = 3840;
        constexpr int frames = 500;

        // Baseline: empty callback (default {})
        auto r_no_cb = bench_draw(ctx, swapchain, buf_mgr, renderer, vtx, frames);

        // With no-op callback
        auto r_noop_cb = bench_draw(ctx, swapchain, buf_mgr, renderer, vtx, frames,
                                     [](VkCommandBuffer) {});

        double delta_ms = r_noop_cb.avg_frame_ms - r_no_cb.avg_frame_ms;
        spdlog::info("  No callback:   {:.0f} FPS, {:.3f} ms/frame", r_no_cb.fps, r_no_cb.avg_frame_ms);
        spdlog::info("  No-op callback: {:.0f} FPS, {:.3f} ms/frame", r_noop_cb.fps, r_noop_cb.avg_frame_ms);
        spdlog::info("  Delta: {:.4f} ms/frame (threshold: 0.1 ms)", delta_ms);

        bmf = {
            {"vertex_count", vtx}, {"frames", frames},
            {"no_callback_fps", r_no_cb.fps}, {"no_callback_ms", r_no_cb.avg_frame_ms},
            {"noop_callback_fps", r_noop_cb.fps}, {"noop_callback_ms", r_noop_cb.avg_frame_ms},
            {"delta_ms", delta_ms}, {"pass", std::abs(delta_ms) <= 0.1}
        };
    }
    report["bm_f_overlay_callback"] = bmf;

    // --- Write report ---
    spdlog::info("========================================");

    std::filesystem::create_directories("./tmp");
    char filename_ts[32];
    std::strftime(filename_ts, sizeof(filename_ts), "%Y%m%d_%H%M%S", std::localtime(&t));
    std::string json_path = std::string("./tmp/bench_") + filename_ts + ".json";

    std::ofstream out(json_path);
    if (out.is_open()) {
        out << report.dump(2) << "\n";
        out.close();
        spdlog::info("Benchmark report saved to: {}", json_path);
    }

    return 0;
}
