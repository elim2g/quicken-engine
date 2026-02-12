# QUICKEN Engine - Setup Guide

## Quick Start

### 1. Install Prerequisites

**Windows:**
- Install Visual Studio 2022 (Community Edition is free)
- Download premake5 from https://premake.github.io/download and add to PATH

**Linux:**
```bash
# Ubuntu/Debian
sudo apt-get install build-essential cmake git

# Download premake5
wget https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-linux.tar.gz
tar -xvf premake-5.0.0-beta2-linux.tar.gz
sudo mv premake5 /usr/local/bin/
```

### 2. Get SDL3

```bash
# From the project root
git clone https://github.com/libsdl-org/SDL --branch main --depth 1 external/SDL3
cd external/SDL3

# Build SDL3
cmake -S . -B build
cmake --build build --config Release

# On Linux, optionally install system-wide
sudo cmake --install build
```

### 3. Generate Project Files

```bash
# Windows (Visual Studio 2022)
premake5 vs2022

# Linux or MinGW
premake5 gmake2
```

### 4. Build

**Visual Studio:**
- Open `QUICKEN.sln`
- Set configuration to "Release"
- Build Solution (Ctrl+Shift+B)

**Make:**
```bash
make config=release -j$(nproc)
```

### 5. Run

```bash
# Windows
.\build\bin\Release-windows-x86_64\quicken.exe

# Linux
./build/bin/Release-linux-x86_64/quicken
```

## Build Configurations

- **Debug**: Full debug symbols, no optimizations, assertions enabled
- **Release**: Maximum optimizations, no debug info, LTO enabled
- **RelWithDebInfo**: Optimized build with debug symbols (for profiling)

## Troubleshooting

### SDL3 Not Found
- Ensure SDL3 is cloned to `external/SDL3/`
- Verify the build completed successfully
- Check that `external/SDL3/include/` contains SDL headers

### Compiler Errors
- Ensure you're using C11-compatible compiler
- Windows: Use Visual Studio 2019 or newer
- Linux: GCC 9+ or Clang 10+

### Performance Issues
- Always use Release or RelWithDebInfo builds for testing
- Debug builds are intentionally slow
- Use a profiler (e.g., Tracy, Superluminal) for optimization

## Next Steps

Once the build works:
1. Verify the executable runs and displays the startup message
2. Begin SDL3 integration in `src/main.c`
3. Implement basic windowing and rendering
4. Set up the main game loop
