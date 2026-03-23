//
//  FakeIrisXEGuC.hpp
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 03/12/25.
//

// FakeIrisXEGuC.hpp
#pragma once

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXEFramebuffer.hpp"


class FakeIrisXEGuC : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEGuC);
    
private:
    FakeIrisXEFramebuffer* fOwner;
    FakeIrisXEGEM* fGuCFwGem;
    FakeIrisXEGEM* fGuCPublicKeyGem;
    FakeIrisXEGEM* fHuCFwGem;
    FakeIrisXEGEM* fDmcFwGem;
    
    // Firmware versions
    uint32_t fGuCVersion;
    uint32_t fHuCVersion;
    uint32_t fDmcVersion;
    
    // GuC log buffer
    FakeIrisXEGEM* fGuCLogGem;
    uint32_t fGuCLogSize;
    
    // V250: GuC CTB (Command Transport Buffer) GEM objects
    FakeIrisXEGEM* fH2GDbGem;
    FakeIrisXEGEM* fH2GCtbGem;
    FakeIrisXEGEM* fG2HDbGem;
    FakeIrisXEGEM* fG2HCtbGem;
    
    // V250: Cached GPU VAs for CTB registers
    uint64_t fH2GDbGpuVA;
    uint64_t fH2GCtbGpuVA;
    uint64_t fG2HDbGpuVA;
    uint64_t fG2HCtbGpuVA;
    
    // V50: Mode tracking
    bool fGuCMode;  // true = GuC submission, false = Execlist fallback

    enum GuCStage {
        kGuCStageIdle = 0,
        kGuCStageForceWake,
        kGuCStageShim,
        kGuCStageWopcm,
        kGuCStageDmaProgram,
        kGuCStageDmaTrigger,
        kGuCStageBootPoll,
        kGuCStageBootSuccess,
        kGuCStageFailure,
    };

    struct GuCStatusDecoded {
        uint8_t bootrom;
        uint8_t ukernel;
        uint8_t mia;
        uint8_t authStatus;
        bool valid;
        bool success;
        bool failure;
    };

    struct GuCStageReport {
        GuCStage stage;
        uint64_t elapsed_us;
        uint32_t raw_status;
        GuCStatusDecoded decoded_status;
        uint32_t retry_index;
    };

    enum GuCFirmwareMode {
        kGuCFirmwareModeAppleOnly = 0,
        kGuCFirmwareModeLinuxReserved,
    };

    GuCStage fLastReportedStage;
    GuCFirmwareMode fFirmwareMode;
    
public:
    static FakeIrisXEGuC* withOwner(FakeIrisXEFramebuffer* owner);
    
    // Initialization
    bool initGuC();
    bool loadGuCFirmware(const uint8_t* fwData, size_t fwSize);
    bool loadHuCFirmware(const uint8_t* fwData, size_t fwSize);
    bool loadDmcFirmware(const uint8_t* fwData, size_t fwSize);
    
    // Enable/Disable
    bool enableGuCSubmission();
    bool disableGuC();
    
    // Submission
    bool submitToGuC(FakeIrisXEGEM* batchGem, uint64_t* outFence);
    
    // Status
    bool isGuCReady();
    void dumpGuCStatus();
    
    // V47: Command submission test
    bool testCommandSubmission();
    
private:
    bool bootGuCFirmware(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr);
    GuCFirmwareMode selectFirmwareMode() const;
    const char* firmwareModeName(GuCFirmwareMode mode) const;
    const char* authStatusName(uint8_t authStatus) const;
    void logBootFailureSignature(const char* reason, uint64_t startNs, uint32_t retryIndex,
                                 uint32_t rawStatusOverride = 0xFFFFFFFFU);
    bool runLinuxBringUpPath(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr,
                             uint32_t retryIndex, uint64_t startNs);
    bool runMinimalBringUpPath(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr,
                              uint32_t retryIndex, uint64_t startNs);
    bool pollForBootFastFail(uint32_t timeoutMs, uint64_t startNs, uint32_t retryIndex);
    void dumpGucMapProbe(const char* tag);

    bool writeRegWithReadback(GuCStage stage, const char* regName,
                              uint32_t reg, uint32_t value, uint32_t* outReadback = nullptr);
    void emitStageReport(GuCStage stage, uint64_t startNs, uint32_t retryIndex,
                         uint32_t rawStatusOverride = 0xFFFFFFFFU);
    GuCStatusDecoded decodeStatus(uint32_t rawStatus) const;
    bool isImpossibleStatusDecode(uint32_t rawStatus, const GuCStatusDecoded& decoded) const;
    uint32_t selectGtPmConfigReg() const;
    bool ownerBooleanPropertyEnabled(const char* key) const;
    void logForceWakeDiagnostics(const char* label) const;
    bool ensureApplePublicKeyBlob(uint64_t* outGpuAddr, bool logBlob);
    bool writeAndPollAppleReg(GuCStage stage, const char* label, uint32_t writeReg,
                              uint32_t writeValue, uint32_t pollReg, uint32_t pollMask,
                              uint32_t expectedValue, uint32_t timeoutMs,
                              uint32_t* outPollValue = nullptr);
    bool pollAppleRegEquals(GuCStage stage, const char* label, uint32_t reg,
                            uint32_t expectedValue, uint32_t timeoutMs,
                            uint32_t* outValue = nullptr);
    bool safeForceWakeDomain(GuCStage stage, const char* label, uint32_t requestReg,
                             uint32_t ackReg, uint32_t requestValue,
                             uint32_t ackMask, uint32_t expectedAckValue);
    bool acquireAppleWakeDomains(GuCStage stage);
    bool runApplePreAuthHandshake(GuCStage stage, uint32_t restoreFreqToken);
    bool writeAppleBootParams(GuCStage stage);
    void issueGuCTlbInvalidate() const;
    void logDoorbellSnapshot(const char* label) const;
    bool programDoorbellEnable(GuCStage stage);
    void producerCoherencyBarrier(const char* reason);
    void consumerCoherencyBarrier(const char* reason);
    void logLinuxBaselineCorrelation(bool bootSuccess);

    // V52.1: ForceWake helpers (matching Apple's SafeForceWake)
    bool acquireForceWake();
    void releaseForceWake();
    
    // V52.1: RSA/Signature data extraction from firmware
    bool extractRSASignature(const uint8_t* fwData, size_t fwSize, uint8_t* signatureOut);
    
    // V56: Program GUC_SHIM_CONTROL (required before DMA per Linux i915)
    void programShimControl();
    
    // V52.1/V56: Apple-style GuC initialization (called before DMA)
    bool initGuCForAppleDMA(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr);
   // bool setupGuCInterrupts();

    bool waitGuCReady(uint32_t timeoutMs = 5000);
    bool uploadFirmware(FakeIrisXEGEM* fwGem, uint32_t fwType);
    
    // V51: Linux-style DMA firmware upload (per Intel i915 driver)
    bool uploadFirmwareViaDMA(uint64_t sourceGpuAddr, uint32_t destOffset, 
                              size_t fwSize, uint32_t dmaFlags);
    
    // V52: Apple-style DMA firmware upload (from mac-gfx-research)
    bool uploadFirmwareViaDMA_Apple(uint64_t sourceGpuAddr, uint32_t destOffset, 
                                    size_t fwSize);
    
    // V52: Unified upload with fallback (Linux first, then Apple)
    bool uploadFirmwareWithFallback(uint64_t sourceGpuAddr, uint32_t destOffset, 
                                    size_t fwSize);
    
    // V53: Enhanced HuC loading with DMA
    bool loadHuCFirmwareWithDMA(const uint8_t* fwData, size_t fwSize);
    
    // V133: RPS/Frequency control for Execlist optimization
    void configureRPS();
    
    // V133: MMIO-based firmware loading (bypass DMA)
    bool loadFirmwareViaMMIO(uint64_t sourceGpuAddr, uint32_t destOffset, size_t fwSize);
    
    // V53: Doorbell initialization (for GuC submission)
    bool initDoorbells();
    
    // V53: Command Transport Buffer setup (CTB for H2G/G2H communication)
    bool initCommandTransportBuffers();
    
    // V53: Full GuC subsystem initialization
    bool initGuCSubsystem();
    
    // V135: Aggressive Linux GT initialization before GuC load
    void initGTPreWorkaround();

    // V214: 10 Linux i915 GPU Improvements
    void initV214Improvements();

    // V215: Additional GT Recovery and Engine Fixes
    void initV215Improvements();

    // V216: Fix Clock Gating Registers and More Aggressive RCS Enable
    void initV216Improvements();

    // V217: Aggressive Power Management + Different RCS Bases
    void initV217Improvements();

    // V218: 10 Parallel Linux i915 Gen12 Improvements
    void initV218Improvements();

    // V219: RCS Active Mode Fix
    void initV219RCSFix();

    // V221: RCS EXEClist Initialization with MI_STORE_DWORD_IMM Proof-of-Execution
    void initV221RCSExeclist();

    // V248: BCS0 Blitter Pipeline Initialization for Display Scanout
    // BCS0 (Blitter Command Streamer 0) is responsible for 2D operations including
    // surface-to-surface copy (used for compositing and display scanout).
    // This function initializes BCS0 with a ring buffer, LRC context, and EXEClist
    // so that GPU-accelerated 2D blits can be submitted for display operations.
    void initBCS0Pipeline();
    
    // V232: Early Power Well Initialization - BEFORE GT gets wedged
    void initV232EarlyPowerWells();
    
    // V233: 10 Parallel Improvements (Based on Linux i915 + DTK Research)
    void initV233AllImprovements();
    
    // V234: ForceWake Retry + VPU Power + Aggressive Reset
    void initV234AggressiveInit();
    
    // V235: 10 More Parallel Improvements (GMCH/L3/CDCLK/etc)
    void initV235MoreImprovements();
    
    // V236: Critical Pre-Init (PMC/ForceWake/Interrupts/DMI)
    void initV236CriticalPreInit();
    
    // V236 Individual improvements (private helpers)
    void initV236PMC();
    void initV236ForceWakeDomains();
    void initV236GTInterrupts();
    void initV236GEMFault();
    void initV236DMI();
    
    // V235 Individual improvements (private helpers)
    void initV235GMCH();
    void initV235L3Cache();
    void initV235CDCLK();
    void initV235PCIeASPM();
    void initV235DDB();
    void initV235GSC();
    void initV235TimerFreq();
    void initV235MediaClock();
    void initV235PCIeDebug();
    void initV235RPS();
    
    // V233 Individual improvements (private helpers)
    void initV233MOCS();
    void initV233ClockGating();
    void initV233VDENPower();
    void initV233DMCVerification();
    void initV233GTInterrupts();
    void initV233SAGV();
    void initV233PPGTT();
    void initV233DisplayPower();
    void initV233GuCAuthVerify();
    void initV233RC6Control();
    
    // V221 Helper Functions - Gen12 RCS LRC + EXEClist Path
    void dumpRcsStateBeforeInit(const char* label);
    bool tryRcsRecoveryPath();
    struct RcsExeclistResources {
        FakeIrisXEGEM* ringGem;
        FakeIrisXEGEM* lrcGem;
        FakeIrisXEGEM* scratchGem;
        uint64_t ringGpuAddr;
        uint64_t lrcGpuAddr;
        uint64_t scratchGpuAddr;
        size_t ringSize;
        uint32_t lrcTailUpdate;  // V241: Ring tail byte offset for LRC
    };
    
    // V230: Context switching support - track multiple contexts
    struct ExeclistContext {
        uint64_t contextDescriptor;
        uint64_t lrcGpuAddr;
        FakeIrisXEGEM* lrcGem;
        bool submitted;
        bool completed;
    };
    
    enum { MAX_CONTEXTS = 4 };
    struct ExeclistContextQueue {
        ExeclistContext contexts[4];
        int count;
        int current;
    };
    
    ExeclistContextQueue fContextQueue;
    
    bool queueRcsContext(uint64_t contextDescriptor, uint64_t lrcGpuAddr, FakeIrisXEGEM* lrcGem);
    bool submitNextContext();
    bool submitContextPair(uint64_t ctxDesc0, uint64_t ctxDesc1);
    void dumpContextQueue();
    bool allocateRcsExeclistResources(RcsExeclistResources& res);
    bool buildGen12RcsLrc(RcsExeclistResources& res, uint32_t ringTailBytes = 0);  // V241: Added ringTailBytes
    uint64_t buildRcsContextDescriptor(uint64_t lrcGpuAddr, uint32_t lrcPages);
    bool submitRcsExeclistContext(uint64_t ctxDescLo, uint64_t ctxDescHi);
    bool pollRcsExeclistProgress(uint32_t timeoutMs, RcsExeclistResources& res, uint32_t expectedValue);
    bool executeRcsTestBatch(RcsExeclistResources& res);
    
    // V246: New helper functions with full diagnostics
    uint64_t buildRcsContextDescriptorV246(uint64_t lrcGpuAddr, uint32_t lrcPages);
    bool buildGen12RcsLrcV246(RcsExeclistResources& res, uint32_t ringTailBytes);
    bool verifyMiStoreDwordImmPacket(RcsExeclistResources& res);
    
    // Internal helper
    void dumpRcsStateAfterRecovery(const char* label);

    // V137: Correct Tiger Lake GuC loading methods
    bool deriveLayoutFromCSS(const uint8_t* fwData, size_t fwSize,
                             size_t* outPayloadOffset, size_t* outPayloadSize);
    bool programWopcmForTgl(uint32_t wopcmSize, uint32_t wopcmOffset);
    bool writeRsaScratch(const uint8_t* fwData, size_t fwSize);
    bool dmaCopyGttToWopcm(uint64_t sourceGpuAddr, uint32_t destOffset, size_t fwSize);
    bool waitForGucBoot(uint32_t timeoutMs = 5000);
    bool loadGuCWithV137Method(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr);
    
    // V138: Fixed WOPCM configuration
    bool guclResetForWopcmV138();
    bool programWopcmForTglV138(uint32_t wopcmSize, uint32_t wopcmOffset);
    bool loadGuCWithV138Method(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr);
    
    // V139: Enhanced diagnostics
    void dumpDmaRegs(const char* label) const;
    void dumpWopcmRegs(const char* label) const;
    void dumpGuCStatusEx(const char* label) const;
    
    // V139: Strict i915 method
    struct GuCFwLayout {
        uint32_t header_offset;
        uint32_t header_size;
        uint32_t ucode_offset;
        uint32_t ucode_size;
        uint32_t rsa_offset;
        uint32_t rsa_size;
        uint32_t dma_copy_size;
    };
    bool parseGuCFirmwareV139(const uint8_t* fwData, size_t fwSize, GuCFwLayout& layout);
    bool writeRsaScratchV139(const uint8_t* fwData, const GuCFwLayout& layout);
    bool dmaCopyHeaderUcodeToWopcmV139(uint64_t fwGgttAddr, const GuCFwLayout& layout);
    bool loadGuCWithV139Method(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr);
    
    // V143: GUC params
    void writeGuCParams();
};
