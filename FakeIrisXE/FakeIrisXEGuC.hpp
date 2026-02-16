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
};
