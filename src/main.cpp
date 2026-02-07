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
#include "process_handle.h"
#include "pipe_transport.h"
#include "contracts.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// IPC receiver thread: reads frames from pipe and pushes to local ring buffers.
// Updates sample rate tracking so grebe's decimation thread and HUD stay in sync.
static void ipc_receiver_func(ITransportConsumer& consumer,
                               std::vector<RingBuffer<int16_t>*>& rings,
                               uint32_t num_channels,
                               std::atomic<bool>& stop,
                               std::atomic<double>& sample_rate_out,
                               DecimationThread& dec_thread) {
    FrameHeaderV2 hdr{};
    std::vector<int16_t> payload;
    double last_rate = 0.0;

    while (!stop.load(std::memory_order_relaxed)) {
        if (!consumer.receive_frame(hdr, payload)) {
            spdlog::info("IPC receiver: pipe closed");
            break;
        }

        // Sync sample rate from grebe-sg
        if (hdr.sample_rate_hz > 0.0 && hdr.sample_rate_hz != last_rate) {
            last_rate = hdr.sample_rate_hz;
            sample_rate_out.store(last_rate, std::memory_order_relaxed);
            dec_thread.set_sample_rate(last_rate);
        }

        uint32_t ch_count = std::min(hdr.channel_count, num_channels);
        for (uint32_t ch = 0; ch < ch_count; ch++) {
            size_t offset = static_cast<size_t>(ch) * hdr.block_length_samples;
            if (offset + hdr.block_length_samples <= payload.size()) {
                rings[ch]->push_bulk(payload.data() + offset, hdr.block_length_samples);
            }
        }
    }
}

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
        // Data source: embedded DataGenerator or IPC from grebe-sg
        // =====================================================================
        std::unique_ptr<DataGenerator> data_gen;
        std::unique_ptr<ProcessHandle> sg_process;
        std::unique_ptr<PipeConsumer> pipe_consumer;
        std::atomic<bool> ipc_receiver_stop{false};
        std::thread ipc_receiver_thread;

        if (opts.embedded) {
            // Embedded mode: DataGenerator in-process (Phase 7 behavior)
            data_gen = std::make_unique<DataGenerator>();
            data_gen->set_drop_counters(drop_ptrs);
            data_gen->start(ring_ptrs, 1'000'000.0, WaveformType::Sine);
            spdlog::info("Embedded mode: DataGenerator in-process");
        } else {
            // IPC mode: spawn grebe-sg
            std::string sg_path = find_sg_binary(argv[0]);
            std::vector<std::string> sg_args;
            sg_args.push_back("--channels=" + std::to_string(opts.num_channels));
            if (opts.ring_size != 16'777'216) {
                sg_args.push_back("--ring-size=" + std::to_string(opts.ring_size));
            }

            sg_process = std::make_unique<ProcessHandle>();
            int stdin_fd = -1, stdout_fd = -1;
            if (!sg_process->spawn_with_pipes(sg_path, sg_args, stdin_fd, stdout_fd)) {
                throw std::runtime_error("Failed to spawn grebe-sg: " + sg_path);
            }
            spdlog::info("IPC mode: spawned grebe-sg PID {}", sg_process->pid());

            pipe_consumer = std::make_unique<PipeConsumer>(stdout_fd, stdin_fd);
            // Receiver thread launched after dec_thread & app are ready
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
        app.data_gen = data_gen.get();  // nullptr in IPC mode
        app.dec_thread = &dec_thread;
        app.benchmark = &benchmark;
        app.profiler = &profiler;
        app.drop_counters = drop_ptrs;
        app.num_channels = opts.num_channels;
        app.enable_profile = opts.enable_profile;
        app.transport = pipe_consumer.get();  // nullptr in embedded mode
        app.current_sample_rate.store(1e6, std::memory_order_relaxed);
        app.current_paused.store(false, std::memory_order_relaxed);

        // Start IPC receiver after dec_thread and app are ready
        if (pipe_consumer) {
            ipc_receiver_thread = std::thread(ipc_receiver_func,
                                              std::ref(*pipe_consumer),
                                              std::ref(ring_ptrs),
                                              opts.num_channels,
                                              std::ref(ipc_receiver_stop),
                                              std::ref(app.current_sample_rate),
                                              std::ref(dec_thread));
        }

        run_main_loop(app);

        // =====================================================================
        // Cleanup
        // =====================================================================
        benchmark.stop_logging();
        dec_thread.stop();

        if (data_gen) {
            data_gen->stop();
        }

        // Stop IPC receiver
        ipc_receiver_stop.store(true, std::memory_order_relaxed);
        pipe_consumer.reset();  // closes pipe fds, unblocks receiver read
        if (ipc_receiver_thread.joinable()) {
            ipc_receiver_thread.join();
        }

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
