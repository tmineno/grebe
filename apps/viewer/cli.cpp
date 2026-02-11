#include "cli.h"

#include <spdlog/spdlog.h>

#include <string>

int parse_cli(int argc, char* argv[], CliOptions& opts) {
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "--log") {
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
        }
    }
    if (!opts.file_path.empty() && opts.embedded) {
        spdlog::error("--file and --embedded are mutually exclusive");
        return 1;
    }
    return 0;
}
