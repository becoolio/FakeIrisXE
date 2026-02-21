#!/bin/bash

# Build script for FakeIrisXE Test App

set -e

echo "Building FakeIrisXE Test App..."

# Create output directory
mkdir -p build

# Compile compliance test
clang -framework IOKit -framework IOSurface -framework CoreFoundation \
    -o build/fxe_compliance_test \
    fxe_compliance_test.c

# Compile user-space ABI test
clang++ -std=c++17 -framework IOKit -framework IOSurface -framework CoreFoundation \
    -o build/FakeIrisXETest \
    FakeIrisXETest.cpp

echo "✅ Build complete:"
echo "  - build/FakeIrisXETest"
echo "  - build/fxe_compliance_test"
echo ""
echo "Usage:"
echo "  sudo ./build/FakeIrisXETest"
echo "  sudo ./build/fxe_compliance_test"
