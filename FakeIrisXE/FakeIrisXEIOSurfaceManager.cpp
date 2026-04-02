#include "FakeIrisXEIOSurfaceManager.hpp"
#include <libkern/OSAtomic.h>
#include "FakeIrisXEGEM.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSString.h>

static inline const OSSymbol* makeSurfKey(uint32_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", (unsigned)v);
    return OSSymbol::withCString(buf);
}

OSDefineMetaClassAndStructors(FakeIrisXEIOSurfaceManager, OSObject)

FakeIrisXEIOSurfaceManager* FakeIrisXEIOSurfaceManager::create() {
    auto* m = OSTypeAlloc(FakeIrisXEIOSurfaceManager);
    if (!m || !m->init()) { if (m) m->release(); return nullptr; }
    return m;
}

bool FakeIrisXEIOSurfaceManager::init() {
    if (!OSObject::init()) return false;
    fMap = OSDictionary::withCapacity(128);
    fLock = IOLockAlloc();
    return (fMap && fLock);
}

void FakeIrisXEIOSurfaceManager::free() {
    if (fMap) { fMap->flushCollection(); fMap->release(); fMap = nullptr; }
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    OSObject::free();
}

IOReturn FakeIrisXEIOSurfaceManager::createSurface(uint32_t surfID, FakeIrisXEGEM* gem, const FakeIrisXESurfaceInfo& info) {
    if (!gem) return kIOReturnBadArgument;
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;
    
    IOLockLock(fLock);
    if (fMap->getObject(key)) {
        IOLockUnlock(fLock);
        key->release();
        return kIOReturnExclusiveAccess;
    }
    FakeIrisXESurface* surf = FakeIrisXESurface::create(surfID, gem, info);
    if (!surf) { IOLockUnlock(fLock); key->release(); return kIOReturnNoMemory; }
    fMap->setObject(key, surf);
    surf->release();
    IOLockUnlock(fLock);
    key->release();
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::retainSurface(uint32_t surfID) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;
    
    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    if (surf) surf->retainClient();
    IOLockUnlock(fLock);
    key->release();
    return surf ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn FakeIrisXEIOSurfaceManager::releaseSurface(uint32_t surfID) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;
    
    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    if (surf) surf->releaseClient();
    IOLockUnlock(fLock);
    key->release();
    return surf ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn FakeIrisXEIOSurfaceManager::destroySurface(uint32_t surfID, bool force) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;
    
    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    if (!surf) { IOLockUnlock(fLock); key->release(); return kIOReturnNotFound; }
    if (force || (surf->releaseClient())) {
        fMap->removeObject(key);
    }
    IOLockUnlock(fLock);
    key->release();
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::mapSurfaceToTask(uint32_t surfID, task_t task, IOMemoryDescriptor** outDesc, uint64_t* outAddr) {
    *outDesc = nullptr;
    *outAddr = 0;
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;
    
    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    if (surf) surf->retain();
    IOLockUnlock(fLock);
    key->release();
    if (!surf) return kIOReturnNotFound;
    IOReturn ret = surf->mapToTask(task, outDesc, outAddr);
    surf->release();
    return ret;
}

IOReturn FakeIrisXEIOSurfaceManager::unmapSurfaceFromTask(uint32_t surfID, IOMemoryMap* map) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;
    
    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    if (surf) surf->retain();
    IOLockUnlock(fLock);
    key->release();
    if (!surf) return kIOReturnNotFound;
    IOReturn ret = surf->unmapFromTask(map);
    surf->release();
    return ret;
}

IOReturn FakeIrisXEIOSurfaceManager::getSurfaceInfo(uint32_t surfID,
                                                    FakeIrisXESurfaceInfo* outInfo,
                                                    uint64_t* outGpuAddr)
{
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    if (surf) surf->retain();
    IOLockUnlock(fLock);
    key->release();

    if (!surf) return kIOReturnNotFound;

    if (outInfo) {
        *outInfo = surf->getInfo();
    }
    if (outGpuAddr) {
        *outGpuAddr = surf->getGpuAddress();
    }

    surf->release();
    return kIOReturnSuccess;
}

FakeIrisXEGEM* FakeIrisXEIOSurfaceManager::getSurfaceGem(uint32_t surfID)
{
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return nullptr;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    FakeIrisXEGEM* gem = surf ? surf->getGem() : nullptr;
    if (gem) gem->retain();
    IOLockUnlock(fLock);
    key->release();

    return gem;
}
