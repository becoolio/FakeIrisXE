//
//  FakeIrisXELRC.cpp
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 01/12/25.
//

// FakeIrisXELRC.cpp
#include "FakeIrisXELRC.hpp"
#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXERing.h"
#include "i915_reg.h"
#include <libkern/OSAtomic.h>

static inline uint32_t read_le32(const uint8_t* p) { return *(const uint32_t*)p; }


bool FakeIrisXELRC::initializeLRCContextImage(
    uint8_t* ctxCpu,
    size_t ctxSize,
    uint64_t pageTableRoot,
    size_t ringSize,
    uint64_t ringGpuAddr,
    uint32_t ringHead,
    uint32_t ringTail,
    uint32_t* outRingCtl)
{
    if (!ctxCpu || ctxSize < (0x100u + 0x14u) || ringSize == 0u || (ringSize & 4095u) != 0u) {
        return false;
    }

    bzero(ctxCpu, ctxSize);

    // V320: Apple-style LRC header initialization
    // PDP0 descriptor (0x2270-0x2274) - page table root
    write_le32(ctxCpu + 0x00, (uint32_t)(pageTableRoot & 0xFFFFFFFFu));
    write_le32(ctxCpu + 0x04, (uint32_t)(pageTableRoot >> 32));
    // PDP1-3 (0x2278-0x228C) - typically zero for single-level PPGTT
    write_le32(ctxCpu + 0x08, 0);
    write_le32(ctxCpu + 0x0C, 0);
    write_le32(ctxCpu + 0x10, 0);
    write_le32(ctxCpu + 0x14, 0);
    write_le32(ctxCpu + 0x18, 0);
    write_le32(ctxCpu + 0x1C, 0);

    // Context control (0x22C) - VALID + legacy mode + privilege
    // Apple uses: (1<<0) VALID | (1<<3) legacy | (1<<8) privilege = 0x109
    const uint32_t ctxCtrl = 0x109u;
    write_le32(ctxCpu + 0x2C, ctxCtrl);

    const uint32_t ringStateOff = 0x100u;
    // Ring head at 0x100, tail at 0x104, start at 0x108, ctl at 0x10C (Apple-style)
    const uint32_t headBytes = ringHead & (uint32_t)(ringSize - 1u);
    const uint32_t tailBytes = ringTail & (uint32_t)(ringSize - 1u);
    write_le32(ctxCpu + ringStateOff + 0x00, headBytes);  // HEAD
    write_le32(ctxCpu + ringStateOff + 0x04, tailBytes);  // TAIL
    write_le32(ctxCpu + ringStateOff + 0x08, (uint32_t)(ringGpuAddr & 0xFFFFFFFFu));  // START_LO
    write_le32(ctxCpu + ringStateOff + 0x0C, (uint32_t)(ringGpuAddr >> 32));  // START_HI

    uint32_t pages = (uint32_t)(ringSize / 4096u);
    if (!pages) {
        pages = 1u;
    }
    const uint32_t ringCtl = ((pages - 1u) << 12) | 1u;
    write_le32(ctxCpu + ringStateOff + 0x10, ringCtl);

    if (outRingCtl) {
        *outRingCtl = ringCtl;
    }

    __sync_synchronize();
    OSSynchronizeIO();
    return true;
}

FakeIrisXEGEM* FakeIrisXELRC::buildLRCContext(
    FakeIrisXEFramebuffer* fb,
    FakeIrisXEGEM* ringGem,
    size_t ringSize,
    uint64_t ringGpuAddr,
    uint32_t ringHead,
    uint32_t ringTail,
    uint64_t pageTableRoot,
    IOReturn* outErr)
{
    if (outErr) *outErr = kIOReturnError;
    if (!fb || !ringGem) return nullptr;

    // V340.4: Gen12 LRC size - Gen12 uses 2KB (0x800) context, not 64KB
    // Gen12 LRC layout: 0x000-0x0FF = header, 0x100-0x1FF = ring state, 0x200+ = per-engine
    const size_t ctxSize = 0x800;  // Gen12 LRC is 2KB
    
    FakeIrisXEGEM* ctxGem = FakeIrisXEGEM::withSize(ctxSize, 0);
    if (!ctxGem) return nullptr;

    ctxGem->pin();
    IOBufferMemoryDescriptor* md = ctxGem->memoryDescriptor();
    if (!md) {
        ctxGem->unpin();
        ctxGem->release();
        return nullptr;
    }

    uint8_t* p = (uint8_t*)md->getBytesNoCopy();
    if (!p) {
        ctxGem->unpin();
        ctxGem->release();
        return nullptr;
    }
    uint64_t ctxGpu = ctxGem->gpuAddress();
    if (!ctxGpu) {
        ctxGpu = fb->ggttMap(ctxGem);
    }
    // V340.4: Gen12 LRC must be 64-byte aligned
    ctxGpu &= ~0x3FULL;
    if (!ctxGpu) {
        ctxGem->unpin();
        ctxGem->release();
        return nullptr;
    }

    IOLog("(FakeIrisXE) [V340.4][LRC] Building Gen12 LRC: ctxSize=0x%zX ctxGpu=0x%llX\n", ctxSize, (unsigned long long)ctxGpu);

    //
    // ===== GEN12 LRC HEADER =====
    //

    const uint64_t effectivePageTableRoot = pageTableRoot ? pageTableRoot : (ctxGpu & ~0xFFFULL);
    uint32_t ringCtl = 0;
    if (!initializeLRCContextImage(p, ctxSize, effectivePageTableRoot, ringSize, ringGpuAddr, ringHead, ringTail, &ringCtl)) {
        ctxGem->unpin();
        ctxGem->release();
        return nullptr;
    }

    // V340.4: Additional Gen12 context setup - CTX_TIMESTAMP (0x20)
    write_le32(p + 0x20, 0);  // Timestamp = 0
    
    // V340.4: CTX_STATUS (0x24) - valid, idle
    write_le32(p + 0x24, 0x00010000);  // STATUS = idle
    
    // V340.4: More Gen12-specific initialization for RCS engine
    // PP_DIR_DCL (0x30) - PPGTT directory base
    write_le32(p + 0x30, (uint32_t)(effectivePageTableRoot & 0xFFFFFFFFu));
    write_le32(p + 0x34, (uint32_t)(effectivePageTableRoot >> 32));

    IOLog("(FakeIrisXE) [V340.4][LRC] Gen12 LRC built: ctxGpu=0x%llX ppgtt=0x%llX ringGpu=0x%llX head=%u tail=%u ctl=0x%08x\n",
          (unsigned long long)ctxGpu,
          (unsigned long long)effectivePageTableRoot,
          (unsigned long long)ringGpuAddr,
          ringHead & (uint32_t)(ringSize - 1u),
          ringTail & (uint32_t)(ringSize - 1u),
          ringCtl);

    if (outErr) *outErr = kIOReturnSuccess;
    return ctxGem;
}

// V270: LRC context management
bool FakeIrisXELRC::validateContextImage(const uint8_t* ctxCpu, size_t ctxSize) {
    if (!ctxCpu || ctxSize < 0x1000) return false;
    uint32_t ctxCtrl = read_le32(ctxCpu + 0x2C);
    return (ctxCtrl & 0x1) != 0;
}

bool FakeIrisXELRC::updateContextPriority(uint8_t* ctxCpu, uint32_t priority) {
    if (!ctxCpu) return false;
    uint32_t ctxCtrl = read_le32(ctxCpu + 0x2C);
    ctxCtrl = (ctxCtrl & ~0xF0) | ((priority & 0xF) << 4);
    write_le32(ctxCpu + 0x2C, ctxCtrl);
    return true;
}

uint32_t FakeIrisXELRC::getContextPriority(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    uint32_t ctxCtrl = read_le32(ctxCpu + 0x2C);
    return (ctxCtrl >> 4) & 0xF;
}

bool FakeIrisXELRC::setContextPreemption(uint8_t* ctxCpu, bool enable) {
    if (!ctxCpu) return false;
    uint32_t ctxCtrl = read_le32(ctxCpu + 0x2C);
    if (enable) ctxCtrl |= (1u << 2);
    else ctxCtrl &= ~(1u << 2);
    write_le32(ctxCpu + 0x2C, ctxCtrl);
    return true;
}

bool FakeIrisXELRC::isContextPreemptible(const uint8_t* ctxCpu) {
    if (!ctxCpu) return false;
    uint32_t ctxCtrl = read_le32(ctxCpu + 0x2C);
    return (ctxCtrl & (1u << 2)) != 0;
}

// V270: Ring state management
bool FakeIrisXELRC::updateRingHead(uint8_t* ctxCpu, uint32_t head) {
    if (!ctxCpu) return false;
    write_le32(ctxCpu + 0x100, head);
    return true;
}

bool FakeIrisXELRC::updateRingTail(uint8_t* ctxCpu, uint32_t tail) {
    if (!ctxCpu) return false;
    write_le32(ctxCpu + 0x104, tail);
    return true;
}

uint32_t FakeIrisXELRC::getRingHead(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    return read_le32(ctxCpu + 0x100);
}

uint32_t FakeIrisXELRC::getRingTail(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    return read_le32(ctxCpu + 0x104);
}

uint64_t FakeIrisXELRC::getRingStart(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    uint32_t lo = read_le32(ctxCpu + 0x108);
    uint32_t hi = read_le32(ctxCpu + 0x10C);
    return ((uint64_t)hi << 32) | lo;
}

uint32_t FakeIrisXELRC::getRingControl(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    return read_le32(ctxCpu + 0x110);
}

bool FakeIrisXELRC::updateRingSize(uint8_t* ctxCpu, size_t newSize) {
    if (!ctxCpu || newSize < 4096) return false;
    uint32_t pages = (uint32_t)(newSize / 4096);
    if (pages == 0) pages = 1;
    uint32_t ringCtl = ((pages - 1) << 12) | 1;
    write_le32(ctxCpu + 0x110, ringCtl);
    return true;
}

// V270: Page table management
bool FakeIrisXELRC::updatePageTableRoot(uint8_t* ctxCpu, uint64_t ppgttRoot) {
    if (!ctxCpu) return false;
    write_le32(ctxCpu + 0x00, (uint32_t)(ppgttRoot & 0xFFFFFFFF));
    write_le32(ctxCpu + 0x04, (uint32_t)(ppgttRoot >> 32));
    return true;
}

uint64_t FakeIrisXELRC::getPageTableRoot(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    uint32_t lo = read_le32(ctxCpu + 0x00);
    uint32_t hi = read_le32(ctxCpu + 0x04);
    return ((uint64_t)hi << 32) | lo;
}

bool FakeIrisXELRC::setContextAActionMode(uint8_t* ctxCpu, uint32_t mode) {
    if (!ctxCpu) return false;
    write_le32(ctxCpu + 0x38, mode);
    return true;
}

uint32_t FakeIrisXELRC::getContextAActionMode(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    return read_le32(ctxCpu + 0x38);
}

// V270: Context save/restore
bool FakeIrisXELRC::saveContextState(const uint8_t* ctxCpu, uint8_t* saveArea, size_t saveSize) {
    if (!ctxCpu || !saveArea || saveSize < 0x1000) return false;
    memcpy(saveArea, ctxCpu, saveSize);
    return true;
}

bool FakeIrisXELRC::restoreContextState(uint8_t* ctxCpu, const uint8_t* saveArea, size_t saveSize) {
    if (!ctxCpu || !saveArea || saveSize < 0x1000) return false;
    memcpy(ctxCpu, saveArea, saveSize);
    return true;
}

bool FakeIrisXELRC::copyContextImage(uint8_t* destCtx, const uint8_t* srcCtx, size_t ctxSize) {
    if (!destCtx || !srcCtx || ctxSize < 0x1000) return false;
    memcpy(destCtx, srcCtx, ctxSize);
    return true;
}

// V270: Context auditing
bool FakeIrisXELRC::auditContextIntegrity(const uint8_t* ctxCpu, size_t ctxSize) {
    if (!ctxCpu || ctxSize < 0x1000) return false;
    uint32_t ctxCtrl = read_le32(ctxCpu + 0x2C);
    if ((ctxCtrl & 0x1) == 0) {
        IOLog("(FakeIrisXE) [V270] Context not valid\n");
        return false;
    }
    return true;
}

void FakeIrisXELRC::dumpContextHeader(const uint8_t* ctxCpu) {
    if (!ctxCpu) return;
    IOLog("(FakeIrisXE) [V270] Context Header:\n");
    IOLog("  PDP0: 0x%llX\n", (unsigned long long)getPageTableRoot(ctxCpu));
    IOLog("  CTX_CTL: 0x%08X\n", read_le32(ctxCpu + 0x2C));
    IOLog("  AA_MODE: 0x%08X\n", read_le32(ctxCpu + 0x38));
}

void FakeIrisXELRC::dumpRingState(const uint8_t* ctxCpu) {
    if (!ctxCpu) return;
    IOLog("(FakeIrisXE) [V270] Ring State:\n");
    IOLog("  HEAD: 0x%08X\n", getRingHead(ctxCpu));
    IOLog("  TAIL: 0x%08X\n", getRingTail(ctxCpu));
    IOLog("  START: 0x%llX\n", (unsigned long long)getRingStart(ctxCpu));
    IOLog("  CTL: 0x%08X\n", getRingControl(ctxCpu));
}

void FakeIrisXELRC::dumpAllContextRegisters(const uint8_t* ctxCpu) {
    if (!ctxCpu) return;
    dumpContextHeader(ctxCpu);
    dumpRingState(ctxCpu);
    IOLog("  PDP1: 0x%08X\n", read_le32(ctxCpu + 0x08));
    IOLog("  PDP2: 0x%08X\n", read_le32(ctxCpu + 0x10));
    IOLog("  PDP3: 0x%08X\n", read_le32(ctxCpu + 0x18));
}

// V270: Hardware context initialization
bool FakeIrisXELRC::initializeContextForRender(uint8_t* ctxCpu, size_t ctxSize) {
    if (!ctxCpu || ctxSize < 0x1000) return false;
    uint32_t ctxCtrl = 0x109 | (1u << 6);
    write_le32(ctxCpu + 0x2C, ctxCtrl);
    write_le32(ctxCpu + 0x38, 0x1);
    return true;
}

bool FakeIrisXELRC::initializeContextForVideo(uint8_t* ctxCpu, size_t ctxSize) {
    if (!ctxCpu || ctxSize < 0x1000) return false;
    uint32_t ctxCtrl = 0x109 | (1u << 7);
    write_le32(ctxCpu + 0x2C, ctxCtrl);
    write_le32(ctxCpu + 0x38, 0x2);
    return true;
}

bool FakeIrisXELRC::initializeContextForBlitter(uint8_t* ctxCpu, size_t ctxSize) {
    if (!ctxCpu || ctxSize < 0x1000) return false;
    uint32_t ctxCtrl = 0x109 | (1u << 8);
    write_le32(ctxCpu + 0x2C, ctxCtrl);
    write_le32(ctxCpu + 0x38, 0x4);
    return true;
}

bool FakeIrisXELRC::initializeContextForCompute(uint8_t* ctxCpu, size_t ctxSize) {
    if (!ctxCpu || ctxSize < 0x1000) return false;
    uint32_t ctxCtrl = 0x109 | (1u << 9);
    write_le32(ctxCpu + 0x2C, ctxCtrl);
    write_le32(ctxCpu + 0x38, 0x8);
    return true;
}

// V270: Context scheduling
bool FakeIrisXELRC::setContextWeight(uint8_t* ctxCpu, uint32_t weight) {
    if (!ctxCpu) return false;
    write_le32(ctxCpu + 0x40, weight & 0xFF);
    return true;
}

uint32_t FakeIrisXELRC::getContextWeight(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    return read_le32(ctxCpu + 0x40) & 0xFF;
}

bool FakeIrisXELRC::setContextTimeSlice(uint8_t* ctxCpu, uint32_t microseconds) {
    if (!ctxCpu) return false;
    write_le32(ctxCpu + 0x44, microseconds);
    return true;
}

uint32_t FakeIrisXELRC::getContextTimeSlice(const uint8_t* ctxCpu) {
    if (!ctxCpu) return 0;
    return read_le32(ctxCpu + 0x44);
}

// V270: Debug and diagnostics
void FakeIrisXELRC::diagnoseContext(uint8_t* ctxCpu) {
    IOLog("(FakeIrisXE) [V270] LRC Context Diagnosis:\n");
    if (!ctxCpu) { IOLog("  Invalid context pointer\n"); return; }
    dumpAllContextRegisters(ctxCpu);
    IOLog("  Priority: %u\n", getContextPriority(ctxCpu));
    IOLog("  Preemptible: %s\n", isContextPreemptible(ctxCpu) ? "YES" : "NO");
    IOLog("  Weight: %u\n", getContextWeight(ctxCpu));
    IOLog("  TimeSlice: %u us\n", getContextTimeSlice(ctxCpu));
}

bool FakeIrisXELRC::verifyContextPermissions(const uint8_t* ctxCpu) {
    if (!ctxCpu) return false;
    uint32_t ctxCtrl = read_le32(ctxCpu + 0x2C);
    return (ctxCtrl & 0x100) != 0;
}

uint32_t FakeIrisXELRC::calculateContextChecksum(const uint8_t* ctxCpu, size_t size) {
    if (!ctxCpu || size == 0) return 0;
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i++) {
        checksum += ctxCpu[i];
    }
    return checksum;
}
