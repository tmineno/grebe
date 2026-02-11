#include "data_generator.h"
#include "file_reader.h"
#include "drop_counter.h"
#include "ring_buffer.h"
#include "ipc/pipe_transport.h"
#include "ipc/contracts.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imfilebrowser.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
    double   frequency_hz = 1'000.0;
    size_t   ring_size    = 67'108'864;  // 64M samples
    uint32_t block_size   = 16384;       // IPC block size (samples/channel/frame)
    std::string file_path;               // --file=PATH: binary file playback
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
        } else if (arg.rfind("--frequency=", 0) == 0) {
            opts.frequency_hz = std::max(1.0, std::stod(arg.substr(12)));
        } else if (arg.rfind("--ring-size=", 0) == 0) {
            std::string val = arg.substr(12);
            size_t sz = std::stoull(val);
            char suffix = val.back();
            if (suffix == 'K' || suffix == 'k') sz *= 1024;
            else if (suffix == 'M' || suffix == 'm') sz *= 1024 * 1024;
            else if (suffix == 'G' || suffix == 'g') sz *= 1024 * 1024 * 1024;
            opts.ring_size = sz;
        } else if (arg.rfind("--block-size=", 0) == 0) {
            opts.block_size = static_cast<uint32_t>(std::stoul(arg.substr(13)));
        } else if (arg.rfind("--file=", 0) == 0) {
            opts.file_path = arg.substr(7);
        }
    }
    return 0;
}

// =========================================================================
// Sender thread: drains ring buffers → sends frames via pipe
// Decoupled from data source: uses atomic<double> for sample rate.
// =========================================================================

static void sender_thread_func(
    std::vector<RingBuffer<int16_t>*>& rings,
    PipeProducer& producer,
    std::atomic<double>& sample_rate_ref,
    std::vector<DropCounter*>& drop_ptrs,
    uint32_t num_channels,
    std::atomic<uint32_t>& block_size_ref,
    std::atomic<bool>& stop_requested)
{
    constexpr uint32_t MAX_BLOCK = 65536;
    std::vector<int16_t> payload(MAX_BLOCK * num_channels);
    std::vector<int16_t> channel_buf(MAX_BLOCK);
    uint64_t sequence = 0;
    uint64_t total_samples_sent = 0;

    while (!stop_requested.load(std::memory_order_relaxed)) {
        uint32_t block_size = block_size_ref.load(std::memory_order_relaxed);

        // Check if all channels have enough data
        bool all_ready = true;
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            if (rings[ch]->size() < block_size) {
                all_ready = false;
                break;
            }
        }

        if (!all_ready) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // Drain block_size from each channel and pack channel-major
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            size_t popped = rings[ch]->pop_bulk(channel_buf.data(), block_size);
            size_t offset = static_cast<size_t>(ch) * block_size;
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
        header.block_length_samples = block_size;
        header.payload_bytes = num_channels * block_size * sizeof(int16_t);
        header.sample_rate_hz = sample_rate_ref.load(std::memory_order_relaxed);

        // Accumulate SG-side drops from all channels
        uint64_t sg_drops = 0;
        for (auto* dc : drop_ptrs) {
            sg_drops += dc->total_dropped();
        }
        header.sg_drops_total = sg_drops;
        header.first_sample_index = total_samples_sent;
        total_samples_sent += block_size;

        if (!producer.send_frame(header, payload.data())) {
            spdlog::info("grebe-sg: pipe closed, stopping sender");
            stop_requested.store(true, std::memory_order_relaxed);
            break;
        }
    }
}

// =========================================================================
// Command reader thread: reads IPC commands from stdin
// Decoupled from data source: uses atomics for pause/sample-rate control.
// =========================================================================

static void command_reader_func(
    PipeProducer& producer,
    std::atomic<double>& cmd_sample_rate,
    std::atomic<bool>& cmd_toggle_paused,
    std::atomic<bool>& stop_requested)
{
    while (!stop_requested.load(std::memory_order_relaxed)) {
        IpcCommand cmd{};
        if (producer.receive_command(cmd)) {
            switch (static_cast<IpcCommand::Type>(cmd.type)) {
            case IpcCommand::SET_SAMPLE_RATE:
                spdlog::info("grebe-sg: set sample rate to {:.0f}", cmd.value);
                cmd_sample_rate.store(cmd.value, std::memory_order_relaxed);
                break;
            case IpcCommand::TOGGLE_PAUSED:
                cmd_toggle_paused.store(true, std::memory_order_relaxed);
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
// Source mode enum
// =========================================================================

enum class SourceMode { Synthetic, File };

// =========================================================================
// Main
// =========================================================================

int main(int argc, char* argv[]) {
    // CRITICAL: redirect spdlog to stderr — stdout is the IPC data pipe
    auto stderr_logger = spdlog::stderr_color_mt("grebe-sg");
    spdlog::set_default_logger(stderr_logger);
    spdlog::set_pattern("[grebe-sg] [%H:%M:%S.%e] [%l] %v");

    SgOptions opts;
    if (int rc = parse_sg_cli(argc, argv, opts); rc != 0) return rc;

    // If --file specified, validate and get channel count from file
    std::unique_ptr<FileReader> file_reader;
    SourceMode source_mode = SourceMode::Synthetic;

    if (!opts.file_path.empty()) {
        try {
            file_reader = std::make_unique<FileReader>(opts.file_path);
            if (file_reader->channel_count() != opts.num_channels) {
                spdlog::warn("File has {}ch, overriding --channels={}",
                             file_reader->channel_count(), opts.num_channels);
                opts.num_channels = file_reader->channel_count();
            }
            source_mode = SourceMode::File;
        } catch (const std::exception& e) {
            spdlog::error("Failed to open file: {}", e.what());
            return 1;
        }
    }

    spdlog::info("Starting: {}ch, {:.0f} SPS, {:.2f} Hz, ring={}, block={}, source={}",
                 opts.num_channels, opts.sample_rate, opts.frequency_hz,
                 opts.ring_size, opts.block_size,
                 source_mode == SourceMode::File ? "file" : "synthetic");

    // Init GLFW + OpenGL (optional — headless if window creation fails)
    bool has_window = false;
    GLFWwindow* window = nullptr;

    if (glfwInit()) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

        window = glfwCreateWindow(400, 600, "grebe-sg", nullptr, nullptr);
        if (window) {
            glfwMakeContextCurrent(window);
            glfwSwapInterval(1);

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::StyleColorsDark();
            ImGui_ImplGlfw_InitForOpenGL(window, true);
            ImGui_ImplOpenGL3_Init("#version 330");
            has_window = true;
            spdlog::info("GUI window initialized");
        } else {
            spdlog::warn("Window creation failed, running headless");
            glfwTerminate();
        }
    } else {
        spdlog::warn("GLFW init failed, running headless");
    }

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

    // Shared atomic for sample rate (used by sender thread)
    std::atomic<double> current_sample_rate{opts.sample_rate};

    // Start data source
    DataGenerator data_gen;
    data_gen.set_drop_counters(drop_ptrs);
    data_gen.set_frequency(opts.frequency_hz);

    if (source_mode == SourceMode::Synthetic) {
        data_gen.start(ring_ptrs, opts.sample_rate, WaveformType::Sine);
        current_sample_rate.store(opts.sample_rate, std::memory_order_relaxed);
    } else {
        file_reader->start(ring_ptrs, drop_ptrs);
        current_sample_rate.store(file_reader->target_sample_rate(), std::memory_order_relaxed);
    }

    // IPC producer (writes to stdout, reads from stdin)
    PipeProducer producer;

    // Start threads
    std::atomic<bool> stop_requested{false};
    std::atomic<uint32_t> block_size{opts.block_size};

    // Command atomics (decoupled from data source)
    std::atomic<double> cmd_sample_rate{0.0};  // 0 = no pending command
    std::atomic<bool> cmd_toggle_paused{false};

    std::thread sender(sender_thread_func,
                       std::ref(ring_ptrs), std::ref(producer),
                       std::ref(current_sample_rate), std::ref(drop_ptrs),
                       opts.num_channels, std::ref(block_size),
                       std::ref(stop_requested));

    std::thread cmd_reader(command_reader_func,
                           std::ref(producer),
                           std::ref(cmd_sample_rate),
                           std::ref(cmd_toggle_paused),
                           std::ref(stop_requested));

    // UI state
    const double rate_options[] = {1e6, 10e6, 100e6, 1e9};
    const char* rate_labels[] = {"1 MSPS", "10 MSPS", "100 MSPS", "1 GSPS"};
    int rate_sel = 0;

    const uint32_t block_options[] = {1024, 2048, 4096, 8192, 16384, 65536};
    const char* block_labels[] = {"1024", "2048", "4096", "8192", "16384", "65536"};
    int block_sel = 4;
    for (int i = 0; i < 6; i++) {
        if (block_options[i] == opts.block_size) { block_sel = i; break; }
    }

    const char* waveform_labels[] = {"Sine", "Square", "Sawtooth", "WhiteNoise", "Chirp"};
    const WaveformType waveform_values[] = {
        WaveformType::Sine, WaveformType::Square, WaveformType::Sawtooth,
        WaveformType::WhiteNoise, WaveformType::Chirp
    };
    std::vector<int> ch_waveform_sel(opts.num_channels, 0);
    double frequency_hz = opts.frequency_hz;

    // File source GUI state
    int source_sel = (source_mode == SourceMode::File) ? 1 : 0;
    char file_path_buf[512] = {};
    if (!opts.file_path.empty()) {
        std::strncpy(file_path_buf, opts.file_path.c_str(), sizeof(file_path_buf) - 1);
    }
    std::string file_error_msg;
    bool file_loop = true;

    // ImGui file browser dialog
    ImGui::FileBrowser file_dialog;
    file_dialog.SetTitle("Open GRB File");
    file_dialog.SetTypeFilters({".grb"});

    // Helper: flush all ring buffers
    auto flush_rings = [&]() {
        for (auto* rb : ring_ptrs) {
            rb->discard_bulk(rb->capacity());
        }
    };

    // Main loop
    if (has_window) {
        while (!glfwWindowShouldClose(window) &&
               !stop_requested.load(std::memory_order_relaxed)) {
            glfwPollEvents();

            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                break;
            }

            // Process IPC commands (from command reader thread)
            {
                double new_rate = cmd_sample_rate.exchange(0.0, std::memory_order_relaxed);
                if (new_rate > 0.0 && source_mode == SourceMode::Synthetic) {
                    data_gen.set_sample_rate(new_rate);
                    current_sample_rate.store(new_rate, std::memory_order_relaxed);
                    // Update rate_sel to match
                    for (int i = 0; i < 4; i++) {
                        if (rate_options[i] == new_rate) { rate_sel = i; break; }
                    }
                }
                if (cmd_toggle_paused.exchange(false, std::memory_order_relaxed)) {
                    if (source_mode == SourceMode::Synthetic) {
                        data_gen.set_paused(!data_gen.is_paused());
                        spdlog::info("grebe-sg: paused={}", data_gen.is_paused());
                    } else if (file_reader) {
                        file_reader->set_paused(!file_reader->is_paused());
                        spdlog::info("grebe-sg: paused={}", file_reader->is_paused());
                    }
                }
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("grebe-sg", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

            ImGui::Text("grebe-sg Signal Generator");
            ImGui::Separator();
            ImGui::Spacing();

            // --- Source selector ---
            ImGui::Text("Source");
            const char* source_labels[] = {"Synthetic", "File"};
            int prev_source_sel = source_sel;
            ImGui::Combo("##source", &source_sel, source_labels, 2);

            if (source_sel != prev_source_sel) {
                file_error_msg.clear();
                if (source_sel == 0 && source_mode == SourceMode::File) {
                    // Switch: File → Synthetic
                    if (file_reader) {
                        file_reader->stop();
                        file_reader.reset();
                    }
                    flush_rings();
                    data_gen.start(ring_ptrs, rate_options[rate_sel], WaveformType::Sine);
                    current_sample_rate.store(rate_options[rate_sel], std::memory_order_relaxed);
                    source_mode = SourceMode::Synthetic;
                    spdlog::info("Switched to Synthetic source");
                } else if (source_sel == 1 && source_mode == SourceMode::Synthetic) {
                    // Switch: Synthetic → File — show file UI, wait for Load button
                    // source_mode stays Synthetic until a file is successfully loaded
                }
            }
            ImGui::Spacing();

            if (source_sel == 0) {
                // ============================================================
                // Synthetic mode controls
                // ============================================================

                // --- Sample Rate ---
                ImGui::Text("Sample Rate");
                for (int i = 0; i < 4; i++) {
                    if (ImGui::RadioButton(rate_labels[i], &rate_sel, i)) {
                        data_gen.set_sample_rate(rate_options[rate_sel]);
                        current_sample_rate.store(rate_options[rate_sel], std::memory_order_relaxed);
                        spdlog::info("Sample rate -> {}", rate_labels[rate_sel]);
                    }
                    if (i % 2 == 0) ImGui::SameLine();
                }
                ImGui::Spacing();

                // --- Block Length ---
                ImGui::Text("Block Length");
                if (ImGui::Combo("##block", &block_sel, block_labels, 6)) {
                    block_size.store(block_options[block_sel], std::memory_order_relaxed);
                    spdlog::info("Block size -> {}", block_options[block_sel]);
                }
                ImGui::Spacing();

                // --- Per-channel Waveforms ---
                ImGui::Text("Waveforms");
                for (uint32_t ch = 0; ch < opts.num_channels; ch++) {
                    char label[32];
                    std::snprintf(label, sizeof(label), "Ch %u##wf%u", ch + 1, ch);
                    if (ImGui::Combo(label, &ch_waveform_sel[ch], waveform_labels, 5)) {
                        data_gen.set_channel_waveform(ch, waveform_values[ch_waveform_sel[ch]]);
                        spdlog::info("Ch {} waveform -> {}", ch + 1,
                                     waveform_labels[ch_waveform_sel[ch]]);
                    }
                }
                ImGui::Spacing();

                // --- Periodic waveform frequency ---
                bool any_periodic = false;
                for (uint32_t ch = 0; ch < opts.num_channels; ch++) {
                    WaveformType wf = waveform_values[ch_waveform_sel[ch]];
                    if (wf == WaveformType::Sine || wf == WaveformType::Square ||
                        wf == WaveformType::Sawtooth || wf == WaveformType::Chirp) {
                        any_periodic = true;
                        break;
                    }
                }
                if (any_periodic) {
                    ImGui::Text("Frequency (Hz)");
                    if (ImGui::InputDouble("##freq_hz", &frequency_hz, 1.0, 100.0, "%.2f")) {
                        if (frequency_hz < 1.0) frequency_hz = 1.0;
                        data_gen.set_frequency(frequency_hz);
                        spdlog::info("Frequency -> {:.2f} Hz", frequency_hz);
                    }
                } else {
                    ImGui::TextDisabled("Frequency (Hz): N/A (noise)");
                }
                ImGui::Spacing();

                // --- Pause / Resume ---
                bool paused = data_gen.is_paused();
                if (ImGui::Button(paused ? "Resume" : "Pause", ImVec2(120, 0))) {
                    data_gen.set_paused(!paused);
                    spdlog::info(paused ? "Resumed" : "Paused");
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // --- Status ---
                ImGui::Text("Status");
                double actual_rate = data_gen.actual_sample_rate();
                const char* suffix = "SPS";
                double display_rate = actual_rate;
                if (display_rate >= 1e9)      { display_rate /= 1e9; suffix = "GSPS"; }
                else if (display_rate >= 1e6) { display_rate /= 1e6; suffix = "MSPS"; }
                else if (display_rate >= 1e3) { display_rate /= 1e3; suffix = "KSPS"; }
                ImGui::Text("Actual Rate: %.1f %s", display_rate, suffix);
                ImGui::Text("Total Samples: %llu",
                             static_cast<unsigned long long>(data_gen.total_samples_generated()));

            } else {
                // ============================================================
                // File mode controls
                // ============================================================

                // --- File info ---
                if (file_reader) {
                    ImGui::Text("File: %s", file_reader->path().c_str());
                    ImGui::Text("Channels: %u", file_reader->channel_count());

                    double file_rate = file_reader->target_sample_rate();
                    const char* rate_suffix = "SPS";
                    double disp = file_rate;
                    if (disp >= 1e9)      { disp /= 1e9; rate_suffix = "GSPS"; }
                    else if (disp >= 1e6) { disp /= 1e6; rate_suffix = "MSPS"; }
                    else if (disp >= 1e3) { disp /= 1e3; rate_suffix = "KSPS"; }
                    ImGui::Text("Sample Rate: %.1f %s", disp, rate_suffix);

                    uint64_t total = file_reader->total_file_samples();
                    uint64_t read = file_reader->total_samples_read();
                    double progress = (total > 0)
                        ? static_cast<double>(read % total) / static_cast<double>(total)
                        : 0.0;
                    ImGui::ProgressBar(static_cast<float>(progress), ImVec2(-1, 0));
                    ImGui::Text("Samples: %llu / %llu",
                                static_cast<unsigned long long>(read),
                                static_cast<unsigned long long>(total));
                    ImGui::Spacing();

                    // --- Loop toggle ---
                    bool loop = file_reader->is_looping();
                    if (ImGui::Checkbox("Loop", &loop)) {
                        file_reader->set_looping(loop);
                        file_loop = loop;
                    }
                    ImGui::Spacing();

                    // --- Pause / Resume ---
                    bool paused = file_reader->is_paused();
                    if (ImGui::Button(paused ? "Resume" : "Pause", ImVec2(120, 0))) {
                        file_reader->set_paused(!paused);
                        spdlog::info(paused ? "Resumed" : "Paused");
                    }
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // --- Status ---
                    ImGui::Text("Status");
                    double actual_rate = file_reader->actual_sample_rate();
                    const char* suffix = "SPS";
                    double display_rate = actual_rate;
                    if (display_rate >= 1e9)      { display_rate /= 1e9; suffix = "GSPS"; }
                    else if (display_rate >= 1e6) { display_rate /= 1e6; suffix = "MSPS"; }
                    else if (display_rate >= 1e3) { display_rate /= 1e3; suffix = "KSPS"; }
                    ImGui::Text("Actual Rate: %.1f %s", display_rate, suffix);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // --- Load new file ---
                if (ImGui::Button("Open File...", ImVec2(-1, 0))) {
                    file_dialog.Open();
                }
                if (!file_error_msg.empty()) {
                    ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", file_error_msg.c_str());
                }
            }

            // --- Common: Drops (both modes) ---
            uint64_t total_drops = 0;
            for (auto* dc : drop_ptrs) {
                total_drops += dc->total_dropped();
            }
            if (total_drops > 0) {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Drops: %llu",
                                   static_cast<unsigned long long>(total_drops));
            } else {
                ImGui::Text("Drops: 0");
            }

            ImGui::End();

            // File browser dialog (rendered as separate popup window)
            file_dialog.Display();
            if (file_dialog.HasSelected()) {
                std::string new_path = file_dialog.GetSelected().string();
                file_dialog.ClearSelected();
                spdlog::info("File selected: {}", new_path);
                file_error_msg.clear();
                try {
                    auto new_reader = std::make_unique<FileReader>(new_path);
                    if (new_reader->channel_count() != opts.num_channels) {
                        file_error_msg = "Channel mismatch: file has " +
                            std::to_string(new_reader->channel_count()) +
                            "ch, expected " + std::to_string(opts.num_channels);
                        spdlog::error("{}", file_error_msg);
                    } else {
                        // Stop current source
                        if (source_mode == SourceMode::Synthetic) {
                            data_gen.stop();
                        } else if (file_reader) {
                            file_reader->stop();
                        }
                        flush_rings();

                        file_reader = std::move(new_reader);
                        file_reader->set_looping(file_loop);
                        file_reader->start(ring_ptrs, drop_ptrs);
                        current_sample_rate.store(
                            file_reader->target_sample_rate(),
                            std::memory_order_relaxed);
                        source_mode = SourceMode::File;
                        source_sel = 1;
                        spdlog::info("Loaded file: {} ({}ch, {:.0f} SPS)",
                                     new_path, file_reader->channel_count(),
                                     file_reader->target_sample_rate());
                    }
                } catch (const std::exception& e) {
                    file_error_msg = e.what();
                    spdlog::error("Failed to load file: {}", file_error_msg);
                }
            }

            ImGui::Render();

            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
    } else {
        // Headless: just wait for stop signal
        while (!stop_requested.load(std::memory_order_relaxed)) {
            // Process IPC commands in headless mode too
            double new_rate = cmd_sample_rate.exchange(0.0, std::memory_order_relaxed);
            if (new_rate > 0.0 && source_mode == SourceMode::Synthetic) {
                data_gen.set_sample_rate(new_rate);
                current_sample_rate.store(new_rate, std::memory_order_relaxed);
            }
            if (cmd_toggle_paused.exchange(false, std::memory_order_relaxed)) {
                if (source_mode == SourceMode::Synthetic) {
                    data_gen.set_paused(!data_gen.is_paused());
                } else if (file_reader) {
                    file_reader->set_paused(!file_reader->is_paused());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Cleanup
    stop_requested.store(true, std::memory_order_relaxed);
    if (source_mode == SourceMode::Synthetic) {
        data_gen.stop();
    } else if (file_reader) {
        file_reader->stop();
    }

    if (sender.joinable()) sender.join();
    if (cmd_reader.joinable()) cmd_reader.join();

    if (has_window) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }

    spdlog::info("Clean shutdown");
    return 0;
}
