//
//  FakeIrisXELRC.hpp
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 01/12/25.
//
// FakeIrisXELRC.hpp
#pragma once
#include <IOKit/IOLib.h>
#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXEFramebuffer.hpp"


class FakeIrisXELRC {
public:
    static bool initializeLRCContextImage(
           uint8_t*                ctxCpu,
           size_t                  ctxSize,
           uint64_t                pageTableRoot,
           size_t                  ringSize,
           uint64_t               ringGpuAddr,
           uint32_t               ringHead,
           uint32_t               ringTail,
           uint32_t*               outRingCtl);

    static FakeIrisXEGEM* buildLRCContext(
           FakeIrisXEFramebuffer* fb,
           FakeIrisXEGEM*         ringGem,
           size_t                 ringSize,
           uint64_t               ringGpuAddr,
           uint32_t               ringHead,
           uint32_t               ringTail,
           uint64_t               pageTableRoot,
           IOReturn*              outErr);
    
    static void write_le32(uint8_t* p, uint32_t v) { *(uint32_t*)p = v; }
    static void write_le64(uint8_t* p, uint64_t v) { *(uint64_t*)p = v; }

    // V270: LRC context management
    static bool validateContextImage(const uint8_t* ctxCpu, size_t ctxSize);
    static bool updateContextPriority(uint8_t* ctxCpu, uint32_t priority);
    static uint32_t getContextPriority(const uint8_t* ctxCpu);
    static bool setContextPreemption(uint8_t* ctxCpu, bool enable);
    static bool isContextPreemptible(const uint8_t* ctxCpu);

    // V270: Ring state management
    static bool updateRingHead(uint8_t* ctxCpu, uint32_t head);
    static bool updateRingTail(uint8_t* ctxCpu, uint32_t tail);
    static uint32_t getRingHead(const uint8_t* ctxCpu);
    static uint32_t getRingTail(const uint8_t* ctxCpu);
    static uint64_t getRingStart(const uint8_t* ctxCpu);
    static uint32_t getRingControl(const uint8_t* ctxCpu);
    static bool updateRingSize(uint8_t* ctxCpu, size_t newSize);

    // V270: Page table management
    static bool updatePageTableRoot(uint8_t* ctxCpu, uint64_t ppgttRoot);
    static uint64_t getPageTableRoot(const uint8_t* ctxCpu);
    static bool setContextAActionMode(uint8_t* ctxCpu, uint32_t mode);
    static uint32_t getContextAActionMode(const uint8_t* ctxCpu);

    // V270: Context save/restore
    static bool saveContextState(const uint8_t* ctxCpu, uint8_t* saveArea, size_t saveSize);
    static bool restoreContextState(uint8_t* ctxCpu, const uint8_t* saveArea, size_t saveSize);
    static bool copyContextImage(uint8_t* destCtx, const uint8_t* srcCtx, size_t ctxSize);

    // V270: LRC region access
    static uint8_t* getContextHeader(uint8_t* ctxCpu) { return ctxCpu; }
    static uint8_t* getRingStateRegion(uint8_t* ctxCpu) { return ctxCpu + 0x100; }
    static uint8_t* getPerContextRegion(uint8_t* ctxCpu) { return ctxCpu + 0x200; }
    static uint8_t* getTimestampRegion(uint8_t* ctxCpu) { return ctxCpu + 0x300; }

    // V270: Context auditing
    static bool auditContextIntegrity(const uint8_t* ctxCpu, size_t ctxSize);
    static void dumpContextHeader(const uint8_t* ctxCpu);
    static void dumpRingState(const uint8_t* ctxCpu);
    static void dumpAllContextRegisters(const uint8_t* ctxCpu);

    // V270: Hardware context initialization
    static bool initializeContextForRender(uint8_t* ctxCpu, size_t ctxSize);
    static bool initializeContextForVideo(uint8_t* ctxCpu, size_t ctxSize);
    static bool initializeContextForBlitter(uint8_t* ctxCpu, size_t ctxSize);
    static bool initializeContextForCompute(uint8_t* ctxCpu, size_t ctxSize);

    // V270: Context scheduling
    static bool setContextWeight(uint8_t* ctxCpu, uint32_t weight);
    static uint32_t getContextWeight(const uint8_t* ctxCpu);
    static bool setContextTimeSlice(uint8_t* ctxCpu, uint32_t microseconds);
    static uint32_t getContextTimeSlice(const uint8_t* ctxCpu);

    // V270: Debug and diagnostics
    static void diagnoseContext(uint8_t* ctxCpu);
    static bool verifyContextPermissions(const uint8_t* ctxCpu);
    static uint32_t calculateContextChecksum(const uint8_t* ctxCpu, size_t size);
};
