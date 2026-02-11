#include "cli.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>

static void print_help() {
    std::puts(
        "grebe-viewer — High-speed real-time waveform viewer\n"
        "\n"
        "Usage: grebe-viewer [OPTIONS]\n"
        "\n"
        "Modes:\n"
        "  (default)        Pipe mode: auto-spawn grebe-sg subprocess\n"
        "  --embedded       Single-process mode (SyntheticSource, no grebe-sg)\n"
        "  --udp=PORT       UDP mode: listen on PORT for external grebe-sg\n"
        "\n"
        "Options:\n"
        "  --channels=N     Number of channels, 1-8 (default: 1)\n"
        "  --ring-size=SIZE Ring buffer size with K/M/G suffix (default: 64M)\n"
        "  --block-size=N   Samples per channel per frame, power of 2 (default: 16384)\n"
        "  --file=PATH      Binary file playback (.grb format, via grebe-sg)\n"
        "  --no-vsync       Disable V-Sync at startup\n"
        "  --minimized      Start window iconified\n"
        "  --log            CSV telemetry logging to ./tmp/\n"
        "  --profile        Automated profiling, JSON report to ./tmp/\n"
        "  --bench          Isolated microbenchmarks, JSON to ./tmp/\n"
        "  --help           Show this help and exit\n"
    );
}

int parse_cli(int argc, char* argv[], CliOptions& opts) {
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 2;  // signal caller to exit with code 0
        } else if (arg == "--log") {
            opts.enable_log = true;
        } else if (arg == "--profile") {
            opts.enable_profile = true;
        } else if (arg == "--bench") {
            opts.enable_bench = true;
        } else if (arg.rfind("--ring-size=", 0) == 0) {
            std::string val = arg.substr(12);
            size_t multiplier = 1;
            if (!val.empty()) {
                char suffix = val.back();
                if (suffix == 'M' || suffix == 'm') { multiplier = 1024ULL * 1024; val.pop_back(); }
                else if (suffix == 'G' || suffix == 'g') { multiplier = 1024ULL * 1024 * 1024; val.pop_back(); }
                else if (suffix == 'K' || suffix == 'k') { multiplier = 1024; val.pop_back(); }
            }
            opts.ring_size = std::stoull(val) * multiplier;
        } else if (arg == "--embedded") {
            opts.embedded = true;
        } else if (arg.rfind("--channels=", 0) == 0) {
            opts.num_channels = static_cast<uint32_t>(std::stoul(arg.substr(11)));
            if (opts.num_channels < 1 || opts.num_channels > 8) {
                spdlog::error("--channels must be 1-8, got {}", opts.num_channels);
                return 1;
            }
        } else if (arg == "--no-vsync") {
            opts.no_vsync = true;
        } else if (arg == "--minimized") {
            opts.minimized = true;
        } else if (arg.rfind("--block-size=", 0) == 0) {
            opts.block_size = static_cast<uint32_t>(std::stoul(arg.substr(13)));
            if (opts.block_size < 1024 || opts.block_size > 65536 ||
                (opts.block_size & (opts.block_size - 1)) != 0) {
                spdlog::error("--block-size must be power of 2, 1024-65536, got {}", opts.block_size);
                return 1;
            }
        } else if (arg.rfind("--file=", 0) == 0) {
            opts.file_path = arg.substr(7);
        } else if (arg.rfind("--udp=", 0) == 0) {
            opts.udp_port = static_cast<uint16_t>(std::stoul(arg.substr(6)));
        }
    }
    if (!opts.file_path.empty() && opts.embedded) {
        spdlog::error("--file and --embedded are mutually exclusive");
        return 1;
    }
    if (opts.udp_port > 0 && opts.embedded) {
        spdlog::error("--udp and --embedded are mutually exclusive");
        return 1;
    }
    return 0;
}
