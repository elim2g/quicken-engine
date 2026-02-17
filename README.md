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
- **CMake**: [Download here](https://cmake.org/download/)
- **Vulkan SDK**: [Download here](https://vulkan.lunarg.com/sdk/home)
- **Compiler**:
  - Windows: Visual Studio 2019+ or Visual Studio Build Tools
  - Linux: GCC 9+ or Clang 10+

### Clone

```bash
git clone --recursive <repo-url>
```

If you already cloned without `--recursive`:
```bash
git submodule update --init
```

### Build and Run

The build scripts automatically compile SDL3 on first run.

**Windows:**
```bat
build.bat
run.bat
```

**Linux:**
```bash
./build.sh
./run.sh
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
