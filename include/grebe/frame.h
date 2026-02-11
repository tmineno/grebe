#pragma once

// Frame — Unified data frame with ownership model (RDD §5.1)
// Phase 10: IStage contract foundation

#include <cstdint>
#include <vector>
#include <functional>
#include <cassert>
#include <cstring>
#include <utility>

namespace grebe {

// Forward declaration for conversion utilities
struct FrameBuffer;

/// Ownership model for Frame data (RDD §5.1).
enum class OwnershipModel : uint8_t {
    Owned,     ///< Frame owns data via std::vector (Pipe/UDP, low-bandwidth)
    Borrowed,  ///< Frame borrows external buffer — shm, DMA (zero-copy)
};

/// Unified data frame carrying channel-major int16_t samples.
///
/// Superset of FrameBuffer (legacy) and FrameHeaderV2 (wire format).
/// Move-only — use to_owned() for explicit deep copy.
class Frame {
public:
    // ---- Metadata (public, directly accessible) ----
    uint64_t sequence            = 0;
    uint64_t producer_ts_ns      = 0;
    uint32_t channel_count       = 0;
    uint32_t samples_per_channel = 0;
    double   sample_rate_hz      = 0.0;
    uint64_t first_sample_index  = 0;
    uint32_t flags               = 0;  // reserved (discontinuity, etc.)

    // ---- Factory: Owned ----

    /// Create an Owned frame with pre-allocated (zeroed) data buffer.
    static Frame make_owned(uint32_t channels, uint32_t samples_per_ch) {
        Frame f;
        f.ownership_ = OwnershipModel::Owned;
        f.channel_count = channels;
        f.samples_per_channel = samples_per_ch;
        f.owned_data_.resize(static_cast<size_t>(channels) * samples_per_ch);
        return f;
    }

    /// Create an Owned frame from a legacy FrameBuffer (copies data).
    static Frame from_frame_buffer(const FrameBuffer& fb);

    // ---- Factory: Borrowed ----

    /// Callback invoked when a Borrowed frame releases its reference.
    using ReleaseCallback = std::function<void(const int16_t*, size_t)>;

    /// Create a Borrowed frame referencing external memory.
    /// The release callback is called on destruction or move-assignment.
    static Frame make_borrowed(const int16_t* ptr, size_t count,
                               ReleaseCallback release) {
        Frame f;
        f.ownership_ = OwnershipModel::Borrowed;
        f.borrowed_ptr_ = ptr;
        f.borrowed_count_ = count;
        f.release_cb_ = std::move(release);
        return f;
    }

    // ---- Data access ----

    OwnershipModel ownership() const { return ownership_; }
    bool is_owned() const { return ownership_ == OwnershipModel::Owned; }
    bool is_borrowed() const { return ownership_ == OwnershipModel::Borrowed; }

    /// Pointer to sample data (read-only, valid for both ownership models).
    const int16_t* data() const {
        return is_owned() ? owned_data_.data() : borrowed_ptr_;
    }

    /// Mutable pointer to sample data (Owned only; asserts on Borrowed).
    int16_t* mutable_data() {
        assert(is_owned() && "mutable_data() requires Owned frame");
        return owned_data_.data();
    }

    /// Total sample count (channel_count * samples_per_channel).
    size_t data_count() const {
        return is_owned()
            ? owned_data_.size()
            : borrowed_count_;
    }

    // ---- Ownership transfer ----

    /// Deep-copy to an Owned frame. Borrowed → Owned copies data.
    /// Owned → new Owned also copies data.
    Frame to_owned() const {
        Frame f;
        f.ownership_ = OwnershipModel::Owned;
        // Copy metadata
        f.sequence            = sequence;
        f.producer_ts_ns      = producer_ts_ns;
        f.channel_count       = channel_count;
        f.samples_per_channel = samples_per_channel;
        f.sample_rate_hz      = sample_rate_hz;
        f.first_sample_index  = first_sample_index;
        f.flags               = flags;
        // Copy data
        const auto count = data_count();
        f.owned_data_.resize(count);
        if (count > 0) {
            std::memcpy(f.owned_data_.data(), data(),
                        count * sizeof(int16_t));
        }
        return f;
    }

    // ---- Conversion to legacy FrameBuffer ----
    FrameBuffer to_frame_buffer() const;

    // ---- Move semantics (copy disabled) ----

    Frame(Frame&& other) noexcept
        : sequence(other.sequence)
        , producer_ts_ns(other.producer_ts_ns)
        , channel_count(other.channel_count)
        , samples_per_channel(other.samples_per_channel)
        , sample_rate_hz(other.sample_rate_hz)
        , first_sample_index(other.first_sample_index)
        , flags(other.flags)
        , ownership_(other.ownership_)
        , owned_data_(std::move(other.owned_data_))
        , borrowed_ptr_(other.borrowed_ptr_)
        , borrowed_count_(other.borrowed_count_)
        , release_cb_(std::move(other.release_cb_))
    {
        // Nullify source to prevent double-release
        other.borrowed_ptr_ = nullptr;
        other.borrowed_count_ = 0;
        other.release_cb_ = nullptr;
        other.ownership_ = OwnershipModel::Owned;
    }

    Frame& operator=(Frame&& other) noexcept {
        if (this != &other) {
            // Release current borrowed reference if any
            release_borrowed();
            // Move fields
            sequence            = other.sequence;
            producer_ts_ns      = other.producer_ts_ns;
            channel_count       = other.channel_count;
            samples_per_channel = other.samples_per_channel;
            sample_rate_hz      = other.sample_rate_hz;
            first_sample_index  = other.first_sample_index;
            flags               = other.flags;
            ownership_          = other.ownership_;
            owned_data_         = std::move(other.owned_data_);
            borrowed_ptr_       = other.borrowed_ptr_;
            borrowed_count_     = other.borrowed_count_;
            release_cb_         = std::move(other.release_cb_);
            // Nullify source
            other.borrowed_ptr_ = nullptr;
            other.borrowed_count_ = 0;
            other.release_cb_ = nullptr;
            other.ownership_ = OwnershipModel::Owned;
        }
        return *this;
    }

    ~Frame() {
        release_borrowed();
    }

private:
    Frame() = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    void release_borrowed() {
        if (is_borrowed() && release_cb_ && borrowed_ptr_) {
            release_cb_(borrowed_ptr_, borrowed_count_);
        }
        release_cb_ = nullptr;
        borrowed_ptr_ = nullptr;
        borrowed_count_ = 0;
    }

    OwnershipModel ownership_ = OwnershipModel::Owned;
    std::vector<int16_t> owned_data_;
    const int16_t* borrowed_ptr_ = nullptr;
    size_t borrowed_count_ = 0;
    ReleaseCallback release_cb_;
};

// ---- FrameBuffer conversion (inline, depends on data_source.h) ----

} // namespace grebe

// Include data_source.h after Frame definition for FrameBuffer access.
// This is intentionally placed after the namespace close to break
// circular dependency (data_source.h does not include frame.h).
#include "grebe/data_source.h"

namespace grebe {

inline Frame Frame::from_frame_buffer(const FrameBuffer& fb) {
    Frame f;
    f.ownership_ = OwnershipModel::Owned;
    f.sequence = fb.sequence;
    f.producer_ts_ns = fb.producer_ts_ns;
    f.channel_count = fb.channel_count;
    f.samples_per_channel = fb.samples_per_channel;
    f.sample_rate_hz = 0.0;  // FrameBuffer lacks this field
    f.first_sample_index = 0;
    f.flags = 0;
    f.owned_data_ = fb.data;  // copy
    return f;
}

inline FrameBuffer Frame::to_frame_buffer() const {
    FrameBuffer fb;
    fb.sequence = sequence;
    fb.producer_ts_ns = producer_ts_ns;
    fb.channel_count = channel_count;
    fb.samples_per_channel = samples_per_channel;
    const auto count = data_count();
    fb.data.resize(count);
    if (count > 0) {
        std::memcpy(fb.data.data(), data(), count * sizeof(int16_t));
    }
    return fb;
}

} // namespace grebe
