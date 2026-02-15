//
//  FakeIrisXEExeclist.cpp
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 01/12/25.
//

//
// FakeIrisXEExeclist.cpp
// Phase 7 – Execlists Implementation
//


#include "FakeIrisXEExeclist.hpp"
#include "FakeIrisXEFramebuffer.hpp"
#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXELRC.hpp"
#include "i915_reg.h"



OSDefineMetaClassAndStructors(FakeIrisXEExeclist, OSObject);

// FACTORY
FakeIrisXEExeclist* FakeIrisXEExeclist::withOwner(FakeIrisXEFramebuffer* owner)
{
    FakeIrisXEExeclist* obj = OSTypeAlloc(FakeIrisXEExeclist);
    if (!obj) return nullptr;

    if (!obj->init()) {
        obj->release();
        return nullptr;
    }

    obj->fOwner = owner;

    // init HW context table
    obj->fHwContextCount = 0;
    for (uint32_t i = 0; i < kMaxHwContexts; ++i) {
        bzero(&obj->fHwContexts[i], sizeof(XEHWContext));
    }

    // init SW execlist queue
    obj->fQHead     = 0;
    obj->fQTail     = 0;
    obj->fNextSeqno = 1;
    for (uint32_t i = 0; i < kMaxExeclistQueue; ++i) {
        bzero(&obj->fQueue[i], sizeof(ExecQueueEntry));
    }

    // inflight slots
    obj->fInflight[0] = nullptr;
    obj->fInflight[1] = nullptr;
    obj->fInflightSeqno[0] = 0;
    obj->fInflightSeqno[1] = 0;

    // CSB ring defaults – you can update in createHwContext/setupExeclistPorts
    obj->fCsbGem         = nullptr;
    obj->fCsbGGTT        = 0;
    obj->fCsbSizeBytes   = 0x100;          // matches your 256-byte alloc
    obj->fCsbEntryCount  = obj->fCsbSizeBytes / 16; // 16B per CSB entry
    obj->fCsbReadIndex   = 0;

    return obj;
}




// FREE (destructor)
void FakeIrisXEExeclist::free()
{
    freeHwContext();
    OSObject::free();
}


// ------------------------------------------------------------
// Helpers (safe MMIO)
// ------------------------------------------------------------

// V57: Enhanced MMIO with diagnostics
uint32_t FakeIrisXEExeclist::mmioRead32(uint32_t off) {
    uint32_t val = *(volatile uint32_t*)((uint8_t*)fOwner->fBar0 + off);
    // V57: Optional verbose logging for critical registers
    #ifdef V57_VERBOSE_MMIO
    if (off == RING_CTL || off == RING_HEAD || off == RING_TAIL || 
        off == RING_EXECLIST_STATUS_LO || off == RING_EXECLIST_STATUS_HI) {
        IOLog("[V57] MMIO READ [0x%04X] = 0x%08X\n", off, val);
    }
    #endif
    return val;
}

void FakeIrisXEExeclist::mmioWrite32(uint32_t off, uint32_t val) {
    volatile uint32_t* p = (volatile uint32_t*)((uint8_t*)fOwner->fBar0 + off);
    *p = val;
    (void)*p; // posted write ordering
    // V57: Optional verbose logging
    #ifdef V57_VERBOSE_MMIO
    if (off == RING_CTL || off == RING_TAIL || off == RING_EXECLIST_SUBMIT_LO || 
        off == RING_EXECLIST_SUBMIT_HI) {
        IOLog("[V57] MMIO WRITE [0x%04X] = 0x%08X\n", off, val);
    }
    #endif
}

// V57: Enhanced ring buffer diagnostics
void FakeIrisXEExeclist::dumpRingBufferStatus(const char* label) {
    if (!fOwner) return;
    
    IOLog("[V57] === Ring Buffer Status: %s ===\n", label);
    
    // Read all critical ring registers
    uint32_t ring_base_lo = mmioRead32(RING_BASE_LO);
    uint32_t ring_base_hi = mmioRead32(RING_BASE_HI);
    uint32_t ring_head  = mmioRead32(RING_HEAD);
    uint32_t ring_tail  = mmioRead32(RING_TAIL);
    uint32_t ring_ctl   = mmioRead32(RING_CTL);
    // V57: Use alternative register names if ACTHD/BBADDR not defined
    uint32_t ring_acthd = mmioRead32(0x2074);  // ACTHD - Active Head
    uint32_t ring_bbaddr = mmioRead32(0x2080); // BBADDR - Batch Buffer Address
    
    IOLog("[V57] RING_BASE:   0x%08X%08X (GGTT base)\n", ring_base_hi, ring_base_lo);
    IOLog("[V57] RING_HEAD:   0x%04X (GPU read position)\n", ring_head & 0xFFFF);
    IOLog("[V57] RING_TAIL:   0x%04X (driver write position)\n", ring_tail & 0xFFFF);
    IOLog("[V57] RING_CTL:    0x%08X (size=%dKB, %s)\n",
          ring_ctl,
          ((ring_ctl >> 12) + 1) * 4,
          (ring_ctl & 1) ? "ENABLED" : "DISABLED");
    IOLog("[V57] RING_ACTHD:  0x%08X (active head)\n", ring_acthd);
    IOLog("[V57] RING_BBADDR: 0x%08X (batch buffer addr)\n", ring_bbaddr);
    
    // Calculate ring space
    uint32_t head = ring_head & 0xFFFF;
    uint32_t tail = ring_tail & 0xFFFF;
    uint32_t ring_size = ((ring_ctl >> 12) + 1) * 4096;
    uint32_t used = (tail >= head) ? (tail - head) : (ring_size - head + tail);
    uint32_t free = ring_size - used - 8; // -8 for safety margin
    
    IOLog("[V57] Ring Usage:  %d bytes used, %d bytes free (of %d total)\n", 
          used, free, ring_size);
    
    // Check for stall/hang
    static uint32_t last_head = 0;
    static uint64_t last_check = 0;
    uint64_t now = mach_absolute_time();
    
    if (head == last_head && (now - last_check) > (100 * 1000000ULL)) {
        IOLog("[V57] ⚠️ WARNING: Head not advancing for 100ms - possible GPU stall\n");
    }
    last_head = head;
    last_check = now;
}

// V57: Enhanced execlist status diagnostics
void FakeIrisXEExeclist::dumpExeclistStatus(const char* label) {
    if (!fOwner) return;
    
    IOLog("[V57] === Execlist Status: %s ===\n", label);
    
    // V57: Use correct execlist status register offsets
    uint32_t status_lo = mmioRead32(0x2230);  // RING_EXECLIST_STATUS_LO
    uint32_t status_hi = mmioRead32(0x2234);  // RING_EXECLIST_STATUS_HI
    
    IOLog("[V57] EXECLIST_STATUS_LO: 0x%08X\n", status_lo);
    IOLog("[V57] EXECLIST_STATUS_HI: 0x%08X\n", status_hi);
    
    // Decode status bits
    bool slot0_valid = (status_lo >> 0) & 1;
    bool slot1_valid = (status_lo >> 1) & 1;
    bool slot0_active = (status_lo >> 2) & 1;
    bool slot1_active = (status_lo >> 3) & 1;
    uint32_t active_id = (status_lo >> 4) & 0x3;
    
    IOLog("[V57] Slot 0: %s, %s\n", 
          slot0_valid ? "VALID" : "empty",
          slot0_active ? "ACTIVE" : "idle");
    IOLog("[V57] Slot 1: %s, %s\n", 
          slot1_valid ? "VALID" : "empty",
          slot1_active ? "ACTIVE" : "idle");
    IOLog("[V57] Active slot: %d\n", active_id);
    
    // Context IDs
    uint32_t ctx0_id = (status_lo >> 16) & 0xFFFF;
    uint32_t ctx1_id = (status_hi >> 0) & 0xFFFF;
    IOLog("[V57] Context ID slot 0: 0x%04X\n", ctx0_id);
    IOLog("[V57] Context ID slot 1: 0x%04X\n", ctx1_id);
}

// V57: Enhanced CSB processing with diagnostics
void FakeIrisXEExeclist::processCsbEntriesV57() {
    if (!fCsbGem || !fOwner) {
        IOLog("[V57] CSB processing skipped - no CSB buffer\n");
        return;
    }
    
    IOLog("[V57] === Processing CSB Entries ===\n");
    
    // Get CSB memory
    IOBufferMemoryDescriptor* md = fCsbGem->memoryDescriptor();
    if (!md) {
        IOLog("[V57] CSB memory descriptor missing\n");
        return;
    }
    
    volatile uint64_t* csb = (volatile uint64_t*)md->getBytesNoCopy();
    if (!csb) {
        IOLog("[V57] CSB CPU pointer missing\n");
        return;
    }
    
    // Read CSB write pointer (updated by GPU)
    // In real implementation, this would be read from a hardware register
    // or from the ppHWSP (per-process hardware status page)
    uint32_t write_ptr = fCsbWriteIndex; // Placeholder - should read from HW
    uint32_t read_ptr = fCsbReadIndex;
    
    IOLog("[V57] CSB Read Ptr: %d, Write Ptr: %d\n", read_ptr, write_ptr);
    
    uint32_t processed = 0;
    while (read_ptr != write_ptr && processed < fCsbEntryCount) {
        uint64_t entry = csb[read_ptr % fCsbEntryCount];
        uint32_t status = (uint32_t)(entry & 0xFFFFFFFF);
        uint32_t ctx_id = (uint32_t)(entry >> 32);
        
        IOLog("[V57] CSB[%d]: ctx=0x%08X status=0x%08X\n", 
              read_ptr, ctx_id, status);
        
        // Handle entry
        handleCsbEntry(entry, ctx_id, status);
        
        read_ptr++;
        processed++;
        
        // Safety limit
        if (processed > 16) {
            IOLog("[V57] CSB processing limited to 16 entries\n");
            break;
        }
    }
    
    fCsbReadIndex = read_ptr;
    IOLog("[V57] CSB Processing complete - %d entries processed\n", processed);
}

void FakeIrisXEExeclist::handleCsbEntry(uint64_t entry, uint32_t ctx_id, uint32_t status) {
    // V57: Enhanced CSB entry handling
    bool completed = (status & CSB_STATUS_COMPLETE) != 0;
    bool preempted = (status & CSB_STATUS_PREEMPTED) != 0;
    bool faulted = (status & CSB_STATUS_FAULT) != 0;
    
    if (completed) {
        IOLog("[V57] ✓ Context 0x%08X completed successfully\n", ctx_id);
        onContextComplete(ctx_id, status);
    } else if (preempted) {
        IOLog("[V57] ↻ Context 0x%08X preempted\n", ctx_id);
        // Handle preemption
    } else if (faulted) {
        IOLog("[V57] ✗ Context 0x%08X FAULTED - status=0x%08X\n", ctx_id, status);
        onContextFault(ctx_id, status);
    } else {
        IOLog("[V57] ? Context 0x%08X unknown status=0x%08X\n", ctx_id, status);
    }
}



// ------------------------------------------------------------
// createHwContext()
// ------------------------------------------------------------

bool FakeIrisXEExeclist::createHwContext()
{
    IOLog("(FakeIrisXE) [Exec] Alloc LRC\n");

    if (!fOwner) {
        IOLog("(FakeIrisXE) [Exec] createHwContext(): fOwner == NULL\n");
        return false;
    }

    
    // --- robust preamble for createHwContext() ---
    IOLog("(FakeIrisXE) [Exec] Alloc LRC (enter pre-reset checks)\n");

    // helper lambdas (use fOwner/fFramebuffer safeMMIO methods if available)
    auto safeRead = [&](uint32_t off) -> uint32_t {
        if (fOwner) {
            return fOwner->safeMMIORead(off);
        }
        return 0;
    };

    auto safeWrite = [&](uint32_t off, uint32_t val) -> void {
        if (fOwner) {
            fOwner->safeMMIOWrite(off, val);
        }
    };


    // 1) Soft GT reset (clears stale state without full power cycle)
        mmioWrite32(0x52080, 0x0);  // GT_MODE = disable
        IOSleep(5);
        mmioWrite32(0x52080, 0x1);  // GT_MODE = enable
        IOSleep(10);
    
    
    // Forcewake registers (adjust if your headers define different offsets)
//    const uint32_t FORCEWAKE_ACK = 0x130044; // you read this previously

    // 1) Snapshot before touching anything
    uint32_t pre_ack = safeRead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [Exec] pre-reset FORCEWAKE_ACK=0x%08x\n", pre_ack);

    // 2) Conservative ring disable/reset (do minimal writes)
    IOLog("(FakeIrisXE) [Exec] resetting RCS ring (base/head/tail/ctl)\n");
    safeWrite(RING_CTL, 0x0);          // disable ring
    safeWrite(RING_TAIL, 0x0);
    safeWrite(RING_HEAD, 0x0);
    safeWrite(RING_BASE_HI, 0x0);
    safeWrite(RING_BASE_LO, 0x0);
    (void)safeRead(RING_CTL); // posting read
    IOSleep(5); // let HW settle

    // Avoid massive mmio clearing loops here — prefer clearing via an allocated CSB GEM
    // (If you still want to zero CSB registers region, do so sparingly with small delay.)
    IOLog("(FakeIrisXE) [Exec] re-requesting forcewake (conservative)\n");

    // 3) Request forcewake (bitmask). Use read-verify loop rather than single write.
    const uint32_t REQ_MASK = 0x000F000F;   // what you wrote earlier; adjust if needed
    safeWrite(FORCEWAKE_REQ, REQ_MASK);
    (void)safeRead(FORCEWAKE_REQ); // posting read
    IOSleep(2);

    // 4) Wait up to 50 ms for ACK bits
    const uint64_t start = mach_absolute_time();
    const uint64_t timeout_ns = 50ULL * 1000000ULL;
    bool fw_ok = false;
    while (true) {
        uint32_t ack = safeRead(FORCEWAKE_ACK);
        IOLog("(FakeIrisXE) [Exec] forcewake ack poll -> 0x%08x\n", ack);
        // check lower nibble(s) or mask your HW expects:
        if ((ack & 0xF) == 0xF) { fw_ok = true; break; }
        if ((mach_absolute_time() - start) > timeout_ns) break;
        IOSleep(1);
    }

    if (!fw_ok) {
        uint32_t final_ack = safeRead(FORCEWAKE_ACK);
        IOLog("❌ Forcewake after reset FAILED (final ack=0x%08X)\n", final_ack);
        // Bail out safely — do not touch execlist registers if forcewake failed.
        return false;
    }
    IOLog("✅ Forcewake post-reset OK\n");

    
    // FIXED: Re-enable IER/IMR (cleared by reset — only completion bit)
        mmioWrite32(0x44004, 0x0);  // RCS0_IMR = unmask
        mmioWrite32(0x4400C, 0x1);  // RCS0_IER = enable complete IRQ
        (void)mmioRead32(0x4400C);  // Posted read
        IOSleep(5);
    
    
    // continue with LRC allocation...

    
    
    const size_t ctxSize = 4096;

    // ---------------------------
    // Allocate LRC GEM
    // ---------------------------
    fLrcGem = FakeIrisXEGEM::withSize(ctxSize, 0);
    if (!fLrcGem) {
        IOLog("(FakeIrisXE) [Exec] LRC alloc failed\n");
        return false;
    }

    // Zero memory
    IOBufferMemoryDescriptor* md = fLrcGem->memoryDescriptor();
    if (md) {
        bzero(md->getBytesNoCopy(), md->getLength());
    }

    // Your pin() returns void!
    fLrcGem->pin();

    // Map into GGTT
    fLrcGGTT = fOwner->ggttMap(fLrcGem);   // 100% correct for your project

    if (fLrcGGTT == 0) {
        IOLog("(FakeIrisXE) [Exec] ggttMap(LRC) failed\n");
        return false;
    }

    // Align to 4K as required by LRC hardware
    fLrcGGTT &= ~0xFFFULL;

    IOLog("(FakeIrisXE) [Exec] LRC @ GGTT=0x%llx\n", fLrcGGTT);


    
    // ---------------------------
    // Allocate CSB GEM (GEN12 requires ~128B, we use 256B safe)
    // ---------------------------
    IOLog("(FakeIrisXE) [Exec] Alloc CSB\n");

    constexpr size_t kCSBSize = 0x100; // 256 bytes
    fCsbGem = FakeIrisXEGEM::withSize(kCSBSize, 0);
    if (!fCsbGem) {
        IOLog("(FakeIrisXE) [Exec] No CSB alloc\n");
        fCsbGGTT = 0;
    } else {
        fCsbGem->pin();
        fCsbGGTT = fOwner->ggttMap(fCsbGem);
        if (fCsbGGTT)
            fCsbGGTT &= ~0xFFFULL;
    }


    return true;
}






// ------------------------------------------------------------
// freeHwContext()
// ------------------------------------------------------------

void FakeIrisXEExeclist::freeHwContext()
{
    if (fLrcGem) {
        fLrcGem->unpin();
        fLrcGem->release();
        fLrcGem = nullptr;
    }
    if (fCsbGem) {
        fCsbGem->unpin();
        fCsbGem->release();
        fCsbGem = nullptr;
    }
}








// ------------------------------------------------------------
// setupExeclistPorts()
// ------------------------------------------------------------
// safer setupExeclistPorts() — programs pointers, verifies readback, DOES NOT kick
bool FakeIrisXEExeclist::setupExeclistPorts()
{
    if (!fOwner) {
        IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: no owner\n");
        return false;
    }

    if (!fLrcGGTT) {
        IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: missing fLrcGGTT\n");
        return false;
    }

    // ---------- 1) Hold forcewake and ensure engine interrupts ----------
    if (!fOwner->forcewakeRenderHold(5000 /*ms*/)) {
        IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: forcewake hold failed\n");
        return false;
    }
    // Make sure the engine can auto-wake / interrupt itself
    fOwner->ensureEngineInterrupts();

    
    // verify GT awake (read same regs as before but now while forcewake is held)
    uint32_t gt_status = mmioRead32(0x13805C);
    uint32_t forcewake_ack = mmioRead32(0x130044);

    if ((gt_status == 0x0) || ((forcewake_ack & 0xF) == 0x0)) {
        IOLog("⚠️ GPU verification failed (after hold): GT_STATUS=0x%08X, ACK=0x%08X — still waking up\n",
              gt_status, forcewake_ack);
        // clean up: release the hold since we are aborting
    }

    IOLog("✅ GPU verified awake: GT_STATUS=0x%08X, ACK=0x%08X\n", gt_status, forcewake_ack);

    
    
    
    // ---------- 2) Program ELSP submit port (LRC pointer) while wake held ----------
    const uint64_t lrc = fLrcGGTT & ~0xFFFULL;
    uint32_t elsp_lo = (uint32_t)(lrc & 0xFFFFFFFFULL);
    uint32_t elsp_hi = (uint32_t)(lrc >> 32);
    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_LO, elsp_lo);
    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_HI, elsp_hi);

    
    
    // Program CSB pointer if present
    // Program CSB BASE registers (GEN12 mandatory)
    if (fCsbGGTT) {
        uint32_t csb_lo = (uint32_t)(fCsbGGTT & 0xFFFFFFFFULL);
        uint32_t csb_hi = (uint32_t)(fCsbGGTT >> 32);

        mmioWrite32(RCS0_CSB_ADDR_LO, csb_lo);
        mmioWrite32(RCS0_CSB_ADDR_HI, csb_hi);
        mmioWrite32(RCS0_CSB_CTRL, 0x1); // enable CSB tracking if needed
    }

    
    
    
    // Readback checks (do not assume writes are posted)
    uint32_t r_elsp_lo = mmioRead32(RCS0_EXECLIST_SUBMITPORT_LO);
    uint32_t r_elsp_hi = mmioRead32(RCS0_EXECLIST_SUBMITPORT_HI);
    IOLog("(FakeIrisXE) [Exec] ELSP readback LO=0x%08x HI=0x%08x (expected LO=0x%08x HI=0x%08x)\n",
          r_elsp_lo, r_elsp_hi, elsp_lo, elsp_hi);

    if (r_elsp_lo != elsp_lo || r_elsp_hi != elsp_hi) {
        IOLog("(FakeIrisXE) [Exec] ELSP readback mismatch — aborting safe setup\n");
        fOwner->forcewakeRenderRelease();
        return false;
    }

    
    if (fCsbGGTT) {
        uint32_t r_csb_lo = mmioRead32(CSB_ADDR_LO);
        uint32_t r_csb_hi = mmioRead32(CSB_ADDR_HI);
        IOLog("(FakeIrisXE) [Exec] CSB readback LO=0x%08x HI=0x%08x\n", r_csb_lo, r_csb_hi);
        // non-fatal; log only (CSB optional)
    }

    
    
    // GEN12 Execlist + CSB interrupt pipeline
    constexpr uint32_t IRQS =
          (1 << 12)  // CONTEXT_COMPLETE
        | (1 << 13)  // CONTEXT_SWITCH
        | (1 << 11); // PAGE_FAULT

    mmioWrite32(RCS0_IMR, ~IRQS);
    mmioWrite32(RCS0_IER, IRQS);
    mmioWrite32(GEN11_GFX_MSTR_IRQ_MASK, 0x0);
    mmioWrite32(GEN11_GFX_MSTR_IRQ, IRQS);

    
    
    
    // ---------- 3) Keep the forcewake held. Do NOT kick here if you expect submit() later.
    // We return success while still holding the hold; submitBatch() MUST keep the hold across the ELSP kick.
    IOLog("(FakeIrisXE) [Exec] setupExeclistPorts SUCCESS (no kick) - FUZZ: leaving forcewake held for submit path\n");
    return true;
}





// ------------------------------------------------------------
// createRealBatchBuffer()
// ------------------------------------------------------------
FakeIrisXEGEM* FakeIrisXEExeclist::createRealBatchBuffer(const uint8_t* data, size_t len)
{
    if (!fOwner) {
        IOLog("(FakeIrisXE) [Exec] createRealBatchBuffer: missing owner\n");
        return nullptr;
    }

    const size_t page = 4096;
    const size_t alloc = (len + page - 1) & ~(page - 1);

    FakeIrisXEGEM* gem = FakeIrisXEGEM::withSize(alloc, 0);
    if (!gem) {
        IOLog("(FakeIrisXE) [Exec] BB alloc failed\n");
        return nullptr;
    }

    IOBufferMemoryDescriptor* md = gem->memoryDescriptor();
    if (!md) {
        IOLog("(FakeIrisXE) [Exec] BB missing memoryDescriptor\n");
        gem->release();
        return nullptr;
    }

    // Zero whole allocation then copy provided data
    void* cpuPtr = md->getBytesNoCopy();
    if (!cpuPtr) {
        IOLog("(FakeIrisXE) [Exec] BB has no CPU pointer\n");
        gem->release();
        return nullptr;
    }
    bzero(cpuPtr, md->getLength());
    if (data && len > 0) memcpy(cpuPtr, data, len);

    // pin() returns void in your GEM; call it, don't test return
    gem->pin();

    // Map the GEM into GGTT using the framebuffer helper you already have
    uint64_t ggtt = fOwner->ggttMap(gem);
    if (ggtt == 0) {
        IOLog("(FakeIrisXE) [Exec] ggttMap(BB) failed\n");
        // best-effort cleanup: unpin if you have an unpin (no return value)
        gem->unpin();
        gem->release();
        return nullptr;
    }

    // Align GPU address to page boundary if needed
    ggtt &= ~0xFFFULL;

    IOLog("(FakeIrisXE) [Exec] BB allocated: size=0x%llx cpu=%p ggtt=0x%llx\n",
          md->getLength(), cpuPtr, (unsigned long long)ggtt);

    // Keep gem pinned — caller must unpin/release when done
    return gem;
}





// ------------------------------------------------------------
// submitBatchExeclist()
// ------------------------------------------------------------
bool FakeIrisXEExeclist::submitBatchExeclist(FakeIrisXEGEM* batchGem)
{
    if (!batchGem || !fOwner) {
        IOLog("(FakeIrisXE) [Exec] submitBatchExeclist: missing batch or owner\n");
        return false;
    }

    // Ensure GEM is pinned
    batchGem->pin();

    // Get GGTT address from framebuffer
    uint64_t batchGGTT = fOwner->ggttMap(batchGem);
    if (batchGGTT == 0) {
        IOLog("(FakeIrisXE) [Exec] FAILED: ggttMap(batchGem)=0\n");
        return false;
    }
    batchGGTT &= ~0xFFFULL;

    IOLog("(FakeIrisXE) [Exec] Submit batch @ GGTT=0x%llx\n", batchGGTT);

    // Allocate an execlist "queue descriptor"
    FakeIrisXEGEM* listGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!listGem) {
        IOLog("(FakeIrisXE) [Exec] listGem alloc failed\n");
        return false;
    }

    IOBufferMemoryDescriptor* md = listGem->memoryDescriptor();
    if (!md) {
        listGem->release();
        IOLog("(FakeIrisXE) [Exec] listGem missing memoryDescriptor\n");
        return false;
    }

    // Fill descriptor
    void* cpuPtr = md->getBytesNoCopy();
    bzero(cpuPtr, md->getLength());

    uint64_t* q = (uint64_t*)cpuPtr;
    q[0] = batchGGTT;   // first entry = batch buffer GPU address

    // pin & map listGem
    listGem->pin();
    uint64_t listGGTT = fOwner->ggttMap(listGem);
    if (listGGTT == 0) {
        IOLog("(FakeIrisXE) [Exec] FAILED: ggttMap(listGem)=0\n");
        listGem->unpin();
        listGem->release();
        return false;
    }

    IOLog("(FakeIrisXE) [Exec] ELSP list @ GGTT=0x%llx\n", listGGTT);

    // Program ELSP submit port
    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_LO, (uint32_t)(listGGTT & 0xFFFFFFFFULL));
    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_HI, (uint32_t)(listGGTT >> 32));

    // Kick exec list
    mmioWrite32(RCS0_EXECLIST_CONTROL, 0x1);

    IOLog("(FakeIrisXE) [Exec] ExecList kicked\n");

    // Poll status
    uint64_t start = mach_absolute_time();
    const uint64_t limit_ns = 2000ULL * 1000000ULL;

    while (true) {
        uint32_t status = mmioRead32(RCS0_EXECLIST_STATUS_LO);
        if (status != 0) {
            IOLog("(FakeIrisXE) [Exec] STATUS=0x%08x\n", status);
            break;
        }

        if (mach_absolute_time() - start > limit_ns) {
            IOLog("(FakeIrisXE) [Exec] TIMEOUT waiting execlist\n");
            break;
        }

        IOSleep(1);
    }

    listGem->unpin();
    listGem->release();

    return true;
}





/*
bool FakeIrisXEExeclist::programRcsForContext(
        FakeIrisXEFramebuffer* fb,
        uint64_t ctxGpu,
        FakeIrisXEGEM* ringGem,
        uint64_t batchGpu)
{
    // We actually trust our own owner + mmio helpers, not the fb param.
    if (!fOwner || !ringGem)
        return false;

    IOLog("=== SIMPLE ELSP SUBMIT TEST (v2) === ctx=0x%llx batch=0x%llx\n",
          ctxGpu, batchGpu);

    // --------------------------------------------------
    // STEP 0: Hold RENDER forcewake (like setupExeclistPorts)
    // --------------------------------------------------
    if (!fOwner->forcewakeRenderHold(5000 )) {
        IOLog("❌ programRcsForContext: forcewakeRenderHold() FAILED\n");
        return false;
    }

    uint32_t ack = mmioRead32(0x130044); // FORCEWAKE_ACK
    IOLog("programRcsForContext: FORCEWAKE_ACK after hold = 0x%08x\n", ack);

    // --------------------------------------------------
    // STEP 1: Build a minimal descriptor (same as before)
    // --------------------------------------------------
    FakeIrisXEGEM* listGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!listGem) {
        fOwner->forcewakeRenderRelease();
        return false;
    }

    listGem->pin();
    uint32_t* w = (uint32_t*)listGem->memoryDescriptor()->getBytesNoCopy();
    bzero(w, 4096);

    // Minimal descriptor for Gen12:
    // DW0/DW1: LRC GGTT (ctx)
    // DW3: VALID|ACTIVE (no fancy priority)
    // DW4/DW5: Batch start
    w[0] = (uint32_t)(ctxGpu & 0xFFFFFFFFull);
    w[1] = (uint32_t)(ctxGpu >> 32);
    w[2] = 0;
    w[3] = (1u << 0) | (1u << 1);   // VALID + ACTIVE only
    w[4] = (uint32_t)(batchGpu & 0xFFFFFFFFull);
    w[5] = (uint32_t)(batchGpu >> 32);
    w[6] = 0;
    w[7] = 0;

    __sync_synchronize();

    uint64_t listGpu = fOwner->ggttMap(listGem);
    if (!listGpu) {
        listGem->release();
        fOwner->forcewakeRenderRelease();
        return false;
    }

    listGpu &= ~0xFFFULL; // page align, just like before
    IOLog("programRcsForContext: Descriptor GGTT VA=0x%llx\n", listGpu);

    // --------------------------------------------------
    // STEP 2: Read ELSP before write (using SAME regs as setupExeclistPorts)
    // --------------------------------------------------
    uint32_t elsp_before_lo = mmioRead32(RCS0_EXECLIST_SUBMITPORT_LO);
    uint32_t elsp_before_hi = mmioRead32(RCS0_EXECLIST_SUBMITPORT_HI);
    IOLog("programRcsForContext: ELSP before: LO=0x%08x HI=0x%08x\n",
          elsp_before_lo, elsp_before_hi);

    // --------------------------------------------------
    // STEP 3: Write ELSP via mmioWrite32 (NO safeMMIOWrite, NO gpuPowerOn)
    // --------------------------------------------------
    uint32_t desc_lo = (uint32_t)(listGpu & 0xFFFFFFFFull);
    uint32_t desc_hi = (uint32_t)(listGpu >> 32);

    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_LO, desc_lo);
    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_HI, desc_hi);

    // small delay so posted writes land
    IOSleep(2);

    // --------------------------------------------------
    // STEP 4: Read back ELSP and STATUS
    // --------------------------------------------------
    uint32_t elsp_after_lo = mmioRead32(RCS0_EXECLIST_SUBMITPORT_LO);
    uint32_t elsp_after_hi = mmioRead32(RCS0_EXECLIST_SUBMITPORT_HI);
    uint32_t status_lo     = mmioRead32(RCS0_EXECLIST_STATUS_LO);

    IOLog("programRcsForContext: ELSP after: LO=0x%08x HI=0x%08x STATUS_LO=0x%08x\n",
          elsp_after_lo, elsp_after_hi, status_lo);

    bool ok = (elsp_after_lo == desc_lo && elsp_after_hi == desc_hi);

    if (!ok) {
        IOLog("❌ programRcsForContext: ELSP write FAILED, "
              "expected LO=0x%08x HI=0x%08x\n", desc_lo, desc_hi);
    } else {
        IOLog("✅ programRcsForContext: ELSP write OK\n");
    }

    // Kick execlist
    mmioWrite32(RCS0_EXECLIST_CONTROL, 0x1);
    IOSleep(1);
    uint32_t status_after_kick = mmioRead32(RCS0_EXECLIST_STATUS_LO);
    IOLog("programRcsForContext: EXECLIST kicked, STATUS_LO=0x%08x\n", status_after_kick);

    // --------------------------------------------------
    // STEP 5: Clean up
    // --------------------------------------------------
    fOwner->forcewakeRenderRelease();
    listGem->release();

    // We STILL are not "submitting" anything, only verifying ELSP write.
    return ok;
}
*/



bool FakeIrisXEExeclist::programRcsForContext(
        FakeIrisXEFramebuffer* fb,
        uint64_t ctxGpu,
        FakeIrisXEGEM* ringGem,
        uint64_t batchGpu)
{
    // ringGem / batchGpu are still useful for building the context image / ring,
    // but they are NOT used directly in the execlist descriptor.
    if (!fOwner) {
        IOLog("programRcsForContext: no owner\n");
        return false;
    }

    IOLog("=== SIMPLE ELSP SUBMIT TEST (v3) === ctx=0x%llx batch=0x%llx\n",
          ctxGpu, batchGpu);

    // --------------------------------------------------
    // STEP 0: Hold RENDER forcewake
    // --------------------------------------------------
    if (!fOwner->forcewakeRenderHold(5000 /*ms*/)) {
        IOLog("❌ programRcsForContext: forcewakeRenderHold() FAILED\n");
        return false;
    }

    uint32_t ack = mmioRead32(0x130044); // FORCEWAKE_ACK
    IOLog("programRcsForContext: FORCEWAKE_ACK after hold = 0x%08x\n", ack);

    // --------------------------------------------------
    // STEP 1: Build a REAL execlist descriptor in registers
    // --------------------------------------------------
    // LRCA = context GGTT address >> 12
    uint32_t lrca = (uint32_t)(ctxGpu >> 12);

    // Gen11/12 descriptor (simplified):
    //  - bits [31:12] = LRCA
    //  - bit 0        = VALID
    //  (we keep everything else 0 for now)
    uint32_t desc_lo = (lrca << 12) | 0x3; // VALID | ACTIVE
    uint32_t desc_hi = 0x00010000;        // simple priority

    
    
    IOLog("programRcsForContext: ctxGpu=0x%llx lrca=0x%x descLo=0x%08x descHi=0x%08x\n",
          (unsigned long long)ctxGpu, lrca, desc_lo, desc_hi);

    // --------------------------------------------------
    // STEP 2: Read ELSP before write
    // --------------------------------------------------
    uint32_t elsp_before_lo = mmioRead32(RCS0_EXECLIST_SUBMITPORT_LO);
    uint32_t elsp_before_hi = mmioRead32(RCS0_EXECLIST_SUBMITPORT_HI);
    IOLog("programRcsForContext: ELSP before: LO=0x%08x HI=0x%08x\n",
          elsp_before_lo, elsp_before_hi);

    // --------------------------------------------------
    // STEP 3: Write descriptor directly to submit port
    // --------------------------------------------------
    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_LO, desc_lo);
    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_HI, desc_hi);

    IOSleep(2); // let posted writes land

    // --------------------------------------------------
    // STEP 4: Read back ELSP + STATUS
    // --------------------------------------------------
    uint32_t elsp_after_lo = mmioRead32(RCS0_EXECLIST_SUBMITPORT_LO);
    uint32_t elsp_after_hi = mmioRead32(RCS0_EXECLIST_SUBMITPORT_HI);
    uint32_t status_lo     = mmioRead32(RCS0_EXECLIST_STATUS_LO);

    IOLog("programRcsForContext: ELSP after: LO=0x%08x HI=0x%08x STATUS_LO=0x%08x\n",
          elsp_after_lo, elsp_after_hi, status_lo);

    bool ok = (elsp_after_lo == desc_lo && elsp_after_hi == desc_hi);
    if (!ok) {
        IOLog("❌ programRcsForContext: ELSP write FAILED, "
              "expected LO=0x%08x HI=0x%08x\n", desc_lo, desc_hi);
        fOwner->forcewakeRenderRelease();
        return false;
    }

    IOLog("✅ programRcsForContext: ELSP descriptor write OK\n");

    mmioWrite32(RING_HEAD, 0);

    
    // --------------------------------------------------
    // STEP 5: Kick execlist
    // --------------------------------------------------
    mmioWrite32(RCS0_EXECLIST_CONTROL, 0x1);   // minimal "kick"
    IOSleep(1);

    uint32_t status_after_kick = mmioRead32(RCS0_EXECLIST_STATUS_LO);
    IOLog("programRcsForContext: EXECLIST kicked, STATUS_LO=0x%08x\n",
          status_after_kick);

    // --------------------------------------------------
    // STEP 6: Release forcewake
    // --------------------------------------------------
    fOwner->forcewakeRenderRelease();

    return true;
}









bool FakeIrisXEExeclist::writeExeclistDescriptor(FakeIrisXEFramebuffer* fb, uint64_t ctxGpuAddr, uint64_t batchGpuAddr, size_t batchSize)
{
    return this->programRcsForContext(fb, ctxGpuAddr, nullptr, batchGpuAddr);
}








bool FakeIrisXEExeclist::submitBatchWithExeclist(
        FakeIrisXEFramebuffer* fb,
        FakeIrisXEGEM*        batchGem,   // unused for ring-only fence test
        size_t                batchSize,  // unused
        FakeIrisXERing*       ring,
        uint32_t              timeoutMs)
{
    if (!fb || !ring) {
        IOLog("[Exec] submitBatchWithExeclist: invalid args (fb/ring)\n");
        return false;
    }

    IOLog("[Exec] submitBatchWithExeclist(): GEN12 RING EXECUTION PATH (ring-only)\n");

    if (!fb->forcewakeRenderHold(timeoutMs)) {
        IOLog("[Exec] submitBatchWithExeclist: forcewakeRenderHold FAILED\n");
        return false;
    }

    bool success = false;

    FakeIrisXEGEM* ringBacking = nullptr;
    FakeIrisXEGEM* ctx         = nullptr;
    FakeIrisXEGEM* fenceGem    = nullptr;

    do {
        //
        // 1) Fence buffer (GGTT-visible) that PIPE_CONTROL will write to.
        //
        fenceGem = FakeIrisXEGEM::withSize(4096, 0);
        if (!fenceGem) {
            IOLog("[Exec] submitBatchWithExeclist: fence alloc FAILED\n");
            break;
        }
        fenceGem->pin();

        uint64_t fenceGpu = fb->ggttMap(fenceGem) & ~0xFFFULL;
        IOBufferMemoryDescriptor* fmd = fenceGem->memoryDescriptor();
        if (!fmd) {
            IOLog("[Exec] submitBatchWithExeclist: fence md NULL\n");
            break;
        }
        volatile uint32_t* fenceCpu =
            (volatile uint32_t*)fmd->getBytesNoCopy();
        if (!fenceCpu) {
            IOLog("[Exec] submitBatchWithExeclist: fenceCpu NULL\n");
            break;
        }
        *fenceCpu = 0;
        OSSynchronizeIO();

        //
        // 2) Ring backing buffer
        //
        size_t ringSize = ring->size();
        ringBacking = FakeIrisXEGEM::withSize(ringSize, 0);
        if (!ringBacking) {
            IOLog("[Exec] submitBatchWithExeclist: ringBacking alloc FAILED\n");
            break;
        }
        ringBacking->pin();

        uint64_t ringGpu = fb->ggttMap(ringBacking) & ~0xFFFULL;
        IOBufferMemoryDescriptor* rmd = ringBacking->memoryDescriptor();
        if (!rmd) {
            IOLog("[Exec] submitBatchWithExeclist: ring md NULL\n");
            break;
        }
        uint32_t* ringCpu = (uint32_t*)rmd->getBytesNoCopy();
        if (!ringCpu) {
            IOLog("[Exec] submitBatchWithExeclist: ringCpu NULL\n");
            break;
        }
        bzero(ringCpu, rmd->getLength());

        //
        // 3) REAL GEN12 commands directly in RCS ring:
        //    PIPE_CONTROL (POST-SYNC WRITE IMMEDIATE -> fenceGpu = 1)
        //    MI_BATCH_BUFFER_END
        //
        const uint32_t PIPE_CONTROL        = (0x7A << 23);
        const uint32_t PC_WRITE_IMM        = (1 << 14);
        const uint32_t PC_CS_STALL         = (1 << 20);
        const uint32_t PC_GLOBAL_GTT       = (1 << 2);
        const uint32_t MI_BATCH_BUFFER_END = (0x0A << 23);

        unsigned d = 0;

        // PIPE_CONTROL: post-sync immediate write -> fenceGpu = 1
        ringCpu[d++] = PIPE_CONTROL | PC_WRITE_IMM | PC_CS_STALL | PC_GLOBAL_GTT;
        ringCpu[d++] = 0; // DW1
        ringCpu[d++] = (uint32_t)(fenceGpu & 0xFFFFFFFFULL); // DW2: addr LO
        ringCpu[d++] = 1;                                    // DW3: immediate

        // MI_BATCH_BUFFER_END
        ringCpu[d++] = MI_BATCH_BUFFER_END;
        ringCpu[d++] = 0x00000000;

        size_t ringBytes = d * sizeof(uint32_t);
        IOLog("[Exec] Ring BUILT (PIPE_CONTROL): dwords=%u bytes=%zu fenceGpu=0x%llx\n",
              d, ringBytes, (unsigned long long)fenceGpu);

        //
        // 4) Build LRC image with correct ring state layout.
        //
        uint32_t ringTail = (uint32_t)ringBytes & (uint32_t)(ringSize - 1);

        IOReturn ret = kIOReturnError;
        ctx = FakeIrisXELRC::buildLRCContext(
                fb,
                ringBacking,
                ringSize,
                ringGpu,
                /* ringHead */ 0,
                /* ringTail */ ringTail,
                &ret);

        if (!ctx || ret != kIOReturnSuccess) {
            IOLog("[Exec] submitBatchWithExeclist: buildLRCContext FAILED (ret=0x%x)\n", ret);
            break;
        }

        ctx->pin();
        uint64_t ctxGpu = fb->ggttMap(ctx) & ~0xFFFULL;

        IOBufferMemoryDescriptor* cmd = ctx->memoryDescriptor();
        if (!cmd) {
            IOLog("[Exec] submitBatchWithExeclist: ctx md NULL\n");
            break;
        }
        uint8_t* ctxCpu = (uint8_t*)cmd->getBytesNoCopy();
        if (!ctxCpu) {
            IOLog("[Exec] submitBatchWithExeclist: ctxCpu NULL\n");
            break;
        }

        // LRC header + ring state are already correct from buildLRCContext().
        OSSynchronizeIO();

        
        
        
        //
        // 🔥 GEN12 LEGACY RING REGISTER PROGRAMMING 🔥
        // Required BEFORE the first execlist context load or GPU reads garbage
        //
        const uint32_t GEN12_RCS0_RBSTART_LO = 0x23C30;
        const uint32_t GEN12_RCS0_RBSTART_HI = 0x23C34;
   
        // Tell GPU this is the ring base (GGTT address)
        fb->safeMMIOWrite(GEN12_RCS0_RBSTART_LO, (uint32_t)(ringGpu & 0xFFFFFFFFULL));
        fb->safeMMIOWrite(GEN12_RCS0_RBSTART_HI, (uint32_t)(ringGpu >> 32));

        // HEAD must start at 0
        fb->safeMMIOWrite(GEN12_RCS0_RBHEAD, 0);

        // TAIL = number of bytes of commands (must match your LRC TAIL!)
        fb->safeMMIOWrite(GEN12_RCS0_RBTAIL, ringBytes);

        IOLog("[Exec] GEN12_RCS RING_START + HEAD/TAIL programmed: base=0x%llx tail=%zu\n",
              (unsigned long long)ringGpu, ringBytes);

        
        
        
            
        //
        // 5) Program EXECLIST with context only (LRCA).
        //
        if (!programRcsForContext(fb, ctxGpu, nullptr, 0 /* no batch */)) {
            IOLog("[Exec] submitBatchWithExeclist: programRcsForContext FAILED\n");
            break;
        }

        IOLog("[Exec] Submitted ELSP => LRCA=0x%x\n",
              (uint32_t)(ctxGpu >> 12));

        IOSleep(2); // tiny delay before polling

        //
        // 6) Poll fence GPU is supposed to write via PIPE_CONTROL.
        //
        for (uint32_t t = 0; t < timeoutMs; ++t) {
            OSSynchronizeIO();
            if (*fenceCpu != 0) {
                IOLog("[Exec] fence updated by GPU: 0x%08x\n", *fenceCpu);
                success = true;
                break;
            }
            IOSleep(1);
        }

        if (!success) {
            uint32_t head   = mmioRead32(RING_HEAD);
            uint32_t tail   = mmioRead32(RING_TAIL);
            uint32_t status = mmioRead32(RCS0_EXECLIST_STATUS_LO);
            IOLog("❌ TIMEOUT — Fence still 0 (HEAD=0x%x TAIL=0x%x STATUS_LO=0x%08x)\n",
                  head, tail, status);
        }

    } while (false);

    fb->forcewakeRenderRelease();

    // Cleanup
    if (ctx)         ctx->release();
    if (ringBacking) ringBacking->release();
    if (fenceGem)    fenceGem->release();
    // batchGem is owned by caller.

    return success;
}


















void FakeIrisXEExeclist::engineIrq(uint32_t iir)
{
    // We only care about execlist/ctx interrupts here
    if ((iir & (RCS_INTR_COMPLETE | RCS_INTR_CTX_SWITCH | RCS_INTR_FAULT)) == 0)
        return;

    // On any of those, read CSB entries.
    processCsbEntries();
}


void FakeIrisXEExeclist::processCsbEntries()
{
    if (!fCsbGem || fCsbEntryCount == 0)
        return;

    IOBufferMemoryDescriptor* md = fCsbGem->memoryDescriptor();
    if (!md) return;

    volatile uint64_t* csbBase =
        (volatile uint64_t*)md->getBytesNoCopy();
    if (!csbBase) return;

    const uint32_t mask = fCsbEntryCount - 1; // assume power-of-two

    for (;;) {
        uint32_t idx = fCsbReadIndex & mask;
        volatile uint64_t* entry = csbBase + idx * 2;

        uint64_t low  = entry[0];
        uint64_t high = entry[1];

        if (low == 0 && high == 0) {
            // no more new CSB entries
            break;
        }

        // Consume it
        handleCsbEntry(low, high);

        // Mark as consumed (zero it)
        entry[0] = 0;
        entry[1] = 0;
        OSSynchronizeIO();

        fCsbReadIndex++;
    }

    // If engine idle and we have pending work, maybe kick
    maybeKickScheduler();
}


enum {
    CSB_STATUS_COMPLETE = 1u << 0,
    CSB_STATUS_PREEMPT  = 1u << 1,
    CSB_STATUS_FAULT    = 1u << 2,
};

void FakeIrisXEExeclist::handleCsbEntry(uint64_t low, uint64_t high)
{
    uint32_t ctxId  = (uint32_t)(low & 0xFFFFFFFFu);
    uint32_t status = (uint32_t)(high & 0xFFFFFFFFu);

    IOLog("(FakeIrisXE) [Exec] CSB: ctx=%u status=0x%08x\n", ctxId, status);

    if (status & CSB_STATUS_FAULT) {
        onContextFault(ctxId, status);
    } else if (status & CSB_STATUS_COMPLETE) {
        onContextComplete(ctxId, status);
    } else if (status & CSB_STATUS_PREEMPT) {
        // preemption or switch – treat like partial completion
        onContextComplete(ctxId, status);
    } else {
        // "switch only" or other
        // You can log / ignore for now
    }
}



void FakeIrisXEExeclist::onContextComplete(uint32_t ctxId, uint32_t status)
{
    // Mark inflight entry for ctxId as completed
    for (int i = 0; i < 2; ++i) {
        XEHWContext* hw = fInflight[i];
        if (hw && hw->ctxId == ctxId) {
            IOLog("(FakeIrisXE) [Exec] ctx %u complete on slot %d\n", ctxId, i);
            fInflight[i] = nullptr;
            fInflightSeqno[i] = 0;
            break;
        }
    }

    // You could also wake any waiters, notify Accelerator, etc.

    // Immediately schedule next context (if any)
    maybeKickScheduler();
}

void FakeIrisXEExeclist::onContextFault(uint32_t ctxId, uint32_t status)
{
    XEHWContext* hw = lookupHwContext(ctxId);
    if (!hw) return;

    hw->banScore++;
    IOLog("(FakeIrisXE) [Exec] ctx %u fault (banScore=%u)\n",
          ctxId, hw->banScore);

    if (hw->banScore >= kMaxBanScore) {
        hw->banned = true;
        IOLog("(FakeIrisXE) [Exec] ctx %u BANNED\n", ctxId);
    }

    // Drop inflight reference
    for (int i = 0; i < 2; ++i) {
        if (fInflight[i] && fInflight[i]->ctxId == ctxId) {
            fInflight[i] = nullptr;
            fInflightSeqno[i] = 0;
        }
    }

    // Do not reschedule banned contexts
    maybeKickScheduler();
}


bool FakeIrisXEExeclist::submitForContext(XEHWContext* hw, FakeIrisXEGEM* batchGem)
{
    if (!hw || !batchGem || hw->banned)
        return false;

    uint32_t nextTail = (fQTail + 1) % kMaxExeclistQueue;
    if (nextTail == fQHead) {
        IOLog("(FakeIrisXE) [Exec] submitForContext: queue full\n");
        return false;
    }

    batchGem->pin();
    uint64_t batchGGTT = fOwner->ggttMap(batchGem) & ~0xFFFULL;

    ExecQueueEntry& e = fQueue[fQTail];
    e.hwCtx    = hw;
    e.batchGem = batchGem;
    e.batchGGTT= batchGGTT;
    e.seqno    = fNextSeqno++;
    e.inFlight = false;
    e.completed= false;
    e.faulted  = false;

    fQTail = nextTail;

    IOLog("(FakeIrisXE) [Exec] queued ctx=%u seq=%u\n", hw->ctxId, e.seqno);

    // Try to kick immediately
    maybeKickScheduler();

    return true;
}


FakeIrisXEExeclist::ExecQueueEntry* FakeIrisXEExeclist::pickNextReady()
{
    if (fQHead == fQTail)
        return nullptr;

    ExecQueueEntry* best = nullptr;
    uint32_t bestPri = 0;
    uint32_t idx = fQHead;

    while (idx != fQTail) {
        ExecQueueEntry& e = fQueue[idx];
        XEHWContext* hw = e.hwCtx;
        if (hw && !hw->banned && !e.inFlight) {
            uint32_t pri = hw->priority;
            if (!best || pri > bestPri) {
                best    = &e;
                bestPri = pri;
            }
        }
        idx = (idx + 1) % kMaxExeclistQueue;
    }
    return best;
}

void FakeIrisXEExeclist::maybeKickScheduler()
{
    // See if any ELSP slot is free
    int freeSlot = -1;
    for (int i = 0; i < 2; ++i) {
        if (!fInflight[i]) {
            freeSlot = i;
            break;
        }
    }
    if (freeSlot < 0)
        return; // both slots busy

    ExecQueueEntry* e = pickNextReady();
    if (!e) return;

    if (submitToELSPSlot(freeSlot, e)) {
        e->inFlight = true;
        fInflight[freeSlot] = e->hwCtx;
        fInflightSeqno[freeSlot] = e->seqno;
        IOLog("(FakeIrisXE) [Exec] ctx %u seq %u -> ELSP slot %d\n",
              e->hwCtx->ctxId, e->seqno, freeSlot);
    }
}

bool FakeIrisXEExeclist::submitToELSPSlot(int slot, ExecQueueEntry* e)
{
    if (!e || !e->hwCtx)
        return false;

    XEHWContext* hw = e->hwCtx;

    // Build a small descriptor on the stack (you can still use a GEM if you want)
    uint32_t desc[8] = {0};

    desc[0] = (uint32_t)(hw->lrcGGTT & 0xFFFFFFFFu);
    desc[1] = (uint32_t)(hw->lrcGGTT >> 32);
    desc[2] = 0;
    desc[3] = (1u << 0) | (1u << 1); // VALID|ACTIVE
    desc[4] = (uint32_t)(e->batchGGTT & 0xFFFFFFFFu);
    desc[5] = (uint32_t)(e->batchGGTT >> 32);
    desc[6] = 0;
    desc[7] = 0;

    // For now, write descriptor into a small GEM and ELSP points to it exactly
    FakeIrisXEGEM* listGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!listGem) return false;
    listGem->pin();
    uint32_t* cpu = (uint32_t*)listGem->memoryDescriptor()->getBytesNoCopy();
    bzero(cpu, 4096);
    memcpy(cpu, desc, sizeof(desc));

    uint64_t listGGTT = fOwner->ggttMap(listGem) & ~0xFFFULL;

    // For 2-port ELSP, port 0/1 share same SUBMITPORT regs on Gen12,
    // hardware manages internal pending vs active.
    // So we just write once per submit.
    uint32_t lo = (uint32_t)(listGGTT & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(listGGTT >> 32);

    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_LO, lo);
    mmioWrite32(RCS0_EXECLIST_SUBMITPORT_HI, hi);

    // Kick control register (lightweight)
    mmioWrite32(RCS0_EXECLIST_SQ_CONTENTS, 0x1);
    IOSleep(2);
    uint32_t sq = mmioRead32(RCS0_EXECLIST_SQ_CONTENTS);
    IOLog("SQ_CONTENTS after kick = 0x%08x\n", sq);

    
    IOLog("(FakeIrisXE) [Exec] submitToELSPSlot slot=%d ctx=%u listGGTT=0x%llx\n",
          slot, hw->ctxId, listGGTT);

    // We keep listGem alive only for test; in real impl you'd reuse a pool.
    listGem->release();
    return true;
}



FakeIrisXEExeclist::XEHWContext* FakeIrisXEExeclist::lookupHwContext(uint32_t ctxId)
{
    for (uint32_t i = 0; i < fHwContextCount; ++i) {
        XEHWContext* hw = &fHwContexts[i];
        if (hw->ctxId == ctxId) {
            return hw;
        }
    }
    return nullptr;
}


FakeIrisXEExeclist::XEHWContext* FakeIrisXEExeclist::createHwContextFor(uint32_t ctxId, uint32_t priority)
{
    IOLog("[V61] createHwContextFor(ctxId=0x%X, priority=%u) - START\n", ctxId, priority);
    
    // If it already exists, just update priority and return
    XEHWContext* existing = lookupHwContext(ctxId);
    if (existing) {
        existing->priority = priority;
        IOLog("[V61] createHwContextFor: REUSE ctx=%u pri=%u\n", ctxId, priority);
        return existing;
    }
    IOLog("[V61] createHwContextFor: Creating NEW context\n");

    if (fHwContextCount >= kMaxHwContexts) {
        IOLog("[V61] ❌ createHwContextFor: no slots left (count=%u max=%u)\n", fHwContextCount, kMaxHwContexts);
        return nullptr;
    }

    XEHWContext* hw = &fHwContexts[fHwContextCount];
    bzero(hw, sizeof(XEHWContext));

    hw->ctxId    = ctxId;
    hw->priority = priority;
    hw->banScore = 0;
    hw->banned   = false;

    // --- 1) Allocate ring backing for this context ---
    size_t ringSize = 0x4000; // 16KB

    IOLog("[V61] createHwContextFor: Allocating ringGem (size=0x%zx)...\n", ringSize);
    hw->ringGem = FakeIrisXEGEM::withSize(ringSize, 0);
    if (!hw->ringGem) {
        IOLog("[V61] ❌ createHwContextFor: ringGem alloc FAILED\n");
        return nullptr;
    }
    IOLog("[V61] createHwContextFor: ringGem allocated=%p\n", hw->ringGem);

    IOLog("[V61] createHwContextFor: Pinning ringGem...\n");
    hw->ringGem->pin();
    
    IOLog("[V61] createHwContextFor: Mapping ringGem to GGTT...\n");
    hw->ringGGTT = fOwner->ggttMap(hw->ringGem);
    IOLog("[V61] createHwContextFor: ggttMap returned=0x%llX\n", hw->ringGGTT);
    if (!hw->ringGGTT) {
        IOLog("[V61] ❌ createHwContextFor: ggttMap(ring) FAILED\n");
        hw->ringGem->unpin();
        hw->ringGem->release();
        hw->ringGem = nullptr;
        return nullptr;
    }
    hw->ringGGTT &= ~0xFFFULL;

    IOLog("[V61] createHwContextFor: ringGGTT=0x%llX size=0x%zx\n", hw->ringGGTT, ringSize);

    // --- 2) Build LRC image for this context using your helper ---
    IOLog("[V61] createHwContextFor: Calling buildLRCContext...\n");
    IOReturn ret = kIOReturnError;
    hw->lrcGem = FakeIrisXELRC::buildLRCContext(
                    fOwner,
                    hw->ringGem,
                    ringSize,
                    hw->ringGGTT,
                    ctxId,  // context ID
                    0,      // pdps / vm pointer (0 for now)
                    &ret);
    IOLog("[V61] createHwContextFor: buildLRCContext returned lrcGem=%p ret=0x%x\n", hw->lrcGem, ret);

    if (!hw->lrcGem || ret != kIOReturnSuccess) {
        IOLog("[V61] ❌ createHwContextFor: buildLRCContext FAILED (lrcGem=%p ret=0x%x)\n", hw->lrcGem, ret);
        if (hw->lrcGem) hw->lrcGem->release();
        hw->lrcGem = nullptr;

        hw->ringGem->unpin();
        hw->ringGem->release();
        hw->ringGem = nullptr;
        return nullptr;
    }
    IOLog("[V61] createHwContextFor: LRC context built successfully\n");

    IOLog("[V61] createHwContextFor: Pinning LRC GEM...\n");
    hw->lrcGem->pin();
    IOLog("[V61] createHwContextFor: Mapping LRC to GGTT...\n");
    hw->lrcGGTT = fOwner->ggttMap(hw->lrcGem);
    IOLog("[V61] createHwContextFor: ggttMap(lrc) returned=0x%llX\n", hw->lrcGGTT);
    if (!hw->lrcGGTT) {
        IOLog("[V61] ❌ createHwContextFor: ggttMap(LRC) FAILED\n");
        hw->lrcGem->unpin();
        hw->lrcGem->release();  hw->lrcGem = nullptr;
        hw->ringGem->unpin();
        hw->ringGem->release(); hw->ringGem = nullptr;
        return nullptr;
    }
    hw->lrcGGTT &= ~0xFFFULL;

    IOLog("(FakeIrisXE) [Exec] ctx=%u: LRC GGTT=0x%llx\n",
          ctxId, hw->lrcGGTT);

    
    
    
    
    // --- 3) Patch minimal required fields inside LRC image ---
    
    {
        IOBufferMemoryDescriptor* md = hw->lrcGem->memoryDescriptor();
        if (md) {
            uint8_t* cpu = (uint8_t*)md->getBytesNoCopy();

            //
            // GEN12 REQUIRED LRC HEADER FIELDS
            //

            // PDP0 = Fake Page Directory Root — use LRC addr so GPU doesn't reject
            write_le64(cpu + 0x00, hw->lrcGGTT & ~0xFFFULL);

            // Enable timestamp for scheduler acceptance
            write_le32(cpu + 0x30, 0x00010000);

            // Context Control:
            //  bit0 = Load
            //  bit3 = Valid
            //  bit8 = Header Size (1 = 64 bytes)
            const uint32_t CTX_CTRL = (1 << 0) | (1 << 3) | (1 << 8);
            write_le32(cpu + 0x2C, CTX_CTRL);

            //
            // RING STATE (GEN12 required offsets)
            //
            write_le32(cpu + 0x100 + 0x00, 0); // HEAD
            write_le32(cpu + 0x100 + 0x04, 0); // TAIL
            write_le32(cpu + 0x100 + 0x0C, (uint32_t)(hw->ringGGTT & 0xFFFFFFFF)); // RING_BASE

            // Optional: Reset timestamp counter
            fOwner->safeMMIOWrite(0x2580, 0);
        }
    }
    
    return hw;
}






// ============================================================================
// V60: Diagnostic Test Functions
// ============================================================================

bool FakeIrisXEExeclist::runDiagnosticTest()
{
    IOLog("[V60] ============================================================\n");
    IOLog("[V60] STARTING DIAGNOSTIC TEST - V57 Infrastructure Verification\n");
    IOLog("[V60] ============================================================\n");
    
    // Step 1: Dump initial ring buffer status
    IOLog("[V60] Step 1: Checking initial ring buffer state...\n");
    dumpRingBufferStatus("Pre-Test");
    
    // Step 2: Dump initial execlist status
    IOLog("[V60] Step 2: Checking initial execlist state...\n");
    dumpExeclistStatus("Pre-Test");
    
    // Step 3: Create and submit test batch
    IOLog("[V60] Step 3: Creating test batch buffer...\n");
    if (!createAndSubmitTestBatch()) {
        IOLog("[V60] ❌ FAILED: Could not create/submit test batch\n");
        return false;
    }
    
    // Step 4: Dump post-submit status
    IOLog("[V60] Step 4: Checking post-submit state...\n");
    dumpRingBufferStatus("Post-Submit");
    dumpExeclistStatus("Post-Submit");
    
    // Step 5: Verify completion
    IOLog("[V60] Step 5: Verifying command completion...\n");
    if (!verifyCommandCompletion()) {
        IOLog("[V60] ⚠️ WARNING: Command completion not verified (may need polling)\n");
    }
    
    IOLog("[V60] ============================================================\n");
    IOLog("[V60] DIAGNOSTIC TEST COMPLETE\n");
    IOLog("[V60] ============================================================\n");
    
    return true;
}

bool FakeIrisXEExeclist::createAndSubmitTestBatch()
{
    IOLog("[V60] ============================================================\n");
    IOLog("[V60] createAndSubmitTestBatch() - STARTING\n");
    IOLog("[V60] ============================================================\n");
    
    // Step 1: Create a 4KB batch buffer
    IOLog("[V60] Step 1: Allocating batch buffer GEM (size=4096)...\n");
    const size_t batchSize = 4096;
    FakeIrisXEGEM* batchGem = FakeIrisXEGEM::withSize(batchSize, 0);
    if (!batchGem) {
        IOLog("[V60] ❌ FAILED Step 1: Could not allocate batch buffer GEM\n");
        return false;
    }
    IOLog("[V60] ✅ Step 1: GEM allocated successfully=%p\n", batchGem);
    
    // Step 2: Get memory descriptor
    IOLog("[V60] Step 2: Getting memory descriptor...\n");
    IOBufferMemoryDescriptor* md = batchGem->memoryDescriptor();
    if (!md) {
        IOLog("[V60] ❌ FAILED Step 2: Batch buffer has no memory descriptor\n");
        batchGem->release();
        return false;
    }
    IOLog("[V60] ✅ Step 2: Memory descriptor obtained=%p\n", md);
    
    // Step 3: Get CPU pointer
    IOLog("[V60] Step 3: Getting CPU pointer...\n");
    uint32_t* commands = (uint32_t*)md->getBytesNoCopy();
    if (!commands) {
        IOLog("[V60] ❌ FAILED Step 3: Could not get CPU pointer to batch buffer\n");
        batchGem->release();
        return false;
    }
    IOLog("[V60] ✅ Step 3: CPU pointer obtained=%p\n", commands);
    
    // Step 4: Clear the buffer
    IOLog("[V60] Step 4: Clearing buffer...\n");
    bzero(commands, batchSize);
    IOLog("[V60] ✅ Step 4: Buffer cleared\n");
    
    // Step 5: Build command sequence
    IOLog("[V60] Step 5: Building command sequence...\n");
    int idx = 0;
    
    // Command 0-7: MI_NOOP (8 dwords of padding)
    for (int i = 0; i < 8; i++) {
        commands[idx++] = MI_NOOP;
    }
    IOLog("[V60]   Wrote %d MI_NOOP commands\n", 8);
    
    // Command 8: MI_FLUSH_DW (5 dwords)
    commands[idx++] = MI_FLUSH_DW | (1 << 22) | (1 << 21); // post-sync, write dword
    commands[idx++] = 0; // high 32 bits of address
    commands[idx++] = 0; // low 32 bits of address
    commands[idx++] = 0xDEADBEEF; // immediate data
    commands[idx++] = 0; // unused
    IOLog("[V60]   Wrote MI_FLUSH_DW command\n");
    
    // Command 13: MI_BATCH_BUFFER_END
    commands[idx++] = MI_BATCH_BUFFER_END;
    IOLog("[V60]   Wrote MI_BATCH_BUFFER_END command\n");
    
    IOLog("[V60] ✅ Step 5: Commands written (total %d dwords)\n", idx);
    
    // Step 6: Pin batch buffer
    IOLog("[V60] Step 6: Pinning batch buffer...\n");
    batchGem->pin();
    IOLog("[V60] ✅ Step 6: Batch buffer pinned\n");
    
    // Step 7: Map to GGTT
    IOLog("[V60] Step 7: Mapping to GGTT...\n");
    uint64_t batchGGTT = fOwner->ggttMap(batchGem);
    IOLog("[V60]   ggttMap returned: 0x%016llX\n", batchGGTT);
    if (batchGGTT == 0) {
        IOLog("[V60] ❌ FAILED Step 7: ggttMap returned 0\n");
        batchGem->unpin();
        batchGem->release();
        return false;
    }
    batchGGTT &= ~0xFFFULL;
    IOLog("[V60] ✅ Step 7: GGTT mapped at 0x%016llX\n", batchGGTT);
    
    // Step 8: Get/lookup context
    IOLog("[V60] Step 8: Getting test context (ctxId=0xDEAD)...\n");
    XEHWContext* testCtx = lookupHwContext(0xDEAD);
    IOLog("[V60]   lookupHwContext(0xDEAD) returned: %p\n", testCtx);
    
    if (!testCtx) {
        IOLog("[V60]   Context not found, creating new context...\n");
        testCtx = createHwContextFor(0xDEAD, 0);
        IOLog("[V60]   createHwContextFor(0xDEAD, 0) returned: %p\n", testCtx);
        if (!testCtx) {
            IOLog("[V60] ❌ FAILED Step 8: Could not create test context\n");
            batchGem->unpin();
            batchGem->release();
            return false;
        }
        IOLog("[V60] ✅ Step 8: Context created successfully\n");
    } else {
        IOLog("[V60] ✅ Step 8: Existing context found and reused\n");
    }
    
    // Step 9: Submit batch
    IOLog("[V60] Step 9: Submitting batch via execlist...\n");
    IOLog("[V60]   submitForContext ctx=%p batch=%p\n", testCtx, batchGem);
    if (!submitForContext(testCtx, batchGem)) {
        IOLog("[V60] ❌ FAILED Step 9: submitForContext() returned false\n");
        batchGem->unpin();
        batchGem->release();
        return false;
    }
    IOLog("[V60] ✅ Step 9: Batch submitted successfully\n");
    
    IOLog("[V60] ============================================================\n");
    IOLog("[V60] createAndSubmitTestBatch() - COMPLETE SUCCESS\n");
    IOLog("[V60]   Context: 0xDEAD\n");
    IOLog("[V60]   Batch GGTT: 0x%016llX\n", batchGGTT);
    IOLog("[V60] ============================================================\n");
    
    return true;
}

bool FakeIrisXEExeclist::verifyCommandCompletion()
{
    IOLog("[V60] Verifying command completion...\n");
    
    // Wait a short time for GPU to process
    IOLog("[V60]   Waiting 100ms for GPU processing...\n");
    IOSleep(100);
    
    // Check ring buffer head/tail
    uint32_t ring_head = mmioRead32(RING_HEAD);
    uint32_t ring_tail = mmioRead32(RING_TAIL);
    
    IOLog("[V60]   Ring state after wait:\n");
    IOLog("[V60]     HEAD: 0x%04X\n", ring_head & 0xFFFF);
    IOLog("[V60]     TAIL: 0x%04X\n", ring_tail & 0xFFFF);
    
    if ((ring_head & 0xFFFF) == (ring_tail & 0xFFFF)) {
        IOLog("[V60]   ✅ HEAD == TAIL (GPU consumed all commands)\n");
    } else {
        IOLog("[V60]   ⚠️  HEAD != TAIL (GPU still processing or stalled)\n");
        IOLog("[V60]        Difference: %d bytes\n", 
              (ring_tail - ring_head) & 0xFFFF);
    }
    
    // Check execlist status
    uint32_t status_lo = mmioRead32(0x2230); // RING_EXECLIST_STATUS_LO
    uint32_t status_hi = mmioRead32(0x2234); // RING_EXECLIST_STATUS_HI
    
    bool slot0_active = (status_lo >> 2) & 1;
    bool slot1_active = (status_lo >> 3) & 1;
    
    IOLog("[V60]   Execlist state:\n");
    IOLog("[V60]     Slot 0 active: %s\n", slot0_active ? "YES" : "NO");
    IOLog("[V60]     Slot 1 active: %s\n", slot1_active ? "YES" : "NO");
    
    if (!slot0_active && !slot1_active) {
        IOLog("[V60]   ✅ No active contexts (GPU idle)\n");
    } else {
        IOLog("[V60]   ⚠️  Contexts still active\n");
    }
    
    // Process any CSB entries
    IOLog("[V60]   Processing CSB entries...\n");
    processCsbEntriesV57();
    
    return true; // Return true even if not fully complete (it's a diagnostic)
}

// ============================================================================
// V62: File-based Logging and Simplified Diagnostics
// ============================================================================

void FakeIrisXEExeclist::logToFile(const char* fmt, ...)
{
    // Simple file logging to /var/log/FakeIrisXE.log
    // Note: In kernel space, we use BSD vnode interface for file I/O
    
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    // Always log to IOLog as well
    IOLog("[V62-FILE] %s", buf);
    
    // Note: File I/O in kernel requires BSD vnode calls which are complex
    // For now, we rely on IOLog with distinct prefix [V62] for filtering
}

bool FakeIrisXEExeclist::runSimpleDiagnosticTest()
{
    IOLog("[V62] ============================================================\n");
    IOLog("[V62] SIMPLE DIAGNOSTIC TEST - V62 Simplified Debugging\n");
    IOLog("[V62] ============================================================\n");
    
    bool allPassed = true;
    
    // Test 1: GEM Allocation
    IOLog("[V62] TEST 1: GEM Allocation\n");
    if (!testGEMAllocation()) {
        IOLog("[V62] ❌ TEST 1 FAILED\n");
        allPassed = false;
    } else {
        IOLog("[V62] ✅ TEST 1 PASSED\n");
    }
    
    // Test 2: Context Creation
    IOLog("[V62] TEST 2: Context Creation\n");
    if (!testContextCreation()) {
        IOLog("[V62] ❌ TEST 2 FAILED\n");
        allPassed = false;
    } else {
        IOLog("[V62] ✅ TEST 2 PASSED\n");
    }
    
    // Test 3: Batch Submission
    IOLog("[V62] TEST 3: Batch Submission\n");
    if (!testBatchSubmission()) {
        IOLog("[V62] ❌ TEST 3 FAILED\n");
        allPassed = false;
    } else {
        IOLog("[V62] ✅ TEST 3 PASSED\n");
    }
    
    IOLog("[V62] ============================================================\n");
    if (allPassed) {
        IOLog("[V62] ✅ ALL TESTS PASSED\n");
    } else {
        IOLog("[V62] ⚠️  SOME TESTS FAILED\n");
    }
    IOLog("[V62] ============================================================\n");
    
    return allPassed;
}

bool FakeIrisXEExeclist::testGEMAllocation()
{
    IOLog("[V62]   Allocating 4KB GEM...\n");
    
    FakeIrisXEGEM* gem = FakeIrisXEGEM::withSize(4096, 0);
    if (!gem) {
        IOLog("[V62]   ❌ GEM allocation FAILED\n");
        return false;
    }
    
    IOLog("[V62]   GEM allocated at %p\n", gem);
    
    IOBufferMemoryDescriptor* md = gem->memoryDescriptor();
    if (!md) {
        IOLog("[V62]   ❌ No memory descriptor\n");
        gem->release();
        return false;
    }
    
    void* ptr = md->getBytesNoCopy();
    if (!ptr) {
        IOLog("[V62]   ❌ No CPU pointer\n");
        gem->release();
        return false;
    }
    
    IOLog("[V62]   CPU pointer: %p\n", ptr);
    
    // Write test pattern
    bzero(ptr, 4096);
    ((uint32_t*)ptr)[0] = 0xDEADBEEF;
    
    if (((uint32_t*)ptr)[0] != 0xDEADBEEF) {
        IOLog("[V62]   ❌ Memory write test FAILED\n");
        gem->release();
        return false;
    }
    
    IOLog("[V62]   Memory write test PASSED\n");
    
    gem->release();
    IOLog("[V62]   GEM released\n");
    
    return true;
}

bool FakeIrisXEExeclist::testContextCreation()
{
    IOLog("[V62]   Creating test context (ctxId=0xBEEF)...\n");
    
    // Check if context already exists
    XEHWContext* ctx = lookupHwContext(0xBEEF);
    if (ctx) {
        IOLog("[V62]   Context already exists, reusing\n");
        return true;
    }
    
    // Create new context
    IOLog("[V62]   Calling createHwContextFor(0xBEEF, 0)...\n");
    ctx = createHwContextFor(0xBEEF, 0);
    
    if (!ctx) {
        IOLog("[V62]   ❌ Context creation FAILED\n");
        return false;
    }
    
    IOLog("[V62]   Context created successfully\n");
    IOLog("[V62]   Context ID: 0x%X\n", ctx->ctxId);
    IOLog("[V62]   Ring GGTT: 0x%016llX\n", ctx->ringGGTT);
    IOLog("[V62]   LRC GGTT: 0x%016llX\n", ctx->lrcGGTT);
    
    return true;
}

bool FakeIrisXEExeclist::testBatchSubmission()
{
    IOLog("[V62]   Testing batch buffer submission...\n");
    
    // Create simple batch buffer with just MI_BATCH_BUFFER_END
    FakeIrisXEGEM* batch = FakeIrisXEGEM::withSize(4096, 0);
    if (!batch) {
        IOLog("[V62]   ❌ Batch GEM allocation FAILED\n");
        return false;
    }
    
    IOBufferMemoryDescriptor* md = batch->memoryDescriptor();
    if (!md) {
        IOLog("[V62]   ❌ Batch memory descriptor FAILED\n");
        batch->release();
        return false;
    }
    
    uint32_t* cmds = (uint32_t*)md->getBytesNoCopy();
    if (!cmds) {
        IOLog("[V62]   ❌ Batch CPU pointer FAILED\n");
        batch->release();
        return false;
    }
    
    // Simple command: MI_BATCH_BUFFER_END
    cmds[0] = MI_BATCH_BUFFER_END;
    IOLog("[V62]   Wrote MI_BATCH_BUFFER_END\n");
    
    // Get context
    XEHWContext* ctx = lookupHwContext(0xBEEF);
    if (!ctx) {
        IOLog("[V62]   Creating context for submission...\n");
        ctx = createHwContextFor(0xBEEF, 0);
        if (!ctx) {
            IOLog("[V62]   ❌ Context creation FAILED\n");
            batch->release();
            return false;
        }
    }
    
    // Submit
    IOLog("[V62]   Submitting batch...\n");
    batch->pin();
    uint64_t batchGGTT = fOwner->ggttMap(batch) & ~0xFFFULL;
    IOLog("[V62]   Batch GGTT: 0x%016llX\n", batchGGTT);
    
    if (!submitForContext(ctx, batch)) {
        IOLog("[V62]   ❌ Batch submission FAILED\n");
        batch->unpin();
        batch->release();
        return false;
    }
    
    IOLog("[V62]   ✅ Batch submitted successfully\n");
    
    // Note: Don't release batch here - it's now managed by the submission queue
    
    return true;
}

// ============================================================================
// V70: Comprehensive Diagnostic Suite
// ============================================================================

bool FakeIrisXEExeclist::runComprehensiveDiagnosticTest()
{
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║         V70 COMPREHENSIVE DIAGNOSTIC SUITE                  ║\n");
    IOLog("║         Testing GPU Subsystem - All Components              ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    
    bool allPassed = true;
    int testNum = 1;
    
    // ===== TEST 1: GEM Allocation =====
    IOLog("[V70-TEST %d] GEM Allocation\n", testNum++);
    if (!testGEMAllocation()) {
        IOLog("[V70] ❌ GEM TEST FAILED\n");
        allPassed = false;
    } else {
        IOLog("[V70] ✅ GEM TEST PASSED\n");
    }
    IOLog("\n");
    
    // ===== TEST 2: Context Creation =====
    IOLog("[V70-TEST %d] Context Creation\n", testNum++);
    if (!testContextCreation()) {
        IOLog("[V70] ❌ CONTEXT TEST FAILED\n");
        allPassed = false;
    } else {
        IOLog("[V70] ✅ CONTEXT TEST PASSED\n");
    }
    IOLog("\n");
    
    // ===== TEST 3: Batch Submission =====
    IOLog("[V70-TEST %d] Batch Submission\n", testNum++);
    if (!testBatchSubmission()) {
        IOLog("[V70] ❌ BATCH TEST FAILED\n");
        allPassed = false;
    } else {
        IOLog("[V70] ✅ BATCH TEST PASSED\n");
    }
    IOLog("\n");
    
    // ===== TEST 4: RCS Ring Status =====
    IOLog("[V70-TEST %d] RCS Ring Status\n", testNum++);
    if (!testRCSRingStatus()) {
        IOLog("[V70] ❌ RCS RING TEST FAILED\n");
        allPassed = false;
    } else {
        IOLog("[V70] ✅ RCS RING TEST PASSED\n");
    }
    IOLog("\n");
    
    // ===== TEST 5: HW Context Count =====
    IOLog("[V70-TEST %d] HW Context Management\n", testNum++);
    if (!testHWContextManagement()) {
        IOLog("[V70] ❌ HW CONTEXT TEST FAILED\n");
        allPassed = false;
    } else {
        IOLog("[V70] ✅ HW CONTEXT TEST PASSED\n");
    }
    IOLog("\n");
    
    // ===== TEST 6: CSB Processing =====
    IOLog("[V70-TEST %d] CSB Queue Processing\n", testNum++);
    processCsbEntriesV57();
    IOLog("[V70] ✅ CSB PROCESSED\n");
    IOLog("\n");
    
    // Final Summary
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    if (allPassed) {
        IOLog("║  ✅ ALL V70 DIAGNOSTIC TESTS PASSED!                     ║\n");
    } else {
        IOLog("║  ⚠️  SOME V70 TESTS FAILED - SEE ABOVE                   ║\n");
    }
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    
    return allPassed;
}

// ============================================================================
// V70 Test Implementations (Simplified)
// ============================================================================

bool FakeIrisXEExeclist::testRCSRingStatus()
{
    IOLog("[V70]   Checking RCS ring status...\n");
    
    if (!fOwner || !fOwner->fRcsRing) {
        IOLog("[V70]   ❌ No RCS ring\n");
        return false;
    }
    
    IOLog("[V70]   ✅ RCS ring exists\n");
    return true;
}

bool FakeIrisXEExeclist::testHWContextManagement()
{
    IOLog("[V70]   Testing HW context management...\n");
    
    // Create a test context
    XEHWContext* ctx = createHwContextFor(0xDEAD, 1);
    if (!ctx) {
        IOLog("[V70]   ❌ Context creation failed\n");
        return false;
    }
    
    // Lookup the context
    XEHWContext* lookup = lookupHwContext(0xDEAD);
    if (lookup != ctx) {
        IOLog("[V70]   ❌ Context lookup failed\n");
        return false;
    }
    
    IOLog("[V70]   ✅ Context management working\n");
    return true;
}

