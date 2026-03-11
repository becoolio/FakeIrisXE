#ifndef _SAFE_REGISTER_ACCESS_HPP_
#define _SAFE_REGISTER_ACCESS_HPP_

#include <stdint.h>
#include <libkern/libkern.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

namespace SafeRegisterAccess {

// Simple power domain determination for Tiger Lake registers
// Based on observations from DTK and Linux i915 driver
static inline uint32_t determinePowerDomainForOffset(uint32_t offset) {
    // GuC registers (0xC000+) - GT domain
    if (offset >= 0xC000 && offset < 0xD000) {
        return 0; // GT domain
    }
    
    // ME/Tiger Lake specific registers
    if (offset >= 0x9400 && offset < 0x9500) {
        return 0; // GT domain (ME registers)
    }
    
    if (offset >= 0xC0F0 && offset < 0xC100) {
        return 0; // GT domain (ME wake registers)
    }
    
    // Render/GT general registers
    if (offset >= 0xA000 && offset < 0xA200) {
        if (offset == 0xA188) { // FORCEWAKE_RENDER_CTL
            return 1; // Render domain
        }
        return 0; // GT domain
    }
    
    // GT power management and context registers
    if (offset >= 0x138000 && offset < 0x138200) {
        return 0; // GT domain
    }
    
    // Media registers
    if (offset >= 0x130000 && offset < 0x130100) {
        // Simplified: treat all media as GT domain for now
        // In a full implementation, we'd distinguish VDBOX/VEBOX
        return 0; // GT domain
    }
    
    // Default to GT domain for unknown registers
    return 0;
}

// Simple forcewake domain management
// This is a simplified version that avoids complex refcounting for initial implementation
static inline bool simpleForceWakeAcquire(uint32_t domain, volatile void* mmioBase) {
    if (!mmioBase) return false;
    
    uint32_t fw_req = 0;
    uint32_t fw_enable_val = 0;
    uint32_t fw_ack_mask = 0;
    uint32_t fw_ack_reg = 0x130044; // Default FORCEWAKE_ACK
    
    switch (domain) {
        case 0: // GT domain
            fw_req = 0xA188; // FORCEWAKE_REQ
            fw_enable_val = 0x00010001; // GEN11_FORCEWAKE_MASKED_ENABLE
            fw_ack_mask = 0x00000001; // GT ready bit
            break;
        case 1: // Render domain
            fw_req = 0xA188; // FORCEWAKE_RENDER_CTL
            fw_enable_val = 0x000F000F; // Aggressive render wake value
            fw_ack_mask = 0x0000000F; // Lower 4 bits for render
            fw_ack_reg = 0x130044; // FORCEWAKE_ACK_RENDER
            break;
        default:
            // For other domains, we'll use GT wake as a simplification
            fw_req = 0xA188;
            fw_enable_val = 0x00010001;
            fw_ack_mask = 0x00000001;
            break;
    }
    
    if (fw_req == 0 || fw_enable_val == 0) {
        return false;
    }
    
    // Write the enable value
    volatile uint32_t* fwReqPtr = (volatile uint32_t*)((uint8_t*)mmioBase + fw_req);
    *fwReqPtr = fw_enable_val;
    
    // Read back to ensure write completed
    (void)*fwReqPtr;
    
    // Poll for acknowledgment with timeout (simple version)
    const uint32_t timeout = 10000; // 10ms timeout
    uint32_t elapsed = 0;
    volatile uint32_t* fwAckPtr = (volatile uint32_t*)((uint8_t*)mmioBase + fw_ack_reg);
    
    while (elapsed < timeout) {
        uint32_t ack = *fwAckPtr;
        if ((ack & fw_ack_mask) == fw_ack_mask) {
            break; // Success
        }
        // Simple delay loop
        for (volatile int i = 0; i < 100; i++) {} // Approximate 100us delay
        elapsed += 100;
    }
    
    if (elapsed >= timeout) {
        IOLog("(FakeIrisXE) ❌ ForceWake domain %u acquire timeout after %uus\n", domain, elapsed);
        return false;
    }
    
    IOLog("(FakeIrisXE) ✅ ForceWake domain %u acquired\n", domain);
    return true;
}

static inline void simpleForceWakeRelease(uint32_t domain, volatile void* mmioBase) {
    if (!mmioBase) return;
    
    uint32_t fw_req = 0;
    uint32_t fw_disable_val = 0;
    
    switch (domain) {
        case 0: // GT domain
            fw_req = 0xA188; // FORCEWAKE_REQ
            fw_disable_val = 0x00010000; // GEN11_FORCEWAKE_MASKED_DISABLE
            break;
        case 1: // Render domain
            fw_req = 0xA188; // FORCEWAKE_RENDER_CTL
            fw_disable_val = 0x00000000; // Disable render wake
            break;
        default:
            // For other domains, we'll use GT wake as a simplification
            fw_req = 0xA188;
            fw_disable_val = 0x00010000;
            break;
    }
    
    if (fw_req != 0 && fw_disable_val != 0) {
        volatile uint32_t* fwReqPtr = (volatile uint32_t*)((uint8_t*)mmioBase + fw_req);
        *fwReqPtr = fw_disable_val;
        
        // Read back to ensure write completed
        (void)*fwReqPtr;
        
        IOLog("(FakeIrisXE) ✅ ForceWake domain %u released\n", domain);
    }
}

// Safe read register with power domain awareness
static inline uint32_t safeReadRegister32(uint32_t offset, volatile void* mmioBase, uint32_t mmioLength) {
    if (!mmioBase || offset >= mmioLength) {
        IOLog("❌ MMIO Read attempted with invalid offset: 0x%08X\n", offset);
        return 0;
    }
    
    // Determine which power domain this register belongs to
    uint32_t domain = determinePowerDomainForOffset(offset);
    
    // Acquire forcewake for this domain
    if (!simpleForceWakeAcquire(domain, mmioBase)) {
        IOLog("❌ Failed to acquire forcewake for domain %u at offset 0x%08X\n", domain, offset);
        return 0xFFFFFFFF; // Error indicator
    }
    
    // Perform the actual read
    uint32_t value = *(volatile uint32_t*)((uint8_t*)mmioBase + offset);
    
    // Release forcewake after read
    simpleForceWakeRelease(domain, mmioBase);
    
    return value;
}

// Safe write register with power domain awareness
static inline void safeWriteRegister32(uint32_t offset, uint32_t value, volatile void* mmioBase, uint32_t mmioLength) {
    if (!mmioBase || offset >= mmioLength) {
        IOLog("❌ MMIO Write attempted with invalid offset: 0x%08X\n", offset);
        return;
    }
    
    // Determine which power domain this register belongs to
    uint32_t domain = determinePowerDomainForOffset(offset);
    
    // Acquire forcewake for this domain
    if (!simpleForceWakeAcquire(domain, mmioBase)) {
        IOLog("❌ Failed to acquire forcewake for domain %u at offset 0x%08X\n", domain, offset);
        return;
    }
    
    // Perform the actual write
    *(volatile uint32_t*)((uint8_t*)mmioBase + offset) = value;
    
    // Read back to ensure write completed (important for proper synchronization)
    (void)*(volatile uint32_t*)((uint8_t*)mmioBase + offset);
    
    // Release forcewake after write
    simpleForceWakeRelease(domain, mmioBase);
}

} // namespace SafeRegisterAccess

#endif /* _SAFE_REGISTER_ACCESS_HPP_ */
