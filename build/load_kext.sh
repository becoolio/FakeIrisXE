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
