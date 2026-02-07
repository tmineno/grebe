#pragma once

#include <cstdint>
#include <vector>

enum class DecimationMode {
    None,
    MinMax,
    LTTB
};

class Decimator {
public:
    // Phase 2 stubs
    static std::vector<int16_t> decimate(const std::vector<int16_t>& input,
                                          DecimationMode mode,
                                          uint32_t target_points);

    // Passthrough (no decimation)
    static std::vector<int16_t> passthrough(const std::vector<int16_t>& input);

    // MinMax decimation: for each bucket, output min and max
    static std::vector<int16_t> minmax(const std::vector<int16_t>& input, uint32_t target_points);

    // LTTB (Largest Triangle Three Buckets)
    static std::vector<int16_t> lttb(const std::vector<int16_t>& input, uint32_t target_points);
};
