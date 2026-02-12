# QUICKEN Engine

A high-performance Arena FPS game engine built from scratch in C11.

## Philosophy

QUICKEN is designed for **extreme performance**. The goal is to achieve 1000+ fps on modern high-end hardware while maintaining QUAKE-like movement mechanics and Arena FPS combat.

This is not a general-purpose game engine. Every feature exists solely to serve the game.

## Features

- **TURNT Movement**: Crouchsliding mechanics similar to QUAKE 4
- **Arena FPS Combat**: Fast-paced QUAKE LIVE-style combat
- **QUAKE Map Support**: Compatible with QUAKE map formats
- **Cross-Platform**: Windows and Linux support
- **Minimal Input Latency**: Optimized for competitive play

## Setup

### Prerequisites

- **premake5**: [Download here](https://premake.github.io/download)
- **Compiler**:
  - Windows: Visual Studio 2019+ or MinGW-w64
  - Linux: GCC 9+ or Clang 10+
- **SDL3**: See below

### Getting SDL3

1. Clone SDL3 into the `external/` directory:
   ```bash
   git clone https://github.com/libsdl-org/SDL --branch main --depth 1 external/SDL3
   ```

2. Build SDL3:

   **Windows (Visual Studio):**
   ```bash
   cd external/SDL3
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```

   **Linux:**
   ```bash
   cd external/SDL3
   cmake -S . -B build
   cmake --build build
   sudo cmake --install build
   ```

### Building QUICKEN

1. Generate project files:
   ```bash
   premake5 vs2022    # For Visual Studio 2022
   premake5 gmake2    # For makefiles (Linux/MinGW)
   ```

2. Build:

   **Visual Studio**: Open `QUICKEN.sln` and build

   **Make**:
   ```bash
   make config=release
   ```

3. Run:
   ```bash
   ./build/bin/Release-[platform]-x86_64/quicken
   ```

## Project Structure

```
quicken-engine/
├── src/           # Engine source code
├── include/       # Public headers
├── external/      # Third-party libraries
├── docs/          # Documentation
├── build/         # Build output (generated)
└── premake5.lua   # Build configuration
```

## Development

See `CLAUDE.md` for AI agent instructions and development guidelines.

## Performance Goals

- **Frame Rate**: 1000+ fps on high-end hardware
- **Input Latency**: < 5ms from input to photon
- **Memory**: Minimal allocations during gameplay
- **CPU**: Efficient multi-threading for simulation and rendering

## License

TBD
