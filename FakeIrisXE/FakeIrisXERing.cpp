#include "FakeIrisXERing.h"
#include "i915_reg.h"
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <stdatomic.h>
#include "FakeIrisXEExeclist.hpp"

static inline uint32_t ringTailReg(uint32_t base)  { return base + 0x30; }
static inline uint32_t ringHeadReg(uint32_t base)  { return base + 0x34; }
static inline uint32_t ringStartReg(uint32_t base) { return base + 0x38; }
static inline uint32_t ringCtlReg(uint32_t base)   { return base + 0x3C; }

static inline void mmio_write32(volatile uint32_t* mmio, uint32_t off, uint32_t val)
{
    volatile uint32_t* addr = (volatile uint32_t*)((uintptr_t)mmio + off);
    *addr = val;
    (void)*addr;
}

static inline uint32_t mmio_read32(volatile uint32_t* mmio, uint32_t off)
{
    return *(volatile uint32_t*)((uintptr_t)mmio + off);
}


FakeIrisXERing::FakeIrisXERing(volatile uint32_t* mmioBase, uint32_t ringBaseOffset)
: mMMIO(mmioBase),
  mRingBaseOffset(ringBaseOffset),
  mRingCPU(nullptr),
  mOwnsRingCPU(false),
  mRingSize(0),
  mRingWriteOffset(0),
  mRingGPUAddr(0)
{
    IOLog("(FakeIrisXE) Ring created with base offset 0x%X\n", ringBaseOffset);
}

FakeIrisXERing::~FakeIrisXERing()
{
    if (mRingCPU && mOwnsRingCPU)
        IOFreeAligned(mRingCPU, mRingSize);
}


bool FakeIrisXERing::allocateRing(size_t bytes)
{
    size_t size = (bytes + 4095) & ~4095ULL;

    void* buf = IOMallocAligned(size, 4096);
    if (!buf) return false;

    bzero(buf, size);

    mRingCPU = (uint32_t*)buf;
    mOwnsRingCPU = true;
    mRingSize = size;
    mRingWriteOffset = 0;
    return true;
}

void FakeIrisXERing::attachRingGPUAddress(uint64_t gpu)
{
    mRingGPUAddr = gpu;
}

void FakeIrisXERing::attachRingCPUAddress(void* cpuAddr)
{
    mRingCPU = static_cast<uint32_t*>(cpuAddr);
    mOwnsRingCPU = false;
    mRingWriteOffset = 0;
}

void FakeIrisXERing::programRingBaseToHW()
{
    if (!mMMIO || !mRingGPUAddr) return;

    uint32_t baseStart = ringStartReg(mRingBaseOffset);

    mmio_write32(mMMIO, baseStart, (uint32_t)mRingGPUAddr);

    (void)mmio_read32(mMMIO, baseStart);
    IOLog("(FakeIrisXE) Ring base programmed: START=0x%X addr=0x%llX\n",
          baseStart, (unsigned long long)mRingGPUAddr);
}

void FakeIrisXERing::enableRing()
{
    if (!mMMIO) return;

    const uint32_t headReg = ringHeadReg(mRingBaseOffset);
    const uint32_t tailReg = ringTailReg(mRingBaseOffset);
    const uint32_t ctlReg = ringCtlReg(mRingBaseOffset);

    // Gen9+ ring CTL: bit0=enable, bits 20:12=(ring pages - 1)
    // where each page is 4KB.
    uint32_t sizePages = static_cast<uint32_t>(mRingSize >> 12);
    if (sizePages == 0) {
        sizePages = 1;
    }
    uint32_t ctlValue = 1u | ((sizePages - 1u) << 12);

    // Reset head/tail before enabling.
    mmio_write32(mMMIO, headReg, 0);
    mmio_write32(mMMIO, tailReg, 0);

    IOLog("(FakeIrisXE)[V152] enableRing: size=%zu bytes, units=%d, CTL=0x%08X\n", 
          mRingSize, sizePages, ctlValue);
    
    mmio_write32(mMMIO, ctlReg, ctlValue);
    IOSleep(1);

    uint32_t ctl = mmio_read32(mMMIO, ctlReg);
    IOLog("(FakeIrisXE)[V152] Ring CTL (0x%X) = 0x%08x\n", ctlReg, ctl);
}



void FakeIrisXERing::pushDword(uint32_t d)
{
    if (!mRingCPU) return;

    size_t idx = (mRingWriteOffset >> 2) % (mRingSize >> 2);
    mRingCPU[idx] = d;

    mRingWriteOffset += 4;
    if (mRingWriteOffset >= mRingSize)
        mRingWriteOffset = 0;
}

void FakeIrisXERing::flushRingCpuCache()
{
    atomic_thread_fence(memory_order_seq_cst);
}

void FakeIrisXERing::updateHWTail()
{
    if (!mMMIO) return;

    uint32_t tailReg = ringTailReg(mRingBaseOffset);
    uint32_t tail = (uint32_t)mRingWriteOffset;
    mmio_write32(mMMIO, tailReg, tail);
    (void)mmio_read32(mMMIO, tailReg);
}

uint32_t FakeIrisXERing::readHWHead()
{
    uint32_t headReg = ringHeadReg(mRingBaseOffset);
    return mmio_read32(mMMIO, headReg);
}

bool FakeIrisXERing::submitBatch64(uint64_t gpu)
{
    if (!mRingCPU) return false;

    const uint32_t MI_BATCH_START_64 = (0x31u << 23) | (1 << 8);
    const uint32_t MI_BATCH_END      = (0x0Au << 23);

    pushDword(MI_BATCH_START_64);
    pushDword((uint32_t)gpu);
    pushDword((uint32_t)(gpu >> 32));
    pushDword(MI_BATCH_END);

    flushRingCpuCache();
    updateHWTail();

    return true;
}
