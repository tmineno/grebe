#pragma once

#include <string>

class VulkanContext;
class Swapchain;
class BufferManager;
class Renderer;

// Runs isolated microbenchmarks (FR-05.3: BM-A, BM-B, BM-C, BM-D)
// Outputs JSON report to ./tmp/bench_<timestamp>.json
// Returns 0 on success.
int run_microbenchmarks(VulkanContext& ctx, Swapchain& swapchain,
                        BufferManager& buf_mgr, Renderer& renderer,
                        const std::string& shader_dir);
