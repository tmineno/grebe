#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

struct GLFWwindow;
class AppCommandQueue;
class VulkanContext;
class Swapchain;
class Renderer;
class BufferManager;
class Hud;
class SyntheticSource;
class IpcSource;
class IngestionThread;
class DecimationThread;
class Benchmark;
class ProfileRunner;
class DropCounter;

struct AppComponents {
    GLFWwindow* window;
    AppCommandQueue* cmd_queue;
    VulkanContext* ctx;
    Swapchain* swapchain;
    Renderer* renderer;
    BufferManager* buf_mgr;
    Hud* hud;
    SyntheticSource* synthetic_source = nullptr;  // non-null in embedded mode
    IpcSource* ipc_source = nullptr;              // non-null in IPC mode
    IngestionThread* ingestion = nullptr;
    DecimationThread* dec_thread;
    Benchmark* benchmark;
    ProfileRunner* profiler;
    std::vector<DropCounter*> drop_counters;
    uint32_t num_channels;
    size_t ring_capacity_samples = 0;     // per-channel ring capacity (samples)
    bool enable_profile;
    std::atomic<double> current_sample_rate{1e6};
    std::atomic<bool>   current_paused{false};
};

void run_main_loop(AppComponents& app);
