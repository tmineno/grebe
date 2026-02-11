#include "app_loop.h"
#include "app_command.h"
#include "vulkan_renderer.h"
#include "synthetic_source.h"
#include "transport_source.h"
#include "grebe/runtime.h"
#include "stages/decimation_stage.h"
#include "stages/visualization_stage.h"
#include "grebe/batch.h"
#include "benchmark.h"
#include "hud.h"
#include "profiler.h"
#include "ipc/transport.h"
#include "ipc/contracts.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    case GLFW_KEY_G:       q.push(CmdDebugDump{});              break;
    default: break;
    }
}

// Map internal DecimationMode to public DecimationAlgorithm for HUD/profiler
static grebe::DecimationAlgorithm to_algorithm(DecimationMode mode) {
    switch (mode) {
    case DecimationMode::None:   return grebe::DecimationAlgorithm::None;
    case DecimationMode::MinMax: return grebe::DecimationAlgorithm::MinMax;
    case DecimationMode::LTTB:   return grebe::DecimationAlgorithm::LTTB;
    }
    return grebe::DecimationAlgorithm::None;
}

// Cycle: None → MinMax → LTTB → None
static DecimationMode next_decimation_mode(DecimationMode m) {
    switch (m) {
    case DecimationMode::None:   return DecimationMode::MinMax;
    case DecimationMode::MinMax: return DecimationMode::LTTB;
    case DecimationMode::LTTB:   return DecimationMode::None;
    }
    return DecimationMode::None;
}

// Algorithm name for display
static const char* decimation_mode_name(DecimationMode m) {
    switch (m) {
    case DecimationMode::None:   return "None";
    case DecimationMode::MinMax: return "MinMax";
    case DecimationMode::LTTB:   return "LTTB";
    }
    return "Unknown";
}

static void process_commands(AppComponents& app) {
    for (auto& cmd : app.cmd_queue->drain()) {
        std::visit([&](auto&& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, CmdSetSampleRate>) {
                if (app.synthetic_source) {
                    app.synthetic_source->set_sample_rate(c.rate);
                } else if (app.transport_source) {
                    IpcCommand ipc{};
                    ipc.type = IpcCommand::SET_SAMPLE_RATE;
                    ipc.value = c.rate;
                    app.transport_source->transport().send_command(ipc);
                }
                if (app.dec_stage) {
                    app.dec_stage->set_sample_rate(c.rate);
                }
                app.current_sample_rate.store(c.rate, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, CmdCycleDecimationMode>) {
                if (app.dec_stage) {
                    auto cur = app.dec_stage->mode();
                    auto next = next_decimation_mode(cur);
                    app.dec_stage->set_mode(next);
                    spdlog::info("Decimation mode → {}", decimation_mode_name(next));
                }
            } else if constexpr (std::is_same_v<T, CmdTogglePaused>) {
                if (app.synthetic_source) {
                    app.synthetic_source->set_paused(!app.synthetic_source->is_paused());
                    app.current_paused.store(app.synthetic_source->is_paused(), std::memory_order_relaxed);
                } else if (app.transport_source) {
                    IpcCommand ipc{};
                    ipc.type = IpcCommand::TOGGLE_PAUSED;
                    app.transport_source->transport().send_command(ipc);
                    app.current_paused.store(!app.current_paused.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
            } else if constexpr (std::is_same_v<T, CmdToggleVsync>) {
                app.render_backend->set_vsync(!app.render_backend->vsync());
                app.hud->on_swapchain_recreated(app.render_backend->image_count());
                spdlog::info("V-Sync {}", app.render_backend->vsync() ? "ON" : "OFF");
            } else if constexpr (std::is_same_v<T, CmdQuit>) {
                glfwSetWindowShouldClose(app.window, GLFW_TRUE);
            } else if constexpr (std::is_same_v<T, CmdDebugDump>) {
                if (app.viz_stage) {
                    app.viz_stage->request_debug_dump("./tmp");
                    spdlog::info("Debug dump requested (G key)");
                }
            }
        }, cmd);
    }
}

void run_main_loop(AppComponents& app) {
    // Register GLFW callbacks
    AppState app_state;
    app_state.cmd_queue = app.cmd_queue;
    app_state.is_ipc_mode = (app.transport_source != nullptr);
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

    // Visible time-span (GUI-side display scaling)
    double visible_time_span_s = 10e-3; // 10 ms default

    // Title update throttle
    double last_title_update = glfwGetTime();

    // Current effective algorithm for display
    auto effective_algo = grebe::DecimationAlgorithm::MinMax;

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


        // Drain ALL frames from runtime pipeline (preserves continuity)
        auto t0 = Benchmark::now();
        std::vector<grebe::Frame> viz_input;
        while (auto f = app.runtime->poll_output()) {
            viz_input.push_back(std::move(*f));
        }
        app.benchmark->set_drain_time(Benchmark::elapsed_ms(t0));

        // Save last pipeline frame for profiler envelope verification
        // (before viz_stage double-decimation)
        grebe::Frame last_pipeline_frame = grebe::Frame::make_owned(0, 0);
        if (!viz_input.empty()) {
            const auto& src = viz_input.back();
            last_pipeline_frame = grebe::Frame::make_owned(src.channel_count, src.samples_per_channel);
            last_pipeline_frame.sample_rate_hz = src.sample_rate_hz;
            last_pipeline_frame.sequence = src.sequence;
            if (src.data_count() > 0) {
                std::memcpy(last_pipeline_frame.mutable_data(), src.data(),
                            src.data_count() * sizeof(int16_t));
            }
        }

        // Feed all frames to visualization stage (accumulate + window + decimate)
        uint32_t per_ch_vtx = 0;
        uint32_t raw_sample_count = 0;
        grebe::Frame display_frame = grebe::Frame::make_owned(0, 0);
        {
            grebe::BatchView view(std::move(viz_input));
            grebe::BatchWriter writer;
            grebe::ExecContext ctx{};
            app.viz_stage->process(view, writer, ctx);

            auto display_frames = writer.take();
            if (!display_frames.empty()) {
                display_frame = std::move(display_frames.back());
                per_ch_vtx = display_frame.samples_per_channel;
                raw_sample_count = display_frame.channel_count * display_frame.samples_per_channel;

                t0 = Benchmark::now();
                app.render_backend->upload_vertices(display_frame.data(), display_frame.data_count());
                app.benchmark->set_upload_time(Benchmark::elapsed_ms(t0));
            } else {
                app.benchmark->set_upload_time(0.0);
            }
        }

        app.benchmark->set_samples_per_frame(raw_sample_count);

        // Get telemetry from runtime
        auto telem = app.runtime->telemetry();
        double dec_time_ms = 0.0;
        uint64_t queue_drops = 0;
        for (auto& st : telem) {
            if (st.name == "DecimationStage") {
                dec_time_ms = st.avg_process_time_ms;
            }
            queue_drops += st.queue_dropped;
        }

        // Effective algorithm
        if (app.dec_stage) {
            effective_algo = to_algorithm(app.dec_stage->effective_mode());
        }

        // Decimation ratio: target / raw_per_channel (approximate)
        double dec_ratio = 1.0;
        if (app.dec_stage && per_ch_vtx > 0) {
            dec_ratio = static_cast<double>(app.dec_stage->target_points())
                      / static_cast<double>(per_ch_vtx);
        }

        app.benchmark->set_decimation_time(dec_time_ms);
        app.benchmark->set_decimation_ratio(dec_ratio);

        // Data rate: from SyntheticSource (embedded) or stored atomic
        double data_rate = app.synthetic_source
            ? app.synthetic_source->actual_sample_rate()
            : app.current_sample_rate.load(std::memory_order_relaxed);
        bool paused = app.synthetic_source
            ? app.synthetic_source->is_paused()
            : app.current_paused.load(std::memory_order_relaxed);

        // Sync decimation stage with actual sample rate
        if (data_rate > 0.0 && app.dec_stage) {
            app.dec_stage->set_sample_rate(data_rate);
        }

        app.benchmark->set_data_rate(data_rate);

        // Promote completed transfers to draw position (timed)
        t0 = Benchmark::now();
        app.render_backend->swap_buffers();
        app.benchmark->set_swap_time(Benchmark::elapsed_ms(t0));

        app.benchmark->set_vertex_count(app.render_backend->vertex_count());

        // Update per-channel vertex counts and first_vertex offsets
        uint32_t first_vtx = 0;
        for (uint32_t ch = 0; ch < app.num_channels; ch++) {
            channel_cmds[ch].vertex_count = static_cast<int>(per_ch_vtx);
            channel_cmds[ch].first_vertex = static_cast<int>(first_vtx);
            first_vtx += per_ch_vtx;
        }

        // SG-side drops (IPC mode only)
        uint64_t sg_drops = app.transport_source ? app.transport_source->sg_drops_total() : 0;

        // Build ImGui frame
        app.hud->new_frame();

        // Dynamic time-span limits
        double min_time_span_s = 1e-6;
        double max_time_span_s = 100e-3;
        if (data_rate > 0.0) {
            const double sample_period_s = 1.0 / data_rate;
            min_time_span_s = std::max(sample_period_s, 64.0 * sample_period_s);
            max_time_span_s = std::max(max_time_span_s, min_time_span_s);
            double clamped = std::clamp(visible_time_span_s, min_time_span_s, max_time_span_s);
            if (clamped != visible_time_span_s) {
                visible_time_span_s = clamped;
                app.viz_stage->set_visible_time_span(visible_time_span_s);
            }
        }

        // Window coverage from visualization stage
        double window_coverage = app.viz_stage->window_coverage();

        auto telemetry = app.benchmark->snapshot();
        app.hud->build_status_bar(telemetry,
                                  paused,
                                  effective_algo,
                                  app.num_channels,
                                  queue_drops,
                                  sg_drops,
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
            app.viz_stage->set_visible_time_span(visible_time_span_s);
            spdlog::info("Visible time span -> {:.3f} ms (range {:.3f}..{:.3f} ms)",
                         visible_time_span_s * 1e3,
                         min_time_span_s * 1e3,
                         max_time_span_s * 1e3);
        }

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
            // Use pipeline frame (single MinMax) for envelope verification,
            // not display frame (double-decimated by viz_stage)
            const bool has_pipeline = last_pipeline_frame.samples_per_channel > 0;
            const int16_t* prof_data = has_pipeline ? last_pipeline_frame.data() : nullptr;
            uint32_t prof_per_ch_vtx = has_pipeline ? last_pipeline_frame.samples_per_channel : 0;

            // Compute per-channel raw sample count from rate ratio
            // raw_spc = original_rate / decimated_rate * decimated_spc
            std::vector<uint32_t> per_ch_raw_vec;
            if (has_pipeline && last_pipeline_frame.sample_rate_hz > 0.0 && data_rate > 0.0) {
                uint32_t raw_spc = static_cast<uint32_t>(
                    std::round(data_rate / last_pipeline_frame.sample_rate_hz
                               * last_pipeline_frame.samples_per_channel));
                per_ch_raw_vec.assign(app.num_channels, raw_spc);
            }

            uint32_t prof_raw_total = per_ch_raw_vec.empty() ? raw_sample_count
                : static_cast<uint32_t>(per_ch_raw_vec[0]) * app.num_channels;

            app.profiler->on_frame(*app.benchmark, app.render_backend->vertex_count(),
                                   data_rate,
                                   queue_drops, sg_drops,
                                   prof_raw_total,
                                   *app.cmd_queue,
                                   prof_data,
                                   prof_per_ch_vtx,
                                   effective_algo,
                                   per_ch_raw_vec.empty() ? nullptr : &per_ch_raw_vec);
        }

        // Update window title with FPS (throttled to 4 Hz)
        double now = glfwGetTime();
        if (now - last_title_update >= 0.25) {
            char title[128];
            std::snprintf(title, sizeof(title),
                          "Grebe | FPS: %.1f | Frame: %.2f ms | %uch | %s%s",
                          telemetry.fps, telemetry.frame_time_ms,
                          app.num_channels,
                          decimation_mode_name(app.dec_stage ? app.dec_stage->effective_mode() : DecimationMode::MinMax),
                          app.transport_source ? " | IPC" : "");
            glfwSetWindowTitle(app.window, title);
            last_title_update = now;
        }
    }
}
