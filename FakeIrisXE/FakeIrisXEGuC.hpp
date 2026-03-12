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
    bool prepareAppleWopcm(GuCStage stage, uint32_t desiredSizeValue, uint32_t desiredOffsetValue);
    bool runLinuxBringUpPath(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr,
                             uint32_t retryIndex, uint64_t startNs);
    bool runAppleBringUpPath(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr,
                             uint32_t retryIndex, uint64_t startNs);
    bool pollForBootFastFail(uint32_t timeoutMs, uint64_t startNs, uint32_t retryIndex);

    bool writeRegWithReadback(GuCStage stage, const char* regName,
                              uint32_t reg, uint32_t value, uint32_t* outReadback = nullptr);
    void emitStageReport(GuCStage stage, uint64_t startNs, uint32_t retryIndex,
                         uint32_t rawStatusOverride = 0xFFFFFFFFU);
    GuCStatusDecoded decodeStatus(uint32_t rawStatus) const;
    bool isImpossibleStatusDecode(uint32_t rawStatus, const GuCStatusDecoded& decoded) const;
    uint32_t selectGtPmConfigReg() const;
    bool ownerBooleanPropertyEnabled(const char* key) const;
    void logForceWakeDiagnostics(const char* label) const;
    void logAppleBootAudit(const char* label) const;
    void logAppleRegisterWindow(const char* label) const;
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
