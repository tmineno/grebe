# Grebe

High-speed time-series data stream visualization using Vulkan.

## Prerequisites

- CMake 3.24+
- C++20 compiler (GCC 12+ / Clang 15+ / MSVC 2022+)
- Vulkan SDK 1.3+ (with `glslc`)

All other dependencies are fetched automatically via CMake FetchContent.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Run

```bash
./build/vulkan-stream-poc
```

## Controls

| Key | Action |
|---|---|
| Esc | Quit |
| V | Toggle V-Sync |
| 1 | 1 MSPS sample rate |
| 2 | 10 MSPS sample rate |
| 3 | 100 MSPS sample rate |
| 4 | 1 GSPS sample rate |
| Space | Pause/Resume data generation |
| D | Cycle decimation mode (None → MinMax → LTTB) |

## CLI Options

```bash
# Normal interactive mode
./build/vulkan-stream-poc

# Enable CSV telemetry logging to ./tmp/
./build/vulkan-stream-poc --log

# Run automated profiling (headless benchmark), outputs JSON report to ./tmp/
./build/vulkan-stream-poc --profile
```

## Profile Report

`--profile` runs three scenarios (1 MSPS, 10 MSPS, 100 MSPS) with 120 warmup frames and 300 measurement frames each. Results are written to `./tmp/profile_<timestamp>.json`.

Each scenario reports statistics (avg, min, max, p50, p95, p99) for:

| Metric | Description |
|---|---|
| `fps` | Frames per second |
| `frame_ms` | Total frame time (ms) |
| `drain_ms` | Ring buffer drain time (ms) |
| `decimate_ms` | Decimation processing time (ms) |
| `upload_ms` | GPU buffer upload time (ms) |
| `swap_ms` | Double-buffer swap time (ms) |
| `render_ms` | Vulkan render + present time (ms) |
| `samples_per_frame` | Raw samples consumed per frame |
| `vertex_count` | Vertices sent to GPU per frame |
| `data_rate` | Effective data rate (samples/sec) |
| `ring_fill` | Ring buffer fill ratio (0.0–1.0) |

Each scenario passes if `fps.avg >= 30`. The process exits with code 0 if all scenarios pass, 1 otherwise.

See [doc/RDD.md](doc/RDD.md) for full specification.
