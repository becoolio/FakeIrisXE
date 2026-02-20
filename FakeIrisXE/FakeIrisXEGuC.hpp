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
    enum HwCsType : uint8_t {
        kHwCsRCS = 0,
        kHwCsCCS = 1,
        kHwCsBCS = 2,
        kHwCsVCS0 = 3,
        kHwCsVCS2 = 4,
        kHwCsVECS0 = 5,
        kHwCsCount = 6
    };

    enum GuCBootStage : uint32_t {
        kGuCBootStagePrepare = 0,
        kGuCBootStageDma = 1,
        kGuCBootStageAuth = 2,
        kGuCBootStageBootWait = 3,
        kGuCBootStageRunning = 4
    };

    enum GuCTimeoutCode : uint32_t {
        kGuCTimeoutNone = 0x0000,
        kGuCTimeoutPrepare = 0x1501,
        kGuCTimeoutDma = 0x1502,
        kGuCTimeoutAuth = 0x1503,
        kGuCTimeoutBootWait = 0x1504
    };

    struct HwCsDesc {
        HwCsType type;
        const char* name;
        uint8_t ukEngineClass;
        uint32_t submitPresenceMask;
        uint32_t mediaFuseBit;
    };

    FakeIrisXEFramebuffer* fOwner;
    FakeIrisXEGEM* fGuCFwGem;
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

    // V149: Single-attempt guard for deterministic bring-up traces
    bool fGuCFirmwareLoadAttempted;
    bool fGuCFirmwareLoadResult;

    // V152: Engine topology and GuC boot stage tracking
    uint32_t fEngineAvailabilityMask;
    GuCBootStage fCurrentGuCBootStage;
    uint32_t fLastGuCStatus;
    uint32_t fLastGuCTimeoutCode;
    
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
    
    // V141: New i915-correct GuC load method
    bool guc_load_fw_v141(const uint8_t* fw, size_t fwLen, uint64_t fwGgttAddr);
    
    // V142: Apple TGL GuC firmware format support
    struct AppleTGLSecurityBlock {
        uint8_t data[256];  // 256-byte security block (SB)
    };
    
    struct AppleTGLCssHeader {
        uint32_t module_type;       // 0x00000006 at offset 0x100
        uint32_t header_len;        // 0xA1 (161 bytes)
        uint32_t header_version;    // 0x10000
        uint32_t module_id;
        uint32_t module_vendor;     // 0x8086 (Intel)
        uint32_t date;
        uint32_t size;              // Total module size
        uint32_t key_size;
        uint32_t modulus_size;
        uint32_t exponent_size;
        uint32_t reserved[22];
    } __attribute__((packed));
    
    struct AppleTGLFirmwareLayout {
        uint32_t sb_offset;         // 0x00
        uint32_t sb_size;           // 0x100 (256 bytes)
        uint32_t css_offset;        // 0x100
        uint32_t css_size;          // From CSS header_len
        uint32_t payload_offset;    // 0x100 + css_size
        uint32_t payload_size;      // Remaining firmware
    };
    
    bool parseAppleTglFirmware(const uint8_t* fwData, size_t fwSize, AppleTGLFirmwareLayout& layout);
    bool loadAppleTglRSAScratch(const uint8_t* fwData, const AppleTGLFirmwareLayout& layout);
    bool loadGuCWithAppleTglMethod(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr);
    bool preflightAppleTglLoad(const uint8_t* fwData, size_t fwSize, const AppleTGLFirmwareLayout& layout);
    void classifyAppleTglFailure(const char* stage, uint32_t status);
    void resetGucForRetry();
    void dumpGuCDiagnostics(const char* stage);

    // V152: TGL-style engine descriptor table and dynamic availability mask
    uint32_t computeEngineAvailabilityMask(uint32_t mediaFuseMask) const;
    uint8_t getUkEngineClassForType(HwCsType type) const;
    bool isEngineAvailable(HwCsType type, uint32_t availabilityMask) const;
    void exportEngineAvailabilityToRegistry(uint32_t availabilityMask);
    void dumpV152BringupSummary(const char* reason,
                                uint32_t submitPresenceMask,
                                uint32_t gpuSku,
                                uint32_t sliceCount,
                                uint32_t subsliceCount,
                                uint32_t euCount,
                                uint32_t vdboxCount,
                                uint32_t veboxCount) const;

    // V152: Explicit GuC stage markers + timeout codes
    void setGuCBootStage(GuCBootStage stage, uint32_t status,
                         uint32_t timeoutCode = kGuCTimeoutNone,
                         const char* detail = nullptr);
};
