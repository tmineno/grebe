#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

struct EnvelopeResult {
    uint32_t total_buckets = 0;
    uint32_t matched_buckets = 0;
    double match_rate = -1.0;  // -1.0 = skipped (not verifiable)
};

class EnvelopeVerifier {
public:
    // Rebuild lookup tables for new parameters.
    // Call when period buffer or bucket dimensions change.
    void rebuild(const int16_t* period_buf, size_t period_len,
                 size_t bucket_size_floor, size_t bucket_size_ceil);

    // Verify one channel's MinMax decimated output.
    // decimated: interleaved [min0, max0, min1, max1, ...], length = num_buckets * 2
    EnvelopeResult verify(const int16_t* decimated, uint32_t num_buckets,
                          size_t raw_sample_count) const;

    bool is_ready() const { return ready_; }
    size_t win_floor() const { return win_floor_; }
    size_t win_ceil() const { return win_ceil_; }

private:
    static uint32_t pack_pair(int16_t lo, int16_t hi);
    static void build_window_set(const int16_t* period_buf, size_t period_len,
                                  size_t window_size, std::unordered_set<uint32_t>& out);

    std::unordered_set<uint32_t> set_floor_;
    std::unordered_set<uint32_t> set_ceil_;
    size_t win_floor_ = 0;
    size_t win_ceil_ = 0;
    bool ready_ = false;
};
