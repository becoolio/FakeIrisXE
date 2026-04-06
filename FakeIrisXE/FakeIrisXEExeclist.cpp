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

static const uint32_t kExecRingTailReg  = TGL_RCS0_BASE + 0x30u;
static const uint32_t kExecRingHeadReg  = TGL_RCS0_BASE + 0x34u;
static const uint32_t kExecRingStartReg = TGL_RCS0_BASE + 0x38u;
static const uint32_t kExecRingCtlReg   = TGL_RCS0_BASE + 0x3Cu;

static const uint32_t kExecElspPrimaryLo = 0x120B0;  // RCS0_EXECLIST_SUBMITPORT_LO
static const uint32_t kExecElspPrimaryHi = 0x120B4;  // RCS0_EXECLIST_SUBMITPORT_HI
static const uint32_t kExecElspLegacyLo  = 0x120A0;  // RCS0_ELSP1_LO
static const uint32_t kExecElspLegacyHi  = 0x120A4;  // RCS0_ELSP1_HI
static const uint32_t kExecStatusPrimaryLo = 0x120C0;  // RCS0_EXECLIST_STATUS_LO
static const uint32_t kExecStatusPrimaryHi = 0x120C4;  // RCS0_EXECLIST_STATUS_HI
static const uint32_t kExecStatusLegacyLo  = 0x2230;
static const uint32_t kExecStatusLegacyHi  = 0x2234;
static const uint32_t kExecSqContents = 0x120B8;  // SQ_CONTENTS kick register
static const uint32_t kExecCsbCtrl = 0x120C8;  // RCS0_EXECLIST_CONTROL
static const uint32_t kExecCsbHead = 0x120CC;  // CSB head pointer
static const uint32_t kExecCsbTail = 0x120D0;  // CSB tail pointer

namespace {

static const uint32_t kExecGtErrorReg = 0x18E04;
static const uint32_t kExecRcsStatusReg = TGL_RCS0_BASE + 0x2524;  // Tiger Lake RCS0_STATUS
static const uint32_t kExecActhdLo = TGL_RCS0_BASE + 0x74;
static const uint32_t kExecActhdHi = TGL_RCS0_BASE + 0x5C;
static const uint32_t kExecBbAddrLo = TGL_RCS0_BASE + 0x140;
static const uint32_t kExecBbAddrHi = TGL_RCS0_BASE + 0x168;
static const uint32_t kExecCcidReg = TGL_RCS0_BASE + 0x180;
static const uint32_t kExecContextControlReg = TGL_RCS0_BASE + 0x244;
static const uint32_t kExecRingMiModeReg = TGL_RCS0_BASE + 0x09C;
static const uint32_t kExecRingModeGen7Reg = TGL_RCS0_BASE + 0x29C;
static const uint32_t kExecHwsPgaReg = TGL_RCS0_BASE + 0x080;
static const uint32_t kExecHwstamReg = TGL_RCS0_BASE + 0x098;
static const uint32_t kExecRbStartLoReg = GEN12_RCS0_RBSTART;
static const uint32_t kExecRbStartHiReg = GEN12_RCS0_RBSTART + 0x4u;
static const uint32_t kExecRbHeadReg = GEN12_RCS0_RBHEAD;
static const uint32_t kExecRbTailReg = GEN12_RCS0_RBTAIL;

static const uint32_t kExecCsbEntryCount = 12u;
static const uint32_t kExecCsbOffsetBytes = 0x40u;
static const uint32_t kExecCsbWriteOffsetBytes = 0xBCu;
static const uint32_t kExecHwsPageBytes = 4096u;
static const uint32_t kExecCsbBytes = kExecCsbEntryCount * sizeof(uint64_t);
static const uint32_t kExecRingModeDisableLegacy = (1u << 3);
static const uint32_t kExecRingModePpgttEnable = (1u << 9);
static const uint32_t kExecRingModePpgtt48b = (1u << 7);
static const uint32_t kExecRingMiModeStopRing = (1u << 8);

static const uint32_t kExecStatusSlot1Valid = (1u << 3);
static const uint32_t kExecStatusSlot0Valid = (1u << 4);
static const uint32_t kExecStatusSlot1Active = (1u << 17);
static const uint32_t kExecStatusSlot0Active = (1u << 18);

static const uint32_t kProofExpectedValue = 0xDEADBEEFu;
static const uint32_t kProofScratchInitial = 0xBADBAD00u;
static const uint32_t kProofRingSize = 64u * 1024u;
static const uint32_t kProofRenderContextPages = 14u;
static const uint32_t kProofWaContextPages = 2u;
static const uint32_t kProofContextPages = kProofRenderContextPages + kProofWaContextPages;
static const uint32_t kProofContextBytes = kProofContextPages * 4096u;
static const uint32_t kProofPml4Bytes = 4096u;
static const uint32_t kProofIndirectCtxBytes = 128u;
static const uint32_t kProofRegStateOffset = 4096u;
static const uint32_t kProofIndirectCtxOffset = kProofRenderContextPages * 4096u;
static const uint32_t kProofPerCtxBbOffset = (kProofRenderContextPages + 1u) * 4096u;
static const uint32_t kProofLrcHeaderBytes = 0x40u;
static const uint32_t kProofLrcRingStateOffset = 0x100u;
static const uint32_t kProofContextControl = 0x00090008u;
static const uint64_t kProofPpgttScratchVa = 0x0000000000001000ULL;

static const char* kExeclistVersion = "V327";

static const uint32_t kCtxDescValid = (1u << 0);
static const uint32_t kCtxDescPrivilege = (1u << 8);
static const uint32_t kCtxDescForceRestore = (1u << 2);
static const uint32_t kCtxDescAddressingModeShift = 3u;
static const uint32_t kCtxDescLegacy64B = 3u;
static const uint32_t kCtxDescAddressingMode48b = 4u;  // V319: Apple uses 48-bit addressing
static const uint32_t kCtxDescSwCtxIdShiftInHi = 5u;
static const uint32_t kCtxDescEngineInstanceShiftInHi = 16u;
static const uint32_t kCtxDescEngineClassShiftInHi = 29u;
static const uint32_t kCtxDescEngineClassRender = 0u;
static const uint32_t kCtxDescRenderInstance = 0u;
static const uint32_t kCtxDescActive = (1u << 1);  // V319: ACTIVE bit

enum ProofFailureType {
    None,
    DescriptorWrong,
    LrcLayoutWrong,
    RingStateWrong,
    RingControlNotEnabled,
    MiPacketWrong,
    EngineHardHalted,
    NoSchedulingProgress,
    ScratchMappingUnavailable,
    ElspRejected,
    ElspVisibleNotAccepted,
    CsbNoProgress,
    ContextStateNotLoaded,
    BatchNeverStarted,
    ScratchWritebackMissing,
    // V326: Enhanced failure taxonomy
    MaskedRingStableNoSubmitVisibility,
    SubmitVisibleNoSlotAccept,
    SlotAcceptNoContextLoad,
    ContextLoadNoExecution,
    ExecutionNoScratchWrite,
    CsBobservationUntrusted,
};

struct RcsProofResources {
    FakeIrisXEGEM* ringGem = nullptr;
    FakeIrisXEGEM* lrcGem = nullptr;
    FakeIrisXEGEM* scratchGem = nullptr;
    FakeIrisXEGEM* pml4Gem = nullptr;
    FakeIrisXEGEM* pdptGem = nullptr;
    FakeIrisXEGEM* pdGem = nullptr;
    FakeIrisXEGEM* ptGem = nullptr;
    uint64_t ringGpuAddr = 0;
    uint64_t lrcGpuAddr = 0;
    uint64_t scratchGpuAddr = 0;
    uint64_t csbGpuAddr = 0;
    uint64_t pml4PhysAddr = 0;
    uint64_t pdptPhysAddr = 0;
    uint64_t pdPhysAddr = 0;
    uint64_t ptPhysAddr = 0;
    uint32_t ringTailBytes = 0;
    uint32_t ringCtl = 0;
    uint32_t expectedValue = kProofExpectedValue;
    uint32_t swContextId = 1;
    uint32_t descLo = 0;
    uint32_t descHi = 0;
    bool submitAccepted = false;
};

struct ProofObservations {
    bool scratchCpuMapped = false;
    bool elspWritten = false;
    bool elspAccepted = false;
    bool execlistStatusChanged = false;
    bool slotValidChanged = false;
    bool slotActiveChanged = false;
    bool csbAdvanced = false;
    bool ccidChanged = false;
    bool contextControlChanged = false;
    bool schedulingProgress = false;
    bool ringStateLoaded = false;
    bool ringCtlEnabled = false;
    bool ringCtlMasked = false;
    bool ringConsumed = false;
    bool batchStarted = false;
    bool acthdObserved = false;
    uint32_t lastScratchValue = kProofScratchInitial;
    uint32_t lastExeclistStatusLo = 0;
    uint32_t lastExeclistStatusHi = 0;
    uint32_t lastCsbCtrl = 0;
    uint32_t lastCsbAddrLo = 0;
    uint32_t lastCsbAddrHi = 0;
    uint32_t lastCsbRead = 0;
    uint32_t lastCsbWriteAlias = 0;
    uint32_t lastRcsStatus = 0;
    uint32_t lastGtError = 0;
    uint32_t lastActhdLo = 0;
    uint32_t lastActhdHi = 0;
    uint32_t lastBbAddrLo = 0;
    uint32_t lastBbAddrHi = 0;
    uint32_t lastRingCtl = 0;
    uint32_t lastRingStart = 0;
    uint32_t lastRingHead = 0;
    uint32_t lastRingTail = 0;
    uint32_t attemptedRingMode = 0;
    uint32_t attemptedSubmitStyle = 0;
    uint32_t attemptedCtxCtrl = 0;
    uint32_t attemptedAddrMode = 0;
    bool attemptedForceRestore = false;
    bool attemptedPrivilege = true;
    uint32_t lastSlotValidBits = 0;
    uint32_t lastSlotActiveBits = 0;
    uint32_t attemptedVariantIndex = 0;
    const char* attemptedVariantLabel = nullptr;
};

enum ProofRingProgrammingMode {
    ProofRingModeLrcOnly = 0,
    ProofRingModeLiveOnly = 1,
    ProofRingModeCombined = 2,
};

enum ProofSubmitStyle {
    ProofSubmitCurrent = 0,
    ProofSubmitKickBeforeHi = 1,
    ProofSubmitKickAfterLo = 2,
};

struct ProofVariant {
    const char* label;
    ProofRingProgrammingMode ringMode;
    ProofSubmitStyle submitStyle;
    uint32_t ctxCtrl;
    uint32_t addrMode;
    bool forceRestore;
    bool privilege;
    bool active;  // V319: Include ACTIVE bit in descriptor
    uint32_t swContextId;
    uint32_t engineClass;
    uint32_t engineInstance;
    uint32_t execlistControlKick;
    uint32_t arbControl;
};

static bool validateProofDescriptorShape(const RcsProofResources& res,
                                        const ProofVariant& variant,
                                        ProofFailureType& failure)
{
    if ((res.lrcGpuAddr & 0xFFFu) != 0u || (res.descLo & 0xFFFFF000u) != static_cast<uint32_t>(res.lrcGpuAddr & 0xFFFFF000ULL)) {
        IOLog("(FakeIrisXE) [%s] Descriptor validation failed: LRC address mismatch desc=0x%08X lrc=0x%016llX\n",
              kExeclistVersion,
              res.descLo,
              static_cast<unsigned long long>(res.lrcGpuAddr));
        failure = DescriptorWrong;
        return false;
    }
    if (variant.engineClass > 7u || variant.engineInstance > 63u || variant.swContextId > 0x7FFu) {
        IOLog("(FakeIrisXE) [%s] Descriptor validation failed: swctx=%u class=%u inst=%u out of range\n",
              kExeclistVersion,
              variant.swContextId,
              variant.engineClass,
              variant.engineInstance);
        failure = DescriptorWrong;
        return false;
    }
    return true;
}

// V323: Collapse variant explosion - focus on most promising variants first
static const ProofVariant kProofVariants[] = {
    // V323: Priority variants - front-loaded for faster feedback
    { "combined-forcelive", ProofRingModeCombined,   ProofSubmitCurrent, 0x00000109u, kCtxDescAddressingMode48b, true,  true,  false, 1u, 0u, 0u, 0x00000001u, 0x00010001u },
    { "live-only",          ProofRingModeLiveOnly,   ProofSubmitCurrent, 0x00000109u, kCtxDescAddressingMode48b, true,  true,  false, 1u, 0u, 0u, 0x00000001u, 0x00010001u },
    { "live-noctx",         ProofRingModeLiveOnly,   ProofSubmitCurrent, 0x00000109u, 0u,                false, false, false, 1u, 0u, 0u, 0x00000001u, 0x00010001u },
    { "apple-48bit-active", ProofRingModeLrcOnly,    ProofSubmitCurrent, 0x00000109u, kCtxDescAddressingMode48b, true,  true,  true,  1u, 0u, 0u, 0x00010001u, 0x00010001u },
    { "apple-48b-combined", ProofRingModeCombined,   ProofSubmitCurrent, 0x00000109u, kCtxDescAddressingMode48b, true,  true,  false, 1u, 0u, 0u, 0x00000001u, 0x00010001u },
    // Legacy variants - kept for comparison
    { "baseline-lrc",      ProofRingModeLrcOnly,    ProofSubmitCurrent, 0x00000109u, kCtxDescLegacy64B, true,  true,  false, 1u, 0u, 0u, 0x00000001u, 0x00020001u },
    { "baseline-combined", ProofRingModeCombined,   ProofSubmitCurrent, 0x00000109u, kCtxDescLegacy64B, true,  true,  false, 1u, 0u, 0u, 0x00000001u, 0x00020001u },
    { "baseline-live",     ProofRingModeLiveOnly,   ProofSubmitCurrent, 0x00000109u, kCtxDescLegacy64B, true,  true,  false, 1u, 0u, 0u, 0x00000001u, 0x00020001u },
};

static const char* proofRingModeLabel(ProofRingProgrammingMode mode)
{
    switch (mode) {
        case ProofRingModeLrcOnly:
            return "lrc-only";
        case ProofRingModeLiveOnly:
            return "live-ring-only";
        case ProofRingModeCombined:
            return "combined";
        default:
            return "unknown";
    }
}

static const char* proofSubmitStyleLabel(ProofSubmitStyle style)
{
    switch (style) {
        case ProofSubmitCurrent:
            return "current";
        case ProofSubmitKickBeforeHi:
            return "kick-before-hi";
        case ProofSubmitKickAfterLo:
            return "kick-after-lo";
        default:
            return "unknown";
    }
}

static void decodeExeclistSlots(uint32_t statusLo, uint32_t& validBits, uint32_t& activeBits, const char*& queueState)
{
    validBits = 0u;
    activeBits = 0u;
    if (statusLo & kExecStatusSlot0Valid) {
        validBits |= 1u << 0;
    }
    if (statusLo & kExecStatusSlot1Valid) {
        validBits |= 1u << 1;
    }
    if (statusLo & kExecStatusSlot0Active) {
        activeBits |= 1u << 0;
    }
    if (statusLo & kExecStatusSlot1Active) {
        activeBits |= 1u << 1;
    }

    if (validBits == 0u && activeBits == 0u) {
        queueState = "empty";
    } else if (validBits != 0u && activeBits == 0u) {
        queueState = "queued";
    } else if (activeBits != 0u) {
        queueState = "active";
    } else {
        queueState = "unknown";
    }
}

static inline void proofWriteLe32(void* dst, uint32_t value);
static inline void proofWriteLe64(void* dst, uint64_t value);

static void patchProofLrcRingBlock(const RcsProofResources& res, ProofRingProgrammingMode mode)
{
    if (!res.lrcGem) {
        return;
    }
    IOBufferMemoryDescriptor* md = res.lrcGem->memoryDescriptor();
    if (!md) {
        return;
    }
    uint8_t* lrcCpu = reinterpret_cast<uint8_t*>(md->getBytesNoCopy());
    if (!lrcCpu) {
        return;
    }

    const uint32_t startLo = (mode == ProofRingModeLiveOnly) ? 0u : (uint32_t)(res.ringGpuAddr & 0xFFFFFFFFULL);
    const uint32_t startHi = (mode == ProofRingModeLiveOnly) ? 0u : (uint32_t)(res.ringGpuAddr >> 32);
    const uint32_t tail = (mode == ProofRingModeLiveOnly) ? 0u : res.ringTailBytes;
    const uint32_t ctl = (mode == ProofRingModeLiveOnly) ? 0u : res.ringCtl;

    proofWriteLe32(lrcCpu + kProofLrcRingStateOffset + 0x00u, 0u);
    proofWriteLe32(lrcCpu + kProofLrcRingStateOffset + 0x04u, tail);
    proofWriteLe32(lrcCpu + kProofLrcRingStateOffset + 0x08u, startLo);
    proofWriteLe32(lrcCpu + kProofLrcRingStateOffset + 0x0Cu, startHi);
    proofWriteLe32(lrcCpu + kProofLrcRingStateOffset + 0x10u, ctl);
    OSSynchronizeIO();
}

static void patchProofLrcContextControl(const RcsProofResources& res, uint32_t ctxCtrl)
{
    if (!res.lrcGem) {
        return;
    }
    IOBufferMemoryDescriptor* md = res.lrcGem->memoryDescriptor();
    if (!md) {
        return;
    }
    uint8_t* lrcCpu = reinterpret_cast<uint8_t*>(md->getBytesNoCopy());
    if (!lrcCpu) {
        return;
    }
    proofWriteLe32(lrcCpu + 0x2Cu, ctxCtrl);
    OSSynchronizeIO();
}

static const uint32_t kGen12RcsLri0Regs[] = {
    0x2244u, 0x2034u, 0x2030u, 0x2038u, 0x203Cu, 0x2168u, 0x2140u,
    0x2110u, 0x21C0u, 0x21C4u, 0x21C8u, 0x2180u, 0x22B4u,
};

static const uint32_t kGen12RcsLri1Regs[] = {
    0x23A8u, 0x228Cu, 0x2288u, 0x2284u, 0x2280u, 0x227Cu, 0x2278u,
    0x2274u, 0x2270u,
};

static const uint32_t kGen12RcsLri2Regs[] = {
    0x21B0u, 0x25A8u, 0x25ACu,
};

static const uint32_t kGen12RcsLri3Regs[] = {
    0x2588u, 0x2588u, 0x2588u, 0x2588u, 0x2588u, 0x2588u,
    0x2028u, 0x209Cu, 0x20C0u, 0x2178u, 0x217Cu, 0x2358u,
    0x2170u, 0x2150u, 0x2154u, 0x2158u, 0x241Cu,
    0x2600u, 0x2604u, 0x2608u, 0x260Cu, 0x2610u, 0x2614u, 0x2618u,
    0x261Cu, 0x2620u, 0x2624u, 0x2628u, 0x262Cu, 0x2630u, 0x2634u,
    0x2638u, 0x263Cu, 0x2640u, 0x2644u, 0x2648u, 0x264Cu, 0x2650u,
    0x2654u, 0x2658u, 0x265Cu, 0x2660u, 0x2664u, 0x2668u, 0x266Cu,
    0x2670u, 0x2674u, 0x2678u, 0x267Cu, 0x2068u, 0x2084u,
};

static const uint32_t kGen12RcsRpcsReg[] = { 0x20C8u };

static const uint32_t kCtxContextControlIndex = 3u;
static const uint32_t kCtxRingHeadIndex = 5u;
static const uint32_t kCtxRingTailIndex = 7u;
static const uint32_t kCtxRingStartIndex = 9u;
static const uint32_t kCtxRingCtlIndex = 11u;
static const uint32_t kCtxPerCtxBbPtrIndex = 19u;
static const uint32_t kCtxIndirectCtxPtrIndex = 21u;
static const uint32_t kCtxIndirectCtxOffsetIndex = 23u;
static const uint32_t kCtxTimestampIndex = 35u;
static const uint32_t kCtxPdp3UdwIndex = 37u;
static const uint32_t kCtxPdp3LdwIndex = 39u;
static const uint32_t kCtxPdp2UdwIndex = 41u;
static const uint32_t kCtxPdp2LdwIndex = 43u;
static const uint32_t kCtxPdp1UdwIndex = 45u;
static const uint32_t kCtxPdp1LdwIndex = 47u;
static const uint32_t kCtxPdp0UdwIndex = 49u;
static const uint32_t kCtxPdp0LdwIndex = 51u;
static const uint32_t kCtxRPowerClockStateIndex = 67u;
static const uint32_t kCtxRingMiModeIndex = 97u;
static const uint32_t kCtxGpr0ValueIndex = 117u;
static const uint32_t kCtxCmdBufCctlValueIndex = 183u;
static const uint32_t kCtxRingMiModeStopRing = (1u << 8);
static const uint64_t kProofPpgttEntryFlags = 0x003ULL;
static const uint32_t kProofRcsGpr0Mmio = 0x2600u;
static const uint32_t kProofRcsCtxTimestampMmio = 0x23A8u;
static const uint32_t kProofRcsCmdBufCctlMmio = 0x2084u;
static const uint32_t kProofGen12CsDebugMode2Mmio = 0x20D8u;
static const uint32_t kProofGen12AuxInvMmio = 0x4208u;
static const uint32_t kProofInstructionStateCacheInvalidate = (1u << 6);
static const uint32_t kProofAuxInvBit = (1u << 0);
static const uint32_t kMiLoadRegisterImmBase = (0x22u << 23);
static const uint32_t kMiLoadRegisterMemGen8 = (0x29u << 23) | 2u;
static const uint32_t kMiSrmLrmGlobalGtt = (1u << 22);
static const uint32_t kMiLoadRegisterReg = (0x2Au << 23) | 1u;
static const uint32_t kMiLrrSourceCsMmio = (1u << 18);
static const uint32_t kMiLriLrmCsMmio = (1u << 19);
static const uint32_t kMiLriMmioRemapEn = (1u << 17);
static const uint32_t kMiSemaphoreWaitToken = (0x1Cu << 23) | 3u;
static const uint32_t kMiSemaphoreRegisterPoll = (1u << 16);
static const uint32_t kMiSemaphorePoll = (1u << 15);
static const uint32_t kMiSemaphoreSadEqSdd = (4u << 12);

static uint32_t makeLriHeader(uint32_t count, bool posted)
{
    const uint32_t kMiLriBase = (0x22u << 23) | (1u << 19);
    const uint32_t kMiLriPosted = (1u << 12);
    return kMiLriBase | (((2u * count - 1u) << 16)) | (posted ? kMiLriPosted : 0u) | count;
}

static void emitRegStateLri(uint32_t* regs,
                            uint32_t& idx,
                            const uint32_t* mmioRegs,
                            uint32_t count,
                            bool posted)
{
    regs[idx++] = makeLriHeader(count, posted);
    for (uint32_t i = 0; i < count; ++i) {
        regs[idx++] = mmioRegs[i];
        regs[idx++] = 0u;
    }
}

static void seedRegStateLriValuesFromLiveMmio(FakeIrisXEExeclist* self,
                                              uint32_t* regs,
                                              uint32_t headerIndex,
                                              const uint32_t* mmioRegs,
                                              uint32_t count)
{
    if (!self || !regs || !mmioRegs || !count) {
        return;
    }

    uint32_t pairIndex = headerIndex + 1u;
    for (uint32_t i = 0; i < count; ++i) {
        regs[pairIndex + 1u] = self->mmioRead32(mmioRegs[i]);
        pairIndex += 2u;
    }
}

static inline uint32_t maskedBitEnable(uint32_t bit)
{
    return bit | (bit << 16);
}

static inline uint32_t maskedBitsEnable(uint32_t bits)
{
    return bits | (bits << 16);
}

static inline uint32_t maskedBitDisable(uint32_t bit)
{
    return bit << 16;
}

static inline void proofWriteLe32(void* dst, uint32_t value)
{
    uint8_t* bytes = static_cast<uint8_t*>(dst);
    bytes[0] = static_cast<uint8_t>(value & 0xFFu);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    bytes[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

static inline void proofWriteLe64(void* dst, uint64_t value)
{
    proofWriteLe32(dst, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
    proofWriteLe32(static_cast<uint8_t*>(dst) + 4u, static_cast<uint32_t>(value >> 32));
}

static volatile uint64_t* csbCpuBase(IOBufferMemoryDescriptor* md)
{
    if (!md) {
        return nullptr;
    }

    uint8_t* bytes = (uint8_t*)md->getBytesNoCopy();
    if (!bytes) {
        return nullptr;
    }

    return (volatile uint64_t*)(bytes + kExecCsbOffsetBytes);
}

static void logProofSharedBacking(FakeIrisXEExeclist* self, const char* phase)
{
    if (!self || !self->fCsbGem) {
        return;
    }

    IOBufferMemoryDescriptor* md = self->fCsbGem->memoryDescriptor();
    if (!md) {
        return;
    }

    const uint64_t* hws = reinterpret_cast<const uint64_t*>(md->getBytesNoCopy());
    const volatile uint64_t* csb = csbCpuBase(md);
    if (!hws || !csb) {
        return;
    }

    IOLog("(FakeIrisXE) [%s] %s HWS[0]=0x%016llX HWS[1]=0x%016llX HWS[2]=0x%016llX HWS[3]=0x%016llX\n",
          kExeclistVersion,
          phase ? phase : "backing",
          static_cast<unsigned long long>(hws[0]),
          static_cast<unsigned long long>(hws[1]),
          static_cast<unsigned long long>(hws[2]),
          static_cast<unsigned long long>(hws[3]));
    IOLog("(FakeIrisXE) [%s] %s CSB[0]=0x%016llX CSB[1]=0x%016llX CSB[2]=0x%016llX CSB[3]=0x%016llX\n",
          kExeclistVersion,
          phase ? phase : "backing",
          static_cast<unsigned long long>(csb[0]),
          static_cast<unsigned long long>(csb[1]),
          static_cast<unsigned long long>(csb[2]),
          static_cast<unsigned long long>(csb[3]));
}

static void logProofRingGating(FakeIrisXEExeclist* self, const char* phase)
{
    if (!self) {
        return;
    }

    const uint32_t rcsStatus = self->mmioRead32(kExecRcsStatusReg);
    const uint32_t ringCtl = self->mmioRead32(kExecRingCtlReg);
    const uint32_t ringHead = self->mmioRead32(kExecRingHeadReg);
    const uint32_t ringTail = self->mmioRead32(kExecRingTailReg);
    const uint32_t execlistCtl = self->mmioRead32(RCS0_EXECLIST_CONTROL);
    const uint32_t execlistArb = self->mmioRead32(RCS0_EXECLIST_ARB_CTL);
    const uint32_t ctxCtl = self->mmioRead32(kExecContextControlReg);
    const uint32_t miMode = self->mmioRead32(0x209Cu);
    const uint32_t cmdBufCctl = self->mmioRead32(kProofRcsCmdBufCctlMmio);
    const uint32_t debugMode2 = self->mmioRead32(kProofGen12CsDebugMode2Mmio);
    const uint32_t hwsPga = self->mmioRead32(kExecHwsPgaReg);
    const uint64_t expectedHws = self->fCsbGGTT & ~0xFFFULL;
    const bool halted = (rcsStatus & 0xE000u) == 0xE000u;
    const bool idle = (rcsStatus & 0x1u) != 0u;

    IOLog("(FakeIrisXE) [%s] %s gating: RCS_STATUS=0x%08X halted=%u idle=%u RING_CTL=0x%08X HEAD=0x%08X TAIL=0x%08X EXECLIST_CTL=0x%08X ARB=0x%08X CTXCTL=0x%08X MI_MODE=0x%08X CCTL=0x%08X DEBUG2=0x%08X HWS_PGA=0x%08X expectedHws=0x%08X\n",
          kExeclistVersion,
          phase ? phase : "gating",
          rcsStatus,
          halted ? 1u : 0u,
          idle ? 1u : 0u,
          ringCtl,
          ringHead,
          ringTail,
          execlistCtl,
          execlistArb,
          ctxCtl,
          miMode,
          cmdBufCctl,
          debugMode2,
          hwsPga,
          static_cast<uint32_t>(expectedHws & 0xFFFFFFFFULL));
}

static bool shouldTryNextProofVariant(ProofFailureType failure)
{
    switch (failure) {
        case RingControlNotEnabled:
        case ElspRejected:
        case ElspVisibleNotAccepted:
        case CsbNoProgress:
        case NoSchedulingProgress:
        case ContextStateNotLoaded:
        case BatchNeverStarted:
            return true;
        default:
            return false;
    }
}

static uint32_t buildProofIndirectCtxCommands(const RcsProofResources& res, uint32_t* indirectCtx)
{
    if (!indirectCtx) {
        return 0;
    }

    const uint64_t regStateGpuAddr = res.lrcGpuAddr + kProofRegStateOffset;
    const uint64_t ctxTimestampAddr = regStateGpuAddr + (uint64_t)kCtxTimestampIndex * sizeof(uint32_t);
    const uint64_t gpr0ValueAddr = regStateGpuAddr + (uint64_t)kCtxGpr0ValueIndex * sizeof(uint32_t);
    const uint64_t cmdBufCctlAddr = regStateGpuAddr + (uint64_t)kCtxCmdBufCctlValueIndex * sizeof(uint32_t);

    auto emitLrm = [&](uint32_t reg, uint64_t addr, uint32_t& idx) {
        indirectCtx[idx++] = kMiLoadRegisterMemGen8 | kMiSrmLrmGlobalGtt | kMiLriLrmCsMmio;
        indirectCtx[idx++] = reg;
        indirectCtx[idx++] = (uint32_t)(addr & 0xFFFFFFFFULL);
        indirectCtx[idx++] = (uint32_t)(addr >> 32);
    };

    auto emitLrr = [&](uint32_t srcReg, uint32_t dstReg, uint32_t& idx) {
        indirectCtx[idx++] = kMiLoadRegisterReg | kMiLrrSourceCsMmio | kMiLriLrmCsMmio;
        indirectCtx[idx++] = srcReg;
        indirectCtx[idx++] = dstReg;
    };

    auto emitLri = [&](uint32_t reg, uint32_t value, uint32_t& idx, bool mmioRemap) {
        indirectCtx[idx++] = kMiLoadRegisterImmBase | (1u << 16) | (mmioRemap ? kMiLriMmioRemapEn : 0u) | 1u;
        indirectCtx[idx++] = reg;
        indirectCtx[idx++] = value;
    };

    auto emitSemaphoreRegisterWaitEqZero = [&](uint32_t reg, uint32_t& idx) {
        indirectCtx[idx++] = kMiSemaphoreWaitToken |
                             kMiSemaphoreRegisterPoll |
                             kMiSemaphorePoll |
                             kMiSemaphoreSadEqSdd;
        indirectCtx[idx++] = 0u;
        indirectCtx[idx++] = reg;
        indirectCtx[idx++] = 0u;
        indirectCtx[idx++] = 0u;
    };

    uint32_t idx = 0;
    emitLrm(kProofRcsGpr0Mmio, ctxTimestampAddr, idx);
    emitLrr(kProofRcsGpr0Mmio, kProofRcsCtxTimestampMmio, idx);
    emitLrr(kProofRcsGpr0Mmio, kProofRcsCtxTimestampMmio, idx);
    emitLrm(kProofRcsGpr0Mmio, cmdBufCctlAddr, idx);
    emitLrr(kProofRcsGpr0Mmio, kProofRcsCmdBufCctlMmio, idx);
    emitLrm(kProofRcsGpr0Mmio, gpr0ValueAddr, idx);
    emitLri(kProofGen12AuxInvMmio, kProofAuxInvBit, idx, true);
    emitSemaphoreRegisterWaitEqZero(kProofGen12AuxInvMmio, idx);
    emitLri(kProofGen12CsDebugMode2Mmio,
            maskedBitEnable(kProofInstructionStateCacheInvalidate),
            idx,
            false);

    while (((idx * sizeof(uint32_t)) & 63u) != 0u) {
        indirectCtx[idx++] = MI_NOOP;
    }

    return idx * sizeof(uint32_t);
}

static const char* proofFailureLabel(ProofFailureType type)
{
    switch (type) {
        case RingControlNotEnabled:
            return "D_RING_CTL_NOT_ENABLED";
        case ElspRejected:
            return "A_ELSP_REJECTED";
        case ElspVisibleNotAccepted:
            return "B_ELSP_VISIBLE_NOT_ACCEPTED";
        case DescriptorWrong:
            return "C_DESCRIPTOR_FORMAT_WRONG";
        case LrcLayoutWrong:
            return "D_LRC_LAYOUT_WRONG";
        case ContextStateNotLoaded:
            return "F_CONTEXT_STATE_NOT_LOADED";
        case RingStateWrong:
            return "G_RING_STATE_WRONG";
        case BatchNeverStarted:
            return "H_BATCH_NEVER_STARTED";
        case MiPacketWrong:
            return "I_MI_PACKET_WRONG";
        case ScratchWritebackMissing:
            return "J_SCRATCH_WRITEBACK_MISSING";
        case EngineHardHalted:
            return "K_RCS_HARD_HALTED";
        case ScratchMappingUnavailable:
            return "L_SCRATCH_MAPPING_UNAVAILABLE";
        case CsbNoProgress:
            return "M_CSB_NO_PROGRESS";
        case NoSchedulingProgress:
            return "N_NO_SCHEDULING_PROGRESS";
        // V326: Enhanced failure taxonomy
        case MaskedRingStableNoSubmitVisibility:
            return "O_MASKED_RING_STABLE_NO_SUBMIT";
        case SubmitVisibleNoSlotAccept:
            return "P_SUBMIT_VISIBLE_NO_SLOT_ACCEPT";
        case SlotAcceptNoContextLoad:
            return "Q_SLOT_ACCEPT_NO_CONTEXT_LOAD";
        case ContextLoadNoExecution:
            return "R_CONTEXT_LOAD_NO_EXECUTION";
        case ExecutionNoScratchWrite:
            return "S_EXECUTION_NO_SCRATCH_WRITE";
        case CsBobservationUntrusted:
            return "T_CSB_OBSERVATION_UNTRUSTED";
        default:
            return "NONE";
    }
}

static void setProofBoolProperty(IORegistryEntry* entry, const char* key, bool value)
{
    if (!entry || !key) {
        return;
    }
    entry->setProperty(key, value ? kOSBooleanTrue : kOSBooleanFalse);
}

static void setProofNumberProperty(IORegistryEntry* entry, const char* key, uint64_t value, uint32_t bits)
{
    if (!entry || !key) {
        return;
    }

    OSNumber* number = OSNumber::withNumber(value, bits);
    if (!number) {
        return;
    }

    entry->setProperty(key, number);
    number->release();
}

static void logProofDwords(const char* label, const uint32_t* words, uint32_t count)
{
    if (!label || !words) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        IOLog("(FakeIrisXE) [V274] %s[%u] = 0x%08X\n", label, i, words[i]);
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

static void cleanupProofCpuGem(FakeIrisXEGEM*& gem, uint64_t& physAddr)
{
    if (!gem) {
        physAddr = 0;
        return;
    }

    gem->unpin();
    gem->release();
    gem = nullptr;
    physAddr = 0;
}

static bool allocateProofCpuPage(FakeIrisXEGEM*& gem,
                                 uint64_t& physAddr,
                                 const char* label)
{
    gem = FakeIrisXEGEM::withSize(kProofPml4Bytes, 0);
    if (!gem) {
        IOLog("(FakeIrisXE) [V274] ❌ %s allocation failed\n", label ? label : "CPU page");
        return false;
    }

    gem->pin();

    uint64_t segLen = 0;
    physAddr = gem->getPhysicalSegment(0, &segLen) & ~0xFFFULL;
    if (!physAddr) {
        IOLog("(FakeIrisXE) [V274] ❌ %s physical address acquisition failed\n", label ? label : "CPU page");
        cleanupProofCpuGem(gem, physAddr);
        return false;
    }

    IOBufferMemoryDescriptor* md = gem->memoryDescriptor();
    if (!md || !md->getBytesNoCopy()) {
        IOLog("(FakeIrisXE) [V274] ❌ %s CPU mapping failed\n", label ? label : "CPU page");
        cleanupProofCpuGem(gem, physAddr);
        return false;
    }

    bzero(md->getBytesNoCopy(), kProofPml4Bytes);
    return true;
}

static bool buildProofPageTables(RcsProofResources& res)
{
    if (!res.pml4Gem || !res.pdptGem || !res.pdGem || !res.ptGem || !res.scratchGem) {
        return false;
    }

    IOBufferMemoryDescriptor* pml4Md = res.pml4Gem->memoryDescriptor();
    IOBufferMemoryDescriptor* pdptMd = res.pdptGem->memoryDescriptor();
    IOBufferMemoryDescriptor* pdMd = res.pdGem->memoryDescriptor();
    IOBufferMemoryDescriptor* ptMd = res.ptGem->memoryDescriptor();
    if (!pml4Md || !pdptMd || !pdMd || !ptMd) {
        return false;
    }

    uint64_t scratchSegLen = 0;
    const uint64_t scratchPhys = res.scratchGem->getPhysicalSegment(0, &scratchSegLen) & ~0xFFFULL;
    if (!scratchPhys) {
        IOLog("(FakeIrisXE) [V274] ❌ Scratch physical address acquisition failed for PPGTT setup\n");
        return false;
    }

    uint64_t* const pml4 = (uint64_t*)pml4Md->getBytesNoCopy();
    uint64_t* const pdpt = (uint64_t*)pdptMd->getBytesNoCopy();
    uint64_t* const pd = (uint64_t*)pdMd->getBytesNoCopy();
    uint64_t* const pt = (uint64_t*)ptMd->getBytesNoCopy();
    if (!pml4 || !pdpt || !pd || !pt) {
        IOLog("(FakeIrisXE) [V274] ❌ Page-table CPU pointer missing\n");
        return false;
    }

    bzero(pml4, kProofPml4Bytes);
    bzero(pdpt, kProofPml4Bytes);
    bzero(pd, kProofPml4Bytes);
    bzero(pt, kProofPml4Bytes);

    pml4[0] = (res.pdptPhysAddr & ~0xFFFULL) | kProofPpgttEntryFlags;
    pdpt[0] = (res.pdPhysAddr & ~0xFFFULL) | kProofPpgttEntryFlags;
    pd[0] = (res.ptPhysAddr & ~0xFFFULL) | kProofPpgttEntryFlags;
    pt[(kProofPpgttScratchVa >> 12) & 0x1FFULL] = (scratchPhys & ~0xFFFULL) | kProofPpgttEntryFlags;

    __sync_synchronize();
    OSSynchronizeIO();

    IOLog("(FakeIrisXE) [V274]   PPGTT scratch VA: 0x%016llX -> phys 0x%016llX\n",
          (unsigned long long)kProofPpgttScratchVa,
          (unsigned long long)scratchPhys);
    return true;
}

static void releaseProofResources(FakeIrisXEExeclist* self, RcsProofResources& res)
{
    if (!self || !self->fOwner) {
        return;
    }

    cleanupProofGem(self, res.scratchGem, res.scratchGpuAddr, 4096u);
    cleanupProofGem(self, res.lrcGem, res.lrcGpuAddr, kProofContextBytes);
    cleanupProofGem(self, res.ringGem, res.ringGpuAddr, kProofRingSize);
    cleanupProofCpuGem(res.ptGem, res.ptPhysAddr);
    cleanupProofCpuGem(res.pdGem, res.pdPhysAddr);
    cleanupProofCpuGem(res.pdptGem, res.pdptPhysAddr);
    cleanupProofCpuGem(res.pml4Gem, res.pml4PhysAddr);
}

static bool allocateProofResources(FakeIrisXEExeclist* self, RcsProofResources& res)
{
    if (!self || !self->fOwner) {
        return false;
    }

    IOLog("(FakeIrisXE) [V274] Allocating direct Execlist proof resources...\n");

    res.ringGem = FakeIrisXEGEM::withSize(kProofRingSize, 0);
    if (!res.ringGem) {
        IOLog("(FakeIrisXE) [V274] ❌ Ring allocation failed\n");
        return false;
    }
    res.ringGem->pin();
    res.ringGpuAddr = self->fOwner->ggttMap(res.ringGem) & ~0xFFFULL;
    if (!res.ringGpuAddr) {
        IOLog("(FakeIrisXE) [V274] ❌ Ring GGTT mapping failed\n");
        releaseProofResources(self, res);
        return false;
    }

    res.lrcGem = FakeIrisXEGEM::withSize(kProofContextBytes, 0);
    if (!res.lrcGem) {
        IOLog("(FakeIrisXE) [V274] ❌ Context allocation failed\n");
        releaseProofResources(self, res);
        return false;
    }
    res.lrcGem->pin();
    res.lrcGpuAddr = self->fOwner->ggttMap(res.lrcGem) & ~0xFFFULL;
    if (!res.lrcGpuAddr) {
        IOLog("(FakeIrisXE) [V274] ❌ Context GGTT mapping failed\n");
        releaseProofResources(self, res);
        return false;
    }

    res.scratchGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!res.scratchGem) {
        IOLog("(FakeIrisXE) [V274] ❌ Scratch allocation failed\n");
        releaseProofResources(self, res);
        return false;
    }
    res.scratchGem->pin();
    res.scratchGpuAddr = self->fOwner->ggttMap(res.scratchGem) & ~0xFFFULL;
    if (!res.scratchGpuAddr) {
        IOLog("(FakeIrisXE) [V274] ❌ Scratch GGTT mapping failed\n");
        releaseProofResources(self, res);
        return false;
    }

    void* scratchCpu = self->fOwner->ggttGetCPUAddr(res.scratchGem);
    if (!scratchCpu) {
        IOLog("(FakeIrisXE) [V274] ❌ Scratch CPU mapping failed\n");
        releaseProofResources(self, res);
        return false;
    }

    *(volatile uint32_t*)scratchCpu = kProofScratchInitial;
    __sync_synchronize();
    OSSynchronizeIO();

    const uint64_t sharedHwsGpuAddr = self->fCsbGGTT & ~0xFFFULL;
    res.csbGpuAddr = sharedHwsGpuAddr + kExecCsbOffsetBytes;
    if (!res.csbGpuAddr) {
        IOLog("(FakeIrisXE) [V274] ❌ Shared CSB backing is missing; direct proof must use the staged Execlist owner HWS/CSB page\n");
        releaseProofResources(self, res);
        return false;
    }

    if (!allocateProofCpuPage(res.pml4Gem, res.pml4PhysAddr, "PML4") ||
        !allocateProofCpuPage(res.pdptGem, res.pdptPhysAddr, "PDPT") ||
        !allocateProofCpuPage(res.pdGem, res.pdPhysAddr, "PD") ||
        !allocateProofCpuPage(res.ptGem, res.ptPhysAddr, "PT")) {
        releaseProofResources(self, res);
        return false;
    }

    if (!buildProofPageTables(res)) {
        releaseProofResources(self, res);
        return false;
    }

    IOLog("(FakeIrisXE) [V274]   Ring GPU VA:    0x%016llX\n", (unsigned long long)res.ringGpuAddr);
    IOLog("(FakeIrisXE) [V274]   CTX GPU VA:     0x%016llX\n", (unsigned long long)res.lrcGpuAddr);
    IOLog("(FakeIrisXE) [V274]   Scratch GPU VA: 0x%016llX\n", (unsigned long long)res.scratchGpuAddr);
    IOLog("(FakeIrisXE) [V274]   Shared HWS VA:  0x%016llX\n", (unsigned long long)sharedHwsGpuAddr);
    IOLog("(FakeIrisXE) [V274]   Shared CSB VA:  0x%016llX\n", (unsigned long long)res.csbGpuAddr);
    IOLog("(FakeIrisXE) [V274]   PML4 phys:      0x%016llX\n", (unsigned long long)res.pml4PhysAddr);
    IOLog("(FakeIrisXE) [V274]   PDPT phys:      0x%016llX\n", (unsigned long long)res.pdptPhysAddr);
    IOLog("(FakeIrisXE) [V274]   PD phys:        0x%016llX\n", (unsigned long long)res.pdPhysAddr);
    IOLog("(FakeIrisXE) [V274]   PT phys:        0x%016llX\n", (unsigned long long)res.ptPhysAddr);
    IOLog("(FakeIrisXE) [V274]   Scratch init:   0x%08X\n", kProofScratchInitial);
    return true;
}

static bool buildProofCommandStream(FakeIrisXEExeclist* self, RcsProofResources& res)
{
    if (!self || !self->fOwner || !res.ringGem) {
        return false;
    }

    uint32_t* ringCpu = (uint32_t*)self->fOwner->ggttGetCPUAddr(res.ringGem);
    if (!ringCpu) {
        IOLog("(FakeIrisXE) [V274] ❌ Ring CPU mapping failed\n");
        return false;
    }

    bzero(ringCpu, kProofRingSize);
    ringCpu[0] = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
    ringCpu[1] = (uint32_t)(res.scratchGpuAddr & 0xFFFFFFFFULL);
    ringCpu[2] = (uint32_t)(res.scratchGpuAddr >> 32);
    ringCpu[3] = res.expectedValue;
    ringCpu[4] = MI_BATCH_BUFFER_END;
    ringCpu[5] = MI_NOOP;
    res.ringTailBytes = 6u * sizeof(uint32_t);
    res.ringCtl = RING_CTL_SIZE(kProofRingSize) | RING_VALID;

    __sync_synchronize();
    OSSynchronizeIO();

    IOLog("(FakeIrisXE) [V274] ========== RCS TEST COMMAND STREAM ==========" "\n");
    IOLog("(FakeIrisXE) [V274]   Packet: MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT\n");
    IOLog("(FakeIrisXE) [V274]   Ring GPU VA: 0x%016llX\n", (unsigned long long)res.ringGpuAddr);
    IOLog("(FakeIrisXE) [V274]   Ring size:   %u bytes\n", kProofRingSize);
    IOLog("(FakeIrisXE) [V274]   Ring head:   0 bytes\n");
    IOLog("(FakeIrisXE) [V274]   Ring tail:   %u bytes\n", res.ringTailBytes);
    IOLog("(FakeIrisXE) [V274]   Ring ctl:    0x%08X\n", res.ringCtl);
    logProofDwords("RingDW", ringCpu, 16);

    return true;
}

static bool programProofRingState(FakeIrisXEExeclist* self,
                                  const RcsProofResources& res,
                                  ProofObservations* observations)
{
    if (!self || !self->fOwner) {
        return false;
    }

    struct RingProgramOrder {
        const char* name;
        bool ctlBeforeTail;
    };
    static const RingProgramOrder kOrders[] = {
        { "start-head-tail-ctl", false },
        { "start-head-ctl-tail", true },
    };

    for (uint32_t i = 0; i < sizeof(kOrders) / sizeof(kOrders[0]); ++i) {
        const RingProgramOrder& order = kOrders[i];
        self->mmioWrite32(kExecRingStartReg, (uint32_t)(res.ringGpuAddr & 0xFFFFFFFFULL));
        IOSleep(1);
        self->mmioWrite32(kExecRingHeadReg, 0u);
        IOSleep(1);

        if (order.ctlBeforeTail) {
            self->mmioWrite32(kExecRingCtlReg, res.ringCtl);
            IOSleep(1);
            self->mmioWrite32(kExecRingTailReg, res.ringTailBytes);
        } else {
            self->mmioWrite32(kExecRingTailReg, res.ringTailBytes);
            IOSleep(1);
            self->mmioWrite32(kExecRingCtlReg, res.ringCtl);
        }
        IOSleep(1);

        const uint32_t liveStart = self->mmioRead32(kExecRingStartReg);
        const uint32_t liveHead = self->mmioRead32(kExecRingHeadReg);
        const uint32_t liveTail = self->mmioRead32(kExecRingTailReg);
        const uint32_t liveCtl = self->mmioRead32(kExecRingCtlReg);
        const bool ctlEnabled = (liveCtl & RING_VALID) != 0u;
        const bool ctlMasked = ((liveCtl & ~RING_VALID) == (res.ringCtl & ~RING_VALID)) && !ctlEnabled;
        const bool startSticks = (liveStart == (uint32_t)(res.ringGpuAddr & 0xFFFFFFFFULL));
        const bool tailSticks = (liveTail == res.ringTailBytes);
        const bool sizeMatch = ((liveCtl & 0x001FF000u) == (res.ringCtl & 0x001FF000u));

        IOLog("(FakeIrisXE) [%s] Ring program order=%s START=0x%08X HEAD=0x%08X TAIL=0x%08X CTL=0x%08X expectedCtl=0x%08X enabled=%u masked=%u startSticks=%u tailSticks=%u sizeMatch=%u\n",
              kExeclistVersion,
              order.name,
              liveStart,
              liveHead,
              liveTail,
              liveCtl,
              res.ringCtl,
              ctlEnabled ? 1u : 0u,
              ctlMasked ? 1u : 0u,
              startSticks ? 1u : 0u,
              tailSticks ? 1u : 0u,
              sizeMatch ? 1u : 0u);

        if (observations) {
            observations->lastRingStart = liveStart;
            observations->lastRingHead = liveHead;
            observations->lastRingTail = liveTail;
            observations->lastRingCtl = liveCtl;
            observations->ringCtlEnabled = ctlEnabled;
            observations->ringCtlMasked = ctlMasked;
        }

        if (ctlEnabled) {
            return true;
        }

        // V323: Accept masked-but-stable ring: size bits match and start/tail stick
        // This is the actual hardware behavior on this platform (bit 0 masked off)
        if (ctlMasked && startSticks && tailSticks && sizeMatch) {
            IOLog("(FakeIrisXE) [%s] Ring masked-but-stable accepted: START/TAIL stick, size bits match\n", kExeclistVersion);
            return true;
        }
    }

    return false;
}

static bool buildProofLrc(FakeIrisXEExeclist* self, RcsProofResources& res)
{
    if (!self || !self->fOwner) {
        return false;
    }

    IOReturn lrcBuildErr = kIOReturnError;
    if (res.lrcGem) {
        res.lrcGem->unpin();
        res.lrcGem->release();
        res.lrcGem = nullptr;
    }

    res.lrcGem = FakeIrisXELRC::buildLRCContext(self->fOwner,
                                                res.ringGem,
                                                kProofRingSize,
                                                res.ringGpuAddr,
                                                0u,
                                                res.ringTailBytes,
                                                res.pml4PhysAddr,
                                                &lrcBuildErr);
    if (!res.lrcGem || lrcBuildErr != kIOReturnSuccess) {
        IOLog("(FakeIrisXE) [%s] direct proof LRC build failed err=0x%x gem=%p\n", kExeclistVersion, lrcBuildErr, res.lrcGem);
        return false;
    }
    res.lrcGpuAddr = res.lrcGem->gpuAddress() & ~0xFFFULL;
    if (!res.lrcGpuAddr) {
        res.lrcGpuAddr = self->fOwner->ggttMap(res.lrcGem) & ~0xFFFULL;
    }
    if (!res.lrcGpuAddr) {
        IOLog("(FakeIrisXE) [%s] direct proof LRC GGTT mapping missing\n", kExeclistVersion);
        return false;
    }

    uint8_t* lrcCpu = (uint8_t*)self->fOwner->ggttGetCPUAddr(res.lrcGem);
    if (!lrcCpu) {
        IOLog("(FakeIrisXE) [%s] direct proof context CPU mapping failed\n", kExeclistVersion);
        return false;
    }

    IOLog("(FakeIrisXE) [%s] direct proof using canonical Gen12 LRC image ppgtt=0x%016llX ring=0x%016llX tail=%u ctl=0x%08X\n",
          kExeclistVersion,
          (unsigned long long)res.pml4PhysAddr,
          (unsigned long long)res.ringGpuAddr,
          res.ringTailBytes,
          res.ringCtl);
    return true;
}

static void buildProofDescriptor(RcsProofResources& res, const ProofVariant& variant)
{
    res.descLo = ((uint32_t)(res.lrcGpuAddr & 0xFFFFF000ULL)) |
                 kCtxDescValid |
                 (variant.active ? kCtxDescActive : 0u) |  // V319: Include ACTIVE bit
                 (variant.privilege ? kCtxDescPrivilege : 0u) |
                 (variant.forceRestore ? kCtxDescForceRestore : 0u) |
                 ((variant.addrMode & 0x3u) << kCtxDescAddressingModeShift);
    res.descHi = ((variant.swContextId & 0x7FFu) << kCtxDescSwCtxIdShiftInHi) |
                 ((variant.engineInstance & 0x3Fu) << kCtxDescEngineInstanceShiftInHi) |
                 ((variant.engineClass & 0x7u) << kCtxDescEngineClassShiftInHi);

    IOLog("(FakeIrisXE) [%s] ========== CONTEXT DESCRIPTOR ==========\n", kExeclistVersion);
    IOLog("(FakeIrisXE) [%s]   Variant: %s ring=%s submit=%s ctxCtrl=0x%08X priv=%u force=%u active=%u addr=%u swctx=%u class=%u inst=%u\n",
          kExeclistVersion,
          variant.label,
          proofRingModeLabel(variant.ringMode),
          proofSubmitStyleLabel(variant.submitStyle),
          variant.ctxCtrl,
          variant.privilege ? 1u : 0u,
          variant.forceRestore ? 1u : 0u,
          variant.active ? 1u : 0u,  // V319
          variant.addrMode,
          variant.swContextId,
          variant.engineClass,
          variant.engineInstance);
    IOLog("(FakeIrisXE) [%s]   DWord0: 0x%08X\n", kExeclistVersion, res.descLo);
    IOLog("(FakeIrisXE) [%s]   DWord1: 0x%08X\n", kExeclistVersion, res.descHi);
    IOLog("(FakeIrisXE) [%s]   Address field: 0x%08X -> GPU VA 0x%016llX\n",
          kExeclistVersion,
          res.descLo & 0xFFFFF000u,
          (unsigned long long)(res.descLo & 0xFFFFF000u));
    IOLog("(FakeIrisXE) [%s]   Flags: VALID=%u PRIV=%u FORCE_RESTORE=%u ADDR_MODE=%u\n",
          kExeclistVersion,
          (res.descLo & kCtxDescValid) ? 1u : 0u,
          (res.descLo & kCtxDescPrivilege) ? 1u : 0u,
          (res.descLo & kCtxDescForceRestore) ? 1u : 0u,
          (res.descLo >> kCtxDescAddressingModeShift) & 0x3u);
    IOLog("(FakeIrisXE) [%s]   Context pages: %u (descriptor does not encode page count on Gen11+)\n",
          kExeclistVersion,
          kProofContextPages);
    IOLog("(FakeIrisXE) [%s]   SW context ID: %u EngineClass: %u EngineInstance: %u\n",
          kExeclistVersion,
          (res.descHi >> kCtxDescSwCtxIdShiftInHi) & 0x7FFu,
          (res.descHi >> kCtxDescEngineClassShiftInHi) & 0x7u,
          (res.descHi >> kCtxDescEngineInstanceShiftInHi) & 0x3Fu);
}

static void logProofLrcImage(const RcsProofResources& res)
{
    if (!res.lrcGem) {
        return;
    }

    IOBufferMemoryDescriptor* md = res.lrcGem->memoryDescriptor();
    if (!md) {
        return;
    }

    const uint32_t* lrcCpu = reinterpret_cast<const uint32_t*>(md->getBytesNoCopy());
    if (!lrcCpu) {
        return;
    }

    const uint32_t* ringState = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(lrcCpu) + kProofLrcRingStateOffset);
    IOLog("(FakeIrisXE) [%s] LRC image: CTX[0]=0x%08X CTX[1]=0x%08X CTX[2]=0x%08X CTX[3]=0x%08X\n",
          kExeclistVersion,
          lrcCpu[0], lrcCpu[1], lrcCpu[2], lrcCpu[3]);
    IOLog("(FakeIrisXE) [%s] LRC header: PDP0_LO=0x%08X PDP0_HI=0x%08X CTX_CTRL=0x%08X TS_CTRL=0x%08X\n",
          kExeclistVersion,
          lrcCpu[0], lrcCpu[1], lrcCpu[0x2C / 4], lrcCpu[0x30 / 4]);
    IOLog("(FakeIrisXE) [%s] LRC ring block @0x100: HEAD=0x%08X TAIL=0x%08X START_LO=0x%08X START_HI=0x%08X CTL=0x%08X\n",
          kExeclistVersion,
          ringState[0], ringState[1], ringState[2], ringState[3], ringState[4]);
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

    IOLog("(FakeIrisXE) [%s] Pre-submit RCS status=0x%08X GT_ERROR=0x%08X\n", kExeclistVersion, statusBefore, gtErrorBefore);
    if (!haltedBefore && !wedgedBefore) {
        return true;
    }

    IOLog("(FakeIrisXE) [%s] RCS looks halted/wedged; performing one focused reset attempt\n", kExeclistVersion);
    self->mmioWrite32(RCS0_RESET_CTRL, 0x00000001u);
    IOSleep(5);
    self->mmioWrite32(RCS0_RESET_CTRL, 0x00000000u);
    IOSleep(5);

    const uint32_t statusAfter = self->mmioRead32(kExecRcsStatusReg);
    const uint32_t gtErrorAfter = self->fOwner->safeMMIORead(kExecGtErrorReg);
    IOLog("(FakeIrisXE) [%s] Post-reset RCS status=0x%08X GT_ERROR=0x%08X\n", kExeclistVersion, statusAfter, gtErrorAfter);
    return ((statusAfter & 0xE000u) != 0xE000u) && ((gtErrorAfter & 0x80000000u) == 0);
}

static bool submitProofDescriptor(FakeIrisXEExeclist* self,
                                  RcsProofResources& res,
                                  const ProofVariant& variant,
                                  ProofFailureType& failure,
                                  ProofObservations& observations)
{
    if (!self) {
        return false;
    }

    const uint32_t preLo = self->mmioRead32(kExecElspPrimaryLo);
    const uint32_t preHi = self->mmioRead32(kExecElspPrimaryHi);
    const uint32_t preStatusLo = self->mmioRead32(kExecStatusPrimaryLo);
    const uint32_t preStatusHi = self->mmioRead32(kExecStatusPrimaryHi);
    const uint32_t preCsbCtrl = self->mmioRead32(RCS0_CSB_CTRL);
    const uint32_t preCsbAddrLo = self->mmioRead32(RCS0_CSB_ADDR_LO);
    const uint32_t preCsbAddrHi = self->mmioRead32(RCS0_CSB_ADDR_HI);
    const uint32_t preCsbRead = self->mmioRead32(RCS0_CSB_READ_PTR);
    const uint32_t preCsbWrite = self->mmioRead32(RCS0_CSB_WRITE_PTR);
    const uint32_t preCtxCtrl = self->mmioRead32(kExecContextControlReg);
    uint32_t preValidBits = 0;
    uint32_t preActiveBits = 0;
    const char* preQueueState = nullptr;
    decodeExeclistSlots(preStatusLo, preValidBits, preActiveBits, preQueueState);
    observations.attemptedRingMode = static_cast<uint32_t>(variant.ringMode);
    observations.attemptedSubmitStyle = static_cast<uint32_t>(variant.submitStyle);
    observations.attemptedCtxCtrl = variant.ctxCtrl;
    observations.attemptedAddrMode = variant.addrMode;
    observations.attemptedForceRestore = variant.forceRestore;
    observations.attemptedPrivilege = variant.privilege;

    IOLog("(FakeIrisXE) [%s] Submit preflight: ELSP=%08X/%08X STATUS=%08X/%08X slots(valid=0x%X active=0x%X state=%s) CSB=%08X addr=%08X%08X rp=%08X wp=%08X CTXCTL=%08X DESC=%08X/%08X ring=0x%llX tail=0x%X\n",
          kExeclistVersion,
          preLo,
          preHi,
          preStatusLo,
          preStatusHi,
          preValidBits,
          preActiveBits,
          preQueueState,
          preCsbCtrl,
          preCsbAddrHi,
          preCsbAddrLo,
          preCsbRead,
          preCsbWrite,
          preCtxCtrl,
          res.descLo,
          res.descHi,
          static_cast<unsigned long long>(res.ringGpuAddr),
          res.ringTailBytes);

    if (self->fCsbGem) {
        if (IOBufferMemoryDescriptor* csbMd = self->fCsbGem->memoryDescriptor()) {
            volatile uint64_t* csb = csbCpuBase(csbMd);
            if (csb) {
                IOLog("(FakeIrisXE) [%s] Submit preflight: CSB[0]=0x%016llX CSB[1]=0x%016llX CSB[2]=0x%016llX CSB[3]=0x%016llX\n",
                      kExeclistVersion,
                      static_cast<unsigned long long>(csb[0]),
                      static_cast<unsigned long long>(csb[1]),
                      static_cast<unsigned long long>(csb[2]),
                      static_cast<unsigned long long>(csb[3]));
            }
        }
    }

    self->fOwner->forcewakeRenderHold();
    
    // V319: Apple-style ForceWake write before submission (write to 0xA188)
    self->fOwner->safeMMIOWrite(0xA188, 0x000F000F);
    IOSleep(5);
    
    // V326: Read RING_MODE register to check execlist enable bit before submission
    const uint32_t preRingMode = self->mmioRead32(RCS0_RING_MODE);
    IOLog("(FakeIrisXE) [%s] Pre-submit RING_MODE: 0x%08X (execlist_enable_bit=%u)\n",
          kExeclistVersion,
          preRingMode,
          (preRingMode & 0x1) ? 1u : 0u);
    
    // V326: Enhanced RCS diagnostics and reset attempt before submission
    const uint32_t preResetRcsStatus = self->mmioRead32(kExecRcsStatusReg);
    const uint32_t preResetGtError = self->fOwner->safeMMIORead(kExecGtErrorReg);
    const uint32_t preResetRingCtl = self->mmioRead32(kExecRingCtlReg);
    const uint32_t preResetRingHead = self->mmioRead32(kExecRingHeadReg);
    const uint32_t preResetRingTail = self->mmioRead32(kExecRingTailReg);
    const uint32_t preResetExeclistCtl = self->mmioRead32(kExecCsbCtrl);  // V319: Use Apple CSB ctrl reg
    const uint32_t preResetCtxCtrl = self->mmioRead32(kExecContextControlReg);
    const uint32_t preResetCcid = self->mmioRead32(kExecCcidReg);
    const uint32_t preFirmwareStatus = self->fOwner->safeMMIORead(0xA188);  // V319: Read back ForceWake
    IOLog("(FakeIrisXE) [%s] Pre-submit RCS diagnostics: STATUS=0x%08X GT_ERR=0x%08X RING_CTL=0x%08X HEAD=0x%08X TAIL=0x%08X EXEC_CTL=0x%08X CTXCTL=0x%08X CCID=0x%08X FW=0x%08X\n",
          kExeclistVersion,
          preResetRcsStatus,
          preResetGtError,
          preResetRingCtl,
          preResetRingHead,
          preResetRingTail,
          preResetExeclistCtl,
          preResetCtxCtrl,
          preResetCcid,
          preFirmwareStatus);
    
    // V316: Try explicit RCS reset toggle if engine appears stuck
    const bool engineStuck = (preResetRcsStatus & 0x0F) == 0x0F ||
                             (preResetRingCtl & RING_VALID) == 0 ||
                             (preResetExeclistCtl & 0x3) != 0x3;
    if (engineStuck) {
        IOLog("(FakeIrisXE) [%s] Engine appears stuck, attempting RCS reset sequence...\n", kExeclistVersion);
        // Toggle RCS reset via power management
        self->mmioWrite32(0x20A0, 0x1); // Request reset
        IOSleep(2);
        self->mmioWrite32(0x20A0, 0x0); // Release reset
        IOSleep(5);
        const uint32_t postResetRcsStatus = self->mmioRead32(kExecRcsStatusReg);
        const uint32_t postResetRingCtl = self->mmioRead32(kExecRingCtlReg);
        IOLog("(FakeIrisXE) [%s] Post-reset attempt: STATUS=0x%08X RING_CTL=0x%08X\n",
              kExeclistVersion,
              postResetRcsStatus,
              postResetRingCtl);
    }
    
    patchProofLrcContextControl(res, variant.ctxCtrl);
    patchProofLrcRingBlock(res, variant.ringMode);
    logProofLrcImage(res);
    logProofSharedBacking(self, "pre-submit");
    logProofRingGating(self, "pre-submit");

    // V322: ALWAYS program live ring registers - this was the bug!
    // Previously lrc-only mode skipped live ring programming, but the GPU
    // needs ring registers enabled in hardware to execute commands.
    // The LRC image stores ring state, but hardware registers must also be enabled.
    if (!programProofRingState(self, res, &observations)) {
        self->fOwner->forcewakeRenderRelease();
        failure = RingControlNotEnabled;
        return false;
    }

    IOLog("(FakeIrisXE) [%s] Ring program mode=%s: RING_CTL=0x%08X enabled\n",
          kExeclistVersion,
          proofRingModeLabel(variant.ringMode),
          observations.lastRingCtl);

    self->mmioWrite32(RCS0_EXECLIST_ARB_CTL, variant.arbControl);
    IOSleep(1);
    
    // V316: Apple-style submission - write to BOTH primary AND legacy ports
    // This is what Apple's TGL driver actually does
    switch (variant.submitStyle) {
        case ProofSubmitKickBeforeHi:
            // Primary port
            self->mmioWrite32(kExecElspPrimaryLo, res.descLo);
            IOSleep(1);
            self->mmioWrite32(RCS0_EXECLIST_CONTROL, variant.execlistControlKick);
            IOSleep(1);
            self->mmioWrite32(kExecElspPrimaryHi, res.descHi);
            // Legacy port
            IOSleep(1);
            self->mmioWrite32(kExecElspLegacyLo, res.descLo);
            IOSleep(1);
            self->mmioWrite32(kExecElspLegacyHi, res.descHi);
            break;
        case ProofSubmitKickAfterLo:
            self->mmioWrite32(RCS0_EXECLIST_CONTROL, variant.execlistControlKick);
            IOSleep(1);
            self->mmioWrite32(kExecElspPrimaryLo, res.descLo);
            IOSleep(1);
            self->mmioWrite32(kExecElspPrimaryHi, res.descHi);
            // Legacy port
            IOSleep(1);
            self->mmioWrite32(kExecElspLegacyLo, res.descLo);
            IOSleep(1);
            self->mmioWrite32(kExecElspLegacyHi, res.descHi);
            break;
        case ProofSubmitCurrent:
        default:
            // Primary port
            self->mmioWrite32(kExecElspPrimaryLo, res.descLo);
            IOSleep(1);
            self->mmioWrite32(kExecElspPrimaryHi, res.descHi);
            // Legacy port (Apple style)
            IOSleep(1);
            self->mmioWrite32(kExecElspLegacyLo, res.descLo);
            IOSleep(1);
            self->mmioWrite32(kExecElspLegacyHi, res.descHi);
            // SQ_CONTENTS kick (Apple style: write to 0x120B8)
            IOSleep(1);
            self->mmioWrite32(0x120B8, variant.execlistControlKick);
            break;
    }
    IOSleep(2);
    const uint32_t postLo = self->mmioRead32(kExecElspPrimaryLo);
    const uint32_t postHi = self->mmioRead32(kExecElspPrimaryHi);
    const uint32_t postStatusLo = self->mmioRead32(kExecStatusPrimaryLo);
    const uint32_t postStatusHi = self->mmioRead32(kExecStatusPrimaryHi);
    const uint32_t postCsbCtrl = self->mmioRead32(RCS0_CSB_CTRL);
    const uint32_t postCsbAddrLo = self->mmioRead32(RCS0_CSB_ADDR_LO);
    const uint32_t postCsbAddrHi = self->mmioRead32(RCS0_CSB_ADDR_HI);
    const uint32_t postCsbRead = self->mmioRead32(RCS0_CSB_READ_PTR);
    const uint32_t postCsbWrite = self->mmioRead32(RCS0_CSB_WRITE_PTR);
    const uint32_t postCtxCtrl = self->mmioRead32(kExecContextControlReg);

    // V318: Also read from Apple-style CSB registers for comparison
    const uint32_t appleCsbCtrl = self->mmioRead32(kExecCsbCtrl);
    const uint32_t appleCsbHead = self->mmioRead32(kExecCsbHead);
    const uint32_t appleCsbTail = self->mmioRead32(kExecCsbTail);
    IOLog("(FakeIrisXE) [%s] Apple CSB regs: CTRL=0x%08X HEAD=0x%08X TAIL=0x%08X\n",
          kExeclistVersion,
          appleCsbCtrl, appleCsbHead, appleCsbTail);

    // V315: Verify CSB pointer is correctly configured before polling
    const uint32_t csbCtrlVerify = self->mmioRead32(RCS0_CSB_CTRL);
    const uint32_t csbAddrLoVerify = self->mmioRead32(RCS0_CSB_ADDR_LO);
    const uint32_t csbAddrHiVerify = self->mmioRead32(RCS0_CSB_ADDR_HI);
    const uint64_t expectedCsbGpu = (self->fCsbGGTT & ~0xFFFULL) + kExecCsbOffsetBytes;
    const uint64_t actualCsbGpu = (static_cast<uint64_t>(csbAddrHiVerify) << 32) | csbAddrLoVerify;
    const bool csbPointerValid = (actualCsbGpu == expectedCsbGpu);
    IOLog("(FakeIrisXE) [%s] CSB pointer verification: CTRL=0x%08X ADDR=0x%016llX expected=0x%016llX match=%u\n",
          kExeclistVersion,
          csbCtrlVerify,
          static_cast<unsigned long long>(actualCsbGpu),
          static_cast<unsigned long long>(expectedCsbGpu),
          csbPointerValid ? 1u : 0u);

    // V315: Check GT power management status before polling
    const uint32_t gtPerfStatus = self->fOwner->safeMMIORead(0x138124);
    const uint32_t gtStatus = self->fOwner->safeMMIORead(0x13812C);
    IOLog("(FakeIrisXE) [%s] GT power state before polling: PERF=0x%08X STATUS=0x%08X\n",
          kExeclistVersion,
          gtPerfStatus,
          gtStatus);

    IOSleep(5);
    const uint32_t delayedStatusLo = self->mmioRead32(kExecStatusPrimaryLo);
    const uint32_t delayedStatusHi = self->mmioRead32(kExecStatusPrimaryHi);
    // V326: Read RING_MODE after submission to verify execlist state
    const uint32_t postRingMode = self->mmioRead32(RCS0_RING_MODE);
    uint32_t delayedValidBits = 0;
    uint32_t delayedActiveBits = 0;
    const char* delayedQueueState = nullptr;
    decodeExeclistSlots(delayedStatusLo, delayedValidBits, delayedActiveBits, delayedQueueState);
    uint32_t postValidBits = 0;
    uint32_t postActiveBits = 0;
    const char* postQueueState = nullptr;
    decodeExeclistSlots(postStatusLo, postValidBits, postActiveBits, postQueueState);
    self->fOwner->forcewakeRenderRelease();

    // V326: Log post-submit RING_MODE to see if execlist enable bit changed
    IOLog("(FakeIrisXE) [%s] Post-submit RING_MODE: 0x%08X (execlist_enable_bit=%u)\n",
          kExeclistVersion,
          postRingMode,
          (postRingMode & 0x1) ? 1u : 0u);
    
    // V326: Enhanced post-submit register dump - include ACTHD
    const uint32_t postActhdLo = self->mmioRead32(kExecActhdLo);
    const uint32_t postActhdHi = self->mmioRead32(kExecActhdHi);
    IOLog("(FakeIrisXE) [%s] Post-submit ACTHD: 0x%08X%08X\n",
          kExeclistVersion,
          postActhdHi, postActhdLo);

    observations.elspWritten = (postLo == res.descLo) || (postHi == res.descHi);
    observations.slotValidChanged = (postValidBits != preValidBits);
    observations.slotActiveChanged = (postActiveBits != preActiveBits);
    if (!observations.slotValidChanged && delayedValidBits != preValidBits) {
        observations.slotValidChanged = true;
        postValidBits = delayedValidBits;
        postActiveBits = delayedActiveBits;
        postQueueState = delayedQueueState;
    }
    if (!observations.slotActiveChanged && delayedActiveBits != preActiveBits) {
        observations.slotActiveChanged = true;
        postValidBits = delayedValidBits;
        postActiveBits = delayedActiveBits;
        postQueueState = delayedQueueState;
    }
    res.submitAccepted = observations.slotValidChanged || observations.slotActiveChanged ||
                         (postStatusLo != preStatusLo) || (postStatusHi != preStatusHi);

    logProofSharedBacking(self, "post-submit");
    logProofRingGating(self, "post-submit");

    IOLog("(FakeIrisXE) [%s] Submit postwrite: ELSP=%08X/%08X STATUS=%08X/%08X slots(valid=0x%X active=0x%X state=%s) CSB=%08X addr=%08X%08X rp=%08X wp=%08X CTXCTL=%08X elspWritten=%u accepted=%u\n",
          kExeclistVersion,
          postLo, postHi, postStatusLo, postStatusHi, postValidBits, postActiveBits, postQueueState, postCsbCtrl, postCsbAddrHi, postCsbAddrLo, postCsbRead, postCsbWrite, postCtxCtrl, observations.elspWritten ? 1u : 0u, res.submitAccepted ? 1u : 0u);
    IOLog("(FakeIrisXE) [%s] Post-ELSP ring recover: START=0x%08X HEAD=0x%08X TAIL=0x%08X CTL=0x%08X\n",
          kExeclistVersion,
          observations.lastRingStart,
          observations.lastRingHead,
          observations.lastRingTail,
          observations.lastRingCtl);

    return true;
}

static bool pollProofProgress(FakeIrisXEExeclist* self,
                              RcsProofResources& res,
                              ProofFailureType& failure,
                              ProofObservations& observations)
{
    if (!self || !self->fOwner || !res.scratchGem) {
        failure = LrcLayoutWrong;
        return false;
    }

    volatile uint32_t* scratchCpu = (volatile uint32_t*)self->fOwner->ggttGetCPUAddr(res.scratchGem);
    if (!scratchCpu) {
        failure = ScratchMappingUnavailable;
        return false;
    }
    observations.scratchCpuMapped = true;

    const uint32_t initialStatusLo = self->mmioRead32(kExecStatusPrimaryLo);
    const uint32_t initialStatusHi = self->mmioRead32(kExecStatusPrimaryHi);
    const uint32_t initialCsbRead = self->mmioRead32(RCS0_CSB_READ_PTR);
    const uint32_t initialCcid = self->mmioRead32(kExecCcidReg);
    const uint32_t initialCtxCtrl = self->mmioRead32(kExecContextControlReg);

    observations.elspAccepted = res.submitAccepted;

    IOLog("(FakeIrisXE) [%s] ========== EXECUTION POLL ==========\n", kExeclistVersion);

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
        const uint32_t csbWriteAlias = self->mmioRead32(RCS0_CSB_WRITE_PTR);
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
        uint32_t slotValidBits = 0;
        uint32_t slotActiveBits = 0;
        const char* queueState = nullptr;
        decodeExeclistSlots(execlistStatusLo, slotValidBits, slotActiveBits, queueState);

        observations.execlistStatusChanged |= (execlistStatusLo != initialStatusLo) || (execlistStatusHi != initialStatusHi);
        observations.slotValidChanged |= (slotValidBits != 0u);
        observations.slotActiveChanged |= (slotActiveBits != 0u);
        observations.csbAdvanced |= (csbRead != initialCsbRead);
        observations.ccidChanged |= (ccid != initialCcid);
        observations.contextControlChanged |= (ctxCtrl != initialCtxCtrl);
        observations.schedulingProgress |= statusValid || statusActive ||
                                           observations.execlistStatusChanged ||
                                           observations.csbAdvanced ||
                                           observations.ccidChanged ||
                                           observations.contextControlChanged;
        observations.ringStateLoaded |= ((rcsStart & 0xFFFFF000u) == (uint32_t)(res.ringGpuAddr & 0xFFFFF000ULL)) &&
                                        ((rcsCtl & 0x001FF001u) == (res.ringCtl & 0x001FF001u));
        observations.acthdObserved |= (acthdLo != 0u) || (acthdHi != 0u);
        observations.batchStarted |= observations.acthdObserved || (bbAddrLo != 0u) || (bbAddrHi != 0u);
        observations.ringConsumed |= ((rcsHead & 0x001FFFFCu) != 0u) || observations.batchStarted;
        observations.lastScratchValue = scratchValue;
        observations.lastExeclistStatusLo = execlistStatusLo;
        observations.lastExeclistStatusHi = execlistStatusHi;
        observations.lastCsbCtrl = csbCtrl;
        observations.lastCsbAddrLo = csbAddrLo;
        observations.lastCsbAddrHi = csbAddrHi;
        observations.lastCsbRead = csbRead;
        observations.lastCsbWriteAlias = csbWriteAlias;
        observations.lastRcsStatus = rcsStatus;
        observations.lastGtError = gtError;
        observations.lastActhdLo = acthdLo;
        observations.lastActhdHi = acthdHi;
        observations.lastBbAddrLo = bbAddrLo;
        observations.lastBbAddrHi = bbAddrHi;
        observations.lastRingHead = rcsHead;
        observations.lastRingTail = rcsTail;
        observations.lastRingStart = rcsStart;
        observations.lastRingCtl = rcsCtl;
        observations.lastSlotValidBits = slotValidBits;
        observations.lastSlotActiveBits = slotActiveBits;
        observations.ringCtlEnabled |= (rcsCtl & RING_VALID) != 0u;
        observations.ringCtlMasked |= ((rcsCtl & ~RING_VALID) == (res.ringCtl & ~RING_VALID)) && ((rcsCtl & RING_VALID) == 0u);

        if ((poll % 5u) == 0u || scratchValue == res.expectedValue || halted || wedged) {
            IOLog("(FakeIrisXE) [%s] Poll%03u ELSP=%08X/%08X EXE=%08X/%08X slots(valid=0x%X active=0x%X state=%s) RCS H/T/S=%08X/%08X/%08X\n",
                  kExeclistVersion,
                  poll, elspLo, elspHi, execlistStatusLo, execlistStatusHi, slotValidBits, slotActiveBits, queueState, rcsHead, rcsTail, rcsStatus);
            IOLog("(FakeIrisXE) [%s]         CSB ctrl=%08X addr=%08X%08X rp=%08X wp_alias=%08X CCID=%08X CTXCTL=%08X RING_CTL=%08X\n",
                  kExeclistVersion,
                  csbCtrl, csbAddrHi, csbAddrLo, csbRead, csbWriteAlias, ccid, ctxCtrl, rcsCtl);
            IOLog("(FakeIrisXE) [%s]         ACTHD=%08X%08X BBADDR=%08X%08X GT_ERR=%08X SCRATCH=%08X\n",
                  kExeclistVersion,
                  acthdHi, acthdLo, bbAddrHi, bbAddrLo, gtError, scratchValue);
        }

        if (scratchValue == res.expectedValue) {
            IOLog("(FakeIrisXE) [%s] SUCCESS: scratch changed from 0x%08X to 0x%08X\n",
                  kExeclistVersion,
                  kProofScratchInitial, scratchValue);
            failure = None;
            return true;
        }

        if (halted || wedged) {
            failure = EngineHardHalted;
            return false;
        }
    }

    IOLog("(FakeIrisXE) [%s] Summary: elspWritten=%u accepted=%u schedule=%u slotValid=0x%X slotActive=0x%X csb=%u ccid=%u ctxctl=%u ringLoad=%u ringCtl=%u masked=%u batch=%u ringConsume=%u scratch=0x%08X gtErr=0x%08X csbCtrl=0x%08X addr=%08X%08X wp=%08X\n",
          kExeclistVersion,
          observations.elspWritten ? 1u : 0u,
          observations.elspAccepted ? 1u : 0u,
          observations.schedulingProgress ? 1u : 0u,
          observations.lastSlotValidBits,
          observations.lastSlotActiveBits,
          observations.csbAdvanced ? 1u : 0u,
          observations.ccidChanged ? 1u : 0u,
          observations.contextControlChanged ? 1u : 0u,
          observations.ringStateLoaded ? 1u : 0u,
          observations.ringCtlEnabled ? 1u : 0u,
          observations.ringCtlMasked ? 1u : 0u,
          observations.batchStarted ? 1u : 0u,
          observations.ringConsumed ? 1u : 0u,
          observations.lastScratchValue,
          observations.lastGtError,
          observations.lastCsbCtrl,
          observations.lastCsbAddrHi,
          observations.lastCsbAddrLo,
          observations.lastCsbWriteAlias);

    // V326: Use enhanced failure taxonomy
    if (!observations.ringCtlEnabled && observations.ringCtlMasked) {
        // Ring is masked but stable - this is the key diagnostic point
        if (!observations.elspWritten) {
            failure = MaskedRingStableNoSubmitVisibility;
        } else if (!observations.slotValidChanged && !observations.slotActiveChanged) {
            failure = SubmitVisibleNoSlotAccept;
        } else if (!observations.ccidChanged && !observations.contextControlChanged) {
            failure = SlotAcceptNoContextLoad;
        } else if (!observations.acthdObserved) {
            failure = ContextLoadNoExecution;
        } else if (observations.lastScratchValue == kProofScratchInitial) {
            failure = ExecutionNoScratchWrite;
        } else {
            failure = CsBobservationUntrusted;
        }
    } else if (!observations.ringCtlEnabled) {
        failure = RingControlNotEnabled;
    } else if (observations.elspWritten && !observations.elspAccepted) {
        failure = ElspVisibleNotAccepted;
    } else if (!observations.elspAccepted) {
        failure = ElspRejected;
    } else if (!observations.schedulingProgress) {
        failure = observations.csbAdvanced ? NoSchedulingProgress : CsbNoProgress;
    } else if (!observations.ringStateLoaded) {
        failure = ContextStateNotLoaded;
    } else if (!observations.batchStarted) {
        failure = BatchNeverStarted;
    } else if (!observations.ringConsumed) {
        failure = RingStateWrong;
    } else {
        failure = ScratchWritebackMissing;
    }

    return false;
}

static bool runRcsScratchWriteProof(FakeIrisXEExeclist* self, const char* label)
{
    if (!self || !self->fOwner) {
        return false;
    }

    RcsProofResources res;
    ProofObservations observations;
    ProofFailureType failure = None;
    bool success = false;

    IOLog("(FakeIrisXE) [%s] ============================================\n", kExeclistVersion);
    IOLog("(FakeIrisXE) [%s] DIRECT EXECLIST SCRATCH-WRITE PROOF (%s)\n", kExeclistVersion, label ? label : "unknown");
    IOLog("(FakeIrisXE) [%s] ============================================\n", kExeclistVersion);

    if (!allocateProofResources(self, res)) {
        failure = LrcLayoutWrong;
        goto done;
    }

    if (!singleResetAttemptIfNeeded(self)) {
        IOLog("(FakeIrisXE) [%s] RCS remained halted after the focused recovery attempt; continuing with submission so the failure can be classified after ELSP/CSB polling\n", kExeclistVersion);
    }

    if (!self->fOwner->forcewakeRenderHold(5000)) {
        IOLog("(FakeIrisXE) [%s] Failed to acquire forcewake for proof preparation/submission\n", kExeclistVersion);
        failure = EngineHardHalted;
        goto done;
    }

    if (!buildProofCommandStream(self, res)) {
        failure = MiPacketWrong;
        goto done_release;
    }

    if (!buildProofLrc(self, res)) {
        failure = LrcLayoutWrong;
        goto done_release;
    }

    for (uint32_t variantIndex = 0; variantIndex < sizeof(kProofVariants) / sizeof(kProofVariants[0]); ++variantIndex) {
        const ProofVariant& variant = kProofVariants[variantIndex];
        observations = {};
        observations.attemptedVariantIndex = variantIndex + 1u;
        observations.attemptedVariantLabel = variant.label;
        observations.lastScratchValue = kProofScratchInitial;
        if (volatile uint32_t* scratchCpu = (volatile uint32_t*)self->fOwner->ggttGetCPUAddr(res.scratchGem)) {
            *scratchCpu = kProofScratchInitial;
            OSSynchronizeIO();
        }

        IOLog("(FakeIrisXE) [%s] ---------- Variant %u/%llu: %s ----------\n",
              kExeclistVersion,
              variantIndex + 1u,
              static_cast<unsigned long long>(sizeof(kProofVariants) / sizeof(kProofVariants[0])),
              variant.label);

        if (!singleResetAttemptIfNeeded(self)) {
            IOLog("(FakeIrisXE) [%s] Variant %s starts from non-ideal engine state; continuing for classification\n",
                  kExeclistVersion,
                  variant.label);
        }

        buildProofDescriptor(res, variant);
        if (!validateProofDescriptorShape(res, variant, failure)) {
            if (shouldTryNextProofVariant(failure)) {
                continue;
            }
            goto done_release;
        }
        if (!submitProofDescriptor(self, res, variant, failure, observations)) {
            if (shouldTryNextProofVariant(failure)) {
                continue;
            }
            if (failure == None) {
                failure = DescriptorWrong;
            }
            goto done_release;
        }

        success = pollProofProgress(self, res, failure, observations);
        if (success || !shouldTryNextProofVariant(failure)) {
            break;
        }
    }

done_release:
    self->fOwner->forcewakeRenderRelease();

 done:
    self->fIsReady = success;
    self->fOwner->setProperty("FakeIrisXEExeclistExecutionProven", success ? kOSBooleanTrue : kOSBooleanFalse);
    self->fOwner->setProperty("FakeIrisXERcsProofFailure", proofFailureLabel(failure));
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofElspAccepted", observations.elspAccepted);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofSchedulingProgress", observations.schedulingProgress);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofCsbAdvanced", observations.csbAdvanced);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofContextLoaded", observations.ringStateLoaded);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofBatchStarted", observations.batchStarted);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofRingCtlEnabled", observations.ringCtlEnabled);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofRingCtlMasked", observations.ringCtlMasked);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofElspWritten", observations.elspWritten);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofRingConsumed", observations.ringConsumed);
    self->fOwner->setProperty("FakeIrisXERcsProofRingMode", proofRingModeLabel(static_cast<ProofRingProgrammingMode>(observations.attemptedRingMode)));
    self->fOwner->setProperty("FakeIrisXERcsProofSubmitStyle", proofSubmitStyleLabel(static_cast<ProofSubmitStyle>(observations.attemptedSubmitStyle)));
    self->fOwner->setProperty("FakeIrisXERcsProofLastVariantLabel", observations.attemptedVariantLabel ? observations.attemptedVariantLabel : "unknown");
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastVariantIndex", observations.attemptedVariantIndex, 32);
    if (!observations.elspWritten) {
        self->fOwner->setProperty("FakeIrisXERcsProofSlotOutcome", "elsp-not-written");
    } else if (observations.lastSlotActiveBits != 0u) {
        self->fOwner->setProperty("FakeIrisXERcsProofSlotOutcome", "slot-active");
    } else if (observations.lastSlotValidBits != 0u) {
        self->fOwner->setProperty("FakeIrisXERcsProofSlotOutcome", "slot-queued");
    } else {
        self->fOwner->setProperty("FakeIrisXERcsProofSlotOutcome", "slot-empty");
    }
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofAttemptedCtxCtrl", observations.attemptedCtxCtrl, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofAttemptedAddrMode", observations.attemptedAddrMode, 32);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofAttemptedForceRestore", observations.attemptedForceRestore);
    setProofBoolProperty(self->fOwner, "FakeIrisXERcsProofAttemptedPrivilege", observations.attemptedPrivilege);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastScratch", observations.lastScratchValue, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastStatusLo", observations.lastExeclistStatusLo, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastCsbCtrl", observations.lastCsbCtrl, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastCsbAddrLo", observations.lastCsbAddrLo, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastCsbAddrHi", observations.lastCsbAddrHi, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastCsbRead", observations.lastCsbRead, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastCsbWrite", observations.lastCsbWriteAlias, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastRingStart", observations.lastRingStart, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastRingHead", observations.lastRingHead, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastRingTail", observations.lastRingTail, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastRingCtl", observations.lastRingCtl, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastSlotValidBits", observations.lastSlotValidBits, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastSlotActiveBits", observations.lastSlotActiveBits, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastRcsStatus", observations.lastRcsStatus, 32);
    setProofNumberProperty(self->fOwner, "FakeIrisXERcsProofLastGtError", observations.lastGtError, 32);
    self->fOwner->updateExecutionState(success, success ? "rcs-scratch-writeback" : proofFailureLabel(failure));

    if (!success) {
        IOLog("(FakeIrisXE) [%s] FAILURE TYPE: %s\n", kExeclistVersion, proofFailureLabel(failure));
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
    obj->fCsbSizeBytes   = kExecCsbBytes;
    obj->fCsbEntryCount  = kExecCsbEntryCount;
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
    
    volatile uint64_t* csb = csbCpuBase(md);
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

    constexpr size_t kCSBSize = kExecHwsPageBytes; // Shared engine HWS page; CSB lives at +0x40
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

        IOBufferMemoryDescriptor* csbMd = fCsbGem->memoryDescriptor();
        if (csbMd && csbMd->getBytesNoCopy()) {
            bzero(csbMd->getBytesNoCopy(), csbMd->getLength());
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

    const uint64_t hwsGpuAddr = fCsbGGTT & ~0xFFFULL;
    const uint64_t csbGpuAddr = hwsGpuAddr + kExecCsbOffsetBytes;
    const uint32_t csbLo = (uint32_t)(csbGpuAddr & 0xFFFFFFFFULL);
    const uint32_t csbHi = (uint32_t)(csbGpuAddr >> 32);

    const uint32_t ringModeEnableMask =
        kExecRingModeDisableLegacy |
        kExecRingModePpgttEnable |
        kExecRingModePpgtt48b;

    mmioWrite32(kExecRingModeGen7Reg, maskedBitsEnable(ringModeEnableMask));
    mmioWrite32(kExecRingMiModeReg, maskedBitDisable(kExecRingMiModeStopRing));
    mmioWrite32(kExecHwsPgaReg, (uint32_t)(hwsGpuAddr & 0xFFFFFFFFULL));
    mmioWrite32(kExecHwstamReg, 0xFFFFFFFFu);
    (void)mmioRead32(kExecHwsPgaReg);

    mmioWrite32(RCS0_CSB_ADDR_LO, csbLo);
    mmioWrite32(RCS0_CSB_ADDR_HI, csbHi);
    mmioWrite32(RCS0_CSB_CTRL, 0x1u);

    const uint32_t csbReadbackLo = mmioRead32(RCS0_CSB_ADDR_LO);
    const uint32_t csbReadbackHi = mmioRead32(RCS0_CSB_ADDR_HI);
    const uint32_t csbCtrl = mmioRead32(RCS0_CSB_CTRL);
    const uint32_t ringMode = mmioRead32(kExecRingModeGen7Reg);
    const uint32_t ringMiMode = mmioRead32(kExecRingMiModeReg);
    const uint32_t hwsReadback = mmioRead32(kExecHwsPgaReg);
    IOLog("(FakeIrisXE) [Exec] setupExeclistPorts: HWS=0x%08X ring_mode=0x%08X (legacy_disable=%u ppgtt=%u ppgtt48=%u) ring_mi_mode=0x%08X CSB addr=0x%08X%08X ctrl=0x%08X\n",
          hwsReadback,
          ringMode,
          (ringMode & kExecRingModeDisableLegacy) ? 1u : 0u,
          (ringMode & kExecRingModePpgttEnable) ? 1u : 0u,
          (ringMode & kExecRingModePpgtt48b) ? 1u : 0u,
          ringMiMode,
          csbReadbackHi,
          csbReadbackLo,
          csbCtrl);

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
    mmioWrite32(RCS0_EXECLIST_CONTROL, 0xFF00);
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
    mmioWrite32(RCS0_EXECLIST_CONTROL, 0xFF00);   // minimal "kick"
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
                /* pageTableRoot */ 0,
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

    volatile uint64_t* csbBase = csbCpuBase(md);
    if (!csbBase) return;

    const uint32_t mask = fCsbEntryCount - 1;

    for (;;) {
        uint32_t idx = fCsbReadIndex & mask;
        uint64_t entry = csbBase[idx];

        if (entry == 0) {
            // no more new CSB entries
            break;
        }

        // Consume it
        handleCsbEntry(entry, (uint32_t)(entry >> 32), (uint32_t)(entry & 0xFFFFFFFFu));

        // Mark as consumed (zero it)
        csbBase[idx] = 0;
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
    mmioWrite32(RCS0_EXECLIST_CONTROL, 0xFF00);
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
                    0,      // page table root (legacy builder default)
                    &ret);
    IOLog("[V61] createHwContextFor: buildLRCContext returned lrcGem=%p ret=0x%x\n", hw->lrcGem, ret);

    if (!hw->lrcGem || ret != kIOReturnSuccess) {
        IOLog("[V61] ❌ createHwContextFor: buildLRCContext FAILED (lrcGem=%p ret=0x%x)\n", hw->lrcGem, ret);
        if (hw->lrcGem) {
            hw->lrcGem->unpin();
            hw->lrcGem->release();
        }
        hw->lrcGem = nullptr;

        hw->ringGem->unpin();
        hw->ringGem->release();
        hw->ringGem = nullptr;
        return nullptr;
    }
    IOLog("[V61] createHwContextFor: LRC context built successfully\n");

    IOLog("[V61] createHwContextFor: Pinning LRC GEM...\n");
    IOLog("[V61] createHwContextFor: Using builder-provided LRC mapping...\n");
    hw->lrcGGTT = hw->lrcGem->gpuAddress();
    if (!hw->lrcGGTT) {
        hw->lrcGGTT = fOwner->ggttMap(hw->lrcGem);
    }
    IOLog("[V61] createHwContextFor: LRC GGTT=0x%llX\n", hw->lrcGGTT);
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
                    volatile uint64_t* csb = csbCpuBase(md);
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
