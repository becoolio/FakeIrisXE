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
static const uint32_t kAltRcsRbStartLo = 0x23C30;
static const uint32_t kAltRcsRbStartHi = 0x23C34;
static const uint32_t kAltRcsRbHead    = 0x23C38;
static const uint32_t kAltRcsRbTail    = 0x23C3C;

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


FakeIrisXERing::FakeIrisXERing(volatile uint32_t* mmioBase,
                               uint32_t ringBaseOffset,
                               uint32_t headReg,
                               uint32_t tailReg,
                               uint32_t startReg,
                               uint32_t ctlReg,
                               uint32_t altStartLo,
                               uint32_t altStartHi,
                               uint32_t altHead,
                               uint32_t altTail)
: mMMIO(mmioBase),
  mRingBaseOffset(ringBaseOffset),
  mHeadReg(headReg ? headReg : ringHeadReg(ringBaseOffset)),
  mTailReg(tailReg ? tailReg : ringTailReg(ringBaseOffset)),
  mStartReg(startReg ? startReg : ringStartReg(ringBaseOffset)),
  mCtlReg(ctlReg ? ctlReg : ringCtlReg(ringBaseOffset)),
  mAltStartLo(altStartLo),
  mAltStartHi(altStartHi),
  mAltHead(altHead),
  mAltTail(altTail),
  mRingCPU(nullptr),
  mOwnsRingCPU(false),
  mRingSize(0),
  mRingWriteOffset(0),
  mRingGPUAddr(0)
{
    if (!mAltStartLo && !mAltStartHi && !mAltHead && !mAltTail && ringBaseOffset == TGL_RCS0_BASE) {
        mAltStartLo = kAltRcsRbStartLo;
        mAltStartHi = kAltRcsRbStartHi;
        mAltHead = kAltRcsRbHead;
        mAltTail = kAltRcsRbTail;
    }
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

    uint32_t baseStart = mStartReg;

    // V206: Add readback of START before writing
    uint32_t start_before = mmio_read32(mMMIO, baseStart);
    IOLog("(FakeIrisXE)[V206] START (0x%X) before=0x%08X\n", baseStart, start_before);
    
    // V206: Try different sequence - write START first, then do readback
    mmio_write32(mMMIO, baseStart, (uint32_t)mRingGPUAddr);
    
    // V206: Immediate readback to check if value stuck
    uint32_t start_after = mmio_read32(mMMIO, baseStart);
    IOLog("(FakeIrisXE)[V206] START (0x%X) after=0x%08X (expected 0x%08X)\n", 
          baseStart, start_after, (uint32_t)mRingGPUAddr);
    
    if (mAltStartLo) {
        mmio_write32(mMMIO, mAltStartLo, (uint32_t)mRingGPUAddr);
    }
    if (mAltStartHi) {
        mmio_write32(mMMIO, mAltStartHi, (uint32_t)(mRingGPUAddr >> 32));
    }

    (void)mmio_read32(mMMIO, baseStart);
    IOLog("(FakeIrisXE) Ring base programmed: START=0x%X ALT_LO=0x%X ALT_HI=0x%X addr=0x%llX\n",
          baseStart, mAltStartLo, mAltStartHi, (unsigned long long)mRingGPUAddr);
}

void FakeIrisXERing::enableRing()
{
    if (!mMMIO) return;

    const uint32_t headReg = mHeadReg;
    const uint32_t tailReg = mTailReg;
    const uint32_t ctlReg = mCtlReg;

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
    if (mAltHead) {
        mmio_write32(mMMIO, mAltHead, 0);
    }
    if (mAltTail) {
        mmio_write32(mMMIO, mAltTail, 0);
    }
    
    // V206: Add more debugging - read back HEAD/TAIL
    uint32_t head_after = mmio_read32(mMMIO, headReg);
    uint32_t tail_after = mmio_read32(mMMIO, tailReg);
    IOLog("(FakeIrisXE)[V206] HEAD/Tail reset: HEAD=0x%08X TAIL=0x%08X\n", head_after, tail_after);

    IOLog("(FakeIrisXE)[V152] enableRing: size=%zu bytes, units=%d, CTL=0x%08X\n", 
          mRingSize, sizePages, ctlValue);
    
    mmio_write32(mMMIO, ctlReg, ctlValue);
    IOSleep(1);

    uint32_t ctl = mmio_read32(mMMIO, ctlReg);
    IOLog("(FakeIrisXE)[V152] Ring CTL (0x%X) = 0x%08x\n", ctlReg, ctl);
    
    // V207: Check if START is still valid after writing CTL
    uint32_t start_check = mmio_read32(mMMIO, mStartReg);
    IOLog("(FakeIrisXE)[V207] START check after CTL: START=0x%08X (expected 0x%llX)\n", 
          start_check, (unsigned long long)mRingGPUAddr);
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

    uint32_t tailReg = mTailReg;
    uint32_t tail = (uint32_t)mRingWriteOffset;
    mmio_write32(mMMIO, tailReg, tail);
    if (mAltTail) {
        mmio_write32(mMMIO, mAltTail, tail);
    }
    (void)mmio_read32(mMMIO, tailReg);
}

uint32_t FakeIrisXERing::readHWHead()
{
    uint32_t headReg = mHeadReg;
    uint32_t head = mmio_read32(mMMIO, headReg);
    if (!head && mAltHead)
        head = mmio_read32(mMMIO, mAltHead);
    return head;
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
