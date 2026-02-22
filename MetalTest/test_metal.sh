#!/bin/bash
# FakeIrisXE Metal Acceleration Diagnostic Script
# Run this after installing V148 and rebooting

echo "═══════════════════════════════════════════════════════════════"
echo "  FakeIrisXE Metal Acceleration Test"
echo "═══════════════════════════════════════════════════════════════"

echo ""
echo "[1] Checking FakeIrisXE in IORegistry..."
if sudo ioreg -l | grep -q "FakeIrisXE"; then
    echo "✅ FakeIrisXE found in IORegistry"
    sudo ioreg -l | grep -i "FakeIrisXE" | head -10
else
    echo "⚠️  FakeIrisXE not found in IORegistry"
fi

echo ""
echo "[2] Checking Metal Support..."
if sudo ioreg -l | grep -q "MetalSupported.*Yes"; then
    echo "✅ Metal is Supported"
    sudo ioreg -l | grep "MetalSupported"
else
    echo "❌ Metal NOT Supported"
fi

echo ""
echo "[3] Checking IOFBAccelerator..."
if sudo ioreg -l | grep -q "IOFBAccelerator.*Yes"; then
    echo "✅ IOFBAccelerator is Active"
    sudo ioreg -l | grep "IOFBAccelerator"
else
    echo "❌ IOFBAccelerator NOT Active"
fi

echo ""
echo "[4] Checking Acceleration Status..."
if sudo ioreg -l | grep -q "IOFBAccelerated.*Yes"; then
    echo "✅ GPU Acceleration is Active!"
else
    echo "❌ GPU Acceleration NOT Active"
fi

echo ""
echo "[5] Checking VRAM..."
sudo ioreg -l | grep -i "IOAccelVRAMSize\|IOAccelVideoMemorySize\|IOAccelMemorySize" | head -5

echo ""
echo "[6] Checking Accelerator Link..."
sudo ioreg -l | grep -i "IOFBAcceleratorLinked\|FakeIrisXEAcceleratorLinked" | head -5

echo ""
echo "[7] GPU Information from system_profiler..."
/usr/sbin/system_profiler SPDisplaysDataType 2>/dev/null | grep -A5 "Intel"

echo ""
echo "[8] Testing Metal with mtl device info..."
# Quick Metal test - just check if we can get device info
/usr/bin/mtl device -l 2>/dev/null || echo "(mtl command not available - install Command Line Tools)"

echo ""
echo "[9] Checking dmesg for recent FakeIrisXE messages..."
echo "   (Showing last 30 FakeIrisXE lines)"
sudo dmesg | grep -i "FakeIrisXE" | tail -30

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "  Test Complete"
echo "═══════════════════════════════════════════════════════════════"
