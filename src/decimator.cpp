#include "decimator.h"

#include <algorithm>
#include <cmath>
#include <limits>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#endif

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

// Scalar MinMax (always available, used for benchmarking)
std::vector<int16_t> Decimator::minmax_scalar(const std::vector<int16_t>& input, uint32_t target_points) {
    if (target_points < 2) return {};
    if (input.size() <= target_points) return input;

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

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)

// SSE2 horizontal min of 8 x int16 packed in __m128i
static inline int16_t hmin_epi16(__m128i v) {
    // Compare high 64 bits with low 64 bits
    v = _mm_min_epi16(v, _mm_shufflehi_epi16(_mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)), _MM_SHUFFLE(1, 0, 3, 2)));
    // Now min is in low 64 bits; compare pairs
    v = _mm_min_epi16(v, _mm_shufflelo_epi16(v, _MM_SHUFFLE(1, 0, 3, 2)));
    // Compare last pair
    v = _mm_min_epi16(v, _mm_shufflelo_epi16(v, _MM_SHUFFLE(0, 1, 0, 1)));
    return static_cast<int16_t>(_mm_extract_epi16(v, 0));
}

// SSE2 horizontal max of 8 x int16 packed in __m128i
static inline int16_t hmax_epi16(__m128i v) {
    v = _mm_max_epi16(v, _mm_shufflehi_epi16(_mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)), _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_max_epi16(v, _mm_shufflelo_epi16(v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_max_epi16(v, _mm_shufflelo_epi16(v, _MM_SHUFFLE(0, 1, 0, 1)));
    return static_cast<int16_t>(_mm_extract_epi16(v, 0));
}

// SIMD MinMax: process 16 int16 values per iteration (2x unrolled SSE2)
std::vector<int16_t> Decimator::minmax(const std::vector<int16_t>& input, uint32_t target_points) {
    if (target_points < 2) return {};
    if (input.size() <= target_points) return input;

    uint32_t num_buckets = target_points / 2;
    std::vector<int16_t> output;
    output.reserve(num_buckets * 2);

    size_t n = input.size();
    const int16_t* data = input.data();

    for (uint32_t b = 0; b < num_buckets; b++) {
        size_t start = (static_cast<size_t>(b) * n) / num_buckets;
        size_t end   = (static_cast<size_t>(b + 1) * n) / num_buckets;
        size_t len   = end - start;

        const int16_t* ptr = data + start;

        __m128i vmin = _mm_set1_epi16(std::numeric_limits<int16_t>::max());
        __m128i vmax = _mm_set1_epi16(std::numeric_limits<int16_t>::min());

        size_t i = 0;

        // Process 16 values per iteration (2x unrolled, 8 int16 per __m128i)
        for (; i + 15 < len; i += 16) {
            __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i));
            __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i + 8));
            vmin = _mm_min_epi16(vmin, v0);
            vmin = _mm_min_epi16(vmin, v1);
            vmax = _mm_max_epi16(vmax, v0);
            vmax = _mm_max_epi16(vmax, v1);
        }

        // Process remaining groups of 8
        for (; i + 7 < len; i += 8) {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i));
            vmin = _mm_min_epi16(vmin, v);
            vmax = _mm_max_epi16(vmax, v);
        }

        // Horizontal reduction
        int16_t lo = hmin_epi16(vmin);
        int16_t hi = hmax_epi16(vmax);

        // Scalar tail for remaining elements
        for (; i < len; i++) {
            int16_t v = ptr[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }

        output.push_back(lo);
        output.push_back(hi);
    }

    return output;
}

#else

// Non-SIMD fallback: use scalar implementation
std::vector<int16_t> Decimator::minmax(const std::vector<int16_t>& input, uint32_t target_points) {
    return minmax_scalar(input, target_points);
}

#endif // SSE2

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
