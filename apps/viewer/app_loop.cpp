#include "app_loop.h"
#include "app_command.h"
#include "drop_counter.h"
#include "vulkan_renderer.h"
#include "synthetic_source.h"
#include "ipc_source.h"
#include "ingestion_thread.h"
#include "grebe/decimation_engine.h"
#include "benchmark.h"
#include "hud.h"
#include "profiler.h"
#include "transport.h"
#include "contracts.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static bool g_framebuffer_resized = false;

struct AppState {
    AppCommandQueue* cmd_queue = nullptr;
    bool is_ipc_mode = false;
};

static void framebuffer_resize_callback(GLFWwindow* /*window*/, int /*width*/, int /*height*/) {
    g_framebuffer_resized = true;
}

static void key_callback(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
    if (action != GLFW_PRESS) return;

    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!state || !state->cmd_queue) return;

    auto& q = *state->cmd_queue;

    switch (key) {
    case GLFW_KEY_ESCAPE:  q.push(CmdQuit{});                   break;
    case GLFW_KEY_V:       q.push(CmdToggleVsync{});            break;
    case GLFW_KEY_D:       q.push(CmdCycleDecimationMode{});    break;
    case GLFW_KEY_1:
    case GLFW_KEY_2:
    case GLFW_KEY_3:
    case GLFW_KEY_4:
        if (!state->is_ipc_mode) {
            const double rates[] = {1e6, 10e6, 100e6, 1e9};
            q.push(CmdSetSampleRate{rates[key - GLFW_KEY_1]});
        }
        break;
    case GLFW_KEY_SPACE:
        if (!state->is_ipc_mode) {
            q.push(CmdTogglePaused{});
        }
        break;
    default: break;
    }
}

static void process_commands(AppComponents& app) {
    for (auto& cmd : app.cmd_queue->drain()) {
        std::visit([&](auto&& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, CmdSetSampleRate>) {
                if (app.synthetic_source) {
                    app.synthetic_source->set_sample_rate(c.rate);
                } else if (app.ipc_source) {
                    IpcCommand ipc{};
                    ipc.type = IpcCommand::SET_SAMPLE_RATE;
                    ipc.value = c.rate;
                    app.ipc_source->transport().send_command(ipc);
                }
                app.dec_engine->set_sample_rate(c.rate);
                app.current_sample_rate.store(c.rate, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, CmdCycleDecimationMode>) {
                app.dec_engine->cycle_algorithm();
            } else if constexpr (std::is_same_v<T, CmdTogglePaused>) {
                if (app.synthetic_source) {
                    app.synthetic_source->set_paused(!app.synthetic_source->is_paused());
                    app.current_paused.store(app.synthetic_source->is_paused(), std::memory_order_relaxed);
                } else if (app.ipc_source) {
                    IpcCommand ipc{};
                    ipc.type = IpcCommand::TOGGLE_PAUSED;
                    app.ipc_source->transport().send_command(ipc);
                    app.current_paused.store(!app.current_paused.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
            } else if constexpr (std::is_same_v<T, CmdToggleVsync>) {
                app.render_backend->set_vsync(!app.render_backend->vsync());
                app.hud->on_swapchain_recreated(app.render_backend->image_count());
                spdlog::info("V-Sync {}", app.render_backend->vsync() ? "ON" : "OFF");
            } else if constexpr (std::is_same_v<T, CmdQuit>) {
                glfwSetWindowShouldClose(app.window, GLFW_TRUE);
            }
        }, cmd);
    }
}

void run_main_loop(AppComponents& app) {
    // Register GLFW callbacks
    AppState app_state;
    app_state.cmd_queue = app.cmd_queue;
    app_state.is_ipc_mode = (app.ipc_source != nullptr);
    glfwSetWindowUserPointer(app.window, &app_state);
    glfwSetFramebufferSizeCallback(app.window, framebuffer_resize_callback);
    glfwSetKeyCallback(app.window, key_callback);

    // Channel color palette (oscilloscope standard)
    struct ChannelColor { float r, g, b; };
    const ChannelColor palette[] = {
        {0.0f, 1.0f, 0.0f},  // Ch0: green
        {1.0f, 1.0f, 0.0f},  // Ch1: yellow
        {0.0f, 1.0f, 1.0f},  // Ch2: cyan
        {1.0f, 0.0f, 1.0f},  // Ch3: magenta
        {1.0f, 0.5f, 0.0f},  // Ch4: orange
        {1.0f, 1.0f, 1.0f},  // Ch5: white
        {1.0f, 0.3f, 0.3f},  // Ch6: red
        {0.3f, 0.5f, 1.0f},  // Ch7: blue
    };

    // Per-channel draw commands
    std::vector<grebe::DrawCommand> channel_cmds(app.num_channels);
    float n = static_cast<float>(app.num_channels);
    for (uint32_t ch = 0; ch < app.num_channels; ch++) {
        auto& dc = channel_cmds[ch];
        dc.amplitude_scale = 0.8f / n;
        dc.vertical_offset = 1.0f - (2.0f * ch + 1.0f) / n;
        dc.horizontal_scale = 1.0f;
        dc.horizontal_offset = 0.0f;
        dc.vertex_count = 0;
        dc.color_r = palette[ch].r;
        dc.color_g = palette[ch].g;
        dc.color_b = palette[ch].b;
        dc.color_a = 1.0f;
    }

    // Per-frame decimation output
    grebe::DecimationOutput dec_output;

    // Visible time-span (runtime clamped by sample rate/ring capacity)
    double visible_time_span_s = 10e-3; // 10 ms default
    app.dec_engine->set_visible_time_span(visible_time_span_s);

    // Title update throttle
    double last_title_update = glfwGetTime();

    while (!glfwWindowShouldClose(app.window)) {
        glfwPollEvents();

        // Process queued commands from keyboard/profiler
        process_commands(app);

        // Handle resize
        if (g_framebuffer_resized) {
            g_framebuffer_resized = false;
            int w, h;
            glfwGetFramebufferSize(app.window, &w, &h);
            if (w > 0 && h > 0) {
                app.render_backend->on_resize(static_cast<uint32_t>(w),
                                              static_cast<uint32_t>(h));
                app.hud->on_swapchain_recreated(app.render_backend->image_count());
            }
            continue;
        }

        // Skip minimized windows (but don't block if profiling)
        {
            int w, h;
            glfwGetFramebufferSize(app.window, &w, &h);
            if (w == 0 || h == 0) {
                if (app.enable_profile) {
                    glfwPollEvents();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } else {
                    glfwWaitEvents();
                }
                continue;
            }
        }

        app.benchmark->frame_begin();

        // Get decimated frame from decimation engine (timed as "drain")
        auto t0 = Benchmark::now();
        bool has_new_data = app.dec_engine->try_get_frame(dec_output);
        app.benchmark->set_drain_time(Benchmark::elapsed_ms(t0));
        app.benchmark->set_samples_per_frame(dec_output.raw_sample_count);
        auto dec_metrics = app.dec_engine->metrics();
        app.benchmark->set_decimation_time(dec_metrics.decimation_time_ms);
        app.benchmark->set_decimation_ratio(dec_metrics.decimation_ratio);

        // Data rate: from SyntheticSource (embedded) or IngestionThread (IPC)
        double data_rate = app.synthetic_source
            ? app.synthetic_source->actual_sample_rate()
            : (app.ingestion ? app.ingestion->sample_rate()
                             : app.current_sample_rate.load(std::memory_order_relaxed));
        bool paused = app.synthetic_source
            ? app.synthetic_source->is_paused()
            : app.current_paused.load(std::memory_order_relaxed);

        // Sync decimation thread with actual sample rate
        if (data_rate > 0.0) {
            app.dec_engine->set_sample_rate(data_rate);
        }

        app.benchmark->set_data_rate(data_rate);
        app.benchmark->set_ring_fill(dec_metrics.ring_fill_ratio);

        // Upload decimated data to GPU (timed)
        t0 = Benchmark::now();
        if (has_new_data && !dec_output.data.empty()) {
            app.render_backend->upload_vertices(dec_output.data.data(), dec_output.data.size());
        }
        app.benchmark->set_upload_time(Benchmark::elapsed_ms(t0));

        // Promote completed transfers to draw position (timed)
        t0 = Benchmark::now();
        app.render_backend->swap_buffers();
        app.benchmark->set_swap_time(Benchmark::elapsed_ms(t0));

        app.benchmark->set_vertex_count(app.render_backend->vertex_count());

        // Update per-channel vertex counts and first_vertex offsets
        uint32_t per_ch_vtx = dec_output.per_channel_vertex_count;
        uint32_t first_vtx = 0;
        for (uint32_t ch = 0; ch < app.num_channels; ch++) {
            channel_cmds[ch].vertex_count = static_cast<int>(per_ch_vtx);
            channel_cmds[ch].first_vertex = static_cast<int>(first_vtx);
            first_vtx += per_ch_vtx;
        }

        // Compute total drops across all channels (viewer-side)
        uint64_t total_drops = 0;
        for (auto* dc : app.drop_counters) {
            if (dc) total_drops += dc->total_dropped();
        }

        // SG-side drops (IPC mode only)
        uint64_t sg_drops = app.ipc_source ? app.ipc_source->sg_drops_total() : 0;

        // Build ImGui frame
        app.hud->new_frame();
        // Sequence gaps
        uint64_t seq_gaps = app.ingestion ? app.ingestion->sequence_gaps() : 0;

        // Dynamic time-span limits
        double min_time_span_s = 1e-6;
        double max_time_span_s = 100e-3;
        if (data_rate > 0.0) {
            const double sample_period_s = 1.0 / data_rate;
            min_time_span_s = std::max(sample_period_s, 64.0 * sample_period_s);
            if (app.ring_capacity_samples > 0) {
                max_time_span_s = 0.95 * (static_cast<double>(app.ring_capacity_samples) / data_rate);
            } else {
                max_time_span_s = 100e-3;
            }
            max_time_span_s = std::max(max_time_span_s, min_time_span_s);
            visible_time_span_s = std::clamp(visible_time_span_s, min_time_span_s, max_time_span_s);
            app.dec_engine->set_visible_time_span(visible_time_span_s);
        }

        // Window coverage: raw_samples / expected_samples_per_frame
        double window_coverage = 0.0;
        {
            double expected = data_rate * visible_time_span_s * static_cast<double>(app.num_channels);
            window_coverage = (expected > 0.0) ? (static_cast<double>(dec_output.raw_sample_count) / expected) : 0.0;
            window_coverage = std::clamp(window_coverage, 0.0, 1.0);
        }

        auto telemetry = app.benchmark->snapshot();
        app.hud->build_status_bar(telemetry,
                                  paused,
                                  dec_metrics.effective_algorithm,
                                  app.num_channels,
                                  total_drops,
                                  sg_drops,
                                  seq_gaps,
                                  window_coverage,
                                  visible_time_span_s,
                                  min_time_span_s,
                                  max_time_span_s);

        // Apply time-span adjustments requested by HUD arrow buttons
        int span_step = app.hud->consume_time_span_step_request();
        if (span_step != 0) {
            if (span_step > 0) {
                visible_time_span_s *= 2.0;
            } else {
                visible_time_span_s *= 0.5;
            }
            visible_time_span_s = std::clamp(visible_time_span_s, min_time_span_s, max_time_span_s);
            app.dec_engine->set_visible_time_span(visible_time_span_s);
            spdlog::info("Visible time span -> {:.3f} ms (range {:.3f}..{:.3f} ms)",
                         visible_time_span_s * 1e3,
                         min_time_span_s * 1e3,
                         max_time_span_s * 1e3);
        }

        // Read producer timestamp before render
        uint64_t producer_ts = app.ingestion ? app.ingestion->last_producer_ts_ns() : 0;

        // Render (timed)
        t0 = Benchmark::now();
        auto hud_region = app.hud->waveform_region();
        grebe::DrawRegion draw_region{
            hud_region.x,
            hud_region.y,
            hud_region.width,
            hud_region.height
        };
        bool ok = app.render_backend->draw_frame_with_overlay(
            channel_cmds.data(), app.num_channels,
            &draw_region, [&](VkCommandBuffer cmd) { app.hud->render(cmd); });
        app.benchmark->set_render_time(Benchmark::elapsed_ms(t0));

        // E2E latency: producer_ts -> render completion
        double e2e_ms = 0.0;
        if (producer_ts > 0) {
            auto now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Benchmark::Clock::now().time_since_epoch()).count());
            e2e_ms = static_cast<double>(now_ns - producer_ts) / 1e6;
        }
        app.benchmark->set_e2e_latency(e2e_ms);
        if (!ok) {
            // Swapchain out of date — recreate
            int w, h;
            glfwGetFramebufferSize(app.window, &w, &h);
            if (w > 0 && h > 0) {
                app.render_backend->on_resize(static_cast<uint32_t>(w),
                                              static_cast<uint32_t>(h));
                app.hud->on_swapchain_recreated(app.render_backend->image_count());
            }
            continue;
        }

        app.benchmark->frame_end();

        // Profile mode: collect metrics and manage scenario transitions
        if (app.enable_profile) {
            app.profiler->on_frame(*app.benchmark, app.render_backend->vertex_count(),
                                   data_rate,
                                   dec_metrics.ring_fill_ratio,
                                   total_drops, sg_drops,
                                   seq_gaps, dec_output.raw_sample_count,
                                   e2e_ms,
                                   *app.cmd_queue,
                                   dec_output.data.empty() ? nullptr : dec_output.data.data(),
                                   dec_output.per_channel_vertex_count,
                                   dec_metrics.effective_algorithm,
                                   dec_output.per_channel_raw_counts.empty() ? nullptr : &dec_output.per_channel_raw_counts);
        }

        // Update window title with FPS (throttled to 4 Hz)
        double now = glfwGetTime();
        if (now - last_title_update >= 0.25) {
            char title[128];
            std::snprintf(title, sizeof(title),
                          "Grebe | FPS: %.1f | Frame: %.2f ms | %uch | %s%s",
                          telemetry.fps, telemetry.frame_time_ms,
                          app.num_channels,
                          grebe::DecimationEngine::algorithm_name(dec_metrics.effective_algorithm),
                          app.ipc_source ? " | IPC" : "");
            glfwSetWindowTitle(app.window, title);
            last_title_update = now;
        }
    }
}
