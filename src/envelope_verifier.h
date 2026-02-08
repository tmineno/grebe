#pragma once

#include <cstdint>
#include <map>
#include <vector>

struct EnvelopeResult {
    uint32_t total_buckets = 0;
    uint32_t matched_buckets = 0;
    double match_rate = -1.0;  // -1.0 = skipped (not verifiable)
};

class EnvelopeVerifier {
public:
    // Set the period buffer for this channel.  Must be called before verify().
    // The buffer pointer must remain valid for the lifetime of the verifier.
    void set_period(const int16_t* period_buf, size_t period_len);

    // Verify one channel's MinMax decimated output.
    // decimated: interleaved [min0, max0, min1, max1, ...], length = num_buckets * 2
    // ch_raw: number of raw input samples that produced this decimated output.
    // Window sets are built lazily and cached per bucket_size.
    EnvelopeResult verify(const int16_t* decimated, uint32_t num_buckets, uint32_t ch_raw);

    bool is_ready() const { return period_buf_ != nullptr && period_len_ > 0; }

    void clear();

private:
    static uint32_t pack_pair(int16_t lo, int16_t hi);
    static void build_window_set(const int16_t* period_buf, size_t period_len,
                                  size_t window_size, std::vector<uint32_t>& out);

    // Ensure the valid set covers the given bucket sizes (build and cache if needed).
    void ensure_bucket_sizes(size_t bs1, size_t bs2);

    const int16_t* period_buf_ = nullptr;
    size_t period_len_ = 0;

    // Cache: bucket_size → sorted valid (min, max) pair set
    std::map<size_t, std::vector<uint32_t>> cache_;
};
