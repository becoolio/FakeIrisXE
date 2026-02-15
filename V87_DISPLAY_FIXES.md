# FakeIrisXE V87 - Display Fixes

## Issues Fixed

### Issue 1: No Color Bars Visible
**Problem:** Test pattern was written AFTER the display plane was enabled

**Root Cause:** 
- Plane was enabled at line ~2457
- Test pattern was written much later at line ~2586
- GPU started scanning immediately when plane enabled, saw only black
- By the time test pattern was written, display was already showing black

**Fix (V87):**
```cpp
// V87: Write test pattern BEFORE enabling plane
// This ensures GPU sees colors immediately
writeTestPattern();  // <- Moved here (BEFORE plane enable)
enablePlane();       // <- Plane sees colors right away
```

### Issue 2: Only 1 Resolution Available
**Problem:** `currentMode` initialized to 0 instead of 1

**Root Cause:**
```cpp
// OLD (V86 and earlier):
currentMode = 0;  // Mode 0 doesn't exist!
```

**Fix (V87):**
```cpp
// NEW (V87):
currentMode = 1;  // Start with 1920x1080 (mode 1)
```

This ensures `getCurrentDisplayMode()` returns a valid mode immediately.

---

## Changes in V87

### 1. Test Pattern Timing
**Location:** `enableController()` method
**Change:** Moved test pattern writing to BEFORE plane enable

```cpp
// NEW ORDER:
1. Map framebuffer into GGTT
2. WRITE TEST PATTERN (colors + borders)
3. Memory barrier + flush
4. Program plane surface address
5. ENABLE PLANE (now sees colors immediately)
```

### 2. Mode Initialization
**Location:** Constructor
**Change:** Initialize `currentMode = 1` instead of `0`

```cpp
// OLD:
currentMode = 0;  // Invalid mode

// NEW:
currentMode = 1;  // 1920x1080 @ 60Hz
```

---

## Expected Results

### What You Should See Now:

1. **During Boot:**
   ```
   [V87] Writing test pattern BEFORE enabling plane...
   [V87] Framebuffer: 0x..., Size: 8294400 bytes
   [V87] ✅ Test pattern written to framebuffer
   [V87] Colors should appear immediately when plane is enabled
   
   PLANE_CTL_1_A (linear/ARGB8888) = 0x86000008
   ✅ Plane enabled - colors should be visible NOW
   ```

2. **On Screen:**
   - **8 vertical color bars** should appear immediately when plane is enabled
   - **White borders** on all edges
   - Colors: Red, Green, Blue, Yellow, Cyan, Magenta, White, Gray

3. **System Preferences:**
   - Should show **6 resolution options** (not just 1)
   - 1920x1080, 1440x900, 1366x768, 1280x720, 1024x768, 2560x1440

---

## Technical Details

### Why Test Pattern Wasn't Showing

**OLD Flow (V86):**
```
Enable Plane (scans black framebuffer)
  ↓
Panel Power (2 seconds)
  ↓
DDI Enable (20ms)
  ↓
Pipe Enable (20ms)
  ↓
Transcoder Enable (20ms)
  ↓
Write Test Pattern (TOO LATE!)
  ↓
GPU has been scanning black for ~3 seconds
```

**NEW Flow (V87):**
```
Map GGTT
  ↓
WRITE TEST PATTERN (colors ready)
  ↓
Memory Flush
  ↓
Program Plane Surface
  ↓
Enable Plane (IMMEDIATELY sees colors!)
  ↓
Colors visible on first frame
```

### Why Only 1 Resolution

**Problem:** `currentMode = 0` means:
- `getCurrentDisplayMode()` returns mode 0
- Mode 0 doesn't exist in our mode table
- WindowServer falls back to safe mode
- Only 1 resolution shown

**Fix:** `currentMode = 1` means:
- `getCurrentDisplayMode()` returns mode 1 (1920x1080)
- Valid mode, WindowServer queries all modes
- All 6 resolutions available

---

## Build Info

**Version:** 1.0.87
**Location:** `/Users/becoolio/Documents/Github/Untitled/FakeIrisXE/build/Release/FakeIrisXE.kext`
**Binary Size:** 1.6 MB
**Build Date:** February 14, 2026

---

## Installation

```bash
# Copy to USB EFI for testing
sudo cp -R /Users/becoolio/Documents/Github/Untitled/FakeIrisXE/build/Release/FakeIrisXE.kext /Volumes/EFI/EFI/OC/Kexts/

# Reboot with USB EFI
```

---

## Next Steps

### Immediate Testing:
1. **Boot with V87**
2. **Look for:** 8 color bars immediately when plane enables
3. **Check:** System Preferences → Displays (should show 6 resolutions)
4. **Report:** What you see

### If Still Issues:
- Check kernel logs: `dmesg | grep -i "V87\|test pattern"`
- Verify plane enable: Look for `PLANE_CTL_1_A = 0x86000008`
- Check mode count: Look for `getDisplayModeCount(): returning 6 modes`

---

## Remaining Issues

### Panel Power (Not Critical):
- PP_STATUS never shows ready (stays 0x00000000)
- **But display works anyway** (BIOS already powered panel)
- **Fix later:** Correct PP_CONTROL register for sleep/wake

### Hardware Acceleration (Future):
- GuC firmware times out
- Currently using execlist fallback
- **Fix later:** Complete GuC loading for Metal/OpenGL

---

## Summary

**V87 Fixes:**
1. ✅ **Test pattern timing** - Written before plane enable
2. ✅ **Mode initialization** - Starts at mode 1 (1920x1080)
3. 🔄 **Ready for testing**

**Expected:**
- Color bars visible during boot
- 6 resolutions available
- Desktop works as before

---

*Document Generated: February 14, 2026*  
*Version: V87*  
*Status: Ready for Testing*
