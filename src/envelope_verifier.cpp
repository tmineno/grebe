#include "envelope_verifier.h"

#include <algorithm>
#include <deque>
#include <utility>

uint32_t EnvelopeVerifier::pack_pair(int16_t lo, int16_t hi) {
    return (static_cast<uint32_t>(static_cast<uint16_t>(lo)) << 16) |
            static_cast<uint32_t>(static_cast<uint16_t>(hi));
}

void EnvelopeVerifier::build_window_set(const int16_t* period_buf, size_t period_len,
                                          size_t window_size,
                                          std::vector<uint32_t>& out) {
    out.clear();
    if (window_size == 0 || period_len == 0) return;

    if (window_size >= period_len) {
        // Window covers full period: single (global_min, global_max) pair
        int16_t gmin = period_buf[0], gmax = period_buf[0];
        for (size_t i = 1; i < period_len; i++) {
            if (period_buf[i] < gmin) gmin = period_buf[i];
            if (period_buf[i] > gmax) gmax = period_buf[i];
        }
        out.push_back(pack_pair(gmin, gmax));
        return;
    }

    // Cyclic sliding window: extend buffer by (window_size - 1) for wrap-around
    size_t total = period_len + window_size - 1;
    std::vector<int16_t> buf(total);
    for (size_t i = 0; i < total; i++) {
        buf[i] = period_buf[i % period_len];
    }

    // Sliding window min/max via monotonic deques, O(total)
    std::vector<int16_t> wmins(period_len), wmaxs(period_len);
    std::deque<size_t> dq_min, dq_max;

    for (size_t i = 0; i < total; i++) {
        while (!dq_min.empty() && buf[dq_min.back()] >= buf[i])
            dq_min.pop_back();
        dq_min.push_back(i);
        if (dq_min.front() + window_size <= i)
            dq_min.pop_front();

        while (!dq_max.empty() && buf[dq_max.back()] <= buf[i])
            dq_max.pop_back();
        dq_max.push_back(i);
        if (dq_max.front() + window_size <= i)
            dq_max.pop_front();

        if (i >= window_size - 1) {
            size_t s = i - window_size + 1;
            wmins[s] = buf[dq_min.front()];
            wmaxs[s] = buf[dq_max.front()];
        }
    }

    out.reserve(period_len);
    for (size_t s = 0; s < period_len; s++) {
        out.push_back(pack_pair(wmins[s], wmaxs[s]));
    }

    // Sort and deduplicate for binary_search in verify()
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void EnvelopeVerifier::rebuild(const int16_t* period_buf, size_t period_len,
                                size_t bucket_size_floor, size_t bucket_size_ceil) {
    ready_ = false;
    win_floor_ = bucket_size_floor;
    win_ceil_ = bucket_size_ceil;

    build_window_set(period_buf, period_len, bucket_size_floor, set_floor_);

    if (bucket_size_ceil != bucket_size_floor) {
        build_window_set(period_buf, period_len, bucket_size_ceil, set_ceil_);
    } else {
        set_ceil_ = set_floor_;
    }

    ready_ = true;
}

EnvelopeResult EnvelopeVerifier::verify(const int16_t* decimated, uint32_t num_buckets,
                                          size_t raw_sample_count) const {
    EnvelopeResult result;
    result.total_buckets = num_buckets;
    result.matched_buckets = 0;

    if (!ready_ || num_buckets == 0 || raw_sample_count == 0) {
        result.match_rate = -1.0;
        return result;
    }

    size_t N = raw_sample_count;
    uint32_t B = num_buckets;

    for (uint32_t b = 0; b < B; b++) {
        size_t bstart = (static_cast<size_t>(b) * N) / B;
        size_t bend   = (static_cast<size_t>(b + 1) * N) / B;
        size_t bsz = bend - bstart;

        int16_t lo = decimated[b * 2];
        int16_t hi = decimated[b * 2 + 1];

        const auto& valid_set = (bsz > win_floor_) ? set_ceil_ : set_floor_;

        // Check with +/-1 LSB tolerance (9 neighbor combinations)
        bool matched = false;
        for (int dl = -1; dl <= 1 && !matched; dl++) {
            for (int dh = -1; dh <= 1 && !matched; dh++) {
                int tlo = static_cast<int>(lo) + dl;
                int thi = static_cast<int>(hi) + dh;
                if (tlo < -32768 || tlo > 32767) continue;
                if (thi < -32768 || thi > 32767) continue;
                if (std::binary_search(valid_set.begin(), valid_set.end(),
                                       pack_pair(static_cast<int16_t>(tlo),
                                                 static_cast<int16_t>(thi)))) {
                    matched = true;
                }
            }
        }

        if (matched) result.matched_buckets++;
    }

    result.match_rate = static_cast<double>(result.matched_buckets) /
                        static_cast<double>(result.total_buckets);
    return result;
}
