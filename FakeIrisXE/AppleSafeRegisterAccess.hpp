#pragma once

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSTypes.h>

class AppleSafeRegisterAccess {
public:
    enum Domain : uint32_t {
        kDomainGlobal = 0,
        kDomainRender = 1,
        kDomainMediaVdbox0 = 2,
        kDomainMediaVebox0 = 3,
        kDomainCount = 4
    };

    static void init();
    static uint32_t determineDomain(uint32_t offset);

    static bool retainDomain(volatile UInt8* mmioBase, uint32_t mmioLength, uint32_t domain,
                             const char* label = nullptr);
    static void releaseDomain(volatile UInt8* mmioBase, uint32_t mmioLength, uint32_t domain,
                              const char* label = nullptr);
    static bool beginSession(volatile UInt8* mmioBase, uint32_t mmioLength, uint32_t domain,
                             const char* label = nullptr);
    static void endSession(volatile UInt8* mmioBase, uint32_t mmioLength, uint32_t domain,
                           const char* label = nullptr);

    static uint32_t read32(volatile UInt8* mmioBase, uint32_t mmioLength, uint32_t offset,
                           const char* label = nullptr);
    static bool write32(volatile UInt8* mmioBase, uint32_t mmioLength, uint32_t offset,
                        uint32_t value, const char* label = nullptr, uint32_t* outReadback = nullptr);

private:
    static IOSimpleLock* sLock;
    static uint32_t sRefCounts[kDomainCount];
    static uint32_t sPinnedCounts[kDomainCount];
    static bool sInitialized;

    static bool validate(volatile UInt8* mmioBase, uint32_t mmioLength, uint32_t offset);
    static bool programDomainLocked(volatile UInt8* mmioBase, uint32_t mmioLength,
                                    uint32_t domain, bool enable, const char* label);
    static uint32_t rawRead32(volatile UInt8* mmioBase, uint32_t offset);
    static void rawWrite32(volatile UInt8* mmioBase, uint32_t offset, uint32_t value);
};
