#pragma once

#include <cstddef>
#include <cstdint>

struct CliOptions {
    bool enable_log = false;
    bool enable_profile = false;
    bool enable_bench = false;
    bool embedded = false;          // --embedded: in-process DataGenerator (no grebe-sg)
    size_t ring_size = 67'108'864;  // 64M samples
    uint32_t num_channels = 1;
    uint32_t block_size = 16384;    // IPC block size (samples/channel/frame)
    bool no_vsync = false;          // --no-vsync: disable V-Sync
};

// Returns 0 on success, non-zero on error (caller should exit with that code).
int parse_cli(int argc, char* argv[], CliOptions& opts);
