#pragma once

#include <stdint.h>
#include <stddef.h>


class FakeIrisXEGEM;
class FakeIrisXEFramebuffer;

class FakeIrisXERing {
public:
    FakeIrisXERing(volatile uint32_t* mmioBase,
                   uint32_t ringBaseOffset = 0x2000,
                   uint32_t headReg = 0,
                   uint32_t tailReg = 0,
                   uint32_t startReg = 0,
                   uint32_t ctlReg = 0,
                   uint32_t altStartLo = 0,
                   uint32_t altStartHi = 0,
                   uint32_t altHead = 0,
                   uint32_t altTail = 0);
    ~FakeIrisXERing();

    // Allocate CPU-visible ring memory
    bool allocateRing(size_t bytes);

    // GPU address provided by GGTT mapping
    void attachRingGPUAddress(uint64_t gpuAddr);
    void attachRingCPUAddress(void* cpuAddr);

    // Program MMIO registers (ring base, enable ring)
    void programRingBaseToHW();
    void enableRing();

    // Software ring operations
    void pushDword(uint32_t dword);
    void flushRingCpuCache();
    void updateHWTail();
    uint32_t readHWHead();

    FakeIrisXEFramebuffer* fOwner;

    
    
    // Submit a batch buffer GPU address using MI_BATCH_BUFFER_START
    bool submitBatch64(uint64_t batchGpuAddr);

    // Getter
    size_t size() const { return mRingSize; }
    uint64_t gpuAddr() const { return mRingGPUAddr; }

    // Set ring size (needed before enableRing since we use GEM for memory)
    void setRingSize(size_t bytes) { mRingSize = bytes; }

private:
    volatile uint32_t* mMMIO;   // BAR0 base
    uint32_t           mRingBaseOffset;  // Engine base offset (0x2000 for RCS0 on TGL)
    uint32_t           mHeadReg;
    uint32_t           mTailReg;
    uint32_t           mStartReg;
    uint32_t           mCtlReg;
    uint32_t           mAltStartLo;
    uint32_t           mAltStartHi;
    uint32_t           mAltHead;
    uint32_t           mAltTail;
    uint32_t*          mRingCPU;
    bool               mOwnsRingCPU;
    size_t             mRingSize;
    uint64_t           mRingWriteOffset;
    uint64_t           mRingGPUAddr;
};
