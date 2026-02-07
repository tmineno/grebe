# Grebe

**Proof of Concept** — High-speed time-series data stream visualization using Vulkan.

Explores the performance limits of a Vulkan-based rendering pipeline for 16-bit / 1 GSPS class ADC data visualization. Measures throughput and bottlenecks at each pipeline stage (data generation, decimation, GPU transfer, draw).

## Prerequisites

- CMake 3.24+
- C++20 compiler (GCC 12+ / Clang 15+ / MSVC 2022+)
- Vulkan SDK 1.3+ (with `glslc`)

All other dependencies are fetched automatically via CMake FetchContent.

## Architecture

```
data_generator (thread) → ring_buffer (SPSC) → decimation_thread → buffer_manager → renderer (LINE_STRIP)
```

- **Data generator**: Period tiling (memcpy) for periodic waveforms at ≥100 MSPS achieves true 1 GSPS throughput. LUT-based fallback for chirp and low-rate modes.
- **Ring buffer**: Lock-free SPSC with bulk memcpy push/pop. Configurable size (default 16M, use 64M+ for 1 GSPS).
- **Decimation**: MinMax (SSE2 SIMD) or LTTB, reducing any input to 3840 vertices/frame.
- **GPU upload**: Triple-buffered staging → device copy with async transfer.
- **Renderer**: Vulkan LINE_STRIP pipeline, int16 vertex format (2 bytes/vertex).

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
# Linux
cmake --build --preset linux-release --target run

# Windows (from WSL2)
cmake --build --preset windows-release --target run

# Or directly
./build/release/grebe
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
./build/release/grebe

# Multi-channel (1-8 channels)
./build/release/grebe --channels=4

# Enable CSV telemetry logging to ./tmp/
./build/release/grebe --log

# Run automated profiling (headless benchmark), outputs JSON report to ./tmp/
./build/release/grebe --profile

# Run profiling with larger ring buffer (needed for 1 GSPS)
./build/release/grebe --profile --ring-size=64M

# Run isolated microbenchmarks (BM-A through BM-E), outputs JSON to ./tmp/
./build/release/grebe --bench
```

## Benchmarks

Convenience targets for profiling and benchmarks:

```bash
cmake --build --preset linux-release --target profile
cmake --build --preset linux-release --target bench
```

## Documentation

- [doc/RDD.md](doc/RDD.md) — Requirements and specification
- [doc/technical_judgment.md](doc/technical_judgment.md) — Technical investigation notes (TI-01 through TI-06)
- [doc/TODO.md](doc/TODO.md) — Milestones and future work

## License

MIT
