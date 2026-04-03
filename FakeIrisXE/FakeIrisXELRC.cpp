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

    write_le64(ctxCpu + 0x00, pageTableRoot & ~0xFFFULL);
    write_le64(ctxCpu + 0x08, 0);
    write_le64(ctxCpu + 0x10, 0);
    write_le64(ctxCpu + 0x18, 0);
    write_le32(ctxCpu + 0x30, 0x00010000);

    const uint32_t ctxCtrl = (1u << 0) | (1u << 3) | (1u << 8);
    write_le32(ctxCpu + 0x2C, ctxCtrl);

    const uint32_t ringStateOff = 0x100u;
    const uint32_t headBytes = ringHead & (uint32_t)(ringSize - 1u);
    const uint32_t tailBytes = ringTail & (uint32_t)(ringSize - 1u);
    write_le32(ctxCpu + ringStateOff + 0x00, headBytes);
    write_le32(ctxCpu + ringStateOff + 0x04, tailBytes);
    write_le32(ctxCpu + ringStateOff + 0x08, (uint32_t)(ringGpuAddr & 0xFFFFFFFFu));
    write_le32(ctxCpu + ringStateOff + 0x0C, (uint32_t)(ringGpuAddr >> 32));

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

    const size_t ctxSize = 16 * 4096;
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
    ctxGpu &= ~0xFFFULL;
    if (!ctxGpu) {
        ctxGem->unpin();
        ctxGem->release();
        return nullptr;
    }

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

    IOLog("GEN12 LRC built: ctxGpu=0x%llx ppgtt=0x%llx ringGpu=0x%llx head=%u tail=%u ctl=0x%08x\n",
          (unsigned long long)ctxGpu,
          (unsigned long long)effectivePageTableRoot,
          (unsigned long long)ringGpuAddr,
          ringHead & (uint32_t)(ringSize - 1u),
          ringTail & (uint32_t)(ringSize - 1u),
          ringCtl);

    if (outErr) *outErr = kIOReturnSuccess;
    return ctxGem;
}
