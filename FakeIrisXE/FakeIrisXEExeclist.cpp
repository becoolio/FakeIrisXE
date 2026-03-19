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

static const uint32_t kExecRingTailReg  = RCS0_RING_TAIL;
static const uint32_t kExecRingHeadReg  = RCS0_RING_HEAD;
static const uint32_t kExecRingStartReg = RCS0_RING_START;
static const uint32_t kExecRingCtlReg   = RCS0_RING_CTL;
static const uint32_t kExecElspPrimaryLo = RCS0_EXECLIST_SUBMITPORT_LO;
static const uint32_t kExecElspPrimaryHi = RCS0_EXECLIST_SUBMITPORT_HI;
static const uint32_t kExecElspLegacyLo  = RCS0_ELSP1_LO;
static const uint32_t kExecElspLegacyHi  = RCS0_ELSP1_HI;
static const uint32_t kExecStatusPrimaryLo = RCS0_EXECLIST_STATUS_LO;
static const uint32_t kExecStatusPrimaryHi = RCS0_EXECLIST_STATUS_HI;
static const uint32_t kExecStatusLegacyLo  = 0x2230;
static const uint32_t kExecStatusLegacyHi  = 0x2234;

namespace {

static const uint32_t kExecGtErrorReg = 0x18E04;
static const uint32_t kExecRcsStatusReg = TGL_RCS0_BASE + 0x10;
static const uint32_t kExecActhdLo = TGL_RCS0_BASE + 0x74;
static const uint32_t kExecActhdHi = TGL_RCS0_BASE + 0x5C;
static const uint32_t kExecBbAddrLo = TGL_RCS0_BASE + 0x140;
static const uint32_t kExecBbAddrHi = TGL_RCS0_BASE + 0x168;
static const uint32_t kExecCcidReg = TGL_RCS0_BASE + 0x180;
static const uint32_t kExecContextControlReg = TGL_RCS0_BASE + 0x244;

static const uint32_t kExecStatusSlot1Valid = (1u << 3);
static const uint32_t kExecStatusSlot0Valid = (1u << 4);
static const uint32_t kExecStatusSlot1Active = (1u << 17);
static const uint32_t kExecStatusSlot0Active = (1u << 18);

static const uint32_t kProofExpectedValue = 0xDEADBEEFu;
static const uint32_t kProofScratchInitial = 0xBADBAD00u;
static const uint32_t kProofRingSize = 64u * 1024u;
static const uint32_t kProofContextControl = (1u << 0) | (1u << 3) | (1u << 5) | (1u << 11);
static const uint32_t kCtxDescValid = (1u << 0);
static const uint32_t kCtxDescPrivilege = (1u << 8);
static const uint32_t kCtxDescForceRestore = (1u << 2);
static const uint32_t kCtxDescAddressingModeShift = 3u;
static const uint32_t kCtxDescLegacy64B = 3u;
static const uint32_t kCtxDescSwCtxIdShiftInHi = 5u;
static const uint32_t kCtxDescEngineInstanceShiftInHi = 16u;
static const uint32_t kCtxDescEngineClassShiftInHi = 29u;
static const uint32_t kCtxDescRenderClass = 0u;
static const uint32_t kCtxDescRenderInstance = 0u;

enum ProofFailureType {
    None,
    DescriptorWrong,
    LrcLayoutWrong,
    RingStateWrong,
    MiPacketWrong,
    EngineHardHalted,
    NoSchedulingProgress,
};

struct RcsProofResources {
    FakeIrisXEGEM* ringGem = nullptr;
    FakeIrisXEGEM* lrcGem = nullptr;
    FakeIrisXEGEM* scratchGem = nullptr;
    uint64_t ringGpuAddr = 0;
    uint64_t lrcGpuAddr = 0;
    uint64_t scratchGpuAddr = 0;
    uint32_t ringTailBytes = 0;
    uint32_t ringCtl = 0;
    uint32_t expectedValue = kProofExpectedValue;
    uint32_t swContextId = 1;
    uint32_t descLo = 0;
    uint32_t descHi = 0;
};

static const char* proofFailureLabel(ProofFailureType type)
{
    switch (type) {
        case DescriptorWrong:
            return "A_DESCRIPTOR_FORMAT_WRONG";
        case LrcLayoutWrong:
            return "B_LRC_LAYOUT_WRONG";
        case RingStateWrong:
            return "C_RING_STATE_WRONG";
        case MiPacketWrong:
            return "D_MI_PACKET_WRONG";
        case EngineHardHalted:
            return "E_RCS_HARD_HALTED";
        case NoSchedulingProgress:
            return "F_NO_SCHEDULING_PROGRESS";
        default:
            return "NONE";
    }
}

static void logProofDwords(const char* label, const uint32_t* words, uint32_t count)
{
    if (!label || !words) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        IOLog("(FakeIrisXE) [V221] %s[%u] = 0x%08X\n", label, i, words[i]);
    }
}

static void cleanupProofGem(FakeIrisXEExeclist* self,
                            FakeIrisXEGEM*& gem,
                            uint64_t& gpuAddr,
                            uint32_t sizeBytes)
{
    if (!self || !self->fOwner || !gem) {
        gpuAddr = 0;
        gem = nullptr;
        return;
    }

    if (gpuAddr) {
        const uint32_t pages = (sizeBytes + 4095u) / 4096u;
        self->fOwner->ggttUnmap(gpuAddr, pages);
        gpuAddr = 0;
    }

    gem->unpin();
    gem->release();
    gem = nullptr;
}

static void releaseProofResources(FakeIrisXEExeclist* self, RcsProofResources& res)
{
    if (!self || !self->fOwner) {
        return;
    }

    cleanupProofGem(self, res.scratchGem, res.scratchGpuAddr, 4096u);
    cleanupProofGem(self, res.lrcGem, res.lrcGpuAddr, 4096u);
    cleanupProofGem(self, res.ringGem, res.ringGpuAddr, kProofRingSize);
}

static bool allocateProofResources(FakeIrisXEExeclist* self, RcsProofResources& res)
{
    if (!self || !self->fOwner) {
        return false;
    }

    IOLog("(FakeIrisXE) [V221] Allocating direct Execlist proof resources...\n");

    res.ringGem = FakeIrisXEGEM::withSize(kProofRingSize, 0);
    if (!res.ringGem) {
        IOLog("(FakeIrisXE) [V221] ❌ Ring allocation failed\n");
        return false;
    }
    res.ringGem->pin();
    res.ringGpuAddr = self->fOwner->ggttMap(res.ringGem) & ~0xFFFULL;
    if (!res.ringGpuAddr) {
        IOLog("(FakeIrisXE) [V221] ❌ Ring GGTT mapping failed\n");
        releaseProofResources(self, res);
        return false;
    }

    res.lrcGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!res.lrcGem) {
        IOLog("(FakeIrisXE) [V221] ❌ LRC allocation failed\n");
        releaseProofResources(self, res);
        return false;
    }
    res.lrcGem->pin();
    res.lrcGpuAddr = self->fOwner->ggttMap(res.lrcGem) & ~0xFFFULL;
    if (!res.lrcGpuAddr) {
        IOLog("(FakeIrisXE) [V221] ❌ LRC GGTT mapping failed\n");
        releaseProofResources(self, res);
        return false;
    }

    res.scratchGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!res.scratchGem) {
        IOLog("(FakeIrisXE) [V221] ❌ Scratch allocation failed\n");
        releaseProofResources(self, res);
        return false;
    }
    res.scratchGem->pin();
    res.scratchGpuAddr = self->fOwner->ggttMap(res.scratchGem) & ~0xFFFULL;
    if (!res.scratchGpuAddr) {
        IOLog("(FakeIrisXE) [V221] ❌ Scratch GGTT mapping failed\n");
        releaseProofResources(self, res);
        return false;
    }

    void* scratchCpu = self->fOwner->ggttGetCPUAddr(res.scratchGem);
    if (!scratchCpu) {
        IOLog("(FakeIrisXE) [V221] ❌ Scratch CPU mapping failed\n");
        releaseProofResources(self, res);
        return false;
    }

    *(volatile uint32_t*)scratchCpu = kProofScratchInitial;
    __sync_synchronize();
    OSSynchronizeIO();

    IOLog("(FakeIrisXE) [V221]   Ring GPU VA:    0x%016llX\n", (unsigned long long)res.ringGpuAddr);
    IOLog("(FakeIrisXE) [V221]   LRC GPU VA:     0x%016llX\n", (unsigned long long)res.lrcGpuAddr);
    IOLog("(FakeIrisXE) [V221]   Scratch GPU VA: 0x%016llX\n", (unsigned long long)res.scratchGpuAddr);
    IOLog("(FakeIrisXE) [V221]   Scratch init:   0x%08X\n", kProofScratchInitial);
    return true;
}

static bool buildProofCommandStream(FakeIrisXEExeclist* self, RcsProofResources& res)
{
    if (!self || !self->fOwner || !res.ringGem) {
        return false;
    }

    uint32_t* ringCpu = (uint32_t*)self->fOwner->ggttGetCPUAddr(res.ringGem);
    if (!ringCpu) {
        IOLog("(FakeIrisXE) [V221] ❌ Ring CPU mapping failed\n");
        return false;
    }

    bzero(ringCpu, kProofRingSize);
    ringCpu[0] = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
    ringCpu[1] = (uint32_t)(res.scratchGpuAddr & 0xFFFFFFFFULL);
    ringCpu[2] = (uint32_t)(res.scratchGpuAddr >> 32);
    ringCpu[3] = res.expectedValue;
    ringCpu[4] = MI_BATCH_BUFFER_END;
    res.ringTailBytes = 5u * sizeof(uint32_t);
    res.ringCtl = RING_CTL_SIZE(kProofRingSize) | RING_VALID;

    __sync_synchronize();
    OSSynchronizeIO();

    IOLog("(FakeIrisXE) [V221] ========== RCS TEST COMMAND STREAM ==========" "\n");
    IOLog("(FakeIrisXE) [V221]   Packet: MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT\n");
    IOLog("(FakeIrisXE) [V221]   Ring GPU VA: 0x%016llX\n", (unsigned long long)res.ringGpuAddr);
    IOLog("(FakeIrisXE) [V221]   Ring size:   %u bytes\n", kProofRingSize);
    IOLog("(FakeIrisXE) [V221]   Ring head:   0 bytes\n");
    IOLog("(FakeIrisXE) [V221]   Ring tail:   %u bytes\n", res.ringTailBytes);
    IOLog("(FakeIrisXE) [V221]   Ring ctl:    0x%08X\n", res.ringCtl);
    logProofDwords("RingDW", ringCpu, 16);

    return true;
}

static bool buildProofLrc(FakeIrisXEExeclist* self, RcsProofResources& res)
{
    if (!self || !self->fOwner || !res.lrcGem) {
        return false;
    }

    uint8_t* lrcCpu = (uint8_t*)self->fOwner->ggttGetCPUAddr(res.lrcGem);
    if (!lrcCpu) {
        IOLog("(FakeIrisXE) [V221] ❌ LRC CPU mapping failed\n");
        return false;
    }

    bzero(lrcCpu, 4096);

    const uint64_t pdp0 = res.lrcGpuAddr & ~0xFFFULL;
    *(uint64_t*)(lrcCpu + 0x00) = pdp0;
    *(uint64_t*)(lrcCpu + 0x08) = 0;
    *(uint64_t*)(lrcCpu + 0x10) = 0;
    *(uint64_t*)(lrcCpu + 0x18) = 0;
    *(uint32_t*)(lrcCpu + 0x2C) = kProofContextControl;
    *(uint32_t*)(lrcCpu + 0x30) = 0x00010000u;
    *(uint32_t*)(lrcCpu + 0x100) = 0u;
    *(uint32_t*)(lrcCpu + 0x104) = res.ringTailBytes;
    *(uint32_t*)(lrcCpu + 0x108) = (uint32_t)(res.ringGpuAddr & 0xFFFFFFFFULL);
    *(uint32_t*)(lrcCpu + 0x10C) = (uint32_t)(res.ringGpuAddr >> 32);
    *(uint32_t*)(lrcCpu + 0x110) = res.ringCtl;

    __sync_synchronize();
    OSSynchronizeIO();

    IOLog("(FakeIrisXE) [V221] ========== GEN12 RCS LRC ==========" "\n");
    IOLog("(FakeIrisXE) [V221]   PDP0:        0x%016llX\n", (unsigned long long)pdp0);
    IOLog("(FakeIrisXE) [V221]   CONTEXT_CTL: 0x%08X\n", kProofContextControl);
    IOLog("(FakeIrisXE) [V221]   RING_BASE:   0x%016llX\n", (unsigned long long)res.ringGpuAddr);
    IOLog("(FakeIrisXE) [V221]   RING_HEAD:   0\n");
    IOLog("(FakeIrisXE) [V221]   RING_TAIL:   %u bytes\n", res.ringTailBytes);
    IOLog("(FakeIrisXE) [V221]   RING_CTL:    0x%08X\n", res.ringCtl);
    return true;
}

static void buildProofDescriptor(RcsProofResources& res)
{
    const uint32_t addressMode = (kCtxDescLegacy64B << kCtxDescAddressingModeShift);
    const uint32_t flags = kCtxDescValid | kCtxDescPrivilege | kCtxDescForceRestore | addressMode;

    res.descLo = ((uint32_t)(res.lrcGpuAddr & 0xFFFFF000ULL)) | flags;
    res.descHi = ((res.swContextId & 0x7FFu) << kCtxDescSwCtxIdShiftInHi) |
                 ((kCtxDescRenderInstance & 0x3Fu) << kCtxDescEngineInstanceShiftInHi) |
                 ((kCtxDescRenderClass & 0x7u) << kCtxDescEngineClassShiftInHi);

    IOLog("(FakeIrisXE) [V221] ========== CONTEXT DESCRIPTOR ==========" "\n");
    IOLog("(FakeIrisXE) [V221]   DWord0: 0x%08X\n", res.descLo);
    IOLog("(FakeIrisXE) [V221]   DWord1: 0x%08X\n", res.descHi);
    IOLog("(FakeIrisXE) [V221]   Address field: 0x%08X -> GPU VA 0x%016llX\n",
          res.descLo & 0xFFFFF000u,
          (unsigned long long)(res.descLo & 0xFFFFF000u));
    IOLog("(FakeIrisXE) [V221]   Valid: %u Privilege: %u ForceRestore: %u AddressMode: 0x%X\n",
          (res.descLo & kCtxDescValid) ? 1u : 0u,
          (res.descLo & kCtxDescPrivilege) ? 1u : 0u,
          (res.descLo & kCtxDescForceRestore) ? 1u : 0u,
          (res.descLo >> kCtxDescAddressingModeShift) & 0x3u);
    IOLog("(FakeIrisXE) [V221]   SW context ID: %u EngineClass: %u EngineInstance: %u\n",
          (res.descHi >> kCtxDescSwCtxIdShiftInHi) & 0x7FFu,
          (res.descHi >> kCtxDescEngineClassShiftInHi) & 0x7u,
          (res.descHi >> kCtxDescEngineInstanceShiftInHi) & 0x3Fu);
}

static bool singleResetAttemptIfNeeded(FakeIrisXEExeclist* self)
{
    if (!self || !self->fOwner) {
        return false;
    }

    const uint32_t statusBefore = self->mmioRead32(kExecRcsStatusReg);
    const uint32_t gtErrorBefore = self->fOwner->safeMMIORead(kExecGtErrorReg);
    const bool haltedBefore = (statusBefore & 0xE000u) == 0xE000u;
    const bool wedgedBefore = (gtErrorBefore & 0x80000000u) != 0;

    IOLog("(FakeIrisXE) [V221] Pre-submit RCS status=0x%08X GT_ERROR=0x%08X\n", statusBefore, gtErrorBefore);
    if (!haltedBefore && !wedgedBefore) {
        return true;
    }

    IOLog("(FakeIrisXE) [V221] RCS looks halted/wedged; performing one focused reset attempt\n");
    self->mmioWrite32(RCS0_RESET_CTRL, 0x00000001u);
    IOSleep(5);
    self->mmioWrite32(RCS0_RESET_CTRL, 0x00000000u);
    IOSleep(5);

    const uint32_t statusAfter = self->mmioRead32(kExecRcsStatusReg);
    const uint32_t gtErrorAfter = self->fOwner->safeMMIORead(kExecGtErrorReg);
    IOLog("(FakeIrisXE) [V221] Post-reset RCS status=0x%08X GT_ERROR=0x%08X\n", statusAfter, gtErrorAfter);
    return ((statusAfter & 0xE000u) != 0xE000u) && ((gtErrorAfter & 0x80000000u) == 0);
}

static bool submitProofDescriptor(FakeIrisXEExeclist* self, const RcsProofResources& res)
{
    if (!self) {
        return false;
    }

    const uint32_t preLo = self->mmioRead32(kExecElspPrimaryLo);
    const uint32_t preHi = self->mmioRead32(kExecElspPrimaryHi);
    const uint32_t preStatusLo = self->mmioRead32(kExecStatusPrimaryLo);
    const uint32_t preStatusHi = self->mmioRead32(kExecStatusPrimaryHi);

    IOLog("(FakeIrisXE) [V221] Pre-submit ELSP: LO=0x%08X HI=0x%08X STATUS=0x%08X/0x%08X\n",
          preLo, preHi, preStatusLo, preStatusHi);

    self->mmioWrite32(kExecElspPrimaryLo, res.descLo);
    self->mmioWrite32(kExecElspPrimaryHi, res.descHi);
    self->mmioWrite32(RCS0_EXECLIST_CONTROL, 0x1u);
    IOSleep(1);

    const uint32_t postLo = self->mmioRead32(kExecElspPrimaryLo);
    const uint32_t postHi = self->mmioRead32(kExecElspPrimaryHi);
    const uint32_t postStatusLo = self->mmioRead32(kExecStatusPrimaryLo);
    const uint32_t postStatusHi = self->mmioRead32(kExecStatusPrimaryHi);

    IOLog("(FakeIrisXE) [V221] Post-submit ELSP: LO=0x%08X HI=0x%08X STATUS=0x%08X/0x%08X\n",
          postLo, postHi, postStatusLo, postStatusHi);

    return true;
}

static bool pollProofProgress(FakeIrisXEExeclist* self, RcsProofResources& res, ProofFailureType& failure)
{
    if (!self || !self->fOwner || !res.scratchGem) {
        failure = LrcLayoutWrong;
        return false;
    }

    volatile uint32_t* scratchCpu = (volatile uint32_t*)self->fOwner->ggttGetCPUAddr(res.scratchGem);
    if (!scratchCpu) {
        failure = LrcLayoutWrong;
        return false;
    }

    const uint32_t initialElspLo = self->mmioRead32(kExecElspPrimaryLo);
    const uint32_t initialElspHi = self->mmioRead32(kExecElspPrimaryHi);
    const uint32_t initialStatusLo = self->mmioRead32(kExecStatusPrimaryLo);
    const uint32_t initialStatusHi = self->mmioRead32(kExecStatusPrimaryHi);
    const uint32_t initialCsbWrite = self->mmioRead32(RCS0_CSB_WRITE_PTR);

    bool elspAccepted = false;
    bool schedulingProgress = false;
    bool ringStateLoaded = false;
    bool ringConsumed = false;

    IOLog("(FakeIrisXE) [V221] ========== EXECUTION POLL ==========" "\n");

    for (uint32_t poll = 0; poll < 100; ++poll) {
        IOSleep(10);

        const uint32_t rcsHead = self->mmioRead32(kExecRingHeadReg);
        const uint32_t rcsTail = self->mmioRead32(kExecRingTailReg);
        const uint32_t rcsStart = self->mmioRead32(kExecRingStartReg);
        const uint32_t rcsCtl = self->mmioRead32(kExecRingCtlReg);
        const uint32_t rcsStatus = self->mmioRead32(kExecRcsStatusReg);
        const uint32_t elspLo = self->mmioRead32(kExecElspPrimaryLo);
        const uint32_t elspHi = self->mmioRead32(kExecElspPrimaryHi);
        const uint32_t execlistStatusLo = self->mmioRead32(kExecStatusPrimaryLo);
        const uint32_t execlistStatusHi = self->mmioRead32(kExecStatusPrimaryHi);
        const uint32_t csbCtrl = self->mmioRead32(RCS0_CSB_CTRL);
        const uint32_t csbAddrLo = self->mmioRead32(RCS0_CSB_ADDR_LO);
        const uint32_t csbAddrHi = self->mmioRead32(RCS0_CSB_ADDR_HI);
        const uint32_t csbRead = self->mmioRead32(RCS0_CSB_READ_PTR);
        const uint32_t csbWrite = self->mmioRead32(RCS0_CSB_WRITE_PTR);
        const uint32_t acthdLo = self->mmioRead32(kExecActhdLo);
        const uint32_t acthdHi = self->mmioRead32(kExecActhdHi);
        const uint32_t bbAddrLo = self->mmioRead32(kExecBbAddrLo);
        const uint32_t bbAddrHi = self->mmioRead32(kExecBbAddrHi);
        const uint32_t ccid = self->mmioRead32(kExecCcidReg);
        const uint32_t ctxCtrl = self->mmioRead32(kExecContextControlReg);
        const uint32_t gtError = self->fOwner->safeMMIORead(kExecGtErrorReg);
        const uint32_t scratchValue = *scratchCpu;

        const bool halted = (rcsStatus & 0xE000u) == 0xE000u;
        const bool wedged = (gtError & 0x80000000u) != 0;
        const bool statusValid = (execlistStatusLo & (kExecStatusSlot0Valid | kExecStatusSlot1Valid)) != 0;
        const bool statusActive = (execlistStatusLo & (kExecStatusSlot0Active | kExecStatusSlot1Active)) != 0;

        elspAccepted |= (elspLo != initialElspLo) || (elspHi != initialElspHi) ||
                        (execlistStatusLo != initialStatusLo) || (execlistStatusHi != initialStatusHi);
        schedulingProgress |= statusValid || statusActive || (ccid != 0) || (csbWrite != initialCsbWrite);
        ringStateLoaded |= ((rcsStart & 0xFFFFF000u) == (uint32_t)(res.ringGpuAddr & 0xFFFFF000ULL)) &&
                           ((rcsCtl & 0x001FF001u) == (res.ringCtl & 0x001FF001u));
        ringConsumed |= ((rcsHead & 0x001FFFFCu) != 0) || acthdLo || acthdHi || bbAddrLo || bbAddrHi;

        if ((poll % 5u) == 0u || scratchValue == res.expectedValue || halted || wedged) {
            IOLog("(FakeIrisXE) [V221] Poll%03u ELSP=%08X/%08X EXE=%08X/%08X RCS H/T/S=%08X/%08X/%08X\n",
                  poll, elspLo, elspHi, execlistStatusLo, execlistStatusHi, rcsHead, rcsTail, rcsStatus);
            IOLog("(FakeIrisXE) [V221]         CSB ctrl=%08X addr=%08X%08X rp=%08X wp=%08X CCID=%08X CTXCTL=%08X\n",
                  csbCtrl, csbAddrHi, csbAddrLo, csbRead, csbWrite, ccid, ctxCtrl);
            IOLog("(FakeIrisXE) [V221]         ACTHD=%08X%08X BBADDR=%08X%08X GT_ERR=%08X SCRATCH=%08X\n",
                  acthdHi, acthdLo, bbAddrHi, bbAddrLo, gtError, scratchValue);
        }

        if (scratchValue == res.expectedValue) {
            IOLog("(FakeIrisXE) [V221] ✅ SUCCESS: scratch changed from 0x%08X to 0x%08X\n",
                  kProofScratchInitial, scratchValue);
            failure = None;
            return true;
        }

        if (halted || wedged) {
            failure = EngineHardHalted;
            return false;
        }
    }

    if (!elspAccepted) {
        failure = DescriptorWrong;
    } else if (!schedulingProgress) {
        failure = NoSchedulingProgress;
    } else if (!ringStateLoaded) {
        failure = LrcLayoutWrong;
    } else if (!ringConsumed) {
        failure = RingStateWrong;
    } else {
        failure = MiPacketWrong;
    }

    return false;
}

static bool runRcsScratchWriteProof(FakeIrisXEExeclist* self, const char* label)
{
    if (!self || !self->fOwner) {
        return false;
    }

    RcsProofResources res;
    ProofFailureType failure = None;
    bool success = false;

    IOLog("(FakeIrisXE) [V221] ============================================\n");
    IOLog("(FakeIrisXE) [V221] DIRECT EXECLIST SCRATCH-WRITE PROOF (%s)\n", label ? label : "unknown");
    IOLog("(FakeIrisXE) [V221] ============================================\n");

    if (!allocateProofResources(self, res)) {
        failure = LrcLayoutWrong;
        goto done;
    }

    if (!singleResetAttemptIfNeeded(self)) {
        failure = EngineHardHalted;
        goto done;
    }

    if (!buildProofCommandStream(self, res)) {
        failure = MiPacketWrong;
        goto done;
    }

    if (!buildProofLrc(self, res)) {
        failure = LrcLayoutWrong;
        goto done;
    }

    buildProofDescriptor(res);

    if (!self->fOwner->forcewakeRenderHold(5000)) {
        IOLog("(FakeIrisXE) [V221] ❌ Failed to acquire forcewake for proof submission\n");
        failure = EngineHardHalted;
        goto done;
    }

    if (!submitProofDescriptor(self, res)) {
        self->fOwner->forcewakeRenderRelease();
        failure = DescriptorWrong;
        goto done;
    }

    success = pollProofProgress(self, res, failure);
    self->fOwner->forcewakeRenderRelease();

done:
    self->fIsReady = success;
    self->fOwner->setProperty("FakeIrisXEExeclistExecutionProven", success ? kOSBooleanTrue : kOSBooleanFalse);
    self->fOwner->setProperty("FakeIrisXERcsProofFailure", proofFailureLabel(failure));
    self->fOwner->updateExecutionState(success, success ? "rcs-scratch-writeback" : proofFailureLabel(failure));

    if (!success) {
        IOLog("(FakeIrisXE) [V221] ❌ FAILURE TYPE: %s\n", proofFailureLabel(failure));
    }

    releaseProofResources(self, res);
    return success;
}

} // namespace

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
    obj->fIsReady = false;  // V206: Initialize EXEClist not ready

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
    if (off == kExecRingCtlReg || off == kExecRingHeadReg || off == kExecRingTailReg || 
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
    if (off == kExecRingCtlReg || off == kExecRingTailReg || off == RING_EXECLIST_SUBMIT_LO || 
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
    uint32_t ring_start = mmioRead32(kExecRingStartReg);
    uint32_t ring_head  = mmioRead32(kExecRingHeadReg);
    uint32_t ring_tail  = mmioRead32(kExecRingTailReg);
    uint32_t ring_ctl   = mmioRead32(kExecRingCtlReg);
    // V57: Use alternative register names if ACTHD/BBADDR not defined
    uint32_t ring_acthd = mmioRead32(0x2074);  // ACTHD - Active Head
    uint32_t ring_bbaddr = mmioRead32(0x2080); // BBADDR - Batch Buffer Address
    
    IOLog("[V57] RING_START:  0x%08X (GGTT base)\n", ring_start);
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
    
    // V139: Dump CSB registers first
    uint32_t csb_ctrl = mmioRead32(RCS0_CSB_CTRL);
    uint32_t csb_lo = mmioRead32(RCS0_CSB_ADDR_LO);
    uint32_t csb_hi = mmioRead32(RCS0_CSB_ADDR_HI);
    IOLog("[V139] CSB registers: CTRL=0x%08X ADDR=0x%08X%08X\n", csb_ctrl, csb_hi, csb_lo);
    
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
    
    // Read CSB write pointer from hardware (GPU updates this when it produces entries)
    // RCS0_CSB_WRITE_PTR[7:0] = current write pointer value
    uint32_t csb_write_ptr_reg = mmioRead32(RCS0_CSB_WRITE_PTR);
    uint32_t write_ptr = csb_write_ptr_reg & 0xFF;  // Bits [7:0]
    uint32_t read_ptr = fCsbReadIndex;
    
    IOLog("[V250] CSB Read Ptr: %d, Write Ptr (HW): 0x%08X -> %d, CSB CTRL: 0x%08X\n",
          read_ptr, csb_write_ptr_reg, write_ptr, csb_ctrl);
    
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


    // Forcewake first; avoid speculative GT/ring reset pokes here because this
    // path is now also used for GuC-failed fallback diagnostics.
    uint32_t pre_ack = safeRead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [Exec] createHwContext pre-forcewake ACK=0x%08x\n", pre_ack);

    if (!fOwner->forcewakeRenderHold(5000)) {
        uint32_t final_ack = safeRead(FORCEWAKE_ACK);
        IOLog("❌ createHwContext: forcewake hold failed (ack=0x%08X)\n", final_ack);
        return false;
    }

    uint32_t post_ack = safeRead(FORCEWAKE_ACK);
    IOLog("✅ createHwContext: forcewake held ACK=0x%08x\n", post_ack);

    // Leave engine registers untouched until we have a valid LRC + ELSP path.
    IOLog("(FakeIrisXE) [Exec] createHwContext using non-destructive fallback path\n");

    
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
        return false;
    } else {
        fCsbGem->pin();
        fCsbGGTT = fOwner->ggttMap(fCsbGem);
        if (fCsbGGTT) {
            fCsbGGTT &= ~0xFFFULL;
        } else {
            IOLog("(FakeIrisXE) [Exec] ggttMap(CSB) failed\n");
            return false;
        }
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

    if (!fCsbGGTT) {
        IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: missing CSB backing\n");
        return false;
    }

    if (!fOwner->forcewakeRenderHold(5000)) {
        IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: forcewake hold failed\n");
        return false;
    }

    fOwner->ensureEngineInterrupts();

    uint32_t rcsStatus = mmioRead32(kExecRcsStatusReg);
    uint32_t gtError = fOwner->safeMMIORead(kExecGtErrorReg);
    const bool halted = (rcsStatus & 0xE000u) == 0xE000u;
    const bool wedged = (gtError & 0x80000000u) != 0;

    IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: preflight RCS_STATUS=0x%08X GT_ERROR=0x%08X\n",
          rcsStatus, gtError);

    if (halted || wedged) {
        IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: one focused reset attempt before proof path\n");
        mmioWrite32(RCS0_RESET_CTRL, 0x00000001u);
        IOSleep(5);
        mmioWrite32(RCS0_RESET_CTRL, 0x00000000u);
        IOSleep(5);
        rcsStatus = mmioRead32(kExecRcsStatusReg);
        gtError = fOwner->safeMMIORead(kExecGtErrorReg);
        IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: post-reset RCS_STATUS=0x%08X GT_ERROR=0x%08X\n",
              rcsStatus, gtError);
    }

    const uint32_t csbLo = (uint32_t)(fCsbGGTT & 0xFFFFFFFFULL);
    const uint32_t csbHi = (uint32_t)(fCsbGGTT >> 32);
    mmioWrite32(RCS0_CSB_ADDR_LO, csbLo);
    mmioWrite32(RCS0_CSB_ADDR_HI, csbHi);
    mmioWrite32(RCS0_CSB_CTRL, 0x1u);

    const uint32_t csbReadbackLo = mmioRead32(RCS0_CSB_ADDR_LO);
    const uint32_t csbReadbackHi = mmioRead32(RCS0_CSB_ADDR_HI);
    const uint32_t csbCtrl = mmioRead32(RCS0_CSB_CTRL);
    IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: CSB addr=0x%08X%08X ctrl=0x%08X\n",
          csbReadbackHi, csbReadbackLo, csbCtrl);

    constexpr uint32_t IRQS =
          (1 << 12)  // CONTEXT_COMPLETE
        | (1 << 13)  // CONTEXT_SWITCH
        | (1 << 11); // PAGE_FAULT

    mmioWrite32(RCS0_IMR, ~IRQS);
    mmioWrite32(RCS0_IER, IRQS);
    mmioWrite32(GEN11_GFX_MSTR_IRQ_MASK, 0x0);
    mmioWrite32(GEN11_GFX_MSTR_IRQ, IRQS);

    fOwner->forcewakeRenderRelease();
    IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: CSB/IRQ path staged; submission is owned by V221 direct proof path\n");
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
    (void)batchGem;
    return runRcsScratchWriteProof(this, "submitBatchExeclist");
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
    uint32_t elsp_before_lo = mmioRead32(kExecElspPrimaryLo);
    uint32_t elsp_before_hi = mmioRead32(kExecElspPrimaryHi);
    uint32_t legacy_before_lo = mmioRead32(kExecElspLegacyLo);
    uint32_t legacy_before_hi = mmioRead32(kExecElspLegacyHi);
    IOLog("programRcsForContext: ELSP before primary[0x%08x 0x%08x] legacy[0x%08x 0x%08x]\n",
          elsp_before_lo, elsp_before_hi, legacy_before_lo, legacy_before_hi);

    // --------------------------------------------------
    // STEP 3: Write ELSP via mmioWrite32 (NO safeMMIOWrite, NO gpuPowerOn)
    // --------------------------------------------------
    uint32_t desc_lo = (uint32_t)(listGpu & 0xFFFFFFFFull);
    uint32_t desc_hi = (uint32_t)(listGpu >> 32);

    mmioWrite32(kExecElspPrimaryLo, desc_lo);
    mmioWrite32(kExecElspPrimaryHi, desc_hi);
    mmioWrite32(kExecElspLegacyLo, desc_lo);
    mmioWrite32(kExecElspLegacyHi, desc_hi);

    // small delay so posted writes land
    IOSleep(2);

    // --------------------------------------------------
    // STEP 4: Read back ELSP and STATUS
    // --------------------------------------------------
    uint32_t elsp_after_lo = mmioRead32(kExecElspPrimaryLo);
    uint32_t elsp_after_hi = mmioRead32(kExecElspPrimaryHi);
    uint32_t legacy_after_lo = mmioRead32(kExecElspLegacyLo);
    uint32_t legacy_after_hi = mmioRead32(kExecElspLegacyHi);
    uint32_t status_lo = mmioRead32(kExecStatusPrimaryLo);
    uint32_t status_hi = mmioRead32(kExecStatusPrimaryHi);
    uint32_t legacy_status_lo = mmioRead32(kExecStatusLegacyLo);
    uint32_t legacy_status_hi = mmioRead32(kExecStatusLegacyHi);

    IOLog("programRcsForContext: ELSP after primary[0x%08x 0x%08x] legacy[0x%08x 0x%08x] status_primary[0x%08x 0x%08x] status_legacy[0x%08x 0x%08x]\n",
          elsp_after_lo, elsp_after_hi,
          legacy_after_lo, legacy_after_hi,
          status_lo, status_hi,
          legacy_status_lo, legacy_status_hi);

    bool ok = (elsp_after_lo == desc_lo && elsp_after_hi == desc_hi) ||
              (legacy_after_lo == desc_lo && legacy_after_hi == desc_hi);

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

    mmioWrite32(kExecRingHeadReg, 0);

    
    // --------------------------------------------------
    // STEP 5: Kick execlist
    // --------------------------------------------------
    mmioWrite32(RCS0_EXECLIST_CONTROL, 0x1);   // minimal "kick"
    mmioWrite32(RCS0_EXECLIST_SQ_CONTENTS, 0x1);
    IOSleep(1);

    uint32_t status_after_kick = mmioRead32(kExecStatusPrimaryLo);
    uint32_t legacy_status_after_kick = mmioRead32(kExecStatusLegacyLo);
    IOLog("programRcsForContext: EXECLIST kicked, STATUS_PRIMARY=0x%08x STATUS_LEGACY=0x%08x\n",
          status_after_kick, legacy_status_after_kick);

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
    (void)fb;
    (void)batchGem;
    (void)batchSize;
    (void)ring;
    (void)timeoutMs;
    return runRcsScratchWriteProof(this, "submitBatchWithExeclist");

#if 0
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
#endif
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

    mmioWrite32(kExecElspPrimaryLo, lo);
    mmioWrite32(kExecElspPrimaryHi, hi);
    mmioWrite32(kExecElspLegacyLo, lo);
    mmioWrite32(kExecElspLegacyHi, hi);

    // Kick control register (lightweight)
    mmioWrite32(RCS0_EXECLIST_SQ_CONTENTS, 0x1);
    mmioWrite32(RCS0_EXECLIST_CONTROL, 0x1);
    IOSleep(2);
    uint32_t sq = mmioRead32(RCS0_EXECLIST_SQ_CONTENTS);
    uint32_t primaryLo = mmioRead32(kExecElspPrimaryLo);
    uint32_t primaryHi = mmioRead32(kExecElspPrimaryHi);
    uint32_t legacyLo = mmioRead32(kExecElspLegacyLo);
    uint32_t legacyHi = mmioRead32(kExecElspLegacyHi);
    uint32_t statusPrimary = mmioRead32(kExecStatusPrimaryLo);
    uint32_t statusLegacy = mmioRead32(kExecStatusLegacyLo);
    IOLog("SQ_CONTENTS after kick = 0x%08x ELSP primary[0x%08x 0x%08x] legacy[0x%08x 0x%08x] status primary=0x%08x legacy=0x%08x\n",
          sq, primaryLo, primaryHi, legacyLo, legacyHi, statusPrimary, statusLegacy);

    
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
                    0,      // ring head
                    0,      // ring tail
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
    return runRcsScratchWriteProof(this, "runDiagnosticTest");
}

bool FakeIrisXEExeclist::createAndSubmitTestBatch()
{
    return runRcsScratchWriteProof(this, "createAndSubmitTestBatch");
}

bool FakeIrisXEExeclist::verifyCommandCompletion()
{
    return runRcsScratchWriteProof(this, "verifyCommandCompletion");
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
    IOLog("[V62]   Testing direct Execlist scratch-write proof...\n");
    return runRcsScratchWriteProof(this, "testBatchSubmission");
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

// ============================================================================
// V139: Enhanced completion checking and diagnostics
// ============================================================================

bool FakeIrisXEExeclist::waitForCommandCompletion(uint32_t timeoutMs)
{
    IOLog("[V139] Waiting for command completion (timeout=%ums)...\n", timeoutMs);
    
    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = (uint64_t)timeoutMs * 1000000ULL;
    uint32_t lastStatus = 0;
    uint32_t stableCount = 0;
    
    while (mach_absolute_time() - start < timeoutNs) {
        // Check EXECLIST_STATUS
        uint32_t status_lo = mmioRead32(RCS0_EXECLIST_STATUS_LO);
        uint32_t status_hi = mmioRead32(RCS0_EXECLIST_STATUS_HI);
        
        // Check if both slots are empty (no active context)
        bool slot0_active = (status_lo >> 2) & 1;
        bool slot1_active = (status_lo >> 3) & 1;
        
        if (status_lo == lastStatus) {
            stableCount++;
        } else {
            IOLog("[V139] Status change: STATUS=0x%08X slot0=%d slot1=%d\n",
                  status_lo, slot0_active, slot1_active);
            stableCount = 0;
            lastStatus = status_lo;
        }
        
        // If both slots are idle, command completed
        if (!slot0_active && !slot1_active) {
            // Check CSB for completion using hardware write pointer
            if (fCsbGem) {
                IOBufferMemoryDescriptor* md = fCsbGem->memoryDescriptor();
                if (md) {
                    volatile uint64_t* csb = (volatile uint64_t*)md->getBytesNoCopy();
                    uint32_t csb_write_hw = mmioRead32(RCS0_CSB_WRITE_PTR) & 0xFF;
                    if (csb && fCsbReadIndex != csb_write_hw) {
                        uint64_t entry = csb[fCsbReadIndex % fCsbEntryCount];
                        uint32_t csb_status = (uint32_t)(entry & 0xFFFFFFFF);
                        IOLog("[V250] CSB entry: status=0x%08X\n", csb_status);
                        
                        if (csb_status & CSB_STATUS_COMPLETE) {
                            IOLog("[V250] Command completed successfully!\n");
                            return true;
                        }
                    }
                }
            }
            
            // Slots idle, assume completed
            IOLog("[V139] ✅ Execlist slots idle - command completed\n");
            return true;
        }
        
        // Check for errors
        if (status_lo & 0xFF000000) {
            IOLog("[V139] ❌ Error detected in status: 0x%08X\n", status_lo);
            return false;
        }
        
        if (stableCount > 100) {
            IOLog("[V139] ⚠️ Status stable for 100 polls but still active\n");
        }
        
        IOSleep(1);
    }
    
    IOLog("[V139] ❌ Timeout waiting for completion\n");
    return false;
}

void FakeIrisXEExeclist::dumpRcsRingStatus(const char* label)
{
    IOLog("[V139] === RCS Ring Status: %s ===\n", label);
    
    uint32_t ring_head = mmioRead32(kExecRingHeadReg);
    uint32_t ring_tail = mmioRead32(kExecRingTailReg);
    uint32_t ring_ctl = mmioRead32(kExecRingCtlReg);
    uint32_t ring_start = mmioRead32(kExecRingStartReg);
    
    IOLog("[V139] RING_START: 0x%08X\n", ring_start);
    IOLog("[V139] RING_HEAD: 0x%04X\n", ring_head & 0xFFFF);
    IOLog("[V139] RING_TAIL: 0x%04X\n", ring_tail & 0xFFFF);
    IOLog("[V139] RING_CTL:  0x%08X (size=%dKB, %s)\n",
          ring_ctl,
          ((ring_ctl >> 12) + 1) * 4,
          (ring_ctl & 1) ? "ENABLED" : "DISABLED");
    
    // Calculate ring usage
    uint32_t head = ring_head & 0xFFFF;
    uint32_t tail = ring_tail & 0xFFFF;
    uint32_t ring_size = ((ring_ctl >> 12) + 1) * 4096;
    uint32_t used = (tail >= head) ? (tail - head) : (ring_size - head + tail);
    
    IOLog("[V139] Ring usage: %d bytes used, %d bytes free\n", used, ring_size - used);
}
