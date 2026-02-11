#include "grebe/decimation_engine.h"
#include "decimation_thread.h"
#include "ring_buffer.h"

namespace grebe {

// Convert between public and internal enum types
static DecimationMode to_internal(DecimationAlgorithm algo) {
    switch (algo) {
    case DecimationAlgorithm::None:   return DecimationMode::None;
    case DecimationAlgorithm::MinMax: return DecimationMode::MinMax;
    case DecimationAlgorithm::LTTB:   return DecimationMode::LTTB;
    }
    return DecimationMode::None;
}

static DecimationAlgorithm from_internal(DecimationMode mode) {
    switch (mode) {
    case DecimationMode::None:   return DecimationAlgorithm::None;
    case DecimationMode::MinMax: return DecimationAlgorithm::MinMax;
    case DecimationMode::LTTB:   return DecimationAlgorithm::LTTB;
    }
    return DecimationAlgorithm::None;
}

struct DecimationEngine::Impl {
    DecimationThread thread;
};

DecimationEngine::DecimationEngine()
    : impl_(std::make_unique<Impl>()) {}

DecimationEngine::~DecimationEngine() {
    stop();
}

void DecimationEngine::start(std::vector<RingBuffer<int16_t>*> rings,
                              const DecimationConfig& config) {
    impl_->thread.start(std::move(rings), config.target_points,
                         to_internal(config.algorithm));
    impl_->thread.set_sample_rate(config.sample_rate);
    impl_->thread.set_visible_time_span(config.visible_time_span_s);
}

void DecimationEngine::stop() {
    impl_->thread.stop();
}

void DecimationEngine::set_algorithm(DecimationAlgorithm algo) {
    impl_->thread.set_mode(to_internal(algo));
}

void DecimationEngine::set_sample_rate(double rate) {
    impl_->thread.set_sample_rate(rate);
}

void DecimationEngine::set_visible_time_span(double seconds) {
    impl_->thread.set_visible_time_span(seconds);
}

void DecimationEngine::set_target_points(uint32_t n) {
    impl_->thread.set_target_points(n);
}

void DecimationEngine::cycle_algorithm() {
    impl_->thread.cycle_mode();
}

bool DecimationEngine::try_get_frame(DecimationOutput& output) {
    uint32_t raw_count = 0;
    bool ok = impl_->thread.try_get_frame(output.data, raw_count,
                                           output.per_channel_raw_counts);
    output.raw_sample_count = raw_count;
    output.per_channel_vertex_count = impl_->thread.per_channel_vertex_count();
    return ok;
}

DecimationMetrics DecimationEngine::metrics() const {
    DecimationMetrics m;
    m.decimation_time_ms = impl_->thread.decimation_time_ms();
    m.decimation_ratio = impl_->thread.decimation_ratio();
    m.ring_fill_ratio = impl_->thread.ring_fill_ratio();
    m.effective_algorithm = from_internal(impl_->thread.effective_mode());
    return m;
}

uint32_t DecimationEngine::channel_count() const {
    return impl_->thread.channel_count();
}

const char* DecimationEngine::algorithm_name(DecimationAlgorithm algo) {
    return DecimationThread::mode_name(to_internal(algo));
}

} // namespace grebe
