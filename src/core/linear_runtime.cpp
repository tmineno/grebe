#include "core/linear_runtime.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

namespace grebe {

LinearRuntime::LinearRuntime()
    : impl_(std::make_unique<Impl>()) {}

LinearRuntime::~LinearRuntime() {
    stop();
}

void LinearRuntime::add_stage(std::unique_ptr<IStage> stage,
                               size_t queue_capacity,
                               BackpressurePolicy policy) {
    Impl::StageEntry entry;
    entry.stage = std::move(stage);
    entry.queue_capacity = queue_capacity;
    entry.policy = policy;
    impl_->entries.push_back(std::move(entry));
}

void LinearRuntime::start() {
    if (impl_->is_running.load()) return;
    if (impl_->entries.empty()) return;

    impl_->stop.store(false);
    impl_->start_time = std::chrono::steady_clock::now();

    const size_t n = impl_->entries.size();

    // Create queues: one output queue per stage
    // queues[0] = output of stage 0 = input of stage 1
    // queues[n-1] = output of last stage = polled by main thread
    impl_->queues.clear();
    for (size_t i = 0; i < n; ++i) {
        auto cap = impl_->entries[i].queue_capacity;
        auto pol = impl_->entries[i].policy;
        // For the first stage's output, use next stage's specified capacity/policy
        if (i + 1 < n) {
            cap = impl_->entries[i + 1].queue_capacity;
            pol = impl_->entries[i + 1].policy;
        }
        impl_->queues.push_back(
            std::make_unique<InProcessQueue>(cap, pol));
    }

    // Create worker threads
    impl_->workers.clear();
    for (size_t i = 0; i < n; ++i) {
        auto w = std::make_unique<Impl::WorkerState>();
        w->frames_processed.store(0);
        w->total_process_ns.store(0);
        w->thread = std::thread(
            &Impl::worker_func, impl_.get(), i);
        impl_->workers.push_back(std::move(w));
    }

    impl_->is_running.store(true);
    spdlog::info("LinearRuntime started with {} stage(s)", n);
}

void LinearRuntime::stop() {
    if (!impl_->is_running.load()) return;

    impl_->stop.store(true);

    // Shutdown all queues to unblock workers
    for (auto& q : impl_->queues) {
        q->shutdown();
    }

    // Join all worker threads
    for (auto& w : impl_->workers) {
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }

    impl_->is_running.store(false);
    spdlog::info("LinearRuntime stopped");
}

bool LinearRuntime::running() const {
    return impl_->is_running.load();
}

std::optional<Frame> LinearRuntime::poll_output() {
    if (impl_->queues.empty()) return std::nullopt;
    return impl_->queues.back()->dequeue();
}

std::optional<Frame> LinearRuntime::poll_latest() {
    if (impl_->queues.empty()) return std::nullopt;

    auto& q = *impl_->queues.back();
    std::optional<Frame> latest;

    // Drain all available frames, keep only the last one
    while (auto f = q.dequeue()) {
        latest = std::move(f);
    }
    return latest;
}

IStage* LinearRuntime::stage(size_t index) {
    if (index >= impl_->entries.size()) return nullptr;
    return impl_->entries[index].stage.get();
}

size_t LinearRuntime::stage_count() const {
    return impl_->entries.size();
}

std::vector<StageTelemetry> LinearRuntime::telemetry() const {
    std::vector<StageTelemetry> result;
    result.reserve(impl_->entries.size());

    for (size_t i = 0; i < impl_->entries.size(); ++i) {
        StageTelemetry st;
        st.name = impl_->entries[i].stage->name();

        uint64_t fp = impl_->workers[i]->frames_processed.load(std::memory_order_relaxed);
        uint64_t tp = impl_->workers[i]->total_process_ns.load(std::memory_order_relaxed);

        st.frames_processed = fp;
        st.avg_process_time_ms = (fp > 0)
            ? (static_cast<double>(tp) / 1e6 / static_cast<double>(fp))
            : 0.0;

        // Input queue drops (stage 0 has no input queue)
        if (i > 0) {
            st.queue_dropped = impl_->queues[i - 1]->total_dropped();
        } else {
            st.queue_dropped = 0;
        }

        result.push_back(std::move(st));
    }
    return result;
}

// --- Worker thread function ---

void LinearRuntime::Impl::worker_func(size_t stage_index) {
    auto& entry = entries[stage_index];
    auto* stage = entry.stage.get();

    // Input queue: stage 0 has none (SourceStage), others read from previous stage's output
    InProcessQueue* input_queue = (stage_index > 0)
        ? queues[stage_index - 1].get()
        : nullptr;

    // Output queue: always present (queues[stage_index])
    InProcessQueue* output_queue = queues[stage_index].get();

    uint64_t iteration = 0;
    auto& fps_counter = workers[stage_index]->frames_processed;
    auto& time_counter = workers[stage_index]->total_process_ns;

    while (!stop.load(std::memory_order_relaxed)) {
        // Build input batch
        std::vector<Frame> input_frames;
        if (input_queue) {
            auto frame = input_queue->dequeue();
            if (!frame) {
                if (stop.load(std::memory_order_relaxed)) break;
                std::this_thread::yield();
                continue;
            }
            input_frames.push_back(std::move(*frame));
        }

        BatchView input(std::move(input_frames));
        BatchWriter output;

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        double wall_s = std::chrono::duration<double>(elapsed).count();
        ExecContext ctx{iteration++, static_cast<uint32_t>(stage_index), wall_s};

        auto t0 = std::chrono::steady_clock::now();
        auto result = stage->process(input, output, ctx);
        auto t1 = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

        time_counter.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);

        // Enqueue output frames
        auto produced = output.take();
        for (auto& f : produced) {
            output_queue->enqueue(std::move(f));
        }

        if (!produced.empty()) {
            fps_counter.fetch_add(static_cast<uint64_t>(produced.size()),
                                  std::memory_order_relaxed);
        }

        switch (result) {
        case StageResult::Ok:
            break;
        case StageResult::NoData:
            if (!stop.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            break;
        case StageResult::EOS:
            return;  // Exit worker
        case StageResult::Retry:
            break;
        case StageResult::Error:
            spdlog::error("Stage '{}' returned Error", stage->name());
            return;
        }
    }
}

} // namespace grebe
