#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

struct GLFWwindow;
class AppCommandQueue;
class VulkanRenderer;
class Hud;
class SyntheticSource;
class TransportSource;
namespace grebe {
    class LinearRuntime;
    class DecimationStage;
    class VisualizationStage;
}
class Benchmark;
class ProfileRunner;

struct AppComponents {
    GLFWwindow* window;
    AppCommandQueue* cmd_queue;
    VulkanRenderer* render_backend;
    Hud* hud;
    SyntheticSource* synthetic_source = nullptr;  // non-null in embedded mode
    TransportSource* transport_source = nullptr;   // non-null in IPC/UDP mode
    grebe::LinearRuntime* runtime = nullptr;
    grebe::DecimationStage* dec_stage = nullptr;   // direct control (mode, rate)
    grebe::VisualizationStage* viz_stage = nullptr;  // display windowing + decimation
    Benchmark* benchmark;
    ProfileRunner* profiler;
    uint32_t num_channels;
    bool enable_profile;
    std::atomic<double> current_sample_rate{1e6};
    std::atomic<bool>   current_paused{false};
};

void run_main_loop(AppComponents& app);
