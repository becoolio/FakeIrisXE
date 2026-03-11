#ifndef _SAFE_FORCE_WAKE_HPP_
#define _SAFE_FORCE_WAKE_HPP_

#include <stdint.h>
#include <libkern/libkern.h>
#include <IOKit/IOLib.h>
class SafeForceWakeManager {
public:
    // Initialize the safe forcewake system
    static void initialize(void);

    // Determine power domain for a given register offset based on Apple's mapping
    static uint32_t determinePowerDomain(uint32_t offset);
    
    // Acquire forcewake for a specific domain with refcounting
    static bool acquireForceWakeDomain(uint32_t domain, void* mmioBase);
    
    // Release forcewake for a specific domain with refcounting
    static void releaseForceWakeDomain(uint32_t domain, void* mmioBase);
    
    // Safe read register with power domain awareness and automatic forcewake management
    static uint32_t safeReadRegister32(uint32_t offset, void* mmioBase, uint32_t mmioLength);
    
    // Safe write register with power domain awareness and automatic forcewake management
    static void safeWriteRegister32(uint32_t offset, uint32_t value, void* mmioBase, uint32_t mmioLength);
};

#endif /* _SAFE_FORCE_WAKE_HPP_ */
