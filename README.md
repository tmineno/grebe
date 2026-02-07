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

See [doc/RDD.md](doc/RDD.md) for full specification.
