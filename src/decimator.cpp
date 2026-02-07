#include "decimator.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

std::vector<int16_t> Decimator::minmax(const std::vector<int16_t>& input, uint32_t target_points) {
    if (target_points < 2) return {};
    if (input.size() <= target_points) return input;

    // Each bucket produces a min/max pair → num_buckets = target_points / 2
    uint32_t num_buckets = target_points / 2;
    std::vector<int16_t> output;
    output.reserve(num_buckets * 2);

    size_t n = input.size();
    const int16_t* data = input.data();

    for (uint32_t b = 0; b < num_buckets; b++) {
        size_t start = (static_cast<size_t>(b) * n) / num_buckets;
        size_t end   = (static_cast<size_t>(b + 1) * n) / num_buckets;

        int16_t lo = std::numeric_limits<int16_t>::max();
        int16_t hi = std::numeric_limits<int16_t>::min();

        for (size_t i = start; i < end; i++) {
            int16_t v = data[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }

        output.push_back(lo);
        output.push_back(hi);
    }

    return output;
}

std::vector<int16_t> Decimator::lttb(const std::vector<int16_t>& input, uint32_t target_points) {
    if (target_points < 3) return {};
    if (input.size() <= target_points) return input;

    size_t n = input.size();
    const int16_t* data = input.data();

    std::vector<int16_t> output;
    output.reserve(target_points);

    // Always keep first point
    output.push_back(data[0]);

    uint32_t num_buckets = target_points - 2;
    double bucket_size = static_cast<double>(n - 2) / static_cast<double>(num_buckets);

    // Previous selected point (x, y)
    double prev_x = 0.0;
    double prev_y = static_cast<double>(data[0]);

    for (uint32_t b = 0; b < num_buckets; b++) {
        // Current bucket range
        size_t bucket_start = 1 + static_cast<size_t>(b * bucket_size);
        size_t bucket_end   = 1 + static_cast<size_t>((b + 1) * bucket_size);
        if (bucket_end > n - 1) bucket_end = n - 1;

        // Next bucket average (or last point for the final bucket)
        double next_avg_x, next_avg_y;
        if (b + 1 < num_buckets) {
            size_t next_start = 1 + static_cast<size_t>((b + 1) * bucket_size);
            size_t next_end   = 1 + static_cast<size_t>((b + 2) * bucket_size);
            if (next_end > n - 1) next_end = n - 1;

            double sum_y = 0.0;
            double count = 0.0;
            double sum_x = 0.0;
            for (size_t i = next_start; i < next_end; i++) {
                sum_x += static_cast<double>(i);
                sum_y += static_cast<double>(data[i]);
                count += 1.0;
            }
            next_avg_x = (count > 0) ? sum_x / count : static_cast<double>(next_start);
            next_avg_y = (count > 0) ? sum_y / count : static_cast<double>(data[next_start]);
        } else {
            next_avg_x = static_cast<double>(n - 1);
            next_avg_y = static_cast<double>(data[n - 1]);
        }

        // Find point in current bucket that maximizes triangle area
        double max_area = -1.0;
        size_t best_idx = bucket_start;

        for (size_t i = bucket_start; i < bucket_end; i++) {
            double cx = static_cast<double>(i);
            double cy = static_cast<double>(data[i]);

            // Triangle area = 0.5 * |prev_x*(cy - next_avg_y) + cx*(next_avg_y - prev_y) + next_avg_x*(prev_y - cy)|
            double area = std::abs(
                prev_x * (cy - next_avg_y) +
                cx * (next_avg_y - prev_y) +
                next_avg_x * (prev_y - cy)
            );

            if (area > max_area) {
                max_area = area;
                best_idx = i;
            }
        }

        output.push_back(data[best_idx]);
        prev_x = static_cast<double>(best_idx);
        prev_y = static_cast<double>(data[best_idx]);
    }

    // Always keep last point
    output.push_back(data[n - 1]);

    return output;
}
