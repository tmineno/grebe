# Grebe

High-speed real-time waveform rendering framework for 16-bit time-series data streams up to 1 GSPS per channel.

**libgrebe** is a pure C++ data pipeline library (ingestion → ring buffer → decimation) with a single dependency (spdlog). **grebe-viewer** is a reference Vulkan application that visualizes the decimated output.

## Prerequisites

- CMake 3.24+
- C++20 compiler (GCC 12+ / Clang 15+ / MSVC 2022+)
- Vulkan SDK 1.3+ (with `glslc`)

All other dependencies are fetched automatically via CMake FetchContent.

## Project Structure

```
include/grebe/          Public API headers (IDataSource, IRenderBackend, DecimationEngine, etc.)
src/                    libgrebe core (decimation, ring buffer, ingestion, synthetic source)
apps/
  viewer/               grebe-viewer (Vulkan renderer, HUD, profiler, benchmarks, IPC source)
  sg/                   grebe-sg (signal generator process, OpenGL GUI, IPC pipe output)
  common/ipc/           Shared IPC protocol (contracts, transport, pipe implementation)
  bench/                grebe-bench (standalone benchmark suite — stub)
doc/                    RDD, TR-001, TODO, technical investigation reports
```

## Architecture

```
┌─ Application (grebe-viewer) ──────────────────────────────────────────┐
│  VulkanRenderer (Vulkan + ImGui HUD)                                  │
│  IpcSource / Benchmark / Profiler / ProcessHandle                     │
├───────────────────────────────────────────────────────────────────────┤
│  libgrebe (data pipeline only, spdlog dependency)                     │
│                                                                       │
│  IDataSource → IngestionThread → N × RingBuffer → DecimationEngine   │
│  (abstract)    (DataSource→Ring)  (lock-free SPSC)  (MinMax/LTTB)    │
└───────────────────────────────────────────────────────────────────────┘
```

Two operational modes:

- **IPC mode** (default): grebe-viewer spawns grebe-sg, which generates data and sends it over a pipe
- **Embedded mode** (`--embedded`): single-process, SyntheticSource generates data in-process

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
# IPC mode (default): grebe-viewer spawns grebe-sg automatically
./build/grebe-viewer

# Embedded mode: single-process, SyntheticSource in-process
./build/grebe-viewer --embedded

# Multi-channel (1-8 channels)
./build/grebe-viewer --channels=4

# Or with preset
cmake --build --preset linux-release --target run
```

## Controls

### grebe-viewer

| Key | Action |
|---|---|
| Esc | Quit |
| V | Toggle V-Sync |
| D | Cycle decimation mode (None → MinMax → LTTB) |
| 1-4 | Set sample rate 1M/10M/100M/1G (embedded mode only) |
| Space | Pause/Resume data generation (embedded mode only) |

In IPC mode, sample rate and pause are controlled via the grebe-sg GUI window.

## CLI Options

### grebe-viewer

| Option | Default | Description |
|---|---|---|
| `--embedded` | off | Single-process mode (no grebe-sg) |
| `--channels=N` | 1 | Number of channels (1-8) |
| `--ring-size=SIZE` | 64M | Ring buffer size (K/M/G suffix supported) |
| `--block-size=SIZE` | 16384 | IPC block size per channel per frame |
| `--no-vsync` | off | Disable V-Sync at startup |
| `--minimized` | off | Start window iconified (useful for headless profiling) |
| `--log` | off | CSV telemetry logging to `./tmp/` |
| `--profile` | off | Automated profiling (1/10/100M/1G SPS), JSON report to `./tmp/` |
| `--bench` | off | Isolated microbenchmarks (BM-A through BM-F), JSON to `./tmp/` |

### grebe-sg (signal generator)

grebe-sg is spawned automatically by grebe-viewer in IPC mode. It provides an OpenGL/ImGui control panel for sample rate, waveform type, and block size. It also accepts CLI arguments:

| Option | Default | Description |
|---|---|---|
| `--channels=N` | 1 | Number of channels (1-8) |
| `--ring-size=SIZE` | 64M | Ring buffer size |
| `--block-size=SIZE` | 16384 | Samples per channel per IPC frame |

### Examples

```bash
# Interactive with 4 channels, V-Sync off
./build/grebe-viewer --channels=4 --no-vsync

# Embedded mode with CSV telemetry
./build/grebe-viewer --embedded --log

# Automated profiling with large ring buffer
./build/grebe-viewer --embedded --profile --ring-size=64M

# Isolated microbenchmarks
./build/grebe-viewer --bench
```

## Benchmarks

Convenience targets for profiling and benchmarks:

```bash
cmake --build --preset linux-release --target profile
cmake --build --preset linux-release --target bench-run
```

## Documentation

- [doc/RDD.md](doc/RDD.md) — Requirements definition (v2.1.0)
- [doc/TR-001.md](doc/TR-001.md) — PoC technical report (performance evidence)
- [doc/TI-phase7.md](doc/TI-phase7.md) — Phase 7 refactoring regression verification
- [doc/TODO.md](doc/TODO.md) — Development milestones and roadmap

## License

MIT
