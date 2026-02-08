# Grebe

**Proof of Concept** — High-speed time-series data stream visualization using Vulkan.

Explores the performance limits of a Vulkan-based rendering pipeline for 16-bit / 1 GSPS class ADC data visualization. Measures throughput and bottlenecks at each pipeline stage (data generation, decimation, GPU transfer, draw).

## Prerequisites

- CMake 3.24+
- C++20 compiler (GCC 12+ / Clang 15+ / MSVC 2022+)
- Vulkan SDK 1.3+ (with `glslc`)

All other dependencies are fetched automatically via CMake FetchContent.

## Architecture

Two-process architecture with IPC pipe (default) or single-process embedded mode:

```
[grebe-sg]  data_generator → ring_buffer → pipe_transport (stdout)
                                                │
                                                ▼
[grebe]     ipc_receiver → ring_buffer(s) → decimation_thread → buffer_manager → renderer
```

**Embedded mode** (`--embedded`): single-process, DataGenerator runs in-process (no grebe-sg):

```
data_generator (thread) → ring_buffer(s) → decimation_thread → buffer_manager → renderer
```

- **Data generator**: Period tiling (memcpy) for periodic waveforms at ≥100 MSPS achieves true 1 GSPS throughput. LUT-based fallback for chirp and low-rate modes.
- **Ring buffer**: Lock-free SPSC with bulk memcpy push/pop. Configurable size (default 64M samples).
- **Decimation**: MinMax (SSE2 SIMD) or LTTB, reducing any input to 3840 vertices/frame per channel.
- **GPU upload**: Triple-buffered staging → device copy with async transfer.
- **Renderer**: Vulkan LINE_STRIP pipeline, int16 vertex format (2 bytes/vertex), per-channel draw calls with push constants.
- **IPC transport**: Binary pipe protocol (FrameHeaderV2 + interleaved channel data), sequence continuity and drop tracking.

## Build

### Using CMake Presets (recommended)

```bash
# Linux
cmake --workflow --preset linux-release

# Windows native (from WSL2, requires VS2022 + Vulkan SDK on Windows)
cmake --workflow --preset windows-release
```

### Manual

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

```bash
# IPC mode (default): grebe spawns grebe-sg automatically
./build/grebe

# Embedded mode: single-process, DataGenerator in-process
./build/grebe --embedded

# Multi-channel (1-8 channels)
./build/grebe --channels=4

# Or with preset
cmake --build --preset linux-release --target run
```

## Controls

### grebe (viewer)

| Key | Action |
|---|---|
| Esc | Quit |
| V | Toggle V-Sync |
| D | Cycle decimation mode (None → MinMax → LTTB) |
| 1-4 | Set sample rate 1M/10M/100M/1G (embedded mode only) |
| Space | Pause/Resume data generation (embedded mode only) |

In IPC mode, sample rate and pause are controlled by the grebe-sg process.

## CLI Options

### grebe (viewer)

| Option | Default | Description |
|---|---|---|
| `--embedded` | off | Single-process mode (no grebe-sg) |
| `--channels=N` | 1 | Number of channels (1-8) |
| `--ring-size=SIZE` | 64M | Ring buffer size (K/M/G suffix supported) |
| `--block-size=SIZE` | 16384 | IPC block size per channel per frame (power of 2, 1024-65536) |
| `--no-vsync` | off | Disable V-Sync at startup |
| `--log` | off | CSV telemetry logging to `./tmp/` |
| `--profile` | off | Automated profiling (1/10/100M/1G SPS), JSON report to `./tmp/` |
| `--bench` | off | Isolated microbenchmarks (BM-A through BM-E), JSON to `./tmp/` |

### grebe-sg (signal generator)

grebe-sg is spawned automatically by grebe in IPC mode. It accepts:

| Option | Default | Description |
|---|---|---|
| `--channels=N` | 1 | Number of channels (1-8) |
| `--ring-size=SIZE` | 64M | Ring buffer size |
| `--block-size=SIZE` | 16384 | Samples per channel per IPC frame |

### Examples

```bash
# Interactive with 4 channels, V-Sync off
./build/grebe --channels=4 --no-vsync

# Embedded mode with CSV telemetry
./build/grebe --embedded --log

# Automated profiling with large ring buffer
./build/grebe --embedded --profile --ring-size=64M

# Isolated microbenchmarks
./build/grebe --bench
```

## Benchmarks

Convenience targets for profiling and benchmarks:

```bash
cmake --build --preset linux-release --target profile
cmake --build --preset linux-release --target bench
```

## Documentation

- [doc/RDD.md](doc/RDD.md) — Requirements and specification
- [doc/technical_judgment.md](doc/technical_judgment.md) — Technical investigation notes (TI-01 through TI-10)
- [doc/TODO.md](doc/TODO.md) — Milestones and future work

## License

MIT
