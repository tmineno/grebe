#pragma once

// LinearRuntime — Internal implementation header
// Phase 13

#include "grebe/runtime.h"
#include "core/in_process_queue.h"

#include <atomic>
#include <thread>
#include <vector>

namespace grebe {

/// Internal state for LinearRuntime (pimpl target).
struct LinearRuntime::Impl {
    struct StageEntry {
        std::unique_ptr<IStage> stage;
        size_t queue_capacity = 64;
        BackpressurePolicy policy = BackpressurePolicy::DropOldest;
    };

    struct WorkerState {
        std::thread thread;
        std::atomic<uint64_t> frames_processed{0};
        std::atomic<uint64_t> total_process_ns{0};
    };

    std::vector<StageEntry> entries;
    std::vector<std::unique_ptr<InProcessQueue>> queues;  // queues[i] = output of stage i
    std::vector<std::unique_ptr<WorkerState>> workers;

    std::atomic<bool> stop{false};
    std::atomic<bool> is_running{false};

    std::chrono::steady_clock::time_point start_time;

    void worker_func(size_t stage_index);
};

} // namespace grebe
