# FakeIrisXE V55 - CRITICAL FINDINGS FROM KERNEL LOGS

**Analysis Date:** February 13, 2026  
**Log Source:** Kernel logs from live system  
**Status:** V55 BOOT TEST COMPLETE - CRITICAL ISSUE IDENTIFIED

---

## 🚨 CRITICAL FINDING: GUC_SHIM_CONTROL WRITE FAILURE

### The Problem:
```
[V55]   GUC_SHIM_CONTROL = 0x0000007F
[V55]   GT_PM_CONFIG = 0x00000001 (doorbell enabled)
[V55]   Verification: SHIM=0x00000000, PM=0x00000000  <-- ❌ WRITE FAILED!
```

**What Happened:**
- V55 wrote 0x0000007F to GUC_SHIM_CONTROL (0x1C0D4)
- Register readback showed 0x00000000
- **The write did NOT stick!**

**Why This Matters:**
- GUC_SHIM_CONTROL enables read caching, clock gating, and debug registers
- Without these bits set, GuC cannot properly initialize
- This explains why GuC CAPS remain at 0x00000000 despite DMA "success"

---

## 📊 COMPLETE V55 BOOT ANALYSIS

### ✅ Phase 1-5: SUCCESSFUL

**1. Kext Loading (04:02:41)**
```
✅ Safety check passed (-fakeirisxe boot-arg detected)
✅ Found matching GPU (8086:9A49)
✅ FakeIrisXEFramebuffer::start() - Entered
```

**2. Accelerator Framework (04:02:41)**
```
✅ [Accel] init
✅ [Accel] probe()
✅ [Accel] start() attaching to framebuffer
✅ [Accel] started; waiting for user client attachShared()
```

**3. Display Pipeline (04:02:48)**
```
✅ FakeIrisXEFB::flushDisplay(): schedule work
✅ FakeIrisXEFB::performFlushNow(): running
✅ FakeIrisXEFB::performFlushNow(): done
```

**4. GGTT & RCS Ring (04:02:48)**
```
✅ [V48] Initializing GGTT aperture...
✅ FakeIrisXEFramebuffer: GGTT mapped at <private>
✅ RING CTL = 0x00000001
✅ createRcsRing() size=262144
✅ createRcsRing() — ring already exists
✅ FakeIrisXEFramebuffer: createRcsRing Succes
```

**5. MOCS Programming (04:02:48)**
```
✅ [V45] Loading firmware (Intel PRM compliant)...
✅ [V45] Programming MOCS...
✅ [V45] programMOCS(): Programming MOCS for Tiger Lake
✅ [V45] programMOCS(): Completed 62 MOCS entries
```

---

### ⚠️ Phase 6: GuC Initialization - PARTIAL SUCCESS

**V55 GuC Initialization Sequence (04:02:48):**

**Step 1: DMC Firmware - ✅ SUCCESS**
```
[V50] Step 1: Attempting DMC firmware load...
[V49] Loading DMC firmware (Linux-compatible)...
[V49] DMC firmware mapped at GGTT=0x100000 (28 bytes)
[V49] DMC firmware address programmed
[V49] ✅ DMC firmware loaded successfully
[V50] ✅ DMC firmware loaded successfully
```

**Step 2: Initial State Check**
```
[V50] Step 2: Checking GuC power state...
[V50] Initial state - STATUS: 0x00000000, RESET: 0x00000000
[V50] Step 3: Attempting GuC initialization...
[V50] Reset held: 0x00000000
[V50] Status after reset: 0x00000000
[V50] GuC Capabilities:
[V52.1] ⚠️ GuC CAPS are zero BEFORE firmware load - this is expected!
[V52.1] ℹ️ Loading GuC firmware should make CAPS accessible...
```

**Step 3: GuC Firmware Preparation - ✅ SUCCESS**
```
[V52.1] Loading GuC firmware to enable GuC hardware...
[GuC] New CSS format firmware v65536
[GuC] Header len: 0xa1 bytes
[GuC] Payload: offset=0xa1, size=0x46a1f
[GuC] Firmware mapped at GGTT=0x101000
```

**Step 4: V55 Pre-DMA Initialization**

**✅ ForceWake Acquisition:**
```
[V55] Step 1: Acquiring ForceWake...
[V52.1] Acquiring ForceWake...
[V52.1] ✅ ForceWake acquired! ACK=0x0000000F
```

**❌ GUC_SHIM_CONTROL Programming (CRITICAL FAILURE):**
```
[V55] Step 2: Programming Shim Control...
[V55] Programming GUC_SHIM_CONTROL...
[V55]   GUC_SHIM_CONTROL = 0x0000007F
[V55]   GT_PM_CONFIG = 0x00000001 (doorbell enabled)
[V55]   Verification: SHIM=0x00000000, PM=0x00000000  <-- ❌ FAIL!
```

**Analysis:**
- Wrote: 0x0000007F (all cache/clock gating bits enabled)
- Read back: 0x00000000
- Register write was NOT persistent
- **Root cause: GuC hardware not powered/configured correctly**

**✅ Apple Reset Sequence:**
```
[V55] Step 3: GuC reset sequence...
[V55]   Wrote 0x1984 = 0x1
[V55]   Wrote 0x9424 = 0x1
[V55]   Wrote 0x9424 = 0x10
[V55]   Status check 0xfce = 0xFFFFFFFF
[V55]   Wrote 0x9024 = 0xcb (conditional)
```

**✅ RSA Signature Extraction & Write:**
```
[V55] Step 4: RSA Signature Setup...
[V55] CSS header: type=0x00000006, header_len=161, key_size=64, modulus_size=64
[V55] ✅ Extracted 256 bytes RSA signature from offset 421
[V55] RSA modulus available: 256 bytes at offset 161
[V55]   Writing RSA key data (modulus) to 0xc184...
[V55]   ✅ Wrote RSA key data (6 dwords from modulus)
[V55]   Writing RSA signature (256 bytes) to 0xc200...
[V55]   ✅ Wrote RSA signature
```

**✅ DMA Parameters:**
```
[V55] Step 5: DMA Parameters...
[V55]   DMA size = 0x46B1F (289567 bytes)
[V55]   Source GGTT = 0x0000000000101000
[V55]   Dest = WOPCM offset 0x2000, space 0x70000
[V55] Step 6: WOPCM Configuration...
[V55]   WOPCM configured
[V55] === Pre-DMA Initialization Complete ===
```

---

### 📉 Phase 7: DMA Upload - APPLE FAILS, LINUX "SUCCEEDS"

**Apple-Style DMA (04:02:48):**
```
[V52.1] Step 2: Attempting DMA upload...
[V52] Attempting firmware upload with fallback...
[V52] Attempt 1: Apple-style DMA upload (mac-gfx-research)
[V52] Starting Apple-style DMA firmware upload...
[V52]   Source: GGTT 0x0000000000101000
[V52]   Dest: WOPCM offset 0x2000
[V52]   Size: 0x46B1F bytes
[V52]   Verified: src=0x000000000000, dst=0x0000000000, size=0x0
[V52]   DMA triggered with magic value 0xFFFF0011
[V52]   Polling for completion (Apple method)...
[V52]     Poll 0: STATUS=0x00000000, byte=0x00
[V52]     Poll 1: STATUS=0x00000000, byte=0x00
...
[V52]     Poll 14: STATUS=0x00000000, byte=0x00
[V52] ❌ Timeout waiting for GuC firmware load (retries: 15)
```

**Apple DMA Status:**
- STATUS register: 0x00000000 (never changed)
- Expected: 0xF0 in bits 8-15 for success
- **Result: FAILED** ❌

**Linux-Style DMA (04:02:48):**
```
[V52] ⚠️ Apple-style DMA failed, trying Linux-style...
[V52] Attempt 2: Linux-style DMA upload (i915 driver)
[V51] Starting DMA firmware upload...
[V51]   Source: GGTT 0x0000000000101000
[V51]   Dest: WOPCM offset 0x2000
[V51]   Destination address written: WOPCM offset 0x2000
[V51]   Transfer size written: 0x46B1F
[V51]   DMA started (CTRL=0x00000005)...
[V51] ✅ Linux-style DMA firmware upload completed successfully
[V52] ✅ Linux-style DMA succeeded!
[V52.1] ✅ DMA upload succeeded!
```

**Linux DMA Analysis:**
- CTRL register: 0x00000005 (START_DMA | UOS_MOVE)
- "Completed successfully" means START_DMA bit cleared
- **BUT: This doesn't mean firmware was accepted!**

---

### ❌ Phase 8: GuC CAPS Check - ZERO (FIRMWARE NOT LOADED)

**Post-DMA CAPS Check (04:02:48):**
```
[V52.1] Releasing ForceWake...
[V52.1] Step 3: Checking GuC capabilities...
[V52.1] CAPS1: 0x00000000, CAPS2: 0x00000000
[V52.1] ⚠️ GuC CAPS still zero - firmware may not have loaded
[V52.1] ✅ Firmware load attempted, re-checking CAPS...
[V52.1] GuC CAPS AFTER firmware load:
[V52.1] ❌ GuC still not accessible after firmware load attempt
[V52.1] 🔄 Falling back to Execlist mode
[V52.1] ℹ️ Execlist provides full GPU functionality without GuC
```

**Critical Finding:**
- DMA "completed" but GuC CAPS still 0x00000000
- Firmware was transferred but NOT authenticated/running
- Root cause: GUC_SHIM_CONTROL not properly configured

---

## 🔬 ROOT CAUSE ANALYSIS

### Primary Issue: GUC_SHIM_CONTROL Register Access

**Evidence:**
```
Write: 0x0000007F
Read:  0x00000000
```

**Possible Causes:**

1. **Wrong Register Offset**
   - Using 0x1C0D4 for GUC_SHIM_CONTROL
   - Intel PRM says this is correct for Gen11/12
   - But maybe different for Tiger Lake specifically?

2. **Register Not Powered**
   - GuC power domain not enabled
   - Need different power well setup
   - ForceWake may not cover GuC registers

3. **Timing Issue**
   - Register written before GuC ready
   - Need delay after reset release
   - Order of operations wrong

4. **Different Register for TGL**
   - Tiger Lake may use different offset
   - Check Intel PRM Vol 7 for TGL-specific values
   - May need GUC_SHIM_CONTROL2 (0x1C0D8)

---

## 📋 WHAT WORKED vs WHAT FAILED

### ✅ WORKED:
1. System boot without panic
2. Kext loading with -fakeirisxe
3. PCI device detection (0x9A49)
4. MMIO BAR0 mapping
5. Power management (ForceWake ACK=0x0F)
6. Framebuffer allocation
7. Display output (1920x1080)
8. Accelerator framework
9. DMC firmware loading
10. RSA signature extraction
11. Apple reset sequence
12. Linux DMA "completion"
13. Execlist fallback

### ❌ FAILED:
1. **GUC_SHIM_CONTROL programming** (verification shows 0x00)
2. Apple DMA upload (timeout, STATUS=0x00)
3. GuC firmware authentication (CAPS=0x00)
4. GuC mode activation

---

## 🎯 V56 DEVELOPMENT PRIORITIES

### CRITICAL (Fix GuC SHIM Control):

**1. Verify GUC_SHIM_CONTROL Register Offset**
```
Action: Check Intel PRM Vol 7 for TGL-specific offset
Current: 0x1C0D4
May need: Different offset for Tiger Lake
Alternative: Try GUC_SHIM_CONTROL2 (0x1C0D8)
```

**2. Check Power Domain Requirements**
```
Action: Verify GuC power well is enabled
Check: Additional power well beyond ForceWake
May need: Write to PWR_WELL_CTL before SHIM_CONTROL
```

**3. Add Register Write Verification Loop**
```cpp
// V56: Add verification with retry
for (int i = 0; i < 10; i++) {
    fOwner->safeMMIOWrite(GUC_SHIM_CONTROL, shimFlags);
    IOSleep(10);
    uint32_t verify = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    if (verify == shimFlags) {
        IOLog("✅ GUC_SHIM_CONTROL verified: 0x%08X\n", verify);
        break;
    }
    IOLog("⚠️ Retry %d: wrote 0x%08X, read 0x%08X\n", i, shimFlags, verify);
}
```

**4. Check Register Access Permissions**
```
Some GuC registers may be:
- Read-only (need different setup sequence)
- Protected (need authentication first)
- Privileged (need specific CPU mode)
```

### HIGH (Alternative Approaches):

**5. Skip GuC, Focus on Execlist**
```
Since GuC not working:
- Optimize execlist submission
- Implement proper CSB handling
- Add context switching
- Focus on stability
```

**6. Try Different Firmware Binary**
```
- Using adlp_guc_70.1.1.bin (Alder Lake P)
- Try tgl_guc_70.bin (Tiger Lake specific)
- May need different CSS format
```

---

## 📊 COMPARISON: V55 vs PREVIOUS

| Component | Before V55 | V55 Status | Improvement |
|-----------|------------|------------|-------------|
| DMC Firmware | Unknown | ✅ Loaded | **NEW** |
| RSA Extraction | Not implemented | ✅ Working | **NEW** |
| GUC_SHIM_CONTROL | Not programmed | ❌ Failed | **NEEDS FIX** |
| Apple DMA | Unknown | ❌ Timeout | **NEEDS DEBUG** |
| Linux DMA | Unknown | ⚠️ "Success" | **PARTIAL** |
| GuC CAPS | Unknown | ❌ 0x00000000 | **NEEDS FIX** |
| Execlist Fallback | Unknown | ✅ Active | **WORKING** |

---

## 🔧 RECOMMENDED V56 IMPLEMENTATION

### Option A: Fix GuC SHIM Control (Recommended)

```cpp
// V56: Enhanced GuC initialization with proper register verification

// 1. Ensure GuC power domain is enabled
// Check PWR_WELL_CTL2 or additional power wells

// 2. Add delay after reset release
IOSleep(100);  // Increase from 10ms to 100ms

// 3. Verify each register write
bool writeVerified = false;
for (int retry = 0; retry < 10 && !writeVerified; retry++) {
    fOwner->safeMMIOWrite(GUC_SHIM_CONTROL, shimFlags);
    IOSleep(10);
    uint32_t verify = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    if (verify == shimFlags) {
        writeVerified = true;
        IOLog("✅ GUC_SHIM_CONTROL verified\n");
    }
}

// 4. If SHIM_CONTROL fails, try SHIM_CONTROL2
if (!writeVerified) {
    IOLog("⚠️ Trying GUC_SHIM_CONTROL2...\n");
    fOwner->safeMMIOWrite(GUC_SHIM_CONTROL2, shimFlags);
}

// 5. Check Intel PRM for TGL-specific sequence
```

### Option B: Optimize Execlist (Fallback)

```cpp
// V56: Make execlist rock-solid
// - Proper context management
// - CSB (Command Stream Buffer) handling
// - Interrupt-driven completion
// - Batch buffer submission
```

---

## 📈 NEXT STEPS

### Immediate (Today):

1. **Check Intel PRM Vol 7**
   - Verify GUC_SHIM_CONTROL offset for Tiger Lake
   - Look for TGL-specific initialization sequence
   - Check if different from Gen11/Gen12

2. **Try Alternative Register**
   - GUC_SHIM_CONTROL2 (0x1C0D8)
   - Different timing
   - Additional power well requirements

3. **Test with TGL-specific Firmware**
   - Use tgl_guc_70.bin instead of adlp_guc_70.1.1.bin
   - May have different CSS header format
   - Could authenticate differently

### Short-term (V56):

1. **Implement register verification with retry**
2. **Add more diagnostic logging**
3. **Try both GuC and optimized Execlist paths**
4. **Document which method works**

---

## 🎓 KEY LESSONS LEARNED

1. **DMA completion ≠ Firmware loaded**
   - Linux DMA "succeeded" but CAPS still zero
   - Firmware transferred but not authenticated
   - Need GuC hardware properly configured first

2. **Register writes need verification**
   - GUC_SHIM_CONTROL write failed silently
   - Always read back and verify register values
   - Add retry logic for critical registers

3. **V55 improvements are valid**
   - RSA extraction works correctly
   - Apple reset sequence executes
   - DMC firmware loads
   - Just need to fix GuC hardware config

4. **Execlist is viable fallback**
   - System booted and display works
   - GPU accessible via execlist
   - Can provide acceleration without GuC

---

## ✅ CONCLUSION

**V55 Test Result: PARTIAL SUCCESS WITH CRITICAL FINDING**

**What Worked:**
- ✅ Complete V55 initialization sequence executed
- ✅ DMC firmware loaded
- ✅ RSA signature extracted and written
- ✅ Linux DMA transfer completed
- ✅ System stable with execlist fallback

**Critical Issue Found:**
- ❌ GUC_SHIM_CONTROL register write not persistent
- ❌ GuC CAPS remain at 0x00000000
- ❌ GuC firmware not authenticating

**Root Cause:**
GuC hardware not properly configured before DMA upload. GUC_SHIM_CONTROL write fails verification.

**V56 Priority:**
1. Fix GUC_SHIM_CONTROL register access
2. Verify correct offset for Tiger Lake
3. Add register write verification
4. If GuC still fails, optimize execlist

**Current Status:**
System is stable and functional using execlist fallback. Display works at 1920x1080. V55 improvements are valid but need GuC hardware configuration fix.

---

**Files Updated:**
- `/Users/becoolio/Documents/Github/Untitled/FakeIrisXE/V55_KERNEL_LOG_ANALYSIS.md`

**Next Action:**
Implement V56 with GUC_SHIM_CONTROL fix and register verification.

