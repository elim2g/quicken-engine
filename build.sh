#!/bin/bash
# QUICKEN Engine Build Script (Linux)
# Automatically builds the project using makefiles

set -e  # Exit on error

# Default to Release build
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

if ! command -v premake5 &> /dev/null; then
    echo "[ERROR] premake5 not found!"
    echo "Install premake5 and add it to your PATH"
    exit 1
fi

# Always regenerate makefiles to ensure they target the current platform
echo "Generating makefiles..."
premake5 gmake

# Build the project
echo "Building..."
make config=$CONFIG -j$(nproc)

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "Build succeeded!"
    echo "========================================"
    echo ""
    echo "Executable: build/bin/$CONFIG-linux-x86_64/quicken"
    echo ""
else
    echo ""
    echo "========================================"
    echo "Build failed!"
    echo "========================================"
    echo ""
    exit 1
fi
