#include "ingestion_thread.h"
#include "drop_counter.h"

#include <spdlog/spdlog.h>

IngestionThread::~IngestionThread() {
    stop();
}

void IngestionThread::start(grebe::IDataSource& source,
                            std::vector<RingBuffer<int16_t>*> rings,
                            std::vector<DropCounter*> drop_counters) {
    stop();
    source_ = &source;
    rings_ = std::move(rings);
    drop_counters_ = std::move(drop_counters);
    stop_requested_.store(false, std::memory_order_relaxed);
    expected_seq_ = 0;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&IngestionThread::thread_func, this);
}

void IngestionThread::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void IngestionThread::thread_func() {
    grebe::FrameBuffer frame;
    uint64_t gap_count = 0;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        auto result = source_->read_frame(frame);

        switch (result) {
        case grebe::ReadResult::Ok:
            break;
        case grebe::ReadResult::NoData:
            continue;
        case grebe::ReadResult::EndOfStream:
            spdlog::info("IngestionThread: source ended");
            return;
        case grebe::ReadResult::Error:
            spdlog::error("IngestionThread: source error");
            return;
        }

        // Sequence gap detection
        if (frame.sequence != expected_seq_ && expected_seq_ > 0) {
            uint64_t gap = (frame.sequence > expected_seq_)
                ? (frame.sequence - expected_seq_) : 1;
            gap_count += gap;
            sequence_gaps_.store(gap_count, std::memory_order_relaxed);
        }
        expected_seq_ = frame.sequence + 1;

        // Update telemetry
        last_producer_ts_ns_.store(frame.producer_ts_ns, std::memory_order_relaxed);
        auto src_info = source_->info();
        sample_rate_.store(src_info.sample_rate_hz, std::memory_order_relaxed);

        // Push samples to ring buffers (channel-major layout)
        uint32_t ch_count = std::min(frame.channel_count,
                                     static_cast<uint32_t>(rings_.size()));
        for (uint32_t ch = 0; ch < ch_count; ch++) {
            size_t offset = static_cast<size_t>(ch) * frame.samples_per_channel;
            if (offset + frame.samples_per_channel <= frame.data.size()) {
                size_t pushed = rings_[ch]->push_bulk(
                    frame.data.data() + offset, frame.samples_per_channel);
                if (ch < drop_counters_.size() && drop_counters_[ch]) {
                    drop_counters_[ch]->record_push(frame.samples_per_channel, pushed);
                }
            }
        }
    }
}
