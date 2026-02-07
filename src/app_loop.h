#pragma once

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

struct AppComponents {
    GLFWwindow* window;
    AppCommandQueue* cmd_queue;
    VulkanContext* ctx;
    Swapchain* swapchain;
    Renderer* renderer;
    BufferManager* buf_mgr;
    Hud* hud;
    DataGenerator* data_gen;
    DecimationThread* dec_thread;
    Benchmark* benchmark;
    ProfileRunner* profiler;
    std::vector<DropCounter*> drop_counters;
    uint32_t num_channels;
    bool enable_profile;
};

void run_main_loop(AppComponents& app);
