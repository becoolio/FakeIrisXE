#!/bin/bash
# FakeIrisXE Kext Installation Script
# Run with: sudo ./install_kext.sh

set -e

KEXT_NAME="FakeIrisXE"
KEXT_SOURCE="/Users/becoolio/Documents/Github/Untitled/FakeIrisXE/build/FakeIrisXE.kext"
KEXT_DEST="/Library/Extensions/${KEXT_NAME}.kext"

echo "=========================================="
echo "FakeIrisXE Kext Installation"
echo "=========================================="

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo "Please run with sudo:"
    echo "  sudo ./install_kext.sh"
    exit 1
fi

# Remove old kext if exists
if [ -d "$KEXT_DEST" ]; then
    echo "Removing old kext..."
    rm -rf "$KEXT_DEST"
fi

# Copy new kext
echo "Copying new kext..."
cp -R "$KEXT_SOURCE" "$KEXT_DEST"

# Fix ownership to root:wheel
echo "Fixing ownership to root:wheel..."
chown -R root:wheel "$KEXT_DEST"

# Fix permissions
echo "Fixing permissions..."
chmod -R 755 "$KEXT_DEST"

# Touch to update modification time
touch "$KEXT_DEST"

# Rebuild kext cache
echo "Rebuilding kext cache..."
kextcache -i /

echo ""
echo "=========================================="
echo "Installation complete!"
echo "=========================================="
echo ""
echo "NOTE: You may need to approve the kext in System Preferences > Security & Privacy"
echo "If prompted, click 'Allow' to load the kext."
echo ""
echo "To load the kext manually:"
echo "  sudo kextload /Library/Extensions/FakeIrisXE.kext"
echo ""
echo "To check if loaded:"
echo "  kextstat | grep FakeIrisXE"
echo ""
