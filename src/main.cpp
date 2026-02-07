#include "app_command.h"
#include "vulkan_context.h"
#include "swapchain.h"
#include "renderer.h"
#include "buffer_manager.h"
#include "data_generator.h"
#include "ring_buffer.h"
#include "decimation_thread.h"
#include "benchmark.h"
#include "hud.h"
#include "profiler.h"
#include "microbench.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
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

static void process_commands(AppCommandQueue& cmd_queue,
                             GLFWwindow* window,
                             DataGenerator& data_gen,
                             DecimationThread& dec_thread,
                             Swapchain& swapchain,
                             VulkanContext& ctx,
                             Renderer& renderer,
                             Hud& hud) {
    for (auto& cmd : cmd_queue.drain()) {
        std::visit([&](auto&& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, CmdSetSampleRate>) {
                data_gen.set_sample_rate(c.rate);
                dec_thread.set_sample_rate(c.rate);
            } else if constexpr (std::is_same_v<T, CmdCycleDecimationMode>) {
                dec_thread.cycle_mode();
            } else if constexpr (std::is_same_v<T, CmdTogglePaused>) {
                data_gen.set_paused(!data_gen.is_paused());
            } else if constexpr (std::is_same_v<T, CmdToggleVsync>) {
                int w, h;
                glfwGetFramebufferSize(window, &w, &h);
                if (w > 0 && h > 0) {
                    swapchain.set_vsync(!swapchain.vsync());
                    swapchain.recreate(ctx, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                    renderer.on_swapchain_recreated(ctx, swapchain);
                    hud.on_swapchain_recreated(swapchain.image_count());
                    spdlog::info("V-Sync {}", swapchain.vsync() ? "ON" : "OFF");
                }
            } else if constexpr (std::is_same_v<T, CmdQuit>) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }, cmd);
    }
}

int main(int argc, char* argv[]) {
    try {
        // Parse CLI options
        bool enable_log = false;
        bool enable_profile = false;
        bool enable_bench = false;
        size_t ring_size = 16'777'216; // 16M samples default
        uint32_t num_channels = 1;
        for (int i = 1; i < argc; i++) {
            std::string arg(argv[i]);
            if (arg == "--log") enable_log = true;
            else if (arg == "--profile") enable_profile = true;
            else if (arg == "--bench") enable_bench = true;
            else if (arg.rfind("--ring-size=", 0) == 0) {
                std::string val = arg.substr(12);
                size_t multiplier = 1;
                if (!val.empty()) {
                    char suffix = val.back();
                    if (suffix == 'M' || suffix == 'm') { multiplier = 1024ULL * 1024; val.pop_back(); }
                    else if (suffix == 'G' || suffix == 'g') { multiplier = 1024ULL * 1024 * 1024; val.pop_back(); }
                    else if (suffix == 'K' || suffix == 'k') { multiplier = 1024; val.pop_back(); }
                }
                ring_size = std::stoull(val) * multiplier;
            }
            else if (arg.rfind("--channels=", 0) == 0) {
                num_channels = static_cast<uint32_t>(std::stoul(arg.substr(11)));
                if (num_channels < 1 || num_channels > 8) {
                    spdlog::error("--channels must be 1-8, got {}", num_channels);
                    return 1;
                }
            }
        }

        // Init GLFW
        if (!glfwInit()) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Vulkan, not OpenGL
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        GLFWwindow* window = glfwCreateWindow(1920, 1080, "Grebe", nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            throw std::runtime_error("Failed to create GLFW window");
        }

        // App state for keyboard callbacks
        AppCommandQueue cmd_queue;
        AppState app_state;
        app_state.cmd_queue = &cmd_queue;
        glfwSetWindowUserPointer(window, &app_state);
        glfwSetFramebufferSizeCallback(window, framebuffer_resize_callback);
        glfwSetKeyCallback(window, key_callback);

        // Init Vulkan
        VulkanContext ctx;
        ctx.init(window);

        // Init swapchain
        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);

        Swapchain swapchain;
        swapchain.init(ctx, static_cast<uint32_t>(fb_width), static_cast<uint32_t>(fb_height));

        // Init buffer manager (triple-buffered)
        BufferManager buf_mgr;
        buf_mgr.init(ctx);

        // Init renderer
        Renderer renderer;
        renderer.init(ctx, swapchain, SHADER_DIR);

        // Bench mode: run microbenchmarks and exit
        if (enable_bench) {
            // Recreate swapchain with V-Sync OFF for draw benchmarks
            swapchain.set_vsync(false);
            swapchain.recreate(ctx, static_cast<uint32_t>(fb_width), static_cast<uint32_t>(fb_height));
            renderer.on_swapchain_recreated(ctx, swapchain);

            int bench_code = run_microbenchmarks(ctx, swapchain, buf_mgr, renderer, SHADER_DIR);

            renderer.destroy();
            buf_mgr.destroy();
            swapchain.destroy(ctx.device());
            ctx.destroy();
            glfwDestroyWindow(window);
            glfwTerminate();
            return bench_code;
        }

        // Init benchmark
        Benchmark benchmark;
        if (enable_log) {
            std::filesystem::create_directories("./tmp");
            std::time_t t = std::time(nullptr);
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
            benchmark.start_logging(std::string("./tmp/telemetry_") + ts + ".csv");
        }

        // Init HUD (ImGui)
        Hud hud;
        hud.init(window, ctx, renderer.render_pass(), swapchain.image_count());

        // Streaming pipeline: N ring buffers (one per channel)
        std::vector<std::unique_ptr<RingBuffer<int16_t>>> ring_buffers;
        std::vector<RingBuffer<int16_t>*> ring_ptrs;
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            ring_buffers.push_back(std::make_unique<RingBuffer<int16_t>>(ring_size + 1));
            ring_ptrs.push_back(ring_buffers.back().get());
        }
        spdlog::info("Ring buffers: {}ch × {} samples ({} MB each)",
                     num_channels, ring_size, ring_size * 2 / (1024 * 1024));

        DataGenerator data_gen;
        data_gen.start(ring_ptrs, 1'000'000.0, WaveformType::Sine);

        // Decimation thread (Phase 2)
        constexpr uint32_t DECIMATE_TARGET = 1920 * 2; // min/max pairs for screen width
        DecimationMode default_mode = DecimationMode::MinMax;
        DecimationThread dec_thread;
        dec_thread.start(ring_ptrs, DECIMATE_TARGET, default_mode);
        dec_thread.set_sample_rate(1'000'000.0);

        spdlog::info("Streaming started: {}ch, 1 MSPS, Sine wave, decimation={}",
                     num_channels, DecimationThread::mode_name(default_mode));

        // Profile mode
        ProfileRunner profiler;
        profiler.set_channel_count(num_channels);
        if (enable_profile) {
            spdlog::info("Profile mode enabled — auto-cycling through scenarios");
            if (!benchmark.is_logging()) {
                std::filesystem::create_directories("./tmp");
                std::time_t pt = std::time(nullptr);
                char pts[32];
                std::strftime(pts, sizeof(pts), "%Y%m%d_%H%M%S", std::localtime(&pt));
                benchmark.start_logging(std::string("./tmp/telemetry_profile_") + pts + ".csv");
            }
        }

        // Per-frame data buffer (receives decimated output)
        std::vector<int16_t> frame_data;

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
        std::vector<WaveformPushConstants> channel_pcs(num_channels);
        float n = static_cast<float>(num_channels);
        for (uint32_t ch = 0; ch < num_channels; ch++) {
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

        // Title update throttle
        double last_title_update = glfwGetTime();

        // Main loop
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // Process queued commands from keyboard/profiler
            process_commands(cmd_queue, window, data_gen, dec_thread,
                             swapchain, ctx, renderer, hud);

            // Handle resize
            if (g_framebuffer_resized) {
                g_framebuffer_resized = false;
                int w, h;
                glfwGetFramebufferSize(window, &w, &h);
                if (w > 0 && h > 0) {
                    swapchain.recreate(ctx, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                    renderer.on_swapchain_recreated(ctx, swapchain);
                    hud.on_swapchain_recreated(swapchain.image_count());
                }
                continue; // skip this frame
            }

            // Skip minimized windows
            {
                int w, h;
                glfwGetFramebufferSize(window, &w, &h);
                if (w == 0 || h == 0) {
                    glfwWaitEvents();
                    continue;
                }
            }

            benchmark.frame_begin();

            // Get decimated frame from decimation thread (timed as "drain")
            auto t0 = Benchmark::now();
            uint32_t raw_samples = 0;
            bool has_new_data = dec_thread.try_get_frame(frame_data, raw_samples);
            benchmark.set_drain_time(Benchmark::elapsed_ms(t0));
            benchmark.set_samples_per_frame(raw_samples);
            benchmark.set_decimation_time(dec_thread.decimation_time_ms());
            benchmark.set_decimation_ratio(dec_thread.decimation_ratio());
            benchmark.set_data_rate(data_gen.actual_sample_rate());
            benchmark.set_ring_fill(dec_thread.ring_fill_ratio());

            // Upload decimated data to GPU (timed)
            t0 = Benchmark::now();
            if (has_new_data && !frame_data.empty()) {
                buf_mgr.upload_streaming(frame_data);
            }
            benchmark.set_upload_time(Benchmark::elapsed_ms(t0));

            // Promote completed transfers to draw position (timed)
            t0 = Benchmark::now();
            buf_mgr.try_swap();
            benchmark.set_swap_time(Benchmark::elapsed_ms(t0));

            benchmark.set_vertex_count(buf_mgr.vertex_count());

            // Update per-channel vertex counts and first_vertex offsets
            uint32_t per_ch_vtx = dec_thread.per_channel_vertex_count();
            uint32_t first_vtx = 0;
            for (uint32_t ch = 0; ch < num_channels; ch++) {
                channel_pcs[ch].vertex_count = static_cast<int>(per_ch_vtx);
                channel_pcs[ch].first_vertex = static_cast<int>(first_vtx);
                first_vtx += per_ch_vtx;
            }

            // Build ImGui frame
            hud.new_frame();
            hud.build_status_bar(benchmark, data_gen.actual_sample_rate(),
                                 dec_thread.ring_fill_ratio(), buf_mgr.vertex_count(),
                                 data_gen.is_paused(), dec_thread.effective_mode(),
                                 num_channels);

            // Render (timed)
            t0 = Benchmark::now();
            bool ok = renderer.draw_frame(ctx, swapchain, buf_mgr, channel_pcs.data(), num_channels, &hud);
            benchmark.set_render_time(Benchmark::elapsed_ms(t0));
            if (!ok) {
                // Swapchain out of date — recreate
                int w, h;
                glfwGetFramebufferSize(window, &w, &h);
                if (w > 0 && h > 0) {
                    swapchain.recreate(ctx, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                    renderer.on_swapchain_recreated(ctx, swapchain);
                    hud.on_swapchain_recreated(swapchain.image_count());
                }
                continue;
            }

            benchmark.frame_end();

            // Profile mode: collect metrics and manage scenario transitions
            if (enable_profile) {
                profiler.on_frame(benchmark, buf_mgr.vertex_count(),
                                  data_gen.actual_sample_rate(),
                                  dec_thread.ring_fill_ratio(),
                                  data_gen, dec_thread, window);
            }

            // Update window title with FPS (throttled to 4 Hz)
            double now = glfwGetTime();
            if (now - last_title_update >= 0.25) {
                char title[128];
                std::snprintf(title, sizeof(title),
                              "Grebe | FPS: %.1f | Frame: %.2f ms | %uch | %s",
                              benchmark.fps(), benchmark.frame_time_avg(),
                              num_channels,
                              DecimationThread::mode_name(dec_thread.effective_mode()));
                glfwSetWindowTitle(window, title);
                last_title_update = now;
            }
        }

        // Cleanup
        benchmark.stop_logging();
        dec_thread.stop();
        data_gen.stop();
        vkDeviceWaitIdle(ctx.device());

        // Generate profile report if in profile mode
        int exit_code = 0;
        if (enable_profile) {
            exit_code = profiler.generate_report();
        }

        hud.destroy();
        renderer.destroy();
        buf_mgr.destroy();
        swapchain.destroy(ctx.device());
        ctx.destroy();

        glfwDestroyWindow(window);
        glfwTerminate();

        spdlog::info("Clean shutdown");
        return exit_code;

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        glfwTerminate();
        return 1;
    }
}
