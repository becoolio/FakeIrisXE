#include "FakeIrisXERing.h"
#include "i915_reg.h"
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <stdatomic.h>
#include "FakeIrisXEExeclist.hpp"

#ifndef RENDER_RING_BASE
#define RENDER_RING_BASE 0x2000
#endif

#ifndef RENDER_RING_HEAD
#define RENDER_RING_HEAD (RENDER_RING_BASE + 0x10)
#endif
#ifndef RENDER_RING_TAIL
#define RENDER_RING_TAIL (RENDER_RING_BASE + 0x20)
#endif
#ifndef RENDER_RING_CTL
#define RENDER_RING_CTL  (RENDER_RING_BASE + 0x30)
#endif
#ifndef RENDER_RING_BASE_LO
#define RENDER_RING_BASE_LO (RENDER_RING_BASE + 0x00)
#endif
#ifndef RENDER_RING_BASE_HI
#define RENDER_RING_BASE_HI (RENDER_RING_BASE + 0x04)
#endif

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
  mRingSize(0),
  mRingWriteOffset(0),
  mRingGPUAddr(0)
{
    IOLog("(FakeIrisXE) Ring created with base offset 0x%X\n", ringBaseOffset);
}

FakeIrisXERing::~FakeIrisXERing()
{
    if (mRingCPU)
        IOFreeAligned(mRingCPU, mRingSize);
}


bool FakeIrisXERing::allocateRing(size_t bytes)
{
    size_t size = (bytes + 4095) & ~4095ULL;

    void* buf = IOMallocAligned(size, 4096);
    if (!buf) return false;

    bzero(buf, size);

    mRingCPU = (uint32_t*)buf;
    mRingSize = size;
    mRingWriteOffset = 0;
    return true;
}

void FakeIrisXERing::attachRingGPUAddress(uint64_t gpu)
{
    mRingGPUAddr = gpu;
}

void FakeIrisXERing::programRingBaseToHW()
{
    if (!mMMIO || !mRingGPUAddr) return;

    uint32_t baseLo = mRingBaseOffset + 0x00;
    uint32_t baseHi = mRingBaseOffset + 0x04;
    
    mmio_write32(mMMIO, baseLo, (uint32_t)mRingGPUAddr);
    mmio_write32(mMMIO, baseHi, (uint32_t)(mRingGPUAddr >> 32));

    (void)mmio_read32(mMMIO, baseLo);
    IOLog("(FakeIrisXE) Ring base programmed: LO=0x%X HI=0x%X addr=0x%llX\n", 
          baseLo, baseHi, (unsigned long long)mRingGPUAddr);
}

void FakeIrisXERing::enableRing()
{
    if (!mMMIO) return;

    uint32_t ctlReg = mRingBaseOffset + 0x30;
    mmio_write32(mMMIO, ctlReg, 1);
    IOSleep(1);

    uint32_t ctl = mmio_read32(mMMIO, ctlReg);
    IOLog("(FakeIrisXE) Ring CTL (0x%X) = 0x%08x\n", ctlReg, ctl);
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

    uint32_t tailReg = mRingBaseOffset + 0x20;
    uint32_t tail = (uint32_t)mRingWriteOffset;
    mmio_write32(mMMIO, tailReg, tail);
    (void)mmio_read32(mMMIO, tailReg);
}

uint32_t FakeIrisXERing::readHWHead()
{
    uint32_t headReg = mRingBaseOffset + 0x10;
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
