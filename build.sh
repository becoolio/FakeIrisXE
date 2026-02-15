#!/bin/bash
#
# Build script for FakeIrisXe V52 Test Kext
# Based on mac-gfx-research Apple DMA implementation + Linux i915 fallback
#

set -e

echo "=========================================="
echo "FakeIrisXe V52 Test Build Script"
echo "Apple+Linux DMA Fallback Implementation"
echo "=========================================="
echo ""

# Configuration
PROJECT_NAME="FakeIrisXE"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
DERIVED_DATA_DIR="${BUILD_DIR}/DerivedData"
KEXT_NAME="FakeIrisXE.kext"
KEXT_BUNDLE_ID="com.anomy.driver.FakeIrisXEFramebuffer"

# SDK and Deployment Target
SDK_VERSION="macosx"
MACOSX_DEPLOYMENT_TARGET="11.0"

# Clean previous build
echo "[1/6] Cleaning previous build..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

# Create build directory structure
echo "[2/6] Setting up build environment..."
mkdir -p "${DERIVED_DATA_DIR}"

# Build using xcodebuild
echo "[3/6] Building kext with Xcode..."
cd "${PROJECT_DIR}"

xcodebuild \
    -project "${PROJECT_NAME}.xcodeproj" \
    -scheme "${PROJECT_NAME}" \
    -configuration "Release" \
    -sdk "${SDK_VERSION}" \
    -derivedDataPath "${DERIVED_DATA_DIR}" \
    MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGN_IDENTITY="" \
    build

# Check if build succeeded
if [ $? -ne 0 ]; then
    echo "❌ Build failed!"
    exit 1
fi

# Locate built kext
BUILT_KEXT=$(find "${DERIVED_DATA_DIR}" -name "${KEXT_NAME}" -type d | head -n1)

if [ -z "$BUILT_KEXT" ]; then
    echo "❌ Could not find built kext!"
    exit 1
fi

echo "✅ Build successful!"
echo "   Location: ${BUILT_KEXT}"

# Copy to build directory
echo "[4/6] Copying kext to build directory..."
cp -R "${BUILT_KEXT}" "${BUILD_DIR}/${KEXT_NAME}"

# Fix permissions
echo "[5/6] Setting permissions..."
chmod -R 755 "${BUILD_DIR}/${KEXT_NAME}"
chmod 644 "${BUILD_DIR}/${KEXT_NAME}/Contents/Info.plist"

# Create load/unload scripts
echo "[6/6] Creating test scripts..."

cat > "${BUILD_DIR}/load_kext.sh" << 'EOF'
#!/bin/bash
# Load FakeIrisXe kext for testing

echo "Loading FakeIrisXe V52..."
echo "Note: Requires root privileges and SIP disabled"

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "❌ Please run as root (use sudo)"
    exit 1
fi

# Unload existing if present
if kextstat | grep -q "FakeIrisXE"; then
    echo "Unloading existing kext..."
    kextunload -b com.anomy.driver.FakeIrisXEFramebuffer
    sleep 2
fi

# Load new kext
echo "Loading kext..."
kextload "$(dirname "$0")/FakeIrisXE.kext"

# Check status
sleep 2
if kextstat | grep -q "FakeIrisXE"; then
    echo "✅ Kext loaded successfully!"
    echo ""
    echo "View logs with:"
    echo "  sudo log show --predicate 'sender == \"FakeIrisXE\"' --last 5m"
else
    echo "❌ Kext failed to load!"
    echo ""
    echo "Check system logs:"
    echo "  sudo log show --predicate 'eventMessage contains \"FakeIrisXE\"' --last 5m"
fi
EOF

cat > "${BUILD_DIR}/unload_kext.sh" << 'EOF'
#!/bin/bash
# Unload FakeIrisXe kext

echo "Unloading FakeIrisXe..."

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "❌ Please run as root (use sudo)"
    exit 1
fi

if kextstat | grep -q "FakeIrisXE"; then
    kextunload -b com.anomy.driver.FakeIrisXEFramebuffer
    echo "✅ Kext unloaded"
else
    echo "ℹ️  Kext not loaded"
fi
EOF

cat > "${BUILD_DIR}/view_logs.sh" << 'EOF'
#!/bin/bash
# View FakeIrisXe logs

echo "Showing FakeIrisXe logs (last 5 minutes)..."
echo ""
echo "=========================================="
sudo log show --predicate 'sender == "FakeIrisXE"' --last 5m --style compact
EOF

chmod +x "${BUILD_DIR}/load_kext.sh"
chmod +x "${BUILD_DIR}/unload_kext.sh"
chmod +x "${BUILD_DIR}/view_logs.sh"

# Create README for testing
cat > "${BUILD_DIR}/TESTING.md" << 'EOF'
# FakeIrisXe V52 Test Build

## What's New in V52

### Apple-Style DMA Implementation
- Based on reverse engineering of Apple's Intel GPU driver (mac-gfx-research)
- Uses magic DMA trigger value `0xFFFF0011` 
- Implements Apple's status polling method
- Checks for status byte `0xF0` (success) or `0xA0/0x60` (failure)

### Linux i915 Fallback
- Tries Linux DMA method first (START_DMA | UOS_MOVE)
- Falls back to Apple method if Linux fails
- Maximizes compatibility across different GPU generations

### Register Offsets
- Linux-style: 0x5820-0x5834 (standard Intel)
- Apple-style: 0x1C570-0x1C584 (from mac-gfx-research analysis)

## Installation

### Prerequisites
1. macOS with SIP disabled (for testing):
   ```bash
   # In Recovery Mode (Cmd+R on boot)
   csrutil disable
   ```

2. Root privileges

### Loading the Kext

```bash
cd build
sudo ./load_kext.sh
```

### Unloading the Kext

```bash
sudo ./unload_kext.sh
```

### Viewing Logs

```bash
# Real-time logs
sudo log stream --predicate 'sender == "FakeIrisXE"'

# Last 5 minutes
sudo ./view_logs.sh
```

## Expected Behavior

### Successful DMA Upload
```
[V52] Attempting firmware upload with fallback...
[V52] Attempt 1: Linux-style DMA upload
[V52] ✅ Linux-style DMA succeeded!
[V52] ✅ Firmware uploaded to GPU WOPCM via DMA
```

### Fallback to Apple Method
```
[V52] Attempt 1: Linux-style DMA upload
[V52] ⚠️ Linux-style DMA failed, trying Apple-style...
[V52] Attempt 2: Apple-style DMA upload
[V52]     Poll 0: STATUS=0xXXXXXX, byte=0xF0
[V52] ✅ GuC firmware loaded successfully!
```

### Failure Case
```
[V52] ❌ Both DMA methods failed!
[V52] ⚠️ Continuing without DMA (may not work on Gen12+)
```

## Troubleshooting

### Kext Won't Load
1. Check SIP is disabled: `csrutil status`
2. Check permissions: `ls -la FakeIrisXE.kext/`
3. Check for existing kext: `kextstat | grep FakeIrisXE`

### DMA Upload Fails
- Expected on some hardware - driver falls back to non-DMA mode
- Check logs to see which method was tried
- Gen12+ Tiger Lake requires DMA for proper operation

### System Logs
```bash
# All system logs mentioning FakeIrisXE
sudo log show --predicate 'eventMessage contains "FakeIrisXE"' --last 10m

# Kernel logs
sudo dmesg | grep -i fakeiris
```

## Supported Hardware

Tested on:
- Tiger Lake (Gen12) - 0x9A49, 0x46A3
- May work on other Intel GPUs with similar architecture

## Version History

- **V52**: Apple+Linux DMA fallback implementation
- **V51**: Initial Linux-style DMA upload
- **V50**: GuC initialization with execlist fallback

## References

- mac-gfx-research: https://github.com/pawan295/mac-gfx-research
- Linux i915: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/i915/gt/uc
EOF

echo ""
echo "=========================================="
echo "✅ Build Complete!"
echo "=========================================="
echo ""
echo "Build outputs:"
echo "  Kext: ${BUILD_DIR}/${KEXT_NAME}"
echo "  Load: ${BUILD_DIR}/load_kext.sh"
echo "  Unload: ${BUILD_DIR}/unload_kext.sh"
echo "  Logs: ${BUILD_DIR}/view_logs.sh"
echo "  Docs: ${BUILD_DIR}/TESTING.md"
echo ""
echo "To test:"
echo "  cd ${BUILD_DIR}"
echo "  sudo ./load_kext.sh"
echo ""
