#!/bin/bash
# QUICKEN Engine Build Script (Linux)
# Automatically builds the project using makefiles.
#
# Usage: ./build.sh [release|debug|relwithdebinfo]
#
# premake5 is optional if Makefile already exists.

set -e

CONFIG="${1:-release}"

echo "========================================"
echo "QUICKEN Engine Build Script (Linux)"
echo "========================================"
echo "Configuration: $CONFIG"
echo ""

# Check for required tools
if ! command -v make &> /dev/null; then
    echo "[ERROR] make not found!"
    echo "Install: sudo apt-get install build-essential"
    exit 1
fi

if ! command -v cmake &> /dev/null; then
    echo "[ERROR] cmake not found!"
    echo "Install: sudo apt-get install cmake"
    exit 1
fi

# Ensure SDL3 submodule is initialized
if [ ! -f "external/SDL3/CMakeLists.txt" ]; then
    echo "SDL3 submodule not found. Initializing..."
    git submodule update --init
fi

# Build SDL3 if not already built
if [ ! -f "external/SDL3/build-linux/libSDL3.so" ]; then
    echo "Building SDL3..."
    cmake -S external/SDL3 -B external/SDL3/build-linux
    cmake --build external/SDL3/build-linux -j$(nproc)
fi

# Try to regenerate makefiles with premake5
if command -v premake5 &> /dev/null; then
    echo "Generating makefiles..."
    premake5 gmake
elif [ -f "Makefile" ]; then
    echo "[NOTE] premake5 not found. Using existing Makefile."
else
    echo "[ERROR] premake5 not found and no Makefile exists!"
    echo "Install premake5 and add it to your PATH."
    exit 1
fi

# Build the project
echo "Building..."
make config=$CONFIG -j$(nproc)

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "Build succeeded!"
    echo "========================================"
    echo ""
    echo "Client: build/bin/$CONFIG-linux-x86_64/quicken"
    echo "Server: build/bin/$CONFIG-linux-x86_64/quicken-server"
    echo ""
else
    echo ""
    echo "========================================"
    echo "Build failed!"
    echo "========================================"
    echo ""
    exit 1
fi
