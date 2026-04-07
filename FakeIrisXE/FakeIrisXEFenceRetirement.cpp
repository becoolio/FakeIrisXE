#include "FakeIrisXEFenceRetirement.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>

struct FenceEntry {
    uint64_t fenceId;
    uint32_t expectedSeqno;
    bool signaled;
};

static uint64_t absDeltaToNs(uint64_t startAbs, uint64_t endAbs)
{
    if (endAbs <= startAbs) return 0;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(endAbs - startAbs, &ns);
    return ns;
}

OSDefineMetaClassAndStructors(FakeIrisXEFenceManager, OSObject)

FakeIrisXEFenceManager* FakeIrisXEFenceManager::create(IOWorkLoop* wl) {
    auto* m = OSTypeAlloc(FakeIrisXEFenceManager);
    if (!m || !m->init()) { if (m) m->release(); return nullptr; }
    m->fWL = wl;
    m->fTimer = nullptr;
    m->fLock = IOLockAlloc();
    m->fSeqnoReader = nullptr;
    m->fSeqnoReaderCtx = nullptr;
    m->fCompletedSeqno = 0;
    m->fNextFenceId = 0;
    m->fFences = OSArray::withCapacity(64);
    if (!m->fLock || !m->fFences) {
        m->release();
        return nullptr;
    }
    return m;
}

void FakeIrisXEFenceManager::setSeqnoReader(uint32_t (*readSeqno)(void*), void* ctx) {
    fSeqnoReader = readSeqno;
    fSeqnoReaderCtx = ctx;
}

uint64_t FakeIrisXEFenceManager::allocFence(uint32_t expectedSeqno) {
    if (!fLock || !fFences) return 0;

    IOLockLock(fLock);

    if (fFences->getCount() > 1024) {
        for (uint32_t i = 0; i < fFences->getCount();) {
            OSData* entryData = OSDynamicCast(OSData, fFences->getObject(i));
            if (!entryData) {
                fFences->removeObject(i);
                continue;
            }

            FenceEntry* existing = (FenceEntry*)entryData->getBytesNoCopy();
            if (existing && existing->signaled) {
                fFences->removeObject(i);
                continue;
            }
            ++i;
        }
    }

    uint64_t id = ++fNextFenceId;
    FenceEntry entry;
    entry.fenceId = id;
    entry.expectedSeqno = expectedSeqno;
    entry.signaled = false;
    
    OSData* entryData = OSData::withBytes(&entry, sizeof(entry));
    if (entryData) {
        fFences->setObject(entryData);
        entryData->release();
    } else {
        id = 0;
    }
    
    IOLockUnlock(fLock);
    return id;
}

IOReturn FakeIrisXEFenceManager::waitFence(uint64_t fenceId, uint32_t timeoutMs) {
    if (fenceId == 0) return kIOReturnBadArgument;

    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = (uint64_t)timeoutMs * 1000000ULL;
    
    while (true) {
        if (isFenceSignaled(fenceId)) {
            return kIOReturnSuccess;
        }
        
        uint64_t now = mach_absolute_time();
        if (absDeltaToNs(start, now) > timeoutNs) {
            IOLog("[FakeIrisXE] WaitFence timeout fenceId=%llu\n", fenceId);
            return kIOReturnTimeout;
        }
        
        IOSleep(1);
    }
}

bool FakeIrisXEFenceManager::isFenceSignaled(uint64_t fenceId) {
    if (!fLock || !fFences || fenceId == 0) return false;

    IOLockLock(fLock);
    uint32_t completed = fSeqnoReader ? fSeqnoReader(fSeqnoReaderCtx) : fCompletedSeqno;
    if (completed > fCompletedSeqno) {
        fCompletedSeqno = completed;
    }
    
    for (uint32_t i = 0; i < fFences->getCount(); i++) {
        OSData* entryData = OSDynamicCast(OSData, fFences->getObject(i));
        if (!entryData) continue;
        
        FenceEntry* entry = (FenceEntry*)entryData->getBytesNoCopy();
        if (entry->fenceId == fenceId) {
            bool signaled = entry->signaled || (fCompletedSeqno >= entry->expectedSeqno);
            if (signaled) {
                entry->signaled = true;
            }
            IOLockUnlock(fLock);
            return signaled;
        }
    }
    
    IOLockUnlock(fLock);
    return false;
}

void FakeIrisXEFenceManager::free() {
    if (fFences) { fFences->release(); fFences = nullptr; }
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    OSObject::free();
}

// V295: Enhanced fence management from IntelFence
OSDefineMetaClassAndStructors(FakeIrisXEFence, OSObject)

FakeIrisXEFence* FakeIrisXEFence::create(uint32_t id) {
    FakeIrisXEFence* fence = new FakeIrisXEFence;
    if (!fence) return nullptr;
    if (!fence->init()) { fence->release(); return nullptr; }
    fence->fenceId = id;
    fence->signaled = false;
    fence->seqno = 0;
    fence->engineId = 0;
    fence->signalTime = 0;
    fence->fenceLock = IOLockAlloc();
    return fence;
}

bool FakeIrisXEFence::init() {
    if (!OSObject::init()) return false;
    fenceLock = IOLockAlloc();
    return fenceLock != nullptr;
}

void FakeIrisXEFence::free() {
    if (fenceLock) { IOLockFree(fenceLock); fenceLock = nullptr; }
    OSObject::free();
}

bool FakeIrisXEFence::wait(uint32_t timeoutMs) {
    IOLockLock(fenceLock);
    if (signaled) { IOLockUnlock(fenceLock); return true; }
    IOLockUnlock(fenceLock);
    
    uint64_t startTime = 0;
    clock_get_uptime(&startTime);
    uint64_t deadline = startTime + (uint64_t)timeoutMs * 1000000ULL;
    
    while (true) {
        IOLockLock(fenceLock);
        if (signaled) { IOLockUnlock(fenceLock); return true; }
        IOLockUnlock(fenceLock);
        
        uint64_t now = 0;
        clock_get_uptime(&now);
        if (now >= deadline) {
            IOLog("(FakeIrisXE) [FENCE] Timeout fence %u seqno=%u\n", fenceId, seqno);
            return false;
        }
        IOSleep(1);
    }
}

void FakeIrisXEFence::signal() {
    IOLockLock(fenceLock);
    if (!signaled) {
        signaled = true;
        clock_get_uptime(&signalTime);
    }
    IOLockUnlock(fenceLock);
}

bool FakeIrisXEFence::isSignaled() const {
    return signaled;
}

void FakeIrisXEFence::reset() {
    IOLockLock(fenceLock);
    signaled = false;
    signalTime = 0;
    IOLockUnlock(fenceLock);
}

// Additional fence manager methods
IOReturn FakeIrisXEFenceManager::signalFence(uint64_t fenceId) {
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fFences->getCount(); i++) {
        OSData* entryData = OSDynamicCast(OSData, fFences->getObject(i));
        if (!entryData) continue;
        FenceEntry* entry = (FenceEntry*)entryData->getBytesNoCopy();
        if (entry->fenceId == fenceId) {
            entry->signaled = true;
            IOLockUnlock(fLock);
            return kIOReturnSuccess;
        }
    }
    IOLockUnlock(fLock);
    return kIOReturnNotFound;
}

void FakeIrisXEFenceManager::cleanupFence(uint64_t fenceId) {
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fFences->getCount(); i++) {
        OSData* entryData = OSDynamicCast(OSData, fFences->getObject(i));
        if (!entryData) continue;
        FenceEntry* entry = (FenceEntry*)entryData->getBytesNoCopy();
        if (entry->fenceId == fenceId) {
            fFences->removeObject(i);
            break;
        }
    }
    IOLockUnlock(fLock);
}

void FakeIrisXEFenceManager::dumpFenceStatus() {
    IOLog("(FakeIrisXE) [FENCE] Status: active=%u completed=%u\n",
          fFences ? fFences->getCount() : 0, fCompletedSeqno);
}
