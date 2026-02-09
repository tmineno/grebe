#include "app_loop.h"
#include "app_command.h"
#include "drop_counter.h"
#include "vulkan_context.h"
#include "swapchain.h"
#include "renderer.h"
#include "buffer_manager.h"
#include "data_generator.h"
#include "decimation_thread.h"
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
        // IPC mode: rate is controlled by grebe-sg UI
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
                if (app.data_gen) {
                    // Embedded mode: direct DataGenerator control
                    app.data_gen->set_sample_rate(c.rate);
                } else if (app.transport) {
                    // IPC mode: forward to grebe-sg
                    IpcCommand ipc{};
                    ipc.type = IpcCommand::SET_SAMPLE_RATE;
                    ipc.value = c.rate;
                    app.transport->send_command(ipc);
                }
                app.dec_thread->set_sample_rate(c.rate);
                app.current_sample_rate.store(c.rate, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, CmdCycleDecimationMode>) {
                app.dec_thread->cycle_mode();
            } else if constexpr (std::is_same_v<T, CmdTogglePaused>) {
                if (app.data_gen) {
                    app.data_gen->set_paused(!app.data_gen->is_paused());
                    app.current_paused.store(app.data_gen->is_paused(), std::memory_order_relaxed);
                } else if (app.transport) {
                    IpcCommand ipc{};
                    ipc.type = IpcCommand::TOGGLE_PAUSED;
                    app.transport->send_command(ipc);
                    app.current_paused.store(!app.current_paused.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
            } else if constexpr (std::is_same_v<T, CmdToggleVsync>) {
                int w, h;
                glfwGetFramebufferSize(app.window, &w, &h);
                if (w > 0 && h > 0) {
                    app.swapchain->set_vsync(!app.swapchain->vsync());
                    app.swapchain->recreate(*app.ctx,
                                            static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                    app.renderer->on_swapchain_recreated(*app.ctx, *app.swapchain);
                    app.hud->on_swapchain_recreated(app.swapchain->image_count());
                    spdlog::info("V-Sync {}", app.swapchain->vsync() ? "ON" : "OFF");
                }
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
    app_state.is_ipc_mode = (app.transport != nullptr);
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

    // Per-channel push constants
    std::vector<WaveformPushConstants> channel_pcs(app.num_channels);
    float n = static_cast<float>(app.num_channels);
    for (uint32_t ch = 0; ch < app.num_channels; ch++) {
        auto& pc = channel_pcs[ch];
        pc.amplitude_scale = 0.8f / n;
        pc.vertical_offset = 1.0f - (2.0f * ch + 1.0f) / n;
        pc.horizontal_scale = 1.0f;
        pc.horizontal_offset = 0.0f;
        pc.vertex_count = 0;
        pc.color_r = palette[ch].r;
        pc.color_g = palette[ch].g;
        pc.color_b = palette[ch].b;
        pc.color_a = 1.0f;
    }

    // Per-frame data buffer (receives decimated output)
    std::vector<int16_t> frame_data;

    // Visible time-span (runtime clamped by sample rate/ring capacity)
    double visible_time_span_s = 10e-3; // 10 ms default
    app.dec_thread->set_visible_time_span(visible_time_span_s);

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
                app.swapchain->recreate(*app.ctx,
                                        static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                app.renderer->on_swapchain_recreated(*app.ctx, *app.swapchain);
                app.hud->on_swapchain_recreated(app.swapchain->image_count());
            }
            continue;
        }

        // Skip minimized windows
        {
            int w, h;
            glfwGetFramebufferSize(app.window, &w, &h);
            if (w == 0 || h == 0) {
                glfwWaitEvents();
                continue;
            }
        }

        app.benchmark->frame_begin();

        // Get decimated frame from decimation thread (timed as "drain")
        auto t0 = Benchmark::now();
        uint32_t raw_samples = 0;
        std::vector<uint32_t> per_ch_raw;
        bool has_new_data = app.dec_thread->try_get_frame(frame_data, raw_samples, per_ch_raw);
        app.benchmark->set_drain_time(Benchmark::elapsed_ms(t0));
        app.benchmark->set_samples_per_frame(raw_samples);
        app.benchmark->set_decimation_time(app.dec_thread->decimation_time_ms());
        app.benchmark->set_decimation_ratio(app.dec_thread->decimation_ratio());

        // Data rate: from DataGenerator (embedded) or atomic updated by receiver (IPC)
        double data_rate = app.data_gen
            ? app.data_gen->actual_sample_rate()
            : app.current_sample_rate.load(std::memory_order_relaxed);
        bool paused = app.data_gen
            ? app.data_gen->is_paused()
            : app.current_paused.load(std::memory_order_relaxed);

        app.benchmark->set_data_rate(data_rate);
        app.benchmark->set_ring_fill(app.dec_thread->ring_fill_ratio());

        // Upload decimated data to GPU (timed)
        t0 = Benchmark::now();
        if (has_new_data && !frame_data.empty()) {
            app.buf_mgr->upload_streaming(frame_data);
        }
        app.benchmark->set_upload_time(Benchmark::elapsed_ms(t0));

        // Promote completed transfers to draw position (timed)
        t0 = Benchmark::now();
        app.buf_mgr->try_swap();
        app.benchmark->set_swap_time(Benchmark::elapsed_ms(t0));

        app.benchmark->set_vertex_count(app.buf_mgr->vertex_count());

        // Update per-channel vertex counts and first_vertex offsets
        uint32_t per_ch_vtx = app.dec_thread->per_channel_vertex_count();
        uint32_t first_vtx = 0;
        for (uint32_t ch = 0; ch < app.num_channels; ch++) {
            channel_pcs[ch].vertex_count = static_cast<int>(per_ch_vtx);
            channel_pcs[ch].first_vertex = static_cast<int>(first_vtx);
            first_vtx += per_ch_vtx;
        }

        // Compute total drops across all channels (viewer-side)
        uint64_t total_drops = 0;
        for (auto* dc : app.drop_counters) {
            if (dc) total_drops += dc->total_dropped();
        }

        // SG-side drops (IPC mode only)
        uint64_t sg_drops = app.sg_drops_total.load(std::memory_order_relaxed);

        // Build ImGui frame
        app.hud->new_frame();
        // Sequence gaps (IPC mode only)
        uint64_t seq_gaps = app.seq_gaps.load(std::memory_order_relaxed);

        // Dynamic time-span limits derived from runtime system settings.
        // Lower: at least one sample period and >= 64 samples for stable visualization.
        // Upper: bounded by per-channel ring capacity.
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
            app.dec_thread->set_visible_time_span(visible_time_span_s);
        }

        // Window coverage: raw_samples / expected_samples_per_frame
        double window_coverage = 0.0;
        {
            double expected = data_rate * visible_time_span_s * static_cast<double>(app.num_channels);
            window_coverage = (expected > 0.0) ? (static_cast<double>(raw_samples) / expected) : 0.0;
            window_coverage = std::clamp(window_coverage, 0.0, 1.0);
        }

        app.hud->build_status_bar(*app.benchmark, data_rate,
                                  app.dec_thread->ring_fill_ratio(),
                                  app.buf_mgr->vertex_count(),
                                  paused,
                                  app.dec_thread->effective_mode(),
                                  app.num_channels,
                                  total_drops,
                                  sg_drops,
                                  seq_gaps,
                                  window_coverage,
                                  visible_time_span_s,
                                  min_time_span_s,
                                  max_time_span_s,
                                  app.benchmark->e2e_latency_avg());

        // Apply time-span adjustments requested by HUD arrow buttons
        int span_step = app.hud->consume_time_span_step_request();
        if (span_step != 0) {
            if (span_step > 0) {
                visible_time_span_s *= 2.0;
            } else {
                visible_time_span_s *= 0.5;
            }
            visible_time_span_s = std::clamp(visible_time_span_s, min_time_span_s, max_time_span_s);
            app.dec_thread->set_visible_time_span(visible_time_span_s);
            spdlog::info("Visible time span -> {:.3f} ms (range {:.3f}..{:.3f} ms)",
                         visible_time_span_s * 1e3,
                         min_time_span_s * 1e3,
                         max_time_span_s * 1e3);
        }

        // Read producer timestamp before render
        uint64_t producer_ts = 0;
        if (app.data_gen) {
            producer_ts = app.data_gen->last_push_ts_ns();
        } else {
            producer_ts = app.latest_producer_ts_ns.load(std::memory_order_relaxed);
        }

        // Render (timed)
        t0 = Benchmark::now();
        auto hud_region = app.hud->waveform_region();
        DrawRegionPx draw_region{
            hud_region.x,
            hud_region.y,
            hud_region.width,
            hud_region.height
        };
        bool ok = app.renderer->draw_frame(*app.ctx, *app.swapchain, *app.buf_mgr,
                                           channel_pcs.data(), app.num_channels,
                                           &draw_region, app.hud);
        app.benchmark->set_render_time(Benchmark::elapsed_ms(t0));

        // E2E latency: producer_ts → render completion
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
                app.swapchain->recreate(*app.ctx,
                                        static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                app.renderer->on_swapchain_recreated(*app.ctx, *app.swapchain);
                app.hud->on_swapchain_recreated(app.swapchain->image_count());
            }
            continue;
        }

        app.benchmark->frame_end();

        // Profile mode: collect metrics and manage scenario transitions
        if (app.enable_profile) {
            app.profiler->on_frame(*app.benchmark, app.buf_mgr->vertex_count(),
                                   data_rate,
                                   app.dec_thread->ring_fill_ratio(),
                                   total_drops, sg_drops,
                                   seq_gaps, raw_samples,
                                   e2e_ms,
                                   *app.cmd_queue,
                                   frame_data.empty() ? nullptr : frame_data.data(),
                                   app.dec_thread->per_channel_vertex_count(),
                                   app.dec_thread->effective_mode(),
                                   per_ch_raw.empty() ? nullptr : &per_ch_raw);
        }

        // Update window title with FPS (throttled to 4 Hz)
        double now = glfwGetTime();
        if (now - last_title_update >= 0.25) {
            char title[128];
            std::snprintf(title, sizeof(title),
                          "Grebe | FPS: %.1f | Frame: %.2f ms | %uch | %s%s",
                          app.benchmark->fps(), app.benchmark->frame_time_avg(),
                          app.num_channels,
                          DecimationThread::mode_name(app.dec_thread->effective_mode()),
                          app.transport ? " | IPC" : "");
            glfwSetWindowTitle(app.window, title);
            last_title_update = now;
        }
    }
}
