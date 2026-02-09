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
class DataGenerator;
class DecimationThread;
class Benchmark;
class ProfileRunner;
class DropCounter;
class ITransportConsumer;

struct AppComponents {
    GLFWwindow* window;
    AppCommandQueue* cmd_queue;
    VulkanContext* ctx;
    Swapchain* swapchain;
    Renderer* renderer;
    BufferManager* buf_mgr;
    Hud* hud;
    DataGenerator* data_gen;          // non-null in embedded mode only
    DecimationThread* dec_thread;
    Benchmark* benchmark;
    ProfileRunner* profiler;
    std::vector<DropCounter*> drop_counters;
    uint32_t num_channels;
    size_t ring_capacity_samples = 0;     // per-channel ring capacity (samples)
    bool enable_profile;
    // IPC mode fields (non-null when not embedded)
    ITransportConsumer* transport = nullptr;
    std::atomic<double> current_sample_rate{1e6};   // updated by receiver thread in IPC mode
    std::atomic<bool>   current_paused{false};
    std::atomic<uint64_t> sg_drops_total{0};       // SG-side drops (IPC only)
    std::atomic<uint64_t> seq_gaps{0};              // IPC sequence gaps (cumulative)
    std::atomic<uint64_t> latest_producer_ts_ns{0}; // latest producer timestamp (IPC only)
};

void run_main_loop(AppComponents& app);
