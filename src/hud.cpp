#include "hud.h"
#include "vulkan_context.h"
#include "benchmark.h"
#include "decimation_thread.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

Hud::~Hud() {
    if (initialized_) {
        destroy();
    }
}

void Hud::init(GLFWwindow* window, VulkanContext& ctx, VkRenderPass render_pass,
               uint32_t image_count) {
    device_cache_ = ctx.device();

    // ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(8.0f, 4.0f);

    // GLFW backend
    ImGui_ImplGlfw_InitForVulkan(window, true);

    // Vulkan backend
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = ctx.instance();
    init_info.PhysicalDevice = ctx.physical_device();
    init_info.Device = ctx.device();
    init_info.QueueFamily = ctx.graphics_queue_family();
    init_info.Queue = ctx.graphics_queue();
    init_info.RenderPass = render_pass;
    init_info.MinImageCount = 2;
    init_info.ImageCount = image_count;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.DescriptorPoolSize = 10;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        throw std::runtime_error("Failed to init ImGui Vulkan backend");
    }

    // Upload font atlas
    ImGui_ImplVulkan_CreateFontsTexture();

    initialized_ = true;
    spdlog::info("HUD (ImGui) initialized");
}

void Hud::destroy() {
    if (!initialized_) return;

    vkDeviceWaitIdle(device_cache_);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    initialized_ = false;
    spdlog::info("HUD destroyed");
}

void Hud::on_swapchain_recreated(uint32_t min_image_count) {
    if (!initialized_) return;
    ImGui_ImplVulkan_SetMinImageCount(min_image_count);
}

void Hud::new_frame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Hud::build_status_bar(const Benchmark& bench, double data_rate,
                            double ring_fill, uint32_t vertex_count, bool paused,
                            DecimationMode dec_mode, uint32_t channel_count,
                            uint64_t total_drops, uint64_t sg_drops,
                            uint64_t seq_gaps, double window_coverage,
                            double e2e_latency_ms) {
    ImGuiIO& io = ImGui::GetIO();
    float bar_height = 44.0f; // two lines
    float screen_width = io.DisplaySize.x;
    float screen_height = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0.0f, screen_height - bar_height));
    ImGui::SetNextWindowSize(ImVec2(screen_width, bar_height));
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;

    ImGui::Begin("##statusbar", nullptr, flags);

    // Format data rate
    const char* rate_suffix = "SPS";
    double display_rate = data_rate;
    if (data_rate >= 1e9)      { display_rate = data_rate / 1e9; rate_suffix = "GSPS"; }
    else if (data_rate >= 1e6) { display_rate = data_rate / 1e6; rate_suffix = "MSPS"; }
    else if (data_rate >= 1e3) { display_rate = data_rate / 1e3; rate_suffix = "KSPS"; }

    // Format vertex count
    const char* vtx_suffix = "";
    double display_vtx = static_cast<double>(vertex_count);
    if (vertex_count >= 1'000'000)  { display_vtx = vertex_count / 1e6; vtx_suffix = "M"; }
    else if (vertex_count >= 1'000) { display_vtx = vertex_count / 1e3; vtx_suffix = "K"; }

    // Line 1: overview
    bool has_drops = (total_drops > 0 || sg_drops > 0);
    bool has_gaps = (seq_gaps > 0);
    // Build alert string for drops/gaps
    char alert_str[128] = "";
    if (has_drops || has_gaps) {
        char* p = alert_str;
        size_t rem = sizeof(alert_str);
        if (sg_drops > 0 && total_drops > 0) {
            int n = std::snprintf(p, rem, "DROP:%llu+SG:%llu",
                          static_cast<unsigned long long>(total_drops),
                          static_cast<unsigned long long>(sg_drops));
            p += n; rem -= static_cast<size_t>(n);
        } else if (sg_drops > 0) {
            int n = std::snprintf(p, rem, "SG-DROP:%llu",
                          static_cast<unsigned long long>(sg_drops));
            p += n; rem -= static_cast<size_t>(n);
        } else if (total_drops > 0) {
            int n = std::snprintf(p, rem, "DROP:%llu",
                          static_cast<unsigned long long>(total_drops));
            p += n; rem -= static_cast<size_t>(n);
        }
        if (has_gaps) {
            if (p != alert_str) { *p++ = ' '; rem--; }
            std::snprintf(p, rem, "GAP:%llu",
                          static_cast<unsigned long long>(seq_gaps));
        }
    }

    if (has_drops || has_gaps) {
        ImGui::Text("FPS: %.1f | Frame: %.2f ms | %uch | Rate: %.1f %s | Ring: %.0f%% | Vtx: %.1f%s | %s | %s%s",
                    bench.fps(), bench.frame_time_avg(), channel_count, display_rate, rate_suffix,
                    ring_fill * 100.0, display_vtx, vtx_suffix,
                    DecimationThread::mode_name(dec_mode),
                    alert_str,
                    paused ? " | PAUSED" : "");
    } else {
        ImGui::Text("FPS: %.1f | Frame: %.2f ms | %uch | Rate: %.1f %s | Ring: %.0f%% | Vtx: %.1f%s | %s%s",
                    bench.fps(), bench.frame_time_avg(), channel_count, display_rate, rate_suffix,
                    ring_fill * 100.0, display_vtx, vtx_suffix,
                    DecimationThread::mode_name(dec_mode),
                    paused ? " | PAUSED" : "");
    }

    // Line 2: per-phase telemetry + window coverage + E2E latency
    ImGui::Text("Drain: %.2f ms | Dec: %.2f ms (%.0f:1) | Upload: %.2f ms | Swap: %.2f ms | Render: %.2f ms | Smp/f: %.0f | WCov: %.0f%% | E2E: %.1f ms",
                bench.drain_time_avg(),
                bench.decimation_time_avg(), bench.decimation_ratio(),
                bench.upload_time_avg(),
                bench.swap_time_avg(), bench.render_time_avg(),
                bench.samples_per_frame_avg(),
                window_coverage * 100.0,
                e2e_latency_ms);

    ImGui::End();
}

void Hud::render(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}
