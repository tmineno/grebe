#include "app_command.h"
#include "app_loop.h"
#include "cli.h"
#include "drop_counter.h"
#include "vulkan_context.h"
#include "swapchain.h"
#include "renderer.h"
#include "buffer_manager.h"
#include "synthetic_source.h"
#include "ipc_source.h"
#include "ingestion_thread.h"
#include "ring_buffer.h"
#include "decimation_thread.h"
#include "benchmark.h"
#include "hud.h"
#include "profiler.h"
#include "microbench.h"
#include "process_handle.h"
#include "pipe_transport.h"
#include "contracts.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <ctime>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Find the grebe-sg binary next to the grebe binary
static std::string find_sg_binary(const char* argv0) {
    std::filesystem::path exe_path(argv0);
    auto sg_path = exe_path.parent_path() / "grebe-sg";
    if (std::filesystem::exists(sg_path)) {
        return sg_path.string();
    }
    // Fallback: try PATH
    return "grebe-sg";
}

int main(int argc, char* argv[]) {
    try {
        CliOptions opts;
        if (int rc = parse_cli(argc, argv, opts); rc != 0) return rc;

#ifndef NDEBUG
        if (opts.enable_profile || opts.enable_bench) {
            spdlog::warn("Running --profile/--bench in Debug build — results are not representative. Use Release.");
        }
#endif

        // Init GLFW
        if (!glfwInit()) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        if (opts.minimized) {
            glfwWindowHint(GLFW_ICONIFIED, GLFW_TRUE);
        }

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

        // Apply --no-vsync if requested
        if (opts.no_vsync) {
            swapchain.set_vsync(false);
            swapchain.recreate(ctx, static_cast<uint32_t>(fb_width),
                               static_cast<uint32_t>(fb_height));
            renderer.on_swapchain_recreated(ctx, swapchain);
            spdlog::info("V-Sync disabled via --no-vsync");
        }

        // Bench mode: run microbenchmarks and exit (no grebe-sg needed)
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

        // =====================================================================
        // Streaming pipeline: ring buffers (used by both modes)
        // =====================================================================
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

        // =====================================================================
        // Data source: SyntheticSource (embedded) or IpcSource (IPC)
        // =====================================================================
        std::unique_ptr<SyntheticSource> synthetic_source;
        std::unique_ptr<IpcSource> ipc_source;
        std::unique_ptr<ProcessHandle> sg_process;
        std::unique_ptr<PipeConsumer> pipe_consumer;
        IngestionThread ingestion;

        if (opts.embedded) {
            // Embedded mode: SyntheticSource + IngestionThread
            synthetic_source = std::make_unique<SyntheticSource>(
                opts.num_channels, 1'000'000.0, WaveformType::Sine);
            synthetic_source->start();
            ingestion.start(*synthetic_source, ring_ptrs, drop_ptrs);
            spdlog::info("Embedded mode: SyntheticSource + IngestionThread");
        } else {
            // IPC mode: spawn grebe-sg, IpcSource + IngestionThread
            std::string sg_path = find_sg_binary(argv[0]);
            std::vector<std::string> sg_args;
            sg_args.push_back("--channels=" + std::to_string(opts.num_channels));
            sg_args.push_back("--block-size=" + std::to_string(opts.block_size));
            if (opts.ring_size != 67'108'864) {
                sg_args.push_back("--ring-size=" + std::to_string(opts.ring_size));
            }

            sg_process = std::make_unique<ProcessHandle>();
            int stdin_fd = -1, stdout_fd = -1;
            if (!sg_process->spawn_with_pipes(sg_path, sg_args, stdin_fd, stdout_fd)) {
                throw std::runtime_error("Failed to spawn grebe-sg: " + sg_path);
            }
            spdlog::info("IPC mode: spawned grebe-sg PID {}", sg_process->pid());

            pipe_consumer = std::make_unique<PipeConsumer>(stdout_fd, stdin_fd);
            ipc_source = std::make_unique<IpcSource>(*pipe_consumer, opts.num_channels);
            ipc_source->start();
            // IngestionThread launched after dec_thread & app are ready
        }

        // =====================================================================
        // Decimation thread (reads from ring buffers, same for both modes)
        // =====================================================================
        constexpr uint32_t DECIMATE_TARGET = 1920 * 2;
        DecimationMode default_mode = DecimationMode::MinMax;
        DecimationThread dec_thread;
        dec_thread.start(ring_ptrs, DECIMATE_TARGET, default_mode);
        dec_thread.set_sample_rate(1'000'000.0);

        spdlog::info("Streaming started: {}ch, 1 MSPS, decimation={}",
                     opts.num_channels, DecimationThread::mode_name(default_mode));

        ProfileRunner profiler;
        profiler.set_channel_count(opts.num_channels);
        profiler.set_synthetic_source(synthetic_source.get());  // nullptr in IPC mode
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

        // Start IPC ingestion after dec_thread is ready
        if (ipc_source) {
            ingestion.start(*ipc_source, ring_ptrs, drop_ptrs);
        }

        // =====================================================================
        // Assemble AppComponents and run main loop
        // =====================================================================
        AppCommandQueue cmd_queue;
        AppComponents app{};
        app.window = window;
        app.cmd_queue = &cmd_queue;
        app.ctx = &ctx;
        app.swapchain = &swapchain;
        app.renderer = &renderer;
        app.buf_mgr = &buf_mgr;
        app.hud = &hud;
        app.synthetic_source = synthetic_source.get();  // nullptr in IPC mode
        app.ipc_source = ipc_source.get();              // nullptr in embedded mode
        app.ingestion = &ingestion;
        app.dec_thread = &dec_thread;
        app.benchmark = &benchmark;
        app.profiler = &profiler;
        app.drop_counters = drop_ptrs;
        app.num_channels = opts.num_channels;
        app.ring_capacity_samples = opts.ring_size;
        app.enable_profile = opts.enable_profile;
        app.current_sample_rate.store(1e6, std::memory_order_relaxed);
        app.current_paused.store(false, std::memory_order_relaxed);

        run_main_loop(app);

        // =====================================================================
        // Cleanup
        // =====================================================================
        benchmark.stop_logging();

        if (synthetic_source) {
            // Embedded: stop source first (read_frame returns EndOfStream),
            // then join ingestion thread
            synthetic_source->stop();
            ingestion.stop();
        } else if (ipc_source) {
            // IPC: close pipe first to unblock blocking receive_frame(),
            // then join ingestion thread
            pipe_consumer.reset();  // closes pipe fds, read_frame returns EndOfStream
            ingestion.stop();
            ipc_source->stop();
        }

        dec_thread.stop();

        // grebe-sg cleanup: ProcessHandle destructor terminates + waits
        sg_process.reset();

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
