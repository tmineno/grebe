#include "app_loop.h"
#include "app_command.h"
#include "vulkan_context.h"
#include "swapchain.h"
#include "renderer.h"
#include "buffer_manager.h"
#include "data_generator.h"
#include "decimation_thread.h"
#include "benchmark.h"
#include "hud.h"
#include "profiler.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <vector>

static bool g_framebuffer_resized = false;

struct AppState {
    AppCommandQueue* cmd_queue = nullptr;
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
    case GLFW_KEY_1:       q.push(CmdSetSampleRate{1e6});       break;
    case GLFW_KEY_2:       q.push(CmdSetSampleRate{10e6});      break;
    case GLFW_KEY_3:       q.push(CmdSetSampleRate{100e6});     break;
    case GLFW_KEY_4:       q.push(CmdSetSampleRate{1e9});       break;
    case GLFW_KEY_SPACE:   q.push(CmdTogglePaused{});           break;
    default: break;
    }
}

static void process_commands(AppComponents& app) {
    for (auto& cmd : app.cmd_queue->drain()) {
        std::visit([&](auto&& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, CmdSetSampleRate>) {
                app.data_gen->set_sample_rate(c.rate);
                app.dec_thread->set_sample_rate(c.rate);
            } else if constexpr (std::is_same_v<T, CmdCycleDecimationMode>) {
                app.dec_thread->cycle_mode();
            } else if constexpr (std::is_same_v<T, CmdTogglePaused>) {
                app.data_gen->set_paused(!app.data_gen->is_paused());
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
        bool has_new_data = app.dec_thread->try_get_frame(frame_data, raw_samples);
        app.benchmark->set_drain_time(Benchmark::elapsed_ms(t0));
        app.benchmark->set_samples_per_frame(raw_samples);
        app.benchmark->set_decimation_time(app.dec_thread->decimation_time_ms());
        app.benchmark->set_decimation_ratio(app.dec_thread->decimation_ratio());
        app.benchmark->set_data_rate(app.data_gen->actual_sample_rate());
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

        // Build ImGui frame
        app.hud->new_frame();
        app.hud->build_status_bar(*app.benchmark, app.data_gen->actual_sample_rate(),
                                  app.dec_thread->ring_fill_ratio(),
                                  app.buf_mgr->vertex_count(),
                                  app.data_gen->is_paused(),
                                  app.dec_thread->effective_mode(),
                                  app.num_channels);

        // Render (timed)
        t0 = Benchmark::now();
        bool ok = app.renderer->draw_frame(*app.ctx, *app.swapchain, *app.buf_mgr,
                                           channel_pcs.data(), app.num_channels, app.hud);
        app.benchmark->set_render_time(Benchmark::elapsed_ms(t0));
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
                                   app.data_gen->actual_sample_rate(),
                                   app.dec_thread->ring_fill_ratio(),
                                   *app.cmd_queue);
        }

        // Update window title with FPS (throttled to 4 Hz)
        double now = glfwGetTime();
        if (now - last_title_update >= 0.25) {
            char title[128];
            std::snprintf(title, sizeof(title),
                          "Grebe | FPS: %.1f | Frame: %.2f ms | %uch | %s",
                          app.benchmark->fps(), app.benchmark->frame_time_avg(),
                          app.num_channels,
                          DecimationThread::mode_name(app.dec_thread->effective_mode()));
            glfwSetWindowTitle(app.window, title);
            last_title_update = now;
        }
    }
}
