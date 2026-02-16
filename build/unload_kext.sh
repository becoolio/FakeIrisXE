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
