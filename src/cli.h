#pragma once

#include <cstddef>
#include <cstdint>

struct CliOptions {
    bool enable_log = false;
    bool enable_profile = false;
    bool enable_bench = false;
    bool embedded = false;          // --embedded: in-process DataGenerator (no grebe-sg)
    size_t ring_size = 16'777'216;  // 16M samples
    uint32_t num_channels = 1;
    uint32_t block_size = 16384;    // IPC block size (samples/channel/frame)
};

// Returns 0 on success, non-zero on error (caller should exit with that code).
int parse_cli(int argc, char* argv[], CliOptions& opts);
