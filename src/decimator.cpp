#include "decimator.h"

std::vector<int16_t> Decimator::decimate(const std::vector<int16_t>& input,
                                          DecimationMode mode,
                                          uint32_t target_points) {
    switch (mode) {
    case DecimationMode::MinMax:
        return minmax(input, target_points);
    case DecimationMode::LTTB:
        return lttb(input, target_points);
    case DecimationMode::None:
    default:
        return passthrough(input);
    }
}

std::vector<int16_t> Decimator::passthrough(const std::vector<int16_t>& input) {
    return input;
}

std::vector<int16_t> Decimator::minmax(const std::vector<int16_t>& /*input*/, uint32_t /*target_points*/) {
    // TODO: Phase 2 implementation
    return {};
}

std::vector<int16_t> Decimator::lttb(const std::vector<int16_t>& /*input*/, uint32_t /*target_points*/) {
    // TODO: Phase 2 implementation
    return {};
}
