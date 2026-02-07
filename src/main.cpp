#include "vulkan_context.h"
#include "swapchain.h"
#include "renderer.h"
#include "buffer_manager.h"
#include "data_generator.h"
#include "benchmark.h"
#include "hud.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

static bool g_framebuffer_resized = false;

static void framebuffer_resize_callback(GLFWwindow* /*window*/, int /*width*/, int /*height*/) {
    g_framebuffer_resized = true;
}

static void key_callback(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
    if (action != GLFW_PRESS) return;

    switch (key) {
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        break;
    case GLFW_KEY_V:
        // V-Sync toggle will be handled in main loop via user data
        break;
    // Phase 1+ key bindings:
    // case GLFW_KEY_1..4: data rate selection
    // case GLFW_KEY_D: decimation mode toggle
    // case GLFW_KEY_B: benchmark start
    // case GLFW_KEY_H: HUD toggle
    // case GLFW_KEY_SPACE: pause/resume
    default:
        break;
    }
}

int main() {
    try {
        // Init GLFW
        if (!glfwInit()) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Vulkan, not OpenGL
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        GLFWwindow* window = glfwCreateWindow(1920, 1080, "Vulkan Stream PoC", nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            throw std::runtime_error("Failed to create GLFW window");
        }

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

        // Init buffer manager
        BufferManager buf_mgr;
        buf_mgr.init(ctx);

        // Generate and upload static sine wave data
        auto sine_data = DataGenerator::generate_static(WaveformType::Sine, 3840, 3.0, 3840.0);
        buf_mgr.upload_vertex_data(sine_data);

        // Init renderer
        Renderer renderer;
        renderer.init(ctx, swapchain, SHADER_DIR);

        // Init benchmark
        Benchmark benchmark;

        // Init HUD (ImGui)
        Hud hud;
        hud.init(window, ctx, renderer.render_pass(), swapchain.image_count());

        // Push constants
        WaveformPushConstants push_constants;
        push_constants.amplitude_scale = 0.8f;
        push_constants.vertical_offset = 0.0f;
        push_constants.horizontal_scale = 1.0f;
        push_constants.horizontal_offset = 0.0f;
        push_constants.vertex_count = static_cast<int>(buf_mgr.vertex_count());

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

            // Build ImGui frame
            hud.new_frame();
            hud.build_status_bar(benchmark.fps(), benchmark.frame_time_avg());

            bool ok = renderer.draw_frame(ctx, swapchain, buf_mgr, push_constants, &hud);
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

            // Update window title with FPS (throttled to 4 Hz)
            double now = glfwGetTime();
            if (now - last_title_update >= 0.25) {
                char title[128];
                std::snprintf(title, sizeof(title),
                              "Vulkan Stream PoC | FPS: %.1f | Frame: %.2f ms",
                              benchmark.fps(), benchmark.frame_time_avg());
                glfwSetWindowTitle(window, title);
                last_title_update = now;
            }
        }

        // Cleanup
        vkDeviceWaitIdle(ctx.device());

        hud.destroy();
        renderer.destroy();
        buf_mgr.destroy();
        swapchain.destroy(ctx.device());
        ctx.destroy();

        glfwDestroyWindow(window);
        glfwTerminate();

        spdlog::info("Clean shutdown");
        return 0;

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        glfwTerminate();
        return 1;
    }
}
