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
    fStatsLock = IOLockAlloc();
    fNextSurfaceID = 1;
    fCursorSurfaceId = 0;
    fCursorX = 0;
    fCursorY = 0;
    fTotalAllocatedMemory = 0;
    fPeakMemoryUsage = 0;
    fFramebufferSurfaceID = 0;
    fFramebufferSet = false;
    bzero(&fStats, sizeof(fStats));
    return (fMap && fLock && fStatsLock);
}

void FakeIrisXEIOSurfaceManager::free() {
    if (fMap) { fMap->flushCollection(); fMap->release(); fMap = nullptr; }
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    if (fStatsLock) { IOLockFree(fStatsLock); fStatsLock = nullptr; }
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

// V260: Surface iteration and statistics
uint32_t FakeIrisXEIOSurfaceManager::getSurfaceCount() {
    IOLockLock(fLock);
    uint32_t count = fMap->getCount();
    IOLockUnlock(fLock);
    return count;
}

bool FakeIrisXEIOSurfaceManager::enumerateSurfaces(uint32_t* surfIDs, uint32_t maxCount, uint32_t* outCount) {
    if (!surfIDs || !outCount) return false;
    *outCount = 0;
    
    IOLockLock(fLock);
    OSCollectionIterator* iter = OSCollectionIterator::withCollection(fMap);
    if (!iter) { IOLockUnlock(fLock); return false; }
    
    while (OSObject* obj = iter->getNextObject()) {
        if (*outCount >= maxCount) break;
        FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, obj);
        if (surf) surfIDs[(*outCount)++] = surf->getID();
    }
    iter->release();
    IOLockUnlock(fLock);
    return true;
}

uint64_t FakeIrisXEIOSurfaceManager::getTotalSurfaceMemory() {
    return fTotalAllocatedMemory;
}

uint64_t FakeIrisXEIOSurfaceManager::getSurfaceGpuMemorySize(uint32_t surfID) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return 0;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    uint64_t size = 0;
    if (surf) {
        const FakeIrisXESurfaceInfo& info = surf->getInfo();
        size = (uint64_t)info.width * info.height * info.bytesPerRow;
    }
    IOLockUnlock(fLock);
    key->release();
    return size;
}

// V260: Surface validation and properties
bool FakeIrisXEIOSurfaceManager::isSurfaceValid(uint32_t surfID) const {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return false;

    IOLockLock(fLock);
    bool valid = fMap->getObject(key) != nullptr;
    IOLockUnlock(fLock);
    key->release();
    return valid;
}

bool FakeIrisXEIOSurfaceManager::isSurfaceMapped(uint32_t surfID) const {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return false;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    bool mapped = surf && surf->getGpuAddress() != 0;
    IOLockUnlock(fLock);
    key->release();
    return mapped;
}

uint32_t FakeIrisXEIOSurfaceManager::getSurfacePixelFormat(uint32_t surfID) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return 0;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    uint32_t format = surf ? surf->getInfo().pixelFormat : 0;
    IOLockUnlock(fLock);
    key->release();
    return format;
}

uint32_t FakeIrisXEIOSurfaceManager::getSurfaceWidth(uint32_t surfID) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return 0;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    uint32_t width = surf ? surf->getInfo().width : 0;
    IOLockUnlock(fLock);
    key->release();
    return width;
}

uint32_t FakeIrisXEIOSurfaceManager::getSurfaceHeight(uint32_t surfID) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return 0;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    uint32_t height = surf ? surf->getInfo().height : 0;
    IOLockUnlock(fLock);
    key->release();
    return height;
}

uint64_t FakeIrisXEIOSurfaceManager::getSurfaceStride(uint32_t surfID) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return 0;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    uint64_t stride = surf ? surf->getInfo().bytesPerRow : 0;
    IOLockUnlock(fLock);
    key->release();
    return stride;
}

// V260: Surface management operations
IOReturn FakeIrisXEIOSurfaceManager::validateSurface(uint32_t surfID, uint32_t width, uint32_t height, uint32_t format) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    IOReturn ret = kIOReturnSuccess;
    if (!surf) ret = kIOReturnNotFound;
    else if (surf->getInfo().width != width || surf->getInfo().height != height || surf->getInfo().pixelFormat != format) ret = kIOReturnBadArgument;
    IOLockUnlock(fLock);
    key->release();
    return ret;
}

IOReturn FakeIrisXEIOSurfaceManager::invalidateSurface(uint32_t surfID) {
    IOLog("(FakeIrisXE) [V260] invalidateSurface(id=%u)\n", surfID);
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    IOReturn ret = surf ? kIOReturnSuccess : kIOReturnNotFound;
    IOLockUnlock(fLock);
    key->release();
    return ret;
}

IOReturn FakeIrisXEIOSurfaceManager::flushSurfaceCache(uint32_t surfID) {
    IOLog("(FakeIrisXE) [V260] flushSurfaceCache(id=%u)\n", surfID);
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    if (surf) {
        void* cpuAddr = surf->getCpuAddress();
        if (cpuAddr) {
            const FakeIrisXESurfaceInfo& info = surf->getInfo();
            char* start = static_cast<char*>(cpuAddr);
            __builtin___clear_cache(start, start + info.width * info.height * info.bytesPerRow);
        }
    }
    IOReturn ret = surf ? kIOReturnSuccess : kIOReturnNotFound;
    IOLockUnlock(fLock);
    key->release();
    return ret;
}

IOReturn FakeIrisXEIOSurfaceManager::trimSurface(uint32_t surfID) {
    IOLog("(FakeIrisXE) [V260] trimSurface(id=%u)\n", surfID);
    return kIOReturnSuccess;
}

// V260: Compression support
bool FakeIrisXEIOSurfaceManager::isSurfaceCompressed(uint32_t surfID) const {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return false;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    bool compressed = surf && (surf->getInfo().pixelFormat & 0x10000000) != 0;
    IOLockUnlock(fLock);
    key->release();
    return compressed;
}

IOReturn FakeIrisXEIOSurfaceManager::setSurfaceCompression(uint32_t surfID, bool enable) {
    IOLog("(FakeIrisXE) [V260] setSurfaceCompression(id=%u, enable=%s)\n", surfID, enable ? "YES" : "NO");
    return kIOReturnSuccess;
}

uint32_t FakeIrisXEIOSurfaceManager::getSurfaceCompressionMode(uint32_t surfID) const {
    return 0;
}

// V260: Display and scanout support
IOReturn FakeIrisXEIOSurfaceManager::setSurfaceDisplay(uint32_t surfID, uint32_t pipe, uint32_t plane) {
    IOLog("(FakeIrisXE) [V260] setSurfaceDisplay(surfID=%u pipe=%u plane=%u)\n", surfID, pipe, plane);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::clearSurfaceDisplay(uint32_t surfID) {
    IOLog("(FakeIrisXE) [V260] clearSurfaceDisplay(surfID=%u)\n", surfID);
    return kIOReturnSuccess;
}

bool FakeIrisXEIOSurfaceManager::isSurfaceDisplayEnabled(uint32_t surfID) const {
    return false;
}

// V260: Cursor surface support
IOReturn FakeIrisXEIOSurfaceManager::setCursorSurface(uint32_t surfID, int32_t x, int32_t y) {
    IOLog("(FakeIrisXE) [V260] setCursorSurface(id=%u x=%d y=%d)\n", surfID, x, y);
    fCursorSurfaceId = surfID;
    fCursorX = x;
    fCursorY = y;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::updateCursorPosition(int32_t x, int32_t y) {
    fCursorX = x;
    fCursorY = y;
    return kIOReturnSuccess;
}

bool FakeIrisXEIOSurfaceManager::isCursorSurfaceActive() const {
    return fCursorSurfaceId != 0;
}

void FakeIrisXEIOSurfaceManager::disableCursorSurface() {
    fCursorSurfaceId = 0;
    fCursorX = 0;
    fCursorY = 0;
}

// V260: YUV plane management
IOReturn FakeIrisXEIOSurfaceManager::setYUVPlanes(uint32_t surfID, uint32_t yPlane, uint32_t uPlane, uint32_t vPlane) {
    IOLog("(FakeIrisXE) [V260] setYUVPlanes(surfID=%u y=%u u=%u v=%u)\n", surfID, yPlane, uPlane, vPlane);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::getYUVPlanes(uint32_t surfID, uint32_t* yPlane, uint32_t* uPlane, uint32_t* vPlane) {
    if (yPlane) *yPlane = 0;
    if (uPlane) *uPlane = 0;
    if (vPlane) *vPlane = 0;
    return kIOReturnSuccess;
}

// V260: Memory management
IOReturn FakeIrisXEIOSurfaceManager::pinSurfaceMemory(uint32_t surfID, uint64_t* outGpuAddr) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    IOReturn ret = kIOReturnSuccess;
    if (surf && outGpuAddr) {
        *outGpuAddr = surf->getGpuAddress();
    } else ret = kIOReturnNotFound;
    IOLockUnlock(fLock);
    key->release();
    return ret;
}

IOReturn FakeIrisXEIOSurfaceManager::unpinSurfaceMemory(uint32_t surfID) {
    return kIOReturnSuccess;
}

bool FakeIrisXEIOSurfaceManager::isSurfacePinned(uint32_t surfID) const {
    return isSurfaceMapped(surfID);
}

// V260: Fence management
IOReturn FakeIrisXEIOSurfaceManager::attachFence(uint32_t surfID, uint32_t fenceId) {
    IOLog("(FakeIrisXE) [V260] attachFence(surfID=%u fenceId=%u)\n", surfID, fenceId);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::waitForFence(uint32_t surfID, uint32_t timeoutMs) {
    IOSleep(timeoutMs);
    return kIOReturnSuccess;
}

bool FakeIrisXEIOSurfaceManager::isFenceSignaled(uint32_t fenceId) const {
    return true;
}

// V260: Statistics and diagnostics
void FakeIrisXEIOSurfaceManager::dumpSurfaceInfo(uint32_t surfID) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return;

    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    if (surf) {
        const FakeIrisXESurfaceInfo& info = surf->getInfo();
        IOLog("(FakeIrisXE) [V260] Surface %u Info:\n", surfID);
        IOLog("  Size: %llu bytes\n", (unsigned long long)(info.width * info.height * info.bytesPerRow));
        IOLog("  Width: %u Height: %u\n", info.width, info.height);
        IOLog("  Stride: %llu\n", (unsigned long long)info.bytesPerRow);
        IOLog("  GPU: 0x%llX\n", (unsigned long long)surf->getGpuAddress());
    }
    IOLockUnlock(fLock);
    key->release();
}

void FakeIrisXEIOSurfaceManager::dumpAllSurfaceInfo() {
    IOLog("(FakeIrisXE) [V260] All Surface Info:\n");
    IOLog("  Total surfaces: %u\n", getSurfaceCount());
    IOLog("  Total memory: %llu bytes\n", (unsigned long long)fTotalAllocatedMemory);
    
    IOLockLock(fLock);
    OSCollectionIterator* iter = OSCollectionIterator::withCollection(fMap);
    if (iter) {
        while (OSObject* obj = iter->getNextObject()) {
            FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, obj);
            if (surf) dumpSurfaceInfo(surf->getID());
        }
        iter->release();
    }
    IOLockUnlock(fLock);
}

uint32_t FakeIrisXEIOSurfaceManager::getActiveSurfaceCount() {
    return getSurfaceCount();
}

uint64_t FakeIrisXEIOSurfaceManager::getPeakMemoryUsage() {
    return fPeakMemoryUsage;
}

void FakeIrisXEIOSurfaceManager::resetStatistics() {
    fTotalAllocatedMemory = 0;
    fPeakMemoryUsage = 0;
    bzero(&fStats, sizeof(fStats));
}

// V280: Enhanced IOSurface methods (from AppleIntelTGLIOSurfaceManager)
IOReturn FakeIrisXEIOSurfaceManager::createSurfaceEx(uint32_t width, uint32_t height, uint32_t pixelFormat, uint32_t flags, uint32_t* outSurfID) {
    if (!outSurfID || width == 0 || height == 0) return kIOReturnBadArgument;
    if (width > 16384 || height > 16384) return kIOReturnBadArgument;
    
    uint64_t size = (uint64_t)width * height * 4;
    if (size > IOSURFACE_MAX_SIZE) return kIOReturnNoMemory;
    
    IOLockLock(fLock);
    uint32_t surfID = fNextSurfaceID++;
    fStats.totalSurfacesCreated++;
    fStats.activeSurfaces = fMap->getCount();
    IOLockUnlock(fLock);
    
    *outSurfID = surfID;
    IOLog("(FakeIrisXE) [V280] createSurfaceEx: %ux%u id=%u\n", width, height, surfID);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::getSurfaceProperties(uint32_t surfID, uint32_t* width, uint32_t* height, uint32_t* format, uint64_t* size) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return kIOReturnNoMemory;
    
    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    IOReturn ret = kIOReturnSuccess;
    if (surf) {
        const FakeIrisXESurfaceInfo& info = surf->getInfo();
        if (width) *width = info.width;
        if (height) *height = info.height;
        if (format) *format = info.pixelFormat;
        if (size) *size = (uint64_t)info.width * info.height * info.bytesPerRow;
    } else ret = kIOReturnNotFound;
    IOLockUnlock(fLock);
    key->release();
    return ret;
}

IOReturn FakeIrisXEIOSurfaceManager::setSurfaceProperties(uint32_t surfID, uint32_t width, uint32_t height, uint32_t format) {
    IOLog("(FakeIrisXE) [V280] setSurfaceProperties(id=%u %ux%u fmt=%u)\n", surfID, width, height, format);
    return kIOReturnSuccess;
}

bool FakeIrisXEIOSurfaceManager::lockSurface(uint32_t surfID, uint32_t lockType, uint32_t timeoutMs) {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return false;
    
    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    bool success = (surf != nullptr);
    IOLockUnlock(fLock);
    key->release();
    return success;
}

IOReturn FakeIrisXEIOSurfaceManager::unlockSurface(uint32_t surfID) {
    return kIOReturnSuccess;
}

bool FakeIrisXEIOSurfaceManager::isSurfaceInUse(uint32_t surfID) const {
    const OSSymbol* key = makeSurfKey(surfID);
    if (!key) return false;
    
    IOLockLock(fLock);
    FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, fMap->getObject(key));
    bool inUse = surf && (surf->getGpuAddress() != 0);
    IOLockUnlock(fLock);
    key->release();
    return inUse;
}

IOReturn FakeIrisXEIOSurfaceManager::markForDisplay(uint32_t surfID, uint32_t displayID) {
    IOLog("(FakeIrisXE) [V280] markForDisplay(id=%u display=%u)\n", surfID, displayID);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::removeFromDisplay(uint32_t surfID) {
    return kIOReturnSuccess;
}

uint32_t FakeIrisXEIOSurfaceManager::getDisplayableSurfaces(uint32_t* surfIDs, uint32_t maxCount) {
    if (!surfIDs || maxCount == 0) return 0;
    
    uint32_t count = 0;
    IOLockLock(fLock);
    OSCollectionIterator* iter = OSCollectionIterator::withCollection(fMap);
    if (iter) {
        while (OSObject* obj = iter->getNextObject()) {
            if (count >= maxCount) break;
            FakeIrisXESurface* surf = OSDynamicCast(FakeIrisXESurface, obj);
            if (surf) surfIDs[count++] = surf->getID();
        }
        iter->release();
    }
    IOLockUnlock(fLock);
    return count;
}

IOReturn FakeIrisXEIOSurfaceManager::setAsFramebuffer(uint32_t surfID) {
    fFramebufferSurfaceID = surfID;
    fFramebufferSet = true;
    IOLog("(FakeIrisXE) [V280] setAsFramebuffer(id=%u)\n", surfID);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::getFramebuffer(uint32_t* outSurfID) {
    if (!outSurfID) return kIOReturnBadArgument;
    if (!fFramebufferSet) return kIOReturnNotFound;
    *outSurfID = fFramebufferSurfaceID;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEIOSurfaceManager::compressSurface(uint32_t surfID) {
    return kIOReturnUnsupported;
}

IOReturn FakeIrisXEIOSurfaceManager::decompressSurface(uint32_t surfID) {
    return kIOReturnUnsupported;
}

void FakeIrisXEIOSurfaceManager::getStatistics(IOSurfaceStatistics* stats) {
    if (!stats) return;
    IOLockLock(fStatsLock);
    *stats = fStats;
    IOLockUnlock(fStatsLock);
}

void FakeIrisXEIOSurfaceManager::printStatistics() {
    IOSurfaceStatistics stats;
    getStatistics(&stats);
    IOLog("(FakeIrisXE) [V280] IOSurface Stats: active=%llu memory=%llu hits=%llu misses=%llu hitratio=%.2f\n",
          stats.activeSurfaces, stats.currentMemoryUsage, stats.lookupHits, stats.lookupMisses, stats.hitRatio);
}
