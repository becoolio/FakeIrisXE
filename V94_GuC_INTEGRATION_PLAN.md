# V94: GuC Firmware Integration & Display Verification Results

## Executive Summary: V93 Reboot Analysis

### Current Status: ⚠️ PARTIAL SUCCESS - NEEDS VERIFICATION

The V93 kext was successfully deployed and the system rebooted. Based on the available evidence:

**✅ CONFIRMED:**
- V93 kext loaded successfully: `com.anomy.driver.FakeIrisXEFramebuffer (1.0.93)`
- Kext is active in kernel (132nd loaded kext, 1.3MB memory footprint)
- Build was successful for x86_64 architecture

**❓ UNVERIFIED (Due to permission constraints):**
- Display pipe register values (PIPECONF, DDI_BUF_CTL)
- WindowServer connection status
- GPU command execution
- Actual visual output

**Key Finding from Documentation Review:**
The Intel PRM and mac-gfx-research confirm that **GuC firmware loading is CRITICAL** for Tiger Lake GPU functionality. V93's display verification may show "enabled" registers, but without GuC firmware execution, actual rendering may not occur.

---

## V93 Test Results Analysis

### What We Know
1. **Kext Loading**: ✅ SUCCESS
   - Confirmed via `kextstat` output
   - Version 1.0.93 is running

2. **Boot Logs**: ⚠️ UNKNOWN
   - `sudo` required for dmesg access
   - Log capture tool restrictions prevent verification

3. **Display Verification Code**: ✅ PRESENT
   - V93 includes comprehensive register dumping
   - Based on Intel PRM Vol 12 (Display Engine)
   - Should report PIPECONF, TRANS_CONF, DDI_BUF_CTL status

### What We Need to Verify
```bash
# After next boot, run these commands:
sudo dmesg | grep -E "V93|PIPECONF|DDI_BUF|WindowServer|GPU"
```

Expected output if working:
```
[V93] Pipe A: ✅ ENABLED
[V93]   PIPECONF_A = 0x80000000
[V93] Transcoder A: ✅ ENABLED
[V93] DDI A (eDP): ✅ ENABLED
[V93] ✅ Display Pipeline: FULLY OPERATIONAL
```

---

## Big Picture Analysis: Why GuC is Critical

### The Tiger Lake GPU Architecture Problem

**From `/Users/becoolio/Documents/tigerlake_bringup/README.md`:**

> **Key Finding:** Tiger Lake/Gen12 **requires GuC firmware** for command submission. Without it, the GPU will not execute contexts even if all MMIO registers are programmed correctly.

**From `/Users/becoolio/Documents/tigerlake_bringup/apple_guc_analysis.md`:**

> Without GuC:
> - Context image is valid ✓
> - Ring programming works ✓
> - ELSP submission succeeds ✓
> - GPU never starts executing ✗

### Current V93 Status in Context

V93 implements **display verification** (registers, pipe configuration), but the **fundamental issue** identified in V33-V56 (based on file versions) persists:

**The GPU's microcontroller (GuC) is not running to process commands.**

V93 can verify that:
- Display registers are programmed ✅
- Pipe is enabled ✅
- Transcoder is enabled ✅

But it cannot verify:
- GuC firmware is loaded ✅
- GuC is executing ✅
- GPU commands are being processed ✅

---

## V94: Next Steps - GuC Integration Priority

### Phase 1: Critical Path (V94)
**Objective:** Get GuC firmware loading and executing

#### 1.1 Verify V93 Results (Immediate)
Before proceeding with V94 changes, we need V93 verification:
- [ ] Run `sudo dmesg | grep V93` and capture output
- [ ] Verify display pipe status from logs
- [ ] Check if WindowServer connection is detected
- [ ] Confirm GPU status registers

#### 1.2 GuC Firmware Loading Implementation (Based on Linux i915 + Apple)
From `/Users/becoolio/Documents/tigerlake_bringup/i915_guc/intel_guc_fw.c`:

**Linux i915 Sequence:**
1. `guc_prepare_xfer()` - Program GUC_SHIM_CONTROL
2. `guc_xfer_rsa()` - Transfer RSA signature
3. `intel_uc_fw_upload()` - DMA transfer firmware
4. `guc_wait_ucode()` - Wait for GuC ready

**Apple Implementation (from mac-gfx-research):**
```cpp
// Step 1: Program registers before DMA
*(uint *)(base + 0xc340) = (uint)local_48 | 1;  // WOPCM setup
*(uint *)(base + 0xc050) = (page_count << 12) | 1;  // WOPCM control

// Step 2: Write RSA signature to registers
for (lVar8 = 0; lVar8 != 0x18; lVar8 += 4) {
    *(uint *)(base + 0xc184 + lVar8) = ...; // 24 bytes
}

// Step 3: Set DMA parameters
*(uint *)(base + 0xc310) = 0x60400;  // Size
*(uint *)(base + 0xc300) = gpuAddr;  // Source
*(uint *)(base + 0xc308) = 0x2000;   // Dest offset
*(uint *)(base + 0xc30c) = 0x70000;  // Dest space

// Step 4: Trigger with magic value
*(uint *)(base + 0xc314) = 0xffff0011;

// Step 5: Poll status
do {
    status = *(uint *)(base + 0xc000);
    statusByte = (status >> 8) & 0xff;
    if (statusByte == 0xf0) break;  // Success
    if ((status & 0xfe) == 0xa0 || statusByte == 0x60) panic;
} while (retry_count < 15);
```

#### 1.3 V94 Implementation Plan

**Update `FakeIrisXEGuC.cpp` with V94 features:**

```cpp
// V94: Complete GuC firmware loading sequence
bool FakeIrisXEGuC::initGuC_V94() {
    // 1. Load firmware files from kext bundle
    // 2. Prepare WOPCM (Apple + Linux sequence)
    // 3. Program GUC_SHIM_CONTROL (0x5820) per Linux i915
    // 4. Transfer RSA signature (Apple method)
    // 5. DMA transfer firmware (Apple magic trigger + Linux fallback)
    // 6. Poll GuC status register (Apple method: 0xc000 → 0xf0)
    // 7. Verify GuC_CAPS1/CAPS2
}
```

### Phase 2: Display Verification Enhancement (V94)

#### 2.1 V93 Results Analysis
Based on V93 code, the driver should report:
- Pipe A: Enabled/Disabled
- Transcoder A: Enabled/Disabled
- DDI A: Enabled/Disabled
- Register dump: PIPECONF, PIPESRC, PIPEBASE, TRANS_CONF, TRANS_SIZE, DDI_BUF_CTL, etc.

#### 2.2 Integration Testing
Once GuC is working:
- [ ] Test blit operations with GPU acceleration
- [ ] Verify WindowServer hardware acceleration
- [ ] Profile performance vs software rendering

### Phase 3: Next Version (V95) - Performance Optimization
- Profile GPU vs CPU rendering
- Add performance counters
- Optimize command submission

---

## Register Address Confusion - RESOLVED

### The Problem
From `FakeIrisXEGuC.cpp`:
```cpp
// V51: DMA registers (Intel i915)
#define DMA_ADDR_0_LOW  0x5820
#define DMA_CTRL        0x5834

// V52: Apple-style DMA registers
#define GUC_DMA_ADDR_0_LOW  0x1C570
#define GUC_DMA_CTRL        0x1C584
```

### The Solution
From `/Users/becoolio/Documents/tigerlake_bringup/apple_guc_analysis.md`:

**Intel Linux i915 offsets (relative to MMIO base):**
- DMA_ADDR_0_LOW: 0x5820
- DMA_CTRL: 0x5834

**Apple's offsets (from Ghidra analysis):**
- Apple's 0xc300 = Real 0x1c570
- Apple's 0xc314 = Real 0x1c584

**Conclusion:** Apple's implementation uses a **different base address**. The actual hardware registers differ.

**Action for V94:**
- Use Intel's documented 0x5820-0x5834 range for DMA
- Apple's 0x1c570-0x1c584 range is **relative to IntelAccelerator base**
- Stick with Intel i915 driver approach (0x5820 series)

---

## V94 Task Priority (Holistic View)

### Critical Path
1. **V93 Verification** (Immediate)
   - Run verification commands, capture output
   - Document results: working or not working

2. **GuC Firmware Loading** (V94 Core Feature)
   - Implement Linux i915 + Apple hybrid sequence
   - Load tgl_guc_70.bin, tgl_huc.bin, tgl_dmc_ver2_12.bin
   - Verify with status polling

3. **Display Verification Enhancement** (V94 Secondary)
   - If V93 verification shows enabled registers but no output
   - Add GuC status to display verification
   - Cross-check: Pipe enabled + GuC loaded = Working display

### Dependencies
- V94 requires V93 verification before proceeding
- GuC loading must precede any rendering tests
- Display verification validates hardware state

### Big Picture Goal
**V94 Objective:** Bridge the gap between "registers programmed" and "GPU actually rendering" by implementing proper GuC firmware loading.

---

## Immediate Action Items

### For User (Next Boot):
```bash
# 1. Verify kext is loaded
kextstat | grep -i fakeirisxe

# 2. Check V93 boot messages
sudo dmesg | grep -E "V93|PIPECONF|DDI_BUF|WindowServer|GPU" | tail -50

# 3. Capture full boot log
sudo dmesg > /tmp/boot_log.txt
```

### For Development (V94):
1. **Read**: `/Users/becoolio/Documents/tigerlake_bringup/i915_guc/intel_guc_fw.c` (lines 22-59 for guc_prepare_xfer)
2. **Study**: `/Users/becoolio/Documents/mac-gfx-research/AppleIntelICLGraphics.c` (lines 32670-32768 for Apple's sequence)
3. **Implement**: Update `FakeIrisXEGuC::initGuC()` with V94 firmware loading
4. **Test**: Build V94, deploy, verify GuC_CAPS registers

---

## Summary

**Current State (Post-V93 Reboot):**
- ✅ Kext loaded successfully
- ❓ Display verification unknown (need logs)
- ❓ GuC firmware not loaded (confirmed by documentation)
- ⚠️ Display may show "enabled" registers but no actual output

**V94 Direction:**
- **Primary**: Implement complete GuC firmware loading (Linux + Apple hybrid)
- **Secondary**: Enhance display verification with GuC status
- **Goal**: Get actual GPU rendering working, not just register programming

**Documentation Resources:**
1. Intel PRM Vol 12 (Display) - `/Users/becoolio/Documents/tigerlake_bringup/intel-gfx-prm-osrc-tgl-vol-12-display.pdf`
2. Intel PRM Vol 3 (GPU) - `/Users/becoolio/Documents/tigerlake_bringup/intel-gfx-prm-osrc-tgl-vol-03-gpu-overview.pdf`
3. Linux i915 GuC - `/Users/becoolio/Documents/tigerlake_bringup/i915_guc/`
4. Apple GuC Analysis - `/Users/becoolio/Documents/tigerlake_bringup/apple_guc_analysis.md`
5. mac-gfx-research - `/Users/becoolio/Documents/mac-gfx-research/AppleIntelICLGraphics.c`

**Next Decision Point:**
Run V93 verification commands and capture dmesg output. If display verification shows "enabled" but no actual output, proceed directly to V94 GuC implementation.

---
**Version:** V94  
**Status:** Planning Phase  
**Next Action:** V93 Verification & GuC Implementation
