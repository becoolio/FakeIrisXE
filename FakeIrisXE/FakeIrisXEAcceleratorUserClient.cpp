#include "FakeIrisXEAcceleratorUserClient.hpp"
#include "FakeIrisXEAccelerator.hpp"
#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXEFenceRetirement.hpp"
#include "FakeIrisXEIOSurfaceManager.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include "FakeIrisXEFramebuffer.hpp"
#include "FakeIrisXEAccelShared.h"


#define GEMSuper OSObject

class GEMHandleTable : public OSObject {
    OSDeclareDefaultStructors(GEMHandleTable);

public:
    OSDictionary* dict;
    IOLock* lock;
    uint32_t nextHandle;

    static GEMHandleTable* create() {
        GEMHandleTable* t = OSTypeAlloc(GEMHandleTable);
        if (!t || !t->init()) {
            if (t) t->release();
            return nullptr;
        }
        t->dict = OSDictionary::withCapacity(256);
        t->lock = IOLockAlloc();
        if (!t->dict || !t->lock) {
            t->release();
            return nullptr;
        }
        t->nextHandle = 1;
        return t;
    }

    bool init() override {
        if (!OSObject::init()) return false;
        dict = nullptr;
        lock = nullptr;
        nextHandle = 1;
        return true;
    }

    void free() override {
        if (dict) dict->release();
        if (lock) IOLockFree(lock);
        GEMSuper::free();
    }

    uint32_t add(FakeIrisXEGEM* gem) {
        IOLockLock(lock);

        char keybuf[32];
        snprintf(keybuf, sizeof(keybuf), "%u", nextHandle);

        const OSSymbol* key = OSSymbol::withCString(keybuf);
        dict->setObject(key, gem);
        key->release();

        uint32_t handle = nextHandle++;
        IOLockUnlock(lock);  // FIX: Added missing unlock
        return handle;
    }

    FakeIrisXEGEM* lookup(uint32_t h) {
        IOLockLock(lock);

        char keybuf[32];
        snprintf(keybuf, sizeof(keybuf), "%u", h);

        const OSSymbol* key = OSSymbol::withCString(keybuf);
        OSObject* o = dict->getObject(key);
        key->release();

        FakeIrisXEGEM* gem = OSDynamicCast(FakeIrisXEGEM, o);
        if (gem) gem->retain();

        IOLockUnlock(lock);
        return gem;
    }

    bool remove(uint32_t h) {
        IOLockLock(lock);

        char keybuf[32];
        snprintf(keybuf, sizeof(keybuf), "%u", h);

        const OSSymbol* key = OSSymbol::withCString(keybuf);

        // V249: FIX - dict->getObject() returns a BORROWED reference (NOT retained).
        // We must NOT call release() on the result. removeObject() handles its own
        // retain/release. Calling gem->release() here was a double-release bug.
        // Per OSDictionary semantics: setObject() retains, getObject() returns a borrowed
        // reference (no retain), removeObject() releases and removes.
        bool exists = dict->getObject(key) != nullptr;

        if (exists) {
            dict->removeObject(key);
            // DO NOT call gem->release() here. OSDictionary::removeObject releases
            // the object when it removes it from the dictionary. We never owned a retain
            // from getObject(), so we have nothing to release.
        }

        key->release();
        IOLockUnlock(lock);
        return exists;
    }
};

OSDefineMetaClassAndStructors(GEMHandleTable, OSObject)




#undef GEMSuper
#define super IOUserClient
OSDefineMetaClassAndStructors(FakeIrisXEAcceleratorUserClient, IOUserClient)

// Rate-limited logging utility
enum : uint32_t {
    kLog_Default = 0,
    kLog_Submit = 1,
    kLog_WaitTimeout = 2,
};

static const char* RLTag(uint32_t cat) {
    switch (cat) {
        case kLog_Submit: return "submit";
        case kLog_WaitTimeout: return "wait-timeout";
        default: return "general";
    }
}

static void RLLog(const char *fmt, ...) {
    static uint64_t last = 0;
    uint64_t now = mach_absolute_time();
    if (last == 0 || (now - last) > 1000000000) { // 1 second throttle
        last = now;
        va_list ap;
        va_start(ap, fmt);
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        IOLog("%s\n", buf);
    }
}

static void RLLog(uint32_t category, const char *fmt, ...) {
    static uint64_t last = 0;
    uint64_t now = mach_absolute_time();
    if (last == 0 || (now - last) > 1000000000) {
        last = now;
        va_list ap;
        va_start(ap, fmt);
        char buf[320];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        IOLog("[FakeIrisXE][%s] %s\n", RLTag(category), buf);
    }
}

bool FakeIrisXEAcceleratorUserClient::initWithTask(task_t task, void* secID, UInt32 type)
{
    if (!super::initWithTask(task, secID, type)) return false;
    fTask = task;
    return true;
}

bool FakeIrisXEAcceleratorUserClient::start(IOService* provider)
{
    if (!super::start(provider)) return false;

    fOwner = OSDynamicCast(FakeIrisXEAccelerator, provider);
    if (!fOwner) return false;

    fHandleTable = GEMHandleTable::create();
    
    // V169: Initialize memType→handle dictionary
    fMemTypeToHandle = OSDictionary::withCapacity(64);
    fMemBindLock = IOLockAlloc();
    fSurfaceRegistry = OSDictionary::withCapacity(64);
    fSurfaceLock = IOLockAlloc();

    if (!fHandleTable || !fMemTypeToHandle || !fMemBindLock || !fSurfaceRegistry || !fSurfaceLock) {
        RLLog("[FakeIrisXE] UserClient start failed: missing allocations");
        if (fHandleTable) {
            fHandleTable->release();
            fHandleTable = nullptr;
        }
        if (fMemTypeToHandle) {
            fMemTypeToHandle->release();
            fMemTypeToHandle = nullptr;
        }
        if (fMemBindLock) {
            IOLockFree(fMemBindLock);
            fMemBindLock = nullptr;
        }
        if (fSurfaceRegistry) {
            fSurfaceRegistry->release();
            fSurfaceRegistry = nullptr;
        }
        if (fSurfaceLock) {
            IOLockFree(fSurfaceLock);
            fSurfaceLock = nullptr;
        }
        return false;
    }
    
    RLLog("[FakeIrisXE] UserClient started with memType dictionary");
    return true;
}

void FakeIrisXEAcceleratorUserClient::stop(IOService* provider)
{
    // V169: Clean up dictionaries
    if (fMemBindLock) {
        IOLockFree(fMemBindLock);
        fMemBindLock = nullptr;
    }
    if (fMemTypeToHandle) {
        fMemTypeToHandle->release();
        fMemTypeToHandle = nullptr;
    }
    if (fSurfaceLock) {
        IOLockFree(fSurfaceLock);
        fSurfaceLock = nullptr;
    }
    if (fSurfaceRegistry) {
        fSurfaceRegistry->release();
        fSurfaceRegistry = nullptr;
    }
    
    if (fHandleTable) {
        fHandleTable->release();
        fHandleTable = nullptr;
    }
    
    fOwner = nullptr;
    super::stop(provider);
}

IOReturn FakeIrisXEAcceleratorUserClient::clientClose() {
    clearMemBindings();
    terminate();
    return kIOReturnSuccess;
}


uint32_t FakeIrisXEAcceleratorUserClient::createGemAndRegister(uint64_t size, uint32_t flags)
{
    if (!fHandleTable) return 0;

    FakeIrisXEGEM* gem = FakeIrisXEGEM::withSize((size_t)size, flags);
    if (!gem) return 0;

    uint32_t handle = fHandleTable->add(gem);
    if (!handle) {
        gem->release();
        return 0;
    }

    gem->release();
    return handle;
}

bool FakeIrisXEAcceleratorUserClient::destroyGemHandle(uint32_t h)
{
    return fHandleTable->remove(h);
}

IOReturn FakeIrisXEAcceleratorUserClient::pinGemHandle(uint32_t handle, uint64_t* outGpuAddr)
{
    if (!outGpuAddr || !fOwner) return kIOReturnBadArgument;

    FakeIrisXEGEM* gem = fHandleTable->lookup(handle);
    if (!gem) return kIOReturnNotFound;

    FakeIrisXEFramebuffer* fb = fOwner->framebuffer();
    if (!fb) {
        gem->release();
        return kIOReturnNotReady;
    }

    gem->pin();

    uint64_t gpuVA = fb->ggttMap(gem);
    if (!gpuVA) {
        gem->unpin();
        gem->release();
        return kIOReturnNoMemory;
    }

    gem->setGpuAddress(gpuVA);

    *outGpuAddr = gpuVA;

    gem->release();
    return kIOReturnSuccess;
}

bool FakeIrisXEAcceleratorUserClient::unpinGemHandle(uint32_t handle) {
    if (!fOwner) return false;

    FakeIrisXEGEM* gem = fHandleTable->lookup(handle);
    if (!gem) return false;

    FakeIrisXEFramebuffer* fb = fOwner->framebuffer();
    if (!fb) {
        gem->release();
        return false;
    }

    uint64_t gpuVA = gem->gpuAddress();    // retrieve
    uint32_t pages = gem->pageCount();

    if (gpuVA && pages) {
        fb->ggttUnmap(gpuVA, pages);
    }
    gem->unpin();

    gem->release();
    return true;
}


IOReturn FakeIrisXEAcceleratorUserClient::getPhysPagesForHandle(
    uint32_t h, void* outBuf, size_t* outSize)
{
    FakeIrisXEGEM* gem = fHandleTable->lookup(h);
    if (!gem) return kIOReturnNotFound;

    uint32_t pages = gem->pageCount();
    size_t need = pages * sizeof(uint64_t);

    if (!outBuf || *outSize < need) {
        *outSize = need;
        gem->release();
        return kIOReturnNoSpace;
    }

    uint64_t offset = 0;
    uint64_t* arr = (uint64_t*)outBuf;

    for (uint32_t i = 0; i < pages; i++) {
        uint64_t len = 0;
        arr[i] = gem->getPhysicalSegment(offset, &len);
        offset += len ? len : 4096;
    }

    *outSize = need;
    gem->release();
    return kIOReturnSuccess;
}

static inline const OSSymbol* makeKey(UInt32 v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", (unsigned)v);
    return OSSymbol::withCString(buf);
}

void FakeIrisXEAcceleratorUserClient::setMemBinding(UInt32 type, uint32_t handle) {
    const OSSymbol* key = makeKey(type);
    if (!key) return;
    IOLockLock(fMemBindLock);
    if (handle == 0) {
        fMemTypeToHandle->removeObject(key);
    } else {
        OSNumber* val = OSNumber::withNumber(handle, 32);
        fMemTypeToHandle->setObject(key, val);
        val->release();
    }
    IOLockUnlock(fMemBindLock);
    key->release();
}

uint32_t FakeIrisXEAcceleratorUserClient::getMemBinding(UInt32 type) {
    uint32_t handle = 0;
    const OSSymbol* key = makeKey(type);
    if (!key) return 0;
    IOLockLock(fMemBindLock);
    OSNumber* val = OSDynamicCast(OSNumber, fMemTypeToHandle->getObject(key));
    if (val != nullptr) handle = val->unsigned32BitValue();
    IOLockUnlock(fMemBindLock);
    key->release();
    return handle;
}

IOReturn FakeIrisXEAcceleratorUserClient::clearMemBindings() {
    if (fMemBindLock) {
        IOLockLock(fMemBindLock);
        if (fMemTypeToHandle) fMemTypeToHandle->flushCollection();
        IOLockUnlock(fMemBindLock);
    }
    return kIOReturnSuccess;
}

const OSSymbol* FakeIrisXEAcceleratorUserClient::keyForUInt32(UInt32 v) {
    return makeKey(v);
}

IOReturn FakeIrisXEAcceleratorUserClient::registerSurface(uint32_t surfID, uint32_t handle,
                                                          uint32_t w, uint32_t h,
                                                          uint32_t rowBytes, uint32_t pixFmt) {
    if (!fSurfaceRegistry || !fSurfaceLock) return kIOReturnNotReady;
    if (surfID == 0 || w == 0 || h == 0 || rowBytes == 0) return kIOReturnBadArgument;
    if (!fHandleTable) return kIOReturnNotReady;
    FakeIrisXEGEM* gem = fHandleTable->lookup(handle);
    if (!gem) return kIOReturnNotFound;
    RLLog("[RegisterSurface] id=%u handle=%u w=%u h=%u stride=%u fmt=0x%x",
          (unsigned)surfID, (unsigned)handle, (unsigned)w, (unsigned)h, (unsigned)rowBytes, (unsigned)pixFmt);
    FakeIrisXESurfaceInfo info = {1, surfID, w, h, rowBytes, pixFmt, handle};

    if (fOwner && fOwner->surfaceManager()) {
        IOReturn r = fOwner->surfaceManager()->createSurface(surfID, gem, info);
        if (r != kIOReturnSuccess && r != kIOReturnExclusiveAccess) {
            gem->release();
            return r;
        }
    }

    gem->release();

    OSData* blob = OSData::withBytes(&info, sizeof(info));
    const OSSymbol* key = makeKey(surfID);
    IOLockLock(fSurfaceLock);
    fSurfaceRegistry->setObject(key, blob);
    IOLockUnlock(fSurfaceLock);
    blob->release(); key->release();
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::unregisterSurface(uint32_t surfID) {
    if (!fSurfaceRegistry || !fSurfaceLock) return kIOReturnNotReady;

    if (fOwner && fOwner->surfaceManager()) {
        (void)fOwner->surfaceManager()->destroySurface(surfID, true);
    }

    const OSSymbol* key = makeKey(surfID);
    IOLockLock(fSurfaceLock);
    fSurfaceRegistry->removeObject(key);
    IOLockUnlock(fSurfaceLock);
    key->release();
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::getSurfaceInfo(uint32_t surfID, void* out, size_t *outSize) {
    if (!outSize) return kIOReturnBadArgument;
    const OSSymbol* key = makeKey(surfID);
    IOLockLock(fSurfaceLock);
    OSData* blob = OSDynamicCast(OSData, fSurfaceRegistry->getObject(key));
    if (blob) blob->retain();
    IOLockUnlock(fSurfaceLock);
    key->release();
    if (!blob) return kIOReturnNotFound;
    size_t len = blob->getLength();
    if (!out || *outSize < len) {
        *outSize = len;
        blob->release();
        return kIOReturnNoSpace;
    }
    memcpy(out, blob->getBytesNoCopy(), len);
    *outSize = len;
    blob->release();
    return kIOReturnSuccess;
}




IOReturn FakeIrisXEAcceleratorUserClient::clientMemoryForType(
    UInt32 type, UInt32* flags, IOMemoryDescriptor** mem)
{
    if (!flags || !mem) return kIOReturnBadArgument;

    uint32_t handle = getMemBinding(type);
    if (handle == 0) {
        RLLog("[clientMemoryForType] type=%u no binding", (unsigned)type);
        return kIOReturnUnsupported;
    }
    RLLog("[clientMemoryForType] type=%u -> handle=%u", (unsigned)type, (unsigned)handle);
    
    if (!fHandleTable) return kIOReturnNotReady;
    FakeIrisXEGEM* gem = fHandleTable->lookup(handle);
    if (!gem) return kIOReturnNotFound;

    IOBufferMemoryDescriptor* desc = gem->memoryDescriptor();
    if (!desc) {
        gem->release();
        return kIOReturnNoMemory;
    }

    desc->retain();
    *mem = desc;
    *flags = 0;

    gem->release();
    return kIOReturnSuccess;
}







IOReturn FakeIrisXEAcceleratorUserClient::externalMethod(
    uint32_t selector,
    IOExternalMethodArguments* args,
    IOExternalMethodDispatch* dispatch,
    OSObject* target,
    void* ref)
{
    IOLog("(FakeIrisXEFramebuffer) [UC] externalMethod selector=%u\n", selector);

    switch (selector) {

        case kFIx_Method_CreateGEM: {
            if (args->scalarInputCount < 2 || args->scalarOutputCount < 1) return kIOReturnBadArgument;
            uint64_t size = args->scalarInput[0];
            uint32_t flags = (uint32_t)args->scalarInput[1];
            uint32_t h = createGemAndRegister(size, flags);
            if (h == 0) return kIOReturnNoMemory;
            args->scalarOutput[0] = h;
            args->scalarOutputCount = 1;
            return kIOReturnSuccess;
        }
        case kFIx_Method_DestroyGEM: {
            if (args->scalarInputCount < 1) return kIOReturnBadArgument;
            return destroyGemHandle((uint32_t)args->scalarInput[0]) ? kIOReturnSuccess : kIOReturnNotFound;
        }
        case kFIx_Method_PinGEM: {
            if (args->scalarInputCount < 1 || args->scalarOutputCount < 1) return kIOReturnBadArgument;
            uint64_t addr = 0;
            IOReturn r = pinGemHandle((uint32_t)args->scalarInput[0], &addr);
            if (r != kIOReturnSuccess) return r;
            args->scalarOutput[0] = addr;
            args->scalarOutputCount = 1;
            return kIOReturnSuccess;
        }
        case kFIx_Method_UnpinGEM: {
            if (args->scalarInputCount < 1) return kIOReturnBadArgument;
            return unpinGemHandle((uint32_t)args->scalarInput[0]) ? kIOReturnSuccess : kIOReturnNotFound;
        }
        case kFIx_Method_GetPhysPages: {
            if (args->scalarInputCount < 1 || !args->structureOutput) return kIOReturnBadArgument;
            size_t outSize = args->structureOutputSize;
            IOReturn r = getPhysPagesForHandle((uint32_t)args->scalarInput[0], args->structureOutput, &outSize);
            args->structureOutputSize = outSize;
            return r;
        }
        case kFIx_Method_BindMemTypeToHandle: {
            if (args->scalarInputCount < 2) return kIOReturnBadArgument;
            setMemBinding((UInt32)args->scalarInput[0], (uint32_t)args->scalarInput[1]);
            return kIOReturnSuccess;
        }
        case kFIx_Method_UnbindMemType: {
            if (args->scalarInputCount < 1) return kIOReturnBadArgument;
            setMemBinding((UInt32)args->scalarInput[0], 0);
            return kIOReturnSuccess;
        }
        case kFIx_Method_RegisterSurface: {
            if (args->scalarInputCount < 6) return kIOReturnBadArgument;
            return registerSurface((uint32_t)args->scalarInput[0],
                                   (uint32_t)args->scalarInput[1],
                                   (uint32_t)args->scalarInput[2],
                                   (uint32_t)args->scalarInput[3],
                                   (uint32_t)args->scalarInput[4],
                                   (uint32_t)args->scalarInput[5]);
        }
        case kFIx_Method_UnregisterSurface: {
            if (args->scalarInputCount < 1) return kIOReturnBadArgument;
            return unregisterSurface((uint32_t)args->scalarInput[0]);
        }
        case kFIx_Method_GetSurfaceInfo: {
            if (args->scalarInputCount < 1 || !args->structureOutput) return kIOReturnBadArgument;
            size_t outSize = args->structureOutputSize;
            IOReturn r = getSurfaceInfo((uint32_t)args->scalarInput[0], args->structureOutput, &outSize);
            args->structureOutputSize = outSize;
            return r;
        }
        case kFIx_Method_Submit: {
            if (!args || args->scalarInputCount < 3 || args->scalarOutputCount < 2) return kIOReturnBadArgument;

            uint32_t batchHandle = (uint32_t)args->scalarInput[0];
            uint32_t surfID      = (uint32_t)args->scalarInput[1];
            uint32_t flags       = (uint32_t)args->scalarInput[2];

            if (!fOwner) return kIOReturnNotReady;

            FakeIrisXEGEM* batchGem = fHandleTable ? fHandleTable->lookup(batchHandle) : nullptr;
            if (!batchGem) {
                return kIOReturnNotFound;
            }

            if (surfID != 0) {
                size_t sz = 0;
                IOReturn surfRet = getSurfaceInfo(surfID, nullptr, &sz);
                if (surfRet != kIOReturnNoSpace && surfRet != kIOReturnSuccess) {
                    batchGem->release();
                    return surfRet;
                }
            }

            uint32_t hwSeq = fOwner->submitBatchWithFence(batchGem, flags);
            batchGem->release();
            if (hwSeq == 0) {
                return kIOReturnIOError;
            }

            FakeIrisXEFenceManager* fm = fOwner->fenceManager;
            uint64_t fenceId = (fm ? fm->allocFence(hwSeq) : 0);
            if (!fm || fenceId == 0) {
                return kIOReturnNoMemory;
            }

            args->scalarOutput[0] = (uint32_t)(fenceId >> 32);
            args->scalarOutput[1] = (uint32_t)(fenceId & 0xFFFFFFFFu);
            args->scalarOutputCount = 2;

            RLLog(kLog_Submit,
                  "Submit: batch=%u surfID=%u flags=0x%x hwSeq=%u => fenceId=0x%llx",
                  (unsigned)batchHandle,
                  (unsigned)surfID,
                  (unsigned)flags,
                  (unsigned)hwSeq,
                  (unsigned long long)fenceId);
            return kIOReturnSuccess;
        }
        case kFIx_Method_WaitFence: {
            if (!args || args->scalarInputCount < 3) return kIOReturnBadArgument;

            uint64_t fenceId = ((uint64_t)args->scalarInput[0] << 32) | (uint64_t)args->scalarInput[1];
            uint32_t timeoutMs = (uint32_t)args->scalarInput[2];

            if (!fOwner) return kIOReturnNotReady;
            FakeIrisXEFenceManager* fm = fOwner->fenceManager;
            if (!fm) return kIOReturnNotReady;

            IOReturn res = fm->waitFence(fenceId, timeoutMs);
            if (res == kIOReturnTimeout) {
                RLLog(kLog_WaitTimeout,
                      "WaitFence timeout: fenceId=0x%llx after %u ms",
                      (unsigned long long)fenceId,
                      (unsigned)timeoutMs);
            }
            return res;
        }
        case kFIx_Method_QueryFence: {
            if (!args || args->scalarInputCount < 2 || args->scalarOutputCount < 1) return kIOReturnBadArgument;

            uint64_t fenceId = ((uint64_t)args->scalarInput[0] << 32) | (uint64_t)args->scalarInput[1];

            if (!fOwner) return kIOReturnNotReady;
            FakeIrisXEFenceManager* fm = fOwner->fenceManager;
            if (!fm) return kIOReturnNotReady;

            bool signaled = fm->isFenceSignaled(fenceId);
            args->scalarOutput[0] = signaled ? 1 : 0;
            args->scalarOutputCount = 1;

            RLLog(kLog_Submit,
                  "QueryFence: fenceId=0x%llx signaled=%d",
                  (unsigned long long)fenceId,
                  signaled ? 1 : 0);
            return kIOReturnSuccess;
        }
        case kFIx_Method_GetCaps: {
            if (args && args->structureOutputSize > 0) {
                args->structureOutputSize = 0;
            }
            return kIOReturnUnsupported;
        }
        case kFIx_Method_GetStats: {
            if (args && args->structureOutputSize > 0) {
                args->structureOutputSize = 0;
            }
            return kIOReturnUnsupported;
        }
        default:
            IOLog("(FakeIrisXEFramebuffer) [UC] externalMethod unsupported selector=%u\n",
                  selector);
            return kIOReturnUnsupported;
    }
}
