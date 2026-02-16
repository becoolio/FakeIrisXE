#!/bin/bash
# Install V104 FakeIrisXE kext
# Run with: sudo bash install_kext.sh

set -e

KEXT_SOURCE="/Users/becoolio/Documents/Github/Untitled/FakeIrisXE/build/Release/FakeIrisXE.kext"
KEXT_DEST="/Library/Extensions/FakeIrisXE.kext"

echo "Removing old kext..."
rm -rf "$KEXT_DEST"

echo "Copying new kext..."
cp -R "$KEXT_SOURCE" "$KEXT_DEST"

echo "Fixing permissions..."
chown -R root:wheel "$KEXT_DEST"
chmod -R 755 "$KEXT_DEST"

echo "Rebuilding kext cache..."
kextcache -m /System/Library/Caches/com.apple.kext.caches/Startup/Extensions.mkext "$KEXT_DEST" 2>/dev/null || true

echo "Done! Please reboot."
echo ""
echo "V104 Changes:"
echo "- Linux DMA (0x5820 offsets) - tries first"
echo "- GT Power Management (PWR_WELL_CTL2/3)"
echo "- RPS/Frequency Control (RC6)"
echo "- Apple DMA fallback"
echo "- Enhanced GuC diagnostics"
