#!/bin/bash

# Build script for V142 Rendering Pipeline Test

set -e

echo "Building FakeIrisXE V142 Rendering Pipeline Test..."

# Create output directory
mkdir -p build

# Compile the test app
echo "Compiling v142_render_test.c..."
clang -O0 -g \
    -framework IOKit \
    -framework IOSurface \
    -framework CoreFoundation \
    -framework CoreGraphics \
    -o build/v142_render_test \
    v142_render_test.c

# Sign with entitlements (ad-hoc signature for testing)
echo "Signing with entitlements..."
codesign --force --sign - \
    --entitlements v142_render_test.entitlements \
    build/v142_render_test

echo ""
echo "✅ Build complete: build/v142_render_test"
echo ""
echo "Usage:"
echo "  sudo ./build/v142_render_test"
echo ""
echo "Expected output:"
echo "  - Creates IOSurface with gradient pattern"
echo "  - Binds to context"
echo "  - Submits PRESENT command via shared ring"
echo "  - Calls Present to trigger display update"
echo "  - You should see a gradient on screen!"
echo ""
echo "Note: Run with sudo for IOKit access"
