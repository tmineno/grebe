#include "bench_udp.h"
#include "ipc/udp_transport.h"
#include "ipc/contracts.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

constexpr uint16_t BENCH_PORT = 19876;

struct UdpBenchResult {
    std::string label;
    uint32_t channels       = 0;
    uint32_t block_size     = 0;
    size_t   datagram_size  = 0;
    double target_rate_msps = 0.0;  // 0 = unlimited
    double duration_s       = 0.0;
    uint64_t frames_sent    = 0;
    uint64_t frames_recv    = 0;
    double frames_per_sec   = 0.0;
    double throughput_msps  = 0.0;
    double throughput_mbps  = 0.0;
    double drop_rate        = 0.0;
};

uint32_t max_block_size(uint32_t channels, size_t max_datagram) {
    return static_cast<uint32_t>(
        (max_datagram - sizeof(FrameHeaderV2)) / (channels * sizeof(int16_t)));
}

UdpBenchResult bench_udp_scenario(const std::string& label,
                                   uint32_t channels,
                                   uint32_t block_size,
                                   double target_rate_msps,
                                   int duration_s,
                                   uint16_t port,
                                   size_t max_datagram) {
    UdpBenchResult result;
    result.label = label;
    result.channels = channels;
    result.block_size = block_size;
    result.datagram_size = max_datagram;
    result.target_rate_msps = target_rate_msps;

    // Compute pacing interval (ns per frame) for rate-limited scenarios
    double frame_interval_ns = 0.0;
    if (target_rate_msps > 0.0) {
        double samples_per_sec = target_rate_msps * 1e6;
        double frames_per_sec = samples_per_sec / block_size;
        frame_interval_ns = 1e9 / frames_per_sec;
    }

    // Create consumer first (binds port), then producer
    UdpConsumer consumer(port);
    UdpProducer producer("127.0.0.1", port);
    producer.set_max_datagram_size(max_datagram);

    // Prepare payload
    size_t payload_bytes = channels * block_size * sizeof(int16_t);
    std::vector<int16_t> payload(channels * block_size, 0);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<int16_t>(i & 0x7FFF);
    }

    std::atomic<uint64_t> recv_frames{0};
    std::atomic<uint64_t> recv_bytes{0};
    std::atomic<bool> sender_done{false};

    // Receiver thread: receive_frame() blocks internally (loops on timeout),
    // only returns false when consumer.close() sets the closed_ flag.
    std::thread receiver([&] {
        FrameHeaderV2 hdr;
        std::vector<int16_t> buf;
        while (consumer.receive_frame(hdr, buf)) {
            recv_frames.fetch_add(1, std::memory_order_relaxed);
            recv_bytes.fetch_add(sizeof(hdr) + hdr.payload_bytes, std::memory_order_relaxed);
        }
    });

    // Sender: runs on main thread for duration_s seconds
    uint64_t sent = 0;
    auto t0 = Clock::now();
    auto deadline = t0 + std::chrono::seconds(duration_s);
    auto next_send = t0;

    while (Clock::now() < deadline) {
        // Rate limiting
        if (frame_interval_ns > 0.0) {
            auto now = Clock::now();
            if (now < next_send) {
                // Busy-wait for sub-microsecond precision
                while (Clock::now() < next_send) {}
            }
            next_send += std::chrono::nanoseconds(static_cast<int64_t>(frame_interval_ns));
            // Prevent drift accumulation if we fall behind
            auto now2 = Clock::now();
            if (next_send < now2 - std::chrono::milliseconds(10)) {
                next_send = now2;
            }
        }

        FrameHeaderV2 hdr;
        hdr.sequence = sent;
        hdr.channel_count = channels;
        hdr.block_length_samples = block_size;
        hdr.payload_bytes = static_cast<uint32_t>(payload_bytes);
        hdr.sample_rate_hz = target_rate_msps > 0.0 ? target_rate_msps * 1e6 : 0.0;

        if (producer.send_frame(hdr, payload.data())) {
            ++sent;
        }
    }

    auto t1 = Clock::now();

    // Brief sleep to let in-flight datagrams arrive, then close to unblock receiver
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    consumer.close();
    receiver.join();

    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    uint64_t received = recv_frames.load();

    result.duration_s = elapsed;
    result.frames_sent = sent;
    result.frames_recv = received;
    result.frames_per_sec = received / elapsed;
    result.throughput_msps = (received * block_size) / elapsed / 1e6;
    result.throughput_mbps = (received * payload_bytes) / elapsed / (1024.0 * 1024.0);
    result.drop_rate = sent > 0 ? static_cast<double>(sent - received) / sent : 0.0;

    return result;
}

nlohmann::json result_to_json(const UdpBenchResult& r) {
    nlohmann::json j;
    j["label"]           = r.label;
    j["channels"]        = r.channels;
    j["block_size"]      = r.block_size;
    j["datagram_size"]   = r.datagram_size;
    j["target_rate_msps"]= r.target_rate_msps;
    j["duration_s"]      = r.duration_s;
    j["frames_sent"]     = r.frames_sent;
    j["frames_recv"]     = r.frames_recv;
    j["frames_per_sec"]  = r.frames_per_sec;
    j["throughput_msps"] = r.throughput_msps;
    j["throughput_mbps"] = r.throughput_mbps;
    j["drop_rate"]       = r.drop_rate;
    return j;
}

} // namespace

nlohmann::json run_bench_udp(int duration_seconds, uint32_t channels, size_t max_datagram_size) {
    spdlog::info("=== BM-H: UDP Loopback Throughput (channels={}, datagram_size={}) ===",
                 channels, max_datagram_size);

    nlohmann::json results = nlohmann::json::array();

    // --- Block size variation (unlimited rate) ---
    struct BlockSizeScenario {
        std::string label;
        uint32_t channels;
        uint32_t block_size;  // 0 = auto (MTU max)
    };

    std::vector<BlockSizeScenario> block_scenarios;
    if (channels == 1) {
        block_scenarios = {
            {"1ch_max",  1, 0},
            {"1ch_256",  1, 256},
            {"1ch_64",   1, 64},
            {"2ch_max",  2, 0},
            {"4ch_max",  4, 0},
        };
    } else {
        // For multi-channel mode, run max block + smaller block scenarios
        std::string prefix = std::to_string(channels) + "ch";
        block_scenarios = {
            {prefix + "_max",  channels, 0},
            {prefix + "_256",  channels, 256},
            {prefix + "_64",   channels, 64},
        };
    }

    spdlog::info("--- Block size variation (unlimited rate) ---");
    for (auto& s : block_scenarios) {
        uint32_t bs = s.block_size > 0 ? s.block_size : max_block_size(s.channels, max_datagram_size);
        // Skip scenarios where the fixed block_size exceeds datagram capacity
        if (s.block_size > 0 && s.block_size > max_block_size(s.channels, max_datagram_size)) {
            spdlog::info("  Skipping: {} ({}ch x {} samples > datagram limit {})",
                         s.label, s.channels, s.block_size, max_block_size(s.channels, max_datagram_size));
            continue;
        }
        spdlog::info("  Running: {} ({}ch x {} samples, unlimited)...", s.label, s.channels, bs);
        auto r = bench_udp_scenario(s.label, s.channels, bs, 0.0, duration_seconds, BENCH_PORT, max_datagram_size);
        spdlog::info("    => {:.1f} MSPS, {:.0f} frames/s, drop {:.2f}%",
                      r.throughput_msps, r.frames_per_sec, r.drop_rate * 100);
        results.push_back(result_to_json(r));
    }

    // --- Target rate scenarios (per-channel MSPS target) ---
    struct RateScenario {
        std::string label_suffix;
        double rate_msps;  // per-channel
    };

    std::vector<RateScenario> rate_scenarios = {
        {"1MSPS",    1.0},
        {"10MSPS",   10.0},
        {"100MSPS",  100.0},
        {"1GSPS",    1000.0},
    };

    std::string ch_prefix = std::to_string(channels) + "ch_";
    uint32_t bs = max_block_size(channels, max_datagram_size);
    spdlog::info("--- Target rate scenarios ({}ch x {} samples/ch) ---", channels, bs);
    for (auto& s : rate_scenarios) {
        std::string label = ch_prefix + s.label_suffix;
        spdlog::info("  Running: {} ({:.0f} MSPS/ch target)...", label, s.rate_msps);
        auto r = bench_udp_scenario(label, channels, bs, s.rate_msps, duration_seconds, BENCH_PORT, max_datagram_size);
        spdlog::info("    => {:.1f} MSPS/ch actual, {:.0f} frames/s, drop {:.2f}%",
                      r.throughput_msps, r.frames_per_sec, r.drop_rate * 100);
        results.push_back(result_to_json(r));
    }

    return results;
}
