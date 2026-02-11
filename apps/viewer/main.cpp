#include "app_command.h"
#include "app_loop.h"
#include "cli.h"
#include "drop_counter.h"
#include "vulkan_renderer.h"
#include "synthetic_source.h"
#include "transport_source.h"
#include "ingestion_thread.h"
#include "ring_buffer.h"
#include "grebe/config.h"
#include "benchmark.h"
#include "hud.h"
#include "profiler.h"
#include "microbench.h"
#include "process_handle.h"
#include "ipc/pipe_transport.h"
#include "ipc/udp_transport.h"
#include "ipc/contracts.h"

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

        // Init Vulkan rendering backend
        VulkanRenderer vk_renderer;
        vk_renderer.initialize(window, SHADER_DIR);

        // Build pipeline configuration from CLI options
        grebe::PipelineConfig pipeline_config;
        pipeline_config.channel_count = opts.num_channels;
        pipeline_config.ring_buffer_size = opts.ring_size;
        pipeline_config.decimation.target_points = 1920 * 2;
        pipeline_config.decimation.algorithm = grebe::DecimationAlgorithm::MinMax;
        pipeline_config.decimation.sample_rate = 1'000'000.0;
        pipeline_config.vsync = !opts.no_vsync;

        // Apply V-Sync from config
        if (!pipeline_config.vsync) {
            vk_renderer.set_vsync(false);
            spdlog::info("V-Sync disabled via --no-vsync");
        }

        // Bench mode: run microbenchmarks and exit (no grebe-sg needed)
        if (opts.enable_bench) {
            vk_renderer.set_vsync(false);

            int bench_code = run_microbenchmarks(
                vk_renderer.vulkan_context(),
                vk_renderer.swapchain_obj(),
                vk_renderer.buffer_manager(),
                vk_renderer.renderer_obj(),
                SHADER_DIR);

            vk_renderer.shutdown();
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
        hud.init(window, vk_renderer.vulkan_context(),
                 vk_renderer.render_pass(), vk_renderer.image_count());

        // =====================================================================
        // Streaming pipeline: ring buffers (used by both modes)
        // =====================================================================
        std::vector<std::unique_ptr<RingBuffer<int16_t>>> ring_buffers;
        std::vector<RingBuffer<int16_t>*> ring_ptrs;
        for (uint32_t ch = 0; ch < pipeline_config.channel_count; ch++) {
            ring_buffers.push_back(
                std::make_unique<RingBuffer<int16_t>>(pipeline_config.ring_buffer_size + 1));
            ring_ptrs.push_back(ring_buffers.back().get());
        }
        spdlog::info("Ring buffers: {}ch x {} samples ({} MB each)",
                     pipeline_config.channel_count, pipeline_config.ring_buffer_size,
                     pipeline_config.ring_buffer_size * 2 / (1024 * 1024));

        // Per-channel drop counters
        std::vector<std::unique_ptr<DropCounter>> drop_counters;
        std::vector<DropCounter*> drop_ptrs;
        for (uint32_t ch = 0; ch < pipeline_config.channel_count; ch++) {
            drop_counters.push_back(std::make_unique<DropCounter>());
            drop_ptrs.push_back(drop_counters.back().get());
        }

        // =====================================================================
        // Data source: SyntheticSource / TransportSource (pipe or UDP)
        // =====================================================================
        std::unique_ptr<SyntheticSource> synthetic_source;
        std::unique_ptr<TransportSource> transport_source;
        std::unique_ptr<ProcessHandle> sg_process;
        std::unique_ptr<PipeConsumer> pipe_consumer;
        std::unique_ptr<UdpConsumer> udp_consumer;
        IngestionThread ingestion;

        if (opts.udp_port > 0) {
            // UDP mode: receive from external grebe-sg, no subprocess
            udp_consumer = std::make_unique<UdpConsumer>(opts.udp_port);
            transport_source = std::make_unique<TransportSource>(
                *udp_consumer, pipeline_config.channel_count);
            transport_source->start();
            spdlog::info("UDP mode: listening on port {}", opts.udp_port);
        } else if (opts.embedded) {
            synthetic_source = std::make_unique<SyntheticSource>(
                pipeline_config.channel_count, 1'000'000.0, WaveformType::Sine);
            synthetic_source->start();
            ingestion.start(*synthetic_source, ring_ptrs, drop_ptrs);
            spdlog::info("Embedded mode: SyntheticSource + IngestionThread");
        } else {
            std::string sg_path = find_sg_binary(argv[0]);
            std::vector<std::string> sg_args;
            sg_args.push_back("--channels=" + std::to_string(pipeline_config.channel_count));
            sg_args.push_back("--block-size=" + std::to_string(opts.block_size));
            if (pipeline_config.ring_buffer_size != 67'108'864) {
                sg_args.push_back("--ring-size=" + std::to_string(pipeline_config.ring_buffer_size));
            }
            if (!opts.file_path.empty()) {
                sg_args.push_back("--file=" + opts.file_path);
            }

            sg_process = std::make_unique<ProcessHandle>();
            int stdin_fd = -1, stdout_fd = -1;
            if (!sg_process->spawn_with_pipes(sg_path, sg_args, stdin_fd, stdout_fd)) {
                throw std::runtime_error("Failed to spawn grebe-sg: " + sg_path);
            }
            spdlog::info("IPC mode: spawned grebe-sg PID {}", sg_process->pid());

            pipe_consumer = std::make_unique<PipeConsumer>(stdout_fd, stdin_fd);
            transport_source = std::make_unique<TransportSource>(
                *pipe_consumer, pipeline_config.channel_count);
            transport_source->start();
        }

        // =====================================================================
        // Decimation engine (reads from ring buffers, same for both modes)
        // =====================================================================
        grebe::DecimationEngine dec_engine;
        dec_engine.start(ring_ptrs, pipeline_config.decimation);

        spdlog::info("Streaming started: {}ch, 1 MSPS, decimation={}",
                     pipeline_config.channel_count,
                     grebe::DecimationEngine::algorithm_name(pipeline_config.decimation.algorithm));

        ProfileRunner profiler;
        profiler.set_channel_count(pipeline_config.channel_count);
        profiler.set_synthetic_source(synthetic_source.get());
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

        // Start IPC ingestion after dec_engine is ready
        if (transport_source) {
            ingestion.start(*transport_source, ring_ptrs, drop_ptrs);
        }

        // =====================================================================
        // Assemble AppComponents and run main loop
        // =====================================================================
        AppCommandQueue cmd_queue;
        AppComponents app{};
        app.window = window;
        app.cmd_queue = &cmd_queue;
        app.render_backend = &vk_renderer;
        app.hud = &hud;
        app.synthetic_source = synthetic_source.get();
        app.transport_source = transport_source.get();
        app.ingestion = &ingestion;
        app.dec_engine = &dec_engine;
        app.benchmark = &benchmark;
        app.profiler = &profiler;
        app.drop_counters = drop_ptrs;
        app.num_channels = pipeline_config.channel_count;
        app.ring_capacity_samples = pipeline_config.ring_buffer_size;
        app.enable_profile = opts.enable_profile;
        app.current_sample_rate.store(1e6, std::memory_order_relaxed);
        app.current_paused.store(false, std::memory_order_relaxed);

        run_main_loop(app);

        // =====================================================================
        // Cleanup
        // =====================================================================
        benchmark.stop_logging();

        if (synthetic_source) {
            synthetic_source->stop();
            ingestion.stop();
        } else if (transport_source) {
            if (pipe_consumer) pipe_consumer.reset();
            if (udp_consumer) udp_consumer.reset();
            ingestion.stop();
            transport_source->stop();
        }

        dec_engine.stop();
        sg_process.reset();

        int exit_code = 0;
        if (opts.enable_profile) {
            exit_code = profiler.generate_report();
        }

        hud.destroy();
        vk_renderer.shutdown();

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
