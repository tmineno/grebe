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

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

static bool g_framebuffer_resized = false;

struct AppState {
    DataGenerator* data_gen = nullptr;
    DecimationThread* dec_thread = nullptr;
};

static void framebuffer_resize_callback(GLFWwindow* /*window*/, int /*width*/, int /*height*/) {
    g_framebuffer_resized = true;
}

static void key_callback(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
    if (action != GLFW_PRESS) return;

    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));

    switch (key) {
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        break;
    case GLFW_KEY_V:
        // V-Sync toggle (future)
        break;
    case GLFW_KEY_D:
        if (state && state->dec_thread) state->dec_thread->cycle_mode();
        break;
    case GLFW_KEY_1:
        if (state && state->data_gen) state->data_gen->set_sample_rate(1e6);
        break;
    case GLFW_KEY_2:
        if (state && state->data_gen) state->data_gen->set_sample_rate(10e6);
        break;
    case GLFW_KEY_3:
        if (state && state->data_gen) state->data_gen->set_sample_rate(100e6);
        break;
    case GLFW_KEY_4:
        if (state && state->data_gen) state->data_gen->set_sample_rate(1e9);
        break;
    case GLFW_KEY_SPACE:
        if (state && state->data_gen)
            state->data_gen->set_paused(!state->data_gen->is_paused());
        break;
    default:
        break;
    }
}

int main(int argc, char* argv[]) {
    try {
        // Parse CLI options
        bool enable_log = false;
        bool enable_profile = false;
        for (int i = 1; i < argc; i++) {
            if (std::string(argv[i]) == "--log") enable_log = true;
            if (std::string(argv[i]) == "--profile") enable_profile = true;
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
        AppState app_state;
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

        // Streaming pipeline
        constexpr size_t RING_BUFFER_CAPACITY = 16'777'216; // 16M samples (32 MB)
        RingBuffer<int16_t> ring_buffer(RING_BUFFER_CAPACITY + 1); // +1 for SPSC sentinel

        DataGenerator data_gen;
        app_state.data_gen = &data_gen;
        data_gen.start(ring_buffer, 1'000'000.0, WaveformType::Sine);

        // Decimation thread (Phase 2)
        constexpr uint32_t DECIMATE_TARGET = 1920 * 2; // min/max pairs for screen width
        DecimationMode default_mode = enable_profile ? DecimationMode::MinMax : DecimationMode::MinMax;
        DecimationThread dec_thread;
        app_state.dec_thread = &dec_thread;
        dec_thread.start(ring_buffer, DECIMATE_TARGET, default_mode);

        spdlog::info("Streaming started: 1 MSPS, Sine wave, decimation={}", DecimationThread::mode_name(default_mode));

        // Profile mode
        ProfileRunner profiler;
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

        // Push constants
        WaveformPushConstants push_constants;
        push_constants.amplitude_scale = 0.8f;
        push_constants.vertical_offset = 0.0f;
        push_constants.horizontal_scale = 1.0f;
        push_constants.horizontal_offset = 0.0f;
        push_constants.vertex_count = 0;

        // Title update throttle
        double last_title_update = glfwGetTime();

        // Main loop
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

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

            // Update push constants with current draw buffer vertex count
            push_constants.vertex_count = static_cast<int>(buf_mgr.vertex_count());

            // Build ImGui frame
            hud.new_frame();
            hud.build_status_bar(benchmark, data_gen.actual_sample_rate(),
                                 dec_thread.ring_fill_ratio(), buf_mgr.vertex_count(),
                                 data_gen.is_paused(), dec_thread.current_mode());

            // Render (timed)
            t0 = Benchmark::now();
            bool ok = renderer.draw_frame(ctx, swapchain, buf_mgr, push_constants, &hud);
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
                                  data_gen, window);
            }

            // Update window title with FPS (throttled to 4 Hz)
            double now = glfwGetTime();
            if (now - last_title_update >= 0.25) {
                char title[128];
                std::snprintf(title, sizeof(title),
                              "Grebe | FPS: %.1f | Frame: %.2f ms | %s",
                              benchmark.fps(), benchmark.frame_time_avg(),
                              DecimationThread::mode_name(dec_thread.current_mode()));
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
