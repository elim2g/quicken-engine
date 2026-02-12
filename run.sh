#!/bin/bash
# QUICKEN Engine Run Script (Linux)
# Builds and runs the game

set -e

CONFIG="${1:-release}"

echo "Building QUICKEN Engine ($CONFIG)..."
./build.sh "$CONFIG"

echo ""
echo "========================================"
echo "Running QUICKEN Engine"
echo "========================================"
echo ""

EXE="build/bin/$CONFIG-linux-x86_64/quicken"

if [ ! -f "$EXE" ]; then
    echo "[ERROR] Executable not found: $EXE"
    exit 1
fi

"$EXE"
