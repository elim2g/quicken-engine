#!/bin/bash
# QUICKEN Engine Clean Script (Linux)
#
# Usage: ./clean.sh          Remove build outputs + premake generated files
#        ./clean.sh --all    Also remove SDL3 cmake build artifacts

set -e

echo "========================================"
echo "QUICKEN Engine Clean"
echo "========================================"
echo ""

# Remove build outputs (obj, lib, bin)
if [ -d "build" ]; then
    echo "Removing build/..."
    rm -rf build
fi

# Remove premake-generated files
if [ -f "QUICKEN.sln" ]; then
    echo "Removing premake-generated solution and projects..."
    rm -f QUICKEN.sln
fi
rm -f *.vcxproj *.vcxproj.filters *.vcxproj.user
rm -f Makefile *.make

# Full clean: also remove SDL3 build artifacts
if [ "$1" = "--all" ]; then
    echo ""
    echo "Removing SDL3 build artifacts..."
    rm -rf external/SDL3/build
    rm -rf external/SDL3/build-linux
fi

echo ""
echo "Clean complete."
if [ "$1" != "--all" ]; then
    echo "Use \"./clean.sh --all\" to also remove SDL3 build artifacts."
fi
