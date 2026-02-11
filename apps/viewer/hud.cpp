#include "hud.h"
#include "vulkan_context.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {

void format_time_span(double seconds, char* out, size_t out_size) {
    if (seconds >= 1.0) {
        std::snprintf(out, out_size, "%.2f s", seconds);
    } else if (seconds >= 1e-3) {
        std::snprintf(out, out_size, "%.2f ms", seconds * 1e3);
    } else if (seconds >= 1e-6) {
        std::snprintf(out, out_size, "%.2f us", seconds * 1e6);
    } else {
        std::snprintf(out, out_size, "%.2f ns", seconds * 1e9);
    }
}

void format_time_tick(double seconds, char* out, size_t out_size) {
    double abs_s = std::abs(seconds);
    if (abs_s >= 1.0) {
        std::snprintf(out, out_size, "%.2f s", seconds);
    } else if (abs_s >= 1e-3) {
        std::snprintf(out, out_size, "%.2f ms", seconds * 1e3);
    } else if (abs_s >= 1e-6) {
        std::snprintf(out, out_size, "%.2f us", seconds * 1e6);
    } else {
        std::snprintf(out, out_size, "%.0f ns", seconds * 1e9);
    }
}

} // namespace

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

int Hud::consume_time_span_step_request() {
    int step = time_span_step_request_;
    time_span_step_request_ = 0;
    return std::clamp(step, -1, 1);
}

void Hud::build_status_bar(const grebe::TelemetrySnapshot& telemetry,
                            bool paused,
                            grebe::DecimationAlgorithm dec_algo, uint32_t channel_count,
                            uint64_t total_drops, uint64_t sg_drops,
                            uint64_t seq_gaps, double window_coverage,
                            double visible_time_span_s,
                            double min_time_span_s,
                            double max_time_span_s) {
    ImGuiIO& io = ImGui::GetIO();
    float bar_height = 44.0f;
    float screen_width = io.DisplaySize.x;
    float screen_height = io.DisplaySize.y;
    char span_str[32];
    format_time_span(visible_time_span_s, span_str, sizeof(span_str));
    char span_min_str[32];
    char span_max_str[32];
    format_time_span(min_time_span_s, span_min_str, sizeof(span_min_str));
    format_time_span(max_time_span_s, span_max_str, sizeof(span_max_str));

    // Layout: left draw field + right config pane + bottom status bar
    const float margin = 12.0f;
    const float pane_target_width = 280.0f;
    float pane_x = std::max(margin, screen_width - pane_target_width - margin);
    float pane_y = margin;
    float pane_w = std::max(180.0f, screen_width - pane_x - margin);
    float pane_h = std::max(120.0f, screen_height - bar_height - margin * 2.0f);

    float field_left = margin;
    float field_top = margin;
    float field_right = std::max(field_left + 220.0f, pane_x - margin);
    float field_bottom = std::max(field_top + 120.0f, screen_height - bar_height - margin);

    // Inner plot region (axes are drawn around this region)
    const float axis_left_pad = 58.0f;
    const float axis_right_pad = 8.0f;
    const float axis_top_pad = 8.0f;
    const float axis_bottom_pad = 30.0f;
    float plot_left = field_left + axis_left_pad;
    float plot_right = std::max(plot_left + 10.0f, field_right - axis_right_pad);
    float plot_top = field_top + axis_top_pad;
    float plot_bottom = std::max(plot_top + 10.0f, field_bottom - axis_bottom_pad);

    int plot_w = std::max(1, static_cast<int>(plot_right - plot_left));
    int plot_h = std::max(1, static_cast<int>(plot_bottom - plot_top));
    waveform_region_.x = static_cast<int32_t>(plot_left);
    waveform_region_.y = static_cast<int32_t>(plot_top);
    waveform_region_.width = static_cast<uint32_t>(plot_w);
    waveform_region_.height = static_cast<uint32_t>(plot_h);

    // Right-side config pane
    ImGui::SetNextWindowPos(ImVec2(pane_x, pane_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pane_w, pane_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGuiWindowFlags pane_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("View Config", nullptr, pane_flags);
    ImGui::TextUnformatted("Visible Time Span");
    if (ImGui::ArrowButton("##span_up", ImGuiDir_Up)) {
        time_span_step_request_ += 1;
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##span_down", ImGuiDir_Down)) {
        time_span_step_request_ -= 1;
    }
    ImGui::SameLine();
    ImGui::Text("%s", span_str);
    ImGui::Text("Range: %s .. %s", span_min_str, span_max_str);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Notes");
    ImGui::BulletText("Waveforms are clipped to the axis field.");
    ImGui::BulletText("Time axis spans from -Span to 0.");
    ImGui::End();

    // Waveform axes and grid overlay
    if (channel_count > 0 && plot_right > plot_left && plot_bottom > plot_top) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImU32 axis_col = IM_COL32(170, 170, 170, 200);
        ImU32 grid_col = IM_COL32(120, 120, 120, 90);
        ImU32 zero_col = IM_COL32(220, 220, 220, 120);
        ImU32 text_col = IM_COL32(220, 220, 220, 220);

        dl->AddText(ImVec2(4.0f, plot_top), text_col, "Amplitude (int16)");
        dl->AddLine(ImVec2(plot_left, plot_top), ImVec2(plot_left, plot_bottom), axis_col, 1.0f);

        float lane_h = (plot_bottom - plot_top) / static_cast<float>(channel_count);
        for (uint32_t ch = 0; ch < channel_count; ch++) {
            float lane_top = plot_top + lane_h * static_cast<float>(ch);
            float lane_bottom = lane_top + lane_h;
            float lane_mid = 0.5f * (lane_top + lane_bottom);

            if (ch > 0) {
                dl->AddLine(ImVec2(plot_left, lane_top), ImVec2(plot_right, lane_top), grid_col, 1.0f);
            }
            dl->AddLine(ImVec2(plot_left, lane_mid), ImVec2(plot_right, lane_mid), zero_col, 1.0f);

            char ch_label[16];
            std::snprintf(ch_label, sizeof(ch_label), "Ch%u", ch);
            dl->AddText(ImVec2(4.0f, lane_top + 2.0f), text_col, ch_label);

            if (lane_h >= 40.0f) {
                dl->AddText(ImVec2(34.0f, lane_top + 2.0f), text_col, "32767");
                dl->AddText(ImVec2(50.0f, lane_mid - 8.0f), text_col, "0");
                dl->AddText(ImVec2(26.0f, lane_bottom - 16.0f), text_col, "-32768");
            } else {
                dl->AddText(ImVec2(50.0f, lane_mid - 8.0f), text_col, "0");
            }
        }

        float axis_y = plot_bottom + 8.0f;
        dl->AddLine(ImVec2(plot_left, axis_y), ImVec2(plot_right, axis_y), axis_col, 1.0f);
        dl->AddText(ImVec2(plot_right - 34.0f, axis_y + 8.0f), text_col, "Time");

        constexpr int tick_count = 6;
        for (int i = 0; i <= tick_count; i++) {
            float t = static_cast<float>(i) / static_cast<float>(tick_count);
            float x = plot_left + (plot_right - plot_left) * t;
            dl->AddLine(ImVec2(x, axis_y - 3.0f), ImVec2(x, axis_y + 3.0f), axis_col, 1.0f);

            double sec = (t - 1.0f) * visible_time_span_s; // [-span, 0]
            char tick_label[32];
            format_time_tick(sec, tick_label, sizeof(tick_label));
            ImVec2 sz = ImGui::CalcTextSize(tick_label);
            dl->AddText(ImVec2(x - sz.x * 0.5f, axis_y + 6.0f), text_col, tick_label);
        }
    }

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
    double display_rate = telemetry.data_rate;
    if (telemetry.data_rate >= 1e9)      { display_rate = telemetry.data_rate / 1e9; rate_suffix = "GSPS"; }
    else if (telemetry.data_rate >= 1e6) { display_rate = telemetry.data_rate / 1e6; rate_suffix = "MSPS"; }
    else if (telemetry.data_rate >= 1e3) { display_rate = telemetry.data_rate / 1e3; rate_suffix = "KSPS"; }

    // Format vertex count
    const char* vtx_suffix = "";
    double display_vtx = static_cast<double>(telemetry.vertex_count);
    if (telemetry.vertex_count >= 1'000'000)  { display_vtx = telemetry.vertex_count / 1e6; vtx_suffix = "M"; }
    else if (telemetry.vertex_count >= 1'000) { display_vtx = telemetry.vertex_count / 1e3; vtx_suffix = "K"; }

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
                    telemetry.fps, telemetry.frame_time_ms, channel_count, display_rate, rate_suffix,
                    telemetry.ring_fill_ratio * 100.0, display_vtx, vtx_suffix,
                    grebe::DecimationEngine::algorithm_name(dec_algo),
                    alert_str,
                    paused ? " | PAUSED" : "");
    } else {
        ImGui::Text("FPS: %.1f | Frame: %.2f ms | %uch | Rate: %.1f %s | Ring: %.0f%% | Vtx: %.1f%s | %s%s",
                    telemetry.fps, telemetry.frame_time_ms, channel_count, display_rate, rate_suffix,
                    telemetry.ring_fill_ratio * 100.0, display_vtx, vtx_suffix,
                    grebe::DecimationEngine::algorithm_name(dec_algo),
                    paused ? " | PAUSED" : "");
    }

    // Line 2: per-phase telemetry + visible span + window coverage + E2E latency
    ImGui::Text("Drain: %.2f ms | Dec: %.2f ms (%.0f:1) | Upload: %.2f ms | Swap: %.2f ms | Render: %.2f ms | Smp/f: %u | Span: %s | WCov: %.0f%% | E2E: %.1f ms",
                telemetry.drain_time_ms,
                telemetry.decimation_time_ms, telemetry.decimation_ratio,
                telemetry.upload_time_ms,
                telemetry.swap_time_ms, telemetry.render_time_ms,
                telemetry.samples_per_frame,
                span_str,
                window_coverage * 100.0,
                telemetry.e2e_latency_ms);

    ImGui::End();
}

void Hud::render(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}
