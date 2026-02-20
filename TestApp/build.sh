#!/bin/bash

# Build script for FakeIrisXE Test App

set -e

echo "Building FakeIrisXE Test App..."

# Create output directory
mkdir -p build

# Compile the test app
clang -framework IOKit -framework IOSurface -framework CoreFoundation \
    -o build/FakeIrisXETest \
    main.c

echo "✅ Build complete: build/FakeIrisXETest"
echo ""
echo "Usage:"
echo "  sudo ./build/FakeIrisXETest           # Run all tests"
echo "  sudo ./build/FakeIrisXETest caps      # Test GetCaps only"
echo "  sudo ./build/FakeIrisXETest context   # Test Create/DestroyContext"
echo "  sudo ./build/FakeIrisXETest surface   # Test BindSurface with IOSurface"
echo "  sudo ./build/FakeIrisXETest present   # Test Present"
echo "  sudo ./build/FakeIrisXETest ring      # Test shared ring"
