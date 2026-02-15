# FakeIrisXe V52 - Implementation Summary

## Overview
**Version:** V52 (1.0.52)  
**Date:** 2026-02-12  
**Based on:** mac-gfx-research Apple DMA analysis + Linux i915 driver

---

## What Was Implemented

### 1. Apple-Style DMA Upload (V52)
**File:** `FakeIrisXEGuC.cpp`

Based on reverse engineering of `IGHardwareGuC::loadGuCBinary()` from Apple's Intel ICL Graphics driver:

**Key Features:**
- Magic DMA trigger value: `0xFFFF0011`
- Apple-specific WOPCM address space: `0x70000`
- Status polling for success code `0xF0`
- Failure detection codes `0xA0` and `0x60`
- 15 retry attempts with 1ms delays (matching Apple's implementation)

**Register Offsets (Apple-style):**
```cpp
GUC_DMA_ADDR_0_LOW    = 0x1C570  // Source (GGTT)
GUC_DMA_ADDR_0_HIGH   = 0x1C574
GUC_DMA_ADDR_1_LOW    = 0x1C578  // Dest (WOPCM offset)
GUC_DMA_ADDR_1_HIGH   = 0x1C57C
GUC_DMA_COPY_SIZE     = 0x1C580
GUC_DMA_CTRL          = 0x1C584
GUC_DMA_STATUS        = 0x1C270
```

### 2. Linux-Style DMA Upload (V51 - Enhanced)
**Preserved and enhanced from V51**

**Key Features:**
- Standard Intel DMA registers: `0x5820-0x5834`
- Flags: `START_DMA | UOS_MOVE`
- Completion polling: START_DMA bit clears
- Timeout: 100ms

**Register Offsets (Linux-style):**
```cpp
DMA_ADDR_0_LOW        = 0x5820
DMA_ADDR_0_HIGH       = 0x5824
DMA_ADDR_1_LOW        = 0x5828
DMA_ADDR_1_HIGH       = 0x582C
DMA_COPY_SIZE         = 0x5830
DMA_CTRL              = 0x5834
```

### 3. Fallback System (V52)
**New unified upload function:**

```cpp
bool uploadFirmwareWithFallback(uint64_t sourceGpuAddr, 
                                uint32_t destOffset, 
                                size_t fwSize)
```

**Logic:**
1. Attempt Linux-style DMA first
2. If fails, automatically try Apple-style DMA
3. If both fail, continue without DMA (with warning)

This maximizes compatibility across different GPU generations and firmware versions.

### 4. Updated Function Signatures
**File:** `FakeIrisXEGuC.hpp`

Added new private methods:
```cpp
// V51: Linux-style DMA firmware upload
bool uploadFirmwareViaDMA(uint64_t sourceGpuAddr, uint32_t destOffset, 
                          size_t fwSize, uint32_t dmaFlags);

// V52: Apple-style DMA firmware upload
bool uploadFirmwareViaDMA_Apple(uint64_t sourceGpuAddr, uint32_t destOffset, 
                                size_t fwSize);

// V52: Unified upload with fallback
bool uploadFirmwareWithFallback(uint64_t sourceGpuAddr, uint32_t destOffset, 
                                size_t fwSize);
```

---

## Files Modified

1. **FakeIrisXEGuC.cpp**
   - Added Apple DMA register definitions
   - Implemented `uploadFirmwareViaDMA_Apple()`
   - Implemented `uploadFirmwareWithFallback()`
   - Updated `loadGuCFirmware()` to use fallback
   - Enhanced logging with V52 tags

2. **FakeIrisXEGuC.hpp**
   - Added function declarations for V52 methods

3. **Info.plist**
   - Updated version to 1.0.52
   - Added CFBundleGetInfoString

4. **build.sh** (NEW)
   - Complete build automation script
   - Creates load/unload/view_logs scripts
   - Generates TESTING.md documentation

---

## Build Instructions

### Prerequisites
- macOS with Xcode installed
- SIP disabled (for testing kexts)
- Root privileges

### Build
```bash
cd ~/Documents/Github/Untitled/FakeIrisXE
./build.sh
```

### Test
```bash
cd build
sudo ./load_kext.sh
sudo ./view_logs.sh
```

---

## Expected Log Output

### Scenario 1: Linux DMA Success (Most Common)
```
[V52] Attempting firmware upload with fallback...
[V52] Attempt 1: Linux-style DMA upload
[V52] ✅ Linux-style DMA succeeded!
[V52] ✅ Firmware uploaded to GPU WOPCM via DMA
```

### Scenario 2: Apple DMA Fallback
```
[V52] Attempt 1: Linux-style DMA upload
[V52] ⚠️ Linux-style DMA failed, trying Apple-style...
[V52] Attempt 2: Apple-style DMA upload
[V52]     Poll 0: STATUS=0xXXXXXX, byte=0xF0
[V52] ✅ GuC firmware loaded successfully!
```

### Scenario 3: Both Methods Fail
```
[V52] ❌ Both DMA methods failed!
[V52] ⚠️ Continuing without DMA (may not work on Gen12+)
```

---

## Technical Details

### Why Two Methods?

**Linux Method:**
- Standard Intel reference implementation
- Works on most hardware
- Uses documented register offsets
- Standard flag-based DMA trigger

**Apple Method:**
- Required for some macOS-specific firmware
- Uses magic trigger value (0xFFFF0011)
- Different register offsets
- Status-based completion detection
- Critical for proper GuC initialization on some SKUs

### Firmware Upload Sequence

1. **Allocate GEM** for firmware (4KB aligned)
2. **Copy firmware** binary to GEM
3. **Pin and map** GEM to GGTT
4. **Program source address** (GGTT virtual address)
5. **Program destination** (WOPCM offset 0x2000)
6. **Set transfer size** (payload + CSS header)
7. **Trigger DMA** (Linux flags OR Apple magic value)
8. **Poll for completion**
9. **Verify success** (bit clear OR status byte)

### Critical Discovery from mac-gfx-research

Apple's driver writes RSA signature to MMIO registers BEFORE DMA:
- Registers 0xc184-0xc200 (24 bytes RSA + 64 bytes signature)
- This may be required for firmware authentication
- Not yet implemented in V52 (future enhancement)

---

## Testing Checklist

- [ ] Build completes without errors
- [ ] Kext loads successfully
- [ ] Linux DMA upload succeeds
- [ ] GuC initialization completes
- [ ] Display output works
- [ ] No kernel panics
- [ ] Can unload kext cleanly

---

## References

1. **mac-gfx-research**
   - Repository: https://github.com/pawan295/mac-gfx-research
   - File: AppleIntelICLGraphics.c
   - Function: IGHardwareGuC::loadGuCBinary() (Line 32670)

2. **Linux i915 Driver**
   - File: drivers/gpu/drm/i915/gt/uc/intel_uc_fw.c
   - Function: uc_fw_xfer() (Line 1040)
   - Function: intel_uc_fw_upload() (Line 1108)

3. **Intel PRM**
   - Volume 3: GPU Overview
   - Volume 7: GT Interface

---

## Next Steps

1. **Test V52** on target hardware
2. **Monitor logs** for DMA success/failure
3. **If Apple method works better:** Consider making it primary
4. **If RSA required:** Implement signature writing
5. **Add support** for more device IDs

---

## Version History

- **V52**: Apple+Linux DMA fallback (2026-02-12)
- **V51**: Linux-style DMA upload
- **V50**: GuC initialization with execlist fallback
- **V49**: DMC firmware loading

---

## Contact

**Project:** FakeIrisXe  
**Author:** Anomy  
**Bundle ID:** com.anomy.driver.FakeIrisXEFramebuffer
