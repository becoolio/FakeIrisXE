#include "AppleSafeRegisterAccess.hpp"

IOSimpleLock* AppleSafeRegisterAccess::sLock = nullptr;
uint32_t AppleSafeRegisterAccess::sRefCounts[AppleSafeRegisterAccess::kDomainCount] = {0, 0, 0, 0};
uint32_t AppleSafeRegisterAccess::sPinnedCounts[AppleSafeRegisterAccess::kDomainCount] = {0, 0, 0, 0};
bool AppleSafeRegisterAccess::sInitialized = false;

namespace {
static constexpr uint32_t kForceWakeReqGlobal = 0xA188;
static constexpr uint32_t kForceWakeAckGlobal = 0x130044;
static constexpr uint32_t kForceWakeReqRender = 0xA278;
static constexpr uint32_t kForceWakeAckRender = 0xA27C;
static constexpr uint32_t kForceWakeReqMediaVdbox0 = 0xA540;
static constexpr uint32_t kForceWakeAckMediaVdbox0 = 0x0D50;
static constexpr uint32_t kForceWakeReqMediaVebox0 = 0xA560;
static constexpr uint32_t kForceWakeAckMediaVebox0 = 0x0D70;

static constexpr uint32_t kEnableGlobal = 0x00010001U;
static constexpr uint32_t kDisableGlobal = 0x00010000U;
static constexpr uint32_t kAckMaskGlobal = 0x00000001U;

static constexpr uint32_t kEnableRender = 0x00010001U;
static constexpr uint32_t kDisableRender = 0x00010000U;
static constexpr uint32_t kAckMaskRender = 0x00000001U;

static constexpr uint32_t kEnableMedia = 0x00010001U;
static constexpr uint32_t kDisableMedia = 0x00010000U;
static constexpr uint32_t kAckMaskMedia = 0x00000001U;

static inline volatile uint32_t* reg32(volatile UInt8* base, uint32_t off) {
    return reinterpret_cast<volatile uint32_t*>(const_cast<UInt8*>(base) + off);
}
}

void AppleSafeRegisterAccess::init() {
    if (!sInitialized) {
        sLock = IOSimpleLockAlloc();
        if (sLock) {
            sInitialized = true;
            IOLog("(FakeIrisXE) [SafeAccess] initialized\n");
        }
    }
}

uint32_t AppleSafeRegisterAccess::rawRead32(volatile UInt8* mmioBase, uint32_t offset) {
    return *reg32(mmioBase, offset);
}

void AppleSafeRegisterAccess::rawWrite32(volatile UInt8* mmioBase, uint32_t offset, uint32_t value) {
    *reg32(mmioBase, offset) = value;
}

bool AppleSafeRegisterAccess::validate(volatile UInt8* mmioBase, uint32_t mmioLength, uint32_t offset) {
    return mmioBase && mmioLength >= sizeof(uint32_t) && offset <= mmioLength - sizeof(uint32_t);
}

uint32_t AppleSafeRegisterAccess::determineDomain(uint32_t offset) {
    switch (offset) {
        case kForceWakeReqGlobal:
        case kForceWakeAckGlobal:
        case 0x941C:
        case 0xC0F4:
        case 0xC014:
        case 0xC180:
        case 0xC184:
        case 0xC1B8:
            return kDomainGlobal;
        case kForceWakeReqRender:
        case kForceWakeAckRender:
        case 0xC05C:
        case 0xC064:
        case 0xC068:
        case 0xC300:
            return kDomainRender;
        case kForceWakeReqMediaVdbox0:
        case kForceWakeAckMediaVdbox0:
            return kDomainMediaVdbox0;
        case kForceWakeReqMediaVebox0:
        case kForceWakeAckMediaVebox0:
            return kDomainMediaVebox0;
        default:
            break;
    }

    if ((offset >= 0x138000U && offset <= 0x138FFFU) ||
        (offset >= 0x9400U && offset <= 0x94FFU) ||
        (offset >= 0xC180U && offset <= 0xC1FFU)) {
        return kDomainGlobal;
    }
    if ((offset >= 0xC000U && offset <= 0xC17FU) ||
        (offset >= 0xC300U && offset <= 0xC34FU) ||
        (offset >= 0x2000U && offset <= 0x2FFFU)) {
        return kDomainRender;
    }
    return kDomainGlobal;
}

bool AppleSafeRegisterAccess::programDomainLocked(volatile UInt8* mmioBase, uint32_t mmioLength,
                                                  uint32_t domain, bool enable, const char* label) {
    if (!validate(mmioBase, mmioLength, kForceWakeReqGlobal) || !validate(mmioBase, mmioLength, kForceWakeAckGlobal)) {
        return false;
    }

    uint32_t req = kForceWakeReqGlobal;
    uint32_t ack = kForceWakeAckGlobal;
    uint32_t value = enable ? kEnableGlobal : kDisableGlobal;
    uint32_t mask = kAckMaskGlobal;

    switch (domain) {
        case kDomainGlobal:
            req = kForceWakeReqGlobal; ack = kForceWakeAckGlobal;
            value = enable ? kEnableGlobal : kDisableGlobal; mask = kAckMaskGlobal;
            break;
        case kDomainRender:
            req = kForceWakeReqRender; ack = kForceWakeAckRender;
            value = enable ? kEnableRender : kDisableRender; mask = kAckMaskRender;
            break;
        case kDomainMediaVdbox0:
            req = kForceWakeReqMediaVdbox0; ack = kForceWakeAckMediaVdbox0;
            value = enable ? kEnableMedia : kDisableMedia; mask = kAckMaskMedia;
            break;
        case kDomainMediaVebox0:
            req = kForceWakeReqMediaVebox0; ack = kForceWakeAckMediaVebox0;
            value = enable ? kEnableMedia : kDisableMedia; mask = kAckMaskMedia;
            break;
        default:
            return false;
    }

    rawWrite32(mmioBase, req, value);
    (void)rawRead32(mmioBase, req);

    if (!enable) {
        return true;
    }

    uint32_t waitedUs = 0;
    while (waitedUs < 10000U) {
        const uint32_t ackVal = rawRead32(mmioBase, ack);
        if ((ackVal & mask) == mask) {
            return true;
        }
        IODelay(100);
        waitedUs += 100;
    }

    IOLog("(FakeIrisXE) [SafeAccess] forcewake timeout domain=%u label=%s\n",
          domain, label ? label : "unknown");
    return false;
}

bool AppleSafeRegisterAccess::retainDomain(volatile UInt8* mmioBase, uint32_t mmioLength,
                                           uint32_t domain, const char* label) {
    init();
    if (!sLock || domain >= kDomainCount) return false;

    IOSimpleLockLock(sLock);
    bool ok = true;
    if (sRefCounts[domain] == 0) {
        ok = programDomainLocked(mmioBase, mmioLength, domain, true, label);
    }
    if (ok) {
        ++sRefCounts[domain];
    }
    IOSimpleLockUnlock(sLock);
    return ok;
}

void AppleSafeRegisterAccess::releaseDomain(volatile UInt8* mmioBase, uint32_t mmioLength,
                                            uint32_t domain, const char* label) {
    init();
    if (!sLock || domain >= kDomainCount) return;

    IOSimpleLockLock(sLock);
    if (sRefCounts[domain] > 0) {
        --sRefCounts[domain];
        if (sRefCounts[domain] == 0) {
            (void)programDomainLocked(mmioBase, mmioLength, domain, false, label);
        }
    }
    IOSimpleLockUnlock(sLock);
}

bool AppleSafeRegisterAccess::beginSession(volatile UInt8* mmioBase, uint32_t mmioLength,
                                          uint32_t domain, const char* label) {
    init();
    if (!sLock || domain >= kDomainCount) return false;

    bool shouldRetain = false;
    IOSimpleLockLock(sLock);
    shouldRetain = (sPinnedCounts[domain] == 0);
    IOSimpleLockUnlock(sLock);

    if (shouldRetain && !retainDomain(mmioBase, mmioLength, domain, label)) {
        return false;
    }

    IOSimpleLockLock(sLock);
    ++sPinnedCounts[domain];
    IOSimpleLockUnlock(sLock);
    return true;
}

void AppleSafeRegisterAccess::endSession(volatile UInt8* mmioBase, uint32_t mmioLength,
                                         uint32_t domain, const char* label) {
    init();
    if (!sLock || domain >= kDomainCount) return;

    bool shouldRelease = false;
    IOSimpleLockLock(sLock);
    if (sPinnedCounts[domain] > 0) {
        --sPinnedCounts[domain];
        shouldRelease = (sPinnedCounts[domain] == 0);
    }
    IOSimpleLockUnlock(sLock);

    if (shouldRelease) {
        releaseDomain(mmioBase, mmioLength, domain, label);
    }
}

uint32_t AppleSafeRegisterAccess::read32(volatile UInt8* mmioBase, uint32_t mmioLength,
                                         uint32_t offset, const char* label) {
    if (!validate(mmioBase, mmioLength, offset)) {
        IOLog("(FakeIrisXE) [SafeAccess] invalid read offset=0x%08X\n", offset);
        return 0xFFFFFFFFU;
    }
    const uint32_t domain = determineDomain(offset);
    bool pinned = false;
    init();
    if (sLock) {
        IOSimpleLockLock(sLock);
        pinned = (domain < kDomainCount && sPinnedCounts[domain] > 0);
        IOSimpleLockUnlock(sLock);
    }
    if (!pinned && !retainDomain(mmioBase, mmioLength, domain, label)) {
        return 0xFFFFFFFFU;
    }
    const uint32_t value = rawRead32(mmioBase, offset);
    if (!pinned) {
        releaseDomain(mmioBase, mmioLength, domain, label);
    }
    return value;
}

bool AppleSafeRegisterAccess::write32(volatile UInt8* mmioBase, uint32_t mmioLength,
                                      uint32_t offset, uint32_t value, const char* label,
                                      uint32_t* outReadback) {
    if (!validate(mmioBase, mmioLength, offset)) {
        IOLog("(FakeIrisXE) [SafeAccess] invalid write offset=0x%08X\n", offset);
        if (outReadback) *outReadback = 0xFFFFFFFFU;
        return false;
    }
    const uint32_t domain = determineDomain(offset);
    bool pinned = false;
    init();
    if (sLock) {
        IOSimpleLockLock(sLock);
        pinned = (domain < kDomainCount && sPinnedCounts[domain] > 0);
        IOSimpleLockUnlock(sLock);
    }
    if (!pinned && !retainDomain(mmioBase, mmioLength, domain, label)) {
        if (outReadback) *outReadback = 0xFFFFFFFFU;
        return false;
    }
    rawWrite32(mmioBase, offset, value);
    const uint32_t readback = rawRead32(mmioBase, offset);
    if (!pinned) {
        releaseDomain(mmioBase, mmioLength, domain, label);
    }
    if (outReadback) *outReadback = readback;
    return readback == value;
}
