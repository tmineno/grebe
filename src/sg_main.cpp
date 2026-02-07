#include "data_generator.h"
#include "drop_counter.h"
#include "ring_buffer.h"
#include "pipe_transport.h"
#include "contracts.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// =========================================================================
// CLI parsing (minimal, grebe-sg specific)
// =========================================================================

struct SgOptions {
    uint32_t num_channels = 1;
    double   sample_rate  = 1'000'000.0;
    size_t   ring_size    = 16'777'216;  // 16M samples
};

static int parse_sg_cli(int argc, char* argv[], SgOptions& opts) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--channels=", 0) == 0) {
            int n = std::stoi(arg.substr(11));
            if (n < 1 || n > 8) {
                spdlog::error("--channels must be 1-8");
                return 1;
            }
            opts.num_channels = static_cast<uint32_t>(n);
        } else if (arg.rfind("--sample-rate=", 0) == 0) {
            opts.sample_rate = std::stod(arg.substr(14));
        } else if (arg.rfind("--ring-size=", 0) == 0) {
            std::string val = arg.substr(12);
            size_t sz = std::stoull(val);
            char suffix = val.back();
            if (suffix == 'K' || suffix == 'k') sz *= 1024;
            else if (suffix == 'M' || suffix == 'm') sz *= 1024 * 1024;
            else if (suffix == 'G' || suffix == 'g') sz *= 1024 * 1024 * 1024;
            opts.ring_size = sz;
        }
    }
    return 0;
}

// =========================================================================
// Sender thread: drains ring buffers → sends frames via pipe
// =========================================================================

static void sender_thread_func(
    std::vector<RingBuffer<int16_t>*>& rings,
    PipeProducer& producer,
    uint32_t num_channels,
    std::atomic<bool>& stop_requested)
{
    constexpr uint32_t BLOCK_SIZE = 4096;
    std::vector<int16_t> payload(BLOCK_SIZE * num_channels);
    std::vector<int16_t> channel_buf(BLOCK_SIZE);
    uint64_t sequence = 0;

    while (!stop_requested.load(std::memory_order_relaxed)) {
        // Check if all channels have enough data
        bool all_ready = true;
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            if (rings[ch]->size() < BLOCK_SIZE) {
                all_ready = false;
                break;
            }
        }

        if (!all_ready) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // Drain BLOCK_SIZE from each channel and pack channel-major
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            size_t popped = rings[ch]->pop_bulk(channel_buf.data(), BLOCK_SIZE);
            size_t offset = static_cast<size_t>(ch) * BLOCK_SIZE;
            std::memcpy(payload.data() + offset, channel_buf.data(), popped * sizeof(int16_t));
        }

        // Build frame header
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();

        FrameHeaderV2 header{};
        header.sequence = sequence++;
        header.producer_ts_ns = static_cast<uint64_t>(ns);
        header.channel_count = num_channels;
        header.block_length_samples = BLOCK_SIZE;
        header.payload_bytes = num_channels * BLOCK_SIZE * sizeof(int16_t);

        if (!producer.send_frame(header, payload.data())) {
            spdlog::info("grebe-sg: pipe closed, stopping sender");
            stop_requested.store(true, std::memory_order_relaxed);
            break;
        }
    }
}

// =========================================================================
// Command reader thread: reads IPC commands from stdin
// =========================================================================

static void command_reader_func(
    PipeProducer& producer,
    DataGenerator& data_gen,
    std::atomic<bool>& stop_requested)
{
    while (!stop_requested.load(std::memory_order_relaxed)) {
        IpcCommand cmd{};
        if (producer.receive_command(cmd)) {
            switch (static_cast<IpcCommand::Type>(cmd.type)) {
            case IpcCommand::SET_SAMPLE_RATE:
                spdlog::info("grebe-sg: set sample rate to {:.0f}", cmd.value);
                data_gen.set_sample_rate(cmd.value);
                break;
            case IpcCommand::TOGGLE_PAUSED:
                data_gen.set_paused(!data_gen.is_paused());
                spdlog::info("grebe-sg: paused={}", data_gen.is_paused());
                break;
            case IpcCommand::QUIT:
                spdlog::info("grebe-sg: quit command received");
                stop_requested.store(true, std::memory_order_relaxed);
                break;
            default:
                spdlog::warn("grebe-sg: unknown command type {}", cmd.type);
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

// =========================================================================
// Main
// =========================================================================

int main(int argc, char* argv[]) {
    spdlog::set_pattern("[grebe-sg] [%H:%M:%S.%e] [%l] %v");

    SgOptions opts;
    if (int rc = parse_sg_cli(argc, argv, opts); rc != 0) return rc;

    spdlog::info("Starting: {}ch, {:.0f} SPS, ring={}",
                 opts.num_channels, opts.sample_rate, opts.ring_size);

    // Init GLFW + OpenGL
    if (!glfwInit()) {
        spdlog::error("Failed to initialize GLFW");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(480, 320, "grebe-sg", nullptr, nullptr);
    if (!window) {
        spdlog::error("Failed to create GLFW window");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // V-Sync on for SG window (low priority rendering)

    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Create ring buffers
    std::vector<std::unique_ptr<RingBuffer<int16_t>>> ring_buffers;
    std::vector<RingBuffer<int16_t>*> ring_ptrs;
    for (uint32_t ch = 0; ch < opts.num_channels; ch++) {
        ring_buffers.push_back(
            std::make_unique<RingBuffer<int16_t>>(opts.ring_size + 1));
        ring_ptrs.push_back(ring_buffers.back().get());
    }

    // Create drop counters
    std::vector<std::unique_ptr<DropCounter>> drop_counters;
    std::vector<DropCounter*> drop_ptrs;
    for (uint32_t ch = 0; ch < opts.num_channels; ch++) {
        drop_counters.push_back(std::make_unique<DropCounter>());
        drop_ptrs.push_back(drop_counters.back().get());
    }

    // Start data generator
    DataGenerator data_gen;
    data_gen.set_drop_counters(drop_ptrs);
    data_gen.start(ring_ptrs, opts.sample_rate, WaveformType::Sine);

    // IPC producer (writes to stdout, reads from stdin)
    PipeProducer producer;

    // Start threads
    std::atomic<bool> stop_requested{false};

    std::thread sender(sender_thread_func,
                       std::ref(ring_ptrs), std::ref(producer),
                       opts.num_channels, std::ref(stop_requested));

    std::thread cmd_reader(command_reader_func,
                           std::ref(producer), std::ref(data_gen),
                           std::ref(stop_requested));

    // Main loop (ImGui status display)
    while (!glfwWindowShouldClose(window) &&
           !stop_requested.load(std::memory_order_relaxed)) {
        glfwPollEvents();

        // Check for Esc key
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            break;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Status window
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("grebe-sg", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        ImGui::Text("grebe-sg Signal Generator");
        ImGui::Separator();

        double rate = data_gen.actual_sample_rate();
        const char* suffix = "SPS";
        if (rate >= 1e9)      { rate /= 1e9; suffix = "GSPS"; }
        else if (rate >= 1e6) { rate /= 1e6; suffix = "MSPS"; }
        else if (rate >= 1e3) { rate /= 1e3; suffix = "KSPS"; }

        ImGui::Text("Channels: %u", opts.num_channels);
        ImGui::Text("Sample Rate: %.1f %s (target: %.0f)", rate, suffix,
                     data_gen.target_sample_rate());
        ImGui::Text("Waveform: Sine");
        ImGui::Text("Paused: %s", data_gen.is_paused() ? "YES" : "NO");
        ImGui::Text("Total Samples: %llu",
                     static_cast<unsigned long long>(data_gen.total_samples_generated()));

        // Drop stats
        uint64_t total_drops = 0;
        for (auto* dc : drop_ptrs) {
            total_drops += dc->total_dropped();
        }
        if (total_drops > 0) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Drops: %llu",
                               static_cast<unsigned long long>(total_drops));
        }

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Cleanup
    stop_requested.store(true, std::memory_order_relaxed);
    data_gen.stop();

    if (sender.joinable()) sender.join();
    if (cmd_reader.joinable()) cmd_reader.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    spdlog::info("Clean shutdown");
    return 0;
}
