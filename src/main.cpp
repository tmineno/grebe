#include "app_command.h"
#include "app_loop.h"
#include "cli.h"
#include "drop_counter.h"
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

#include <ctime>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    try {
        CliOptions opts;
        if (int rc = parse_cli(argc, argv, opts); rc != 0) return rc;

        // Init GLFW
        if (!glfwInit()) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        GLFWwindow* window = glfwCreateWindow(1920, 1080, "Grebe", nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            throw std::runtime_error("Failed to create GLFW window");
        }

        // Init Vulkan
        VulkanContext ctx;
        ctx.init(window);

        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);

        Swapchain swapchain;
        swapchain.init(ctx, static_cast<uint32_t>(fb_width), static_cast<uint32_t>(fb_height));

        BufferManager buf_mgr;
        buf_mgr.init(ctx);

        Renderer renderer;
        renderer.init(ctx, swapchain, SHADER_DIR);

        // Bench mode: run microbenchmarks and exit
        if (opts.enable_bench) {
            swapchain.set_vsync(false);
            swapchain.recreate(ctx, static_cast<uint32_t>(fb_width),
                               static_cast<uint32_t>(fb_height));
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

        // Init telemetry
        Benchmark benchmark;
        if (opts.enable_log) {
            std::filesystem::create_directories("./tmp");
            std::time_t t = std::time(nullptr);
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
            benchmark.start_logging(std::string("./tmp/telemetry_") + ts + ".csv");
        }

        Hud hud;
        hud.init(window, ctx, renderer.render_pass(), swapchain.image_count());

        // Streaming pipeline
        std::vector<std::unique_ptr<RingBuffer<int16_t>>> ring_buffers;
        std::vector<RingBuffer<int16_t>*> ring_ptrs;
        for (uint32_t ch = 0; ch < opts.num_channels; ch++) {
            ring_buffers.push_back(
                std::make_unique<RingBuffer<int16_t>>(opts.ring_size + 1));
            ring_ptrs.push_back(ring_buffers.back().get());
        }
        spdlog::info("Ring buffers: {}ch x {} samples ({} MB each)",
                     opts.num_channels, opts.ring_size,
                     opts.ring_size * 2 / (1024 * 1024));

        // Per-channel drop counters
        std::vector<std::unique_ptr<DropCounter>> drop_counters;
        std::vector<DropCounter*> drop_ptrs;
        for (uint32_t ch = 0; ch < opts.num_channels; ch++) {
            drop_counters.push_back(std::make_unique<DropCounter>());
            drop_ptrs.push_back(drop_counters.back().get());
        }

        DataGenerator data_gen;
        data_gen.set_drop_counters(drop_ptrs);
        data_gen.start(ring_ptrs, 1'000'000.0, WaveformType::Sine);

        constexpr uint32_t DECIMATE_TARGET = 1920 * 2;
        DecimationMode default_mode = DecimationMode::MinMax;
        DecimationThread dec_thread;
        dec_thread.start(ring_ptrs, DECIMATE_TARGET, default_mode);
        dec_thread.set_sample_rate(1'000'000.0);

        spdlog::info("Streaming started: {}ch, 1 MSPS, Sine wave, decimation={}",
                     opts.num_channels, DecimationThread::mode_name(default_mode));

        ProfileRunner profiler;
        profiler.set_channel_count(opts.num_channels);
        if (opts.enable_profile) {
            spdlog::info("Profile mode enabled");
            if (!benchmark.is_logging()) {
                std::filesystem::create_directories("./tmp");
                std::time_t pt = std::time(nullptr);
                char pts[32];
                std::strftime(pts, sizeof(pts), "%Y%m%d_%H%M%S", std::localtime(&pt));
                benchmark.start_logging(
                    std::string("./tmp/telemetry_profile_") + pts + ".csv");
            }
        }

        // Run main loop
        AppCommandQueue cmd_queue;
        AppComponents app{};
        app.window = window;
        app.cmd_queue = &cmd_queue;
        app.ctx = &ctx;
        app.swapchain = &swapchain;
        app.renderer = &renderer;
        app.buf_mgr = &buf_mgr;
        app.hud = &hud;
        app.data_gen = &data_gen;
        app.dec_thread = &dec_thread;
        app.benchmark = &benchmark;
        app.profiler = &profiler;
        app.drop_counters = drop_ptrs;
        app.num_channels = opts.num_channels;
        app.enable_profile = opts.enable_profile;

        run_main_loop(app);

        // Cleanup
        benchmark.stop_logging();
        dec_thread.stop();
        data_gen.stop();
        vkDeviceWaitIdle(ctx.device());

        int exit_code = 0;
        if (opts.enable_profile) {
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
