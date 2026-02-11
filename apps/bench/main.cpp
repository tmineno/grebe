// grebe-bench: Performance benchmark suite
// Usage: grebe-bench [--udp] [--duration=N] [--help]

#include "bench_udp.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

struct BenchOptions {
    bool run_udp      = false;
    bool run_all      = false;
    int  duration     = 5;
    uint32_t channels = 1;        // channel count for rate scenarios
    size_t datagram_size = 1400;  // max UDP datagram bytes
    uint32_t burst_size = 1;     // sendmmsg/recvmmsg batch size (1 = no batching)
    std::string json_path;  // empty = auto-generate
};

void print_help(const char* argv0) {
    std::printf(
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --all          Run all benchmarks (default if no category specified)\n"
        "  --udp          UDP loopback throughput (BM-H)\n"
        "  --channels=N       Channel count for rate scenarios (default: 1, max: 8)\n"
        "  --duration=N       Duration in seconds for transport benchmarks (default: 5)\n"
        "  --datagram-size=N  Max UDP datagram bytes (default: 1400, max: 65000)\n"
        "  --udp-burst=N      sendmmsg/recvmmsg batch size (default: 1 = no batching, Linux only)\n"
        "  --json=PATH        Output JSON path (default: ./tmp/bench_<ts>.json)\n"
        "  --help             Show this help\n",
        argv0);
}

BenchOptions parse_args(int argc, char* argv[]) {
    BenchOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--all") {
            opts.run_all = true;
        } else if (arg == "--udp") {
            opts.run_udp = true;
        } else if (arg.rfind("--channels=", 0) == 0) {
            opts.channels = static_cast<uint32_t>(std::stoi(arg.substr(11)));
            if (opts.channels < 1) opts.channels = 1;
            if (opts.channels > 8) opts.channels = 8;
        } else if (arg.rfind("--duration=", 0) == 0) {
            opts.duration = std::stoi(arg.substr(11));
        } else if (arg.rfind("--datagram-size=", 0) == 0) {
            opts.datagram_size = static_cast<size_t>(std::stoi(arg.substr(16)));
            if (opts.datagram_size > 65000) opts.datagram_size = 65000;
            if (opts.datagram_size < 128) opts.datagram_size = 128;
        } else if (arg.rfind("--udp-burst=", 0) == 0) {
            opts.burst_size = static_cast<uint32_t>(std::stoi(arg.substr(12)));
            if (opts.burst_size < 1) opts.burst_size = 1;
            if (opts.burst_size > 256) opts.burst_size = 256;
        } else if (arg.rfind("--json=", 0) == 0) {
            opts.json_path = arg.substr(7);
        } else if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            std::exit(0);
        } else {
            spdlog::warn("Unknown option: {}", arg);
        }
    }
    // Default: run all if no category specified
    if (!opts.run_udp) {
        opts.run_all = true;
    }
    return opts;
}

std::string make_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

std::string make_timestamp_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);

    spdlog::info("grebe-bench: starting (duration={}s, channels={}, datagram_size={}, burst={})",
                 opts.duration, opts.channels, opts.datagram_size, opts.burst_size);

    nlohmann::json report;
    report["timestamp"] = make_timestamp_iso();
#ifdef _WIN32
    report["platform"] = "windows";
#else
    report["platform"] = "linux";
#endif

    // --- BM-H: UDP loopback ---
    if (opts.run_udp || opts.run_all) {
        report["bm_h_udp_loopback"] = run_bench_udp(opts.duration, opts.channels,
                                                     opts.datagram_size, opts.burst_size);
    }

    // --- Write JSON report ---
    std::string json_path = opts.json_path;
    if (json_path.empty()) {
        std::filesystem::create_directories("./tmp");
        json_path = "./tmp/bench_" + make_timestamp() + ".json";
    }

    std::ofstream ofs(json_path);
    if (ofs.is_open()) {
        ofs << report.dump(2) << "\n";
        ofs.close();
        spdlog::info("Report written to: {}", json_path);
    } else {
        spdlog::error("Failed to write report to: {}", json_path);
    }

    // --- Summary ---
    spdlog::info("=== grebe-bench complete ===");

    return 0;
}
