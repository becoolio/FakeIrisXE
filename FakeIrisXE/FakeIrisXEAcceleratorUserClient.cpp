#include "FakeIrisXEAcceleratorUserClient.hpp"

#include "FakeIrisXEAccelShared.h"
#include "FakeIrisXEAccelerator.hpp"
#include "FakeIrisXEExeclist.hpp"
#include "FakeIrisXEFramebuffer.hpp"
#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXEIosurfaceCompat.hpp"
#include "FakeIrisXETrace.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <string.h>

volatile int32_t gFakeIrisXEGlobalPhase = 0;

static IOBufferMemoryDescriptor* AllocateSharedRingPage() {
    IOBufferMemoryDescriptor* md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryKernelUserShared | kIODirectionInOut,
        XE_PAGE,
        4096);
    if (!md) return nullptr;

    void* base = md->getBytesNoCopy();
    if (!base) {
        md->release();
        return nullptr;
    }

    bzero(base, XE_PAGE);
    XEHdr* hdr = (XEHdr*)base;
    hdr->magic = XE_MAGIC;
    hdr->version = XE_VERSION;
    hdr->capacity = (uint32_t)(XE_PAGE - sizeof(XEHdr));
    hdr->head = 0;
    hdr->tail = 0;
    return md;
}

#define super OSObject
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
        super::free();
    }

    uint32_t add(FakeIrisXEGEM* gem) {
        IOLockLock(lock);
        char keybuf[32];
        snprintf(keybuf, sizeof(keybuf), "%u", nextHandle);

        const OSSymbol* key = OSSymbol::withCString(keybuf);
        dict->setObject(key, gem);
        key->release();

        gem->retain();
        const uint32_t handle = nextHandle++;
        IOLockUnlock(lock);
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
        bool exists = dict->getObject(key) != nullptr;

        if (exists) {
            FakeIrisXEGEM* gem = OSDynamicCast(FakeIrisXEGEM, dict->getObject(key));
            if (gem) gem->release();
            dict->removeObject(key);
        }

        key->release();
        IOLockUnlock(lock);
        return exists;
    }
};
OSDefineMetaClassAndStructors(GEMHandleTable, OSObject)

struct FXESurfaceBindingRecord {
    uint32_t surfaceId;
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
    uint32_t stride;
    uint32_t flags;
};

static void HandleKey(uint64_t handle, char* buf, size_t bufSize) {
    snprintf(buf, bufSize, "%016llx", (unsigned long long)handle);
}

static bool IsRangeReady(const FakeIrisXEAccelerator* owner) {
    return owner && owner->fExeclistFromFB && owner->fRcsRingFromFB;
}

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(FakeIrisXEAcceleratorUserClient, IOUserClient)

const IOExternalMethodDispatch FakeIrisXEAcceleratorUserClient::sDispatchTable[8] = {
    {&FakeIrisXEAcceleratorUserClient::sGetCaps, 0, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize}, // 0
    {&FakeIrisXEAcceleratorUserClient::sCreateContext, 0, sizeof(XECreateCtxIn), 0, sizeof(XECreateCtxOut)},    // 1
    {&FakeIrisXEAcceleratorUserClient::sDestroyContext, 1, 0, 0, 0},                                             // 2
    {&FakeIrisXEAcceleratorUserClient::sBindSurface, 0, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize}, // 3
    {&FakeIrisXEAcceleratorUserClient::sPresent, kIOUCVariableStructureSize, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize}, // 4
    {nullptr, 0, 0, 0, 0},                                                                                         // 5
    {nullptr, 0, 0, 0, 0},                                                                                         // 6
    {&FakeIrisXEAcceleratorUserClient::sFenceTest, 0, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize}, // 7
};

bool FakeIrisXEAcceleratorUserClient::initWithTask(task_t task, void* secID, UInt32 type) {
    if (!super::initWithTask(task, secID, type)) return false;
    fTask = task;
    return true;
}

bool FakeIrisXEAcceleratorUserClient::start(IOService* provider) {
    FXE_PHASE("UC", 100, "start enter provider=%p", provider);

    if (!super::start(provider)) return false;

    fOwner = OSDynamicCast(FakeIrisXEAccelerator, provider);
    if (!fOwner) {
        FXE_LOG("[UC] provider cast failed");
        return false;
    }

    fHandleTable = GEMHandleTable::create();
    fSurfaceBindings = OSDictionary::withCapacity(32);
    fSurfaceLock = IOLockAlloc();
    fSurfaceSalt = 1;
    fCompletionCounter = 0;

    if (!fHandleTable || !fSurfaceBindings || !fSurfaceLock) {
        FXE_LOG("[UC] allocation failure handleTable=%p bindings=%p lock=%p",
                fHandleTable, fSurfaceBindings, fSurfaceLock);
        return false;
    }

    fOwner->setProperty("FakeIrisXEUCReady", kOSBooleanTrue);
    FXE_PHASE("UC", 101, "start ready owner=%p", fOwner);
    return true;
}

void FakeIrisXEAcceleratorUserClient::stop(IOService* provider) {
    FXE_PHASE("UC", 900, "stop enter provider=%p", provider);

    clearSurfaceBindings();
    if (fSurfaceLock) {
        IOLockFree(fSurfaceLock);
        fSurfaceLock = nullptr;
    }
    if (fSurfaceBindings) {
        fSurfaceBindings->release();
        fSurfaceBindings = nullptr;
    }

    if (fHandleTable) {
        fHandleTable->release();
        fHandleTable = nullptr;
    }

    if (fOwner) {
        fOwner->setProperty("FakeIrisXEUCReady", kOSBooleanFalse);
    }

    fOwner = nullptr;
    super::stop(provider);
    FXE_PHASE("UC", 901, "stop done");
}

IOReturn FakeIrisXEAcceleratorUserClient::clientClose() {
    FXE_PHASE("UC", 910, "clientClose");
    clearSurfaceBindings();
    terminate();
    return kIOReturnSuccess;
}

uint64_t FakeIrisXEAcceleratorUserClient::makeSurfaceHandle(uint32_t surfaceId) {
    const uint32_t salt = (uint32_t)OSIncrementAtomic((volatile SInt32*)&fSurfaceSalt);
    return (uint64_t(salt) << 32) | uint64_t(surfaceId);
}

bool FakeIrisXEAcceleratorUserClient::storeSurfaceBinding(uint64_t handle,
                                                          uint32_t surfaceId,
                                                          uint32_t width,
                                                          uint32_t height,
                                                          uint32_t pixelFormat,
                                                          uint32_t stride,
                                                          uint32_t flags) {
    if (!fSurfaceBindings || !fSurfaceLock) return false;

    FXESurfaceBindingRecord rec = {};
    rec.surfaceId = surfaceId;
    rec.width = width;
    rec.height = height;
    rec.pixelFormat = pixelFormat;
    rec.stride = stride;
    rec.flags = flags;

    OSData* data = OSData::withBytes(&rec, sizeof(rec));
    if (!data) return false;

    char keyBuf[32];
    HandleKey(handle, keyBuf, sizeof(keyBuf));
    const OSSymbol* key = OSSymbol::withCString(keyBuf);
    if (!key) {
        data->release();
        return false;
    }

    IOLockLock(fSurfaceLock);
    fSurfaceBindings->setObject(key, data);
    IOLockUnlock(fSurfaceLock);

    key->release();
    data->release();
    return true;
}

bool FakeIrisXEAcceleratorUserClient::getSurfaceBinding(uint64_t handle,
                                                        uint32_t* surfaceId,
                                                        uint32_t* width,
                                                        uint32_t* height,
                                                        uint32_t* pixelFormat,
                                                        uint32_t* stride,
                                                        uint32_t* flags) {
    if (!fSurfaceBindings || !fSurfaceLock) return false;

    char keyBuf[32];
    HandleKey(handle, keyBuf, sizeof(keyBuf));
    const OSSymbol* key = OSSymbol::withCString(keyBuf);
    if (!key) return false;

    IOLockLock(fSurfaceLock);
    OSData* data = OSDynamicCast(OSData, fSurfaceBindings->getObject(key));
    bool ok = false;
    if (data && data->getLength() == sizeof(FXESurfaceBindingRecord)) {
        const FXESurfaceBindingRecord* rec = (const FXESurfaceBindingRecord*)data->getBytesNoCopy();
        if (rec) {
            if (surfaceId) *surfaceId = rec->surfaceId;
            if (width) *width = rec->width;
            if (height) *height = rec->height;
            if (pixelFormat) *pixelFormat = rec->pixelFormat;
            if (stride) *stride = rec->stride;
            if (flags) *flags = rec->flags;
            ok = true;
        }
    }
    IOLockUnlock(fSurfaceLock);

    key->release();
    return ok;
}

void FakeIrisXEAcceleratorUserClient::clearSurfaceBindings() {
    if (!fSurfaceBindings || !fSurfaceLock) return;
    IOLockLock(fSurfaceLock);
    fSurfaceBindings->flushCollection();
    IOLockUnlock(fSurfaceLock);
}

uint32_t FakeIrisXEAcceleratorUserClient::createGemAndRegister(uint64_t size, uint32_t flags) {
    FakeIrisXEGEM* gem = FakeIrisXEGEM::withSize((size_t)size, flags);
    if (!gem) return 0;

    uint32_t handle = fHandleTable->add(gem);
    gem->release();
    return handle;
}

bool FakeIrisXEAcceleratorUserClient::destroyGemHandle(uint32_t h) {
    return fHandleTable->remove(h);
}

IOReturn FakeIrisXEAcceleratorUserClient::pinGemHandle(uint32_t handle, uint64_t* outGpuAddr) {
    FakeIrisXEGEM* gem = fHandleTable->lookup(handle);
    if (!gem) return kIOReturnNotFound;

    gem->pin();

    uint64_t gpuVA = fOwner->fFramebuffer->ggttMap(gem);
    gem->setGpuAddress(gpuVA);
    *outGpuAddr = gpuVA;

    gem->release();
    return kIOReturnSuccess;
}

bool FakeIrisXEAcceleratorUserClient::unpinGemHandle(uint32_t handle) {
    FakeIrisXEGEM* gem = fHandleTable->lookup(handle);
    if (!gem) return false;

    uint64_t gpuVA = gem->gpuAddress();
    uint32_t pages = gem->pageCount();

    fOwner->fFramebuffer->ggttUnmap(gpuVA, pages);
    gem->unpin();

    gem->release();
    return true;
}

IOReturn FakeIrisXEAcceleratorUserClient::getPhysPagesForHandle(uint32_t h, void* outBuf, size_t* outSize) {
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

IOReturn FakeIrisXEAcceleratorUserClient::clientMemoryForType(UInt32 type, UInt32* flags, IOMemoryDescriptor** mem) {
    if (!mem) return kIOReturnBadArgument;

    if (type == 0) {
        if (!fOwner) return kIOReturnNotReady;
        IOBufferMemoryDescriptor* shared = fOwner->getSharedMD();
        if (!shared) {
            shared = AllocateSharedRingPage();
            if (!shared) return kIOReturnNoMemory;
            if (!fOwner->attachShared(shared)) {
                shared->release();
                return kIOReturnError;
            }
            shared->release();
            shared = fOwner->getSharedMD();
        }
        if (!shared) return kIOReturnNoMemory;
        shared->retain();
        *mem = shared;
        if (flags) *flags = 0;
        return kIOReturnSuccess;
    }

    if (type == 1) {
        const size_t pixelBufferSize = 640 * 480 * 4;
        IOBufferMemoryDescriptor* pixelBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(
            kernel_task,
            kIOMemoryKernelUserShared | kIODirectionInOut,
            pixelBufferSize,
            page_size);

        if (!pixelBuffer) return kIOReturnNoMemory;

        void* bufferPtr = pixelBuffer->getBytesNoCopy();
        if (bufferPtr) {
            memset(bufferPtr, 0, pixelBufferSize);
        }

        if (fOwner) {
            fOwner->setPixelBuffer(pixelBuffer);
        }

        pixelBuffer->retain();
        *mem = pixelBuffer;
        if (flags) *flags = 0;
        return kIOReturnSuccess;
    }

    uint32_t handle = fLastRequestedGemHandle;
    FakeIrisXEGEM* gem = fHandleTable->lookup(handle);
    if (!gem) return kIOReturnNotFound;

    IOBufferMemoryDescriptor* desc = gem->memoryDescriptor();
    if (!desc) {
        gem->release();
        return kIOReturnNoMemory;
    }

    desc->retain();
    *mem = desc;
    if (flags) *flags = 0;

    gem->release();
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::sGetCaps(OSObject* target, void*, IOExternalMethodArguments* args) {
    FakeIrisXEAcceleratorUserClient* uc = OSDynamicCast(FakeIrisXEAcceleratorUserClient, target);
    return uc ? uc->methodGetCaps(args) : kIOReturnBadArgument;
}

IOReturn FakeIrisXEAcceleratorUserClient::sCreateContext(OSObject* target, void*, IOExternalMethodArguments* args) {
    FakeIrisXEAcceleratorUserClient* uc = OSDynamicCast(FakeIrisXEAcceleratorUserClient, target);
    return uc ? uc->methodCreateContext(args) : kIOReturnBadArgument;
}

IOReturn FakeIrisXEAcceleratorUserClient::sDestroyContext(OSObject* target, void*, IOExternalMethodArguments* args) {
    FakeIrisXEAcceleratorUserClient* uc = OSDynamicCast(FakeIrisXEAcceleratorUserClient, target);
    return uc ? uc->methodDestroyContext(args) : kIOReturnBadArgument;
}

IOReturn FakeIrisXEAcceleratorUserClient::sBindSurface(OSObject* target, void*, IOExternalMethodArguments* args) {
    FakeIrisXEAcceleratorUserClient* uc = OSDynamicCast(FakeIrisXEAcceleratorUserClient, target);
    return uc ? uc->methodBindSurface(args) : kIOReturnBadArgument;
}

IOReturn FakeIrisXEAcceleratorUserClient::sPresent(OSObject* target, void*, IOExternalMethodArguments* args) {
    FakeIrisXEAcceleratorUserClient* uc = OSDynamicCast(FakeIrisXEAcceleratorUserClient, target);
    return uc ? uc->methodPresent(args) : kIOReturnBadArgument;
}

IOReturn FakeIrisXEAcceleratorUserClient::sFenceTest(OSObject* target, void*, IOExternalMethodArguments* args) {
    FakeIrisXEAcceleratorUserClient* uc = OSDynamicCast(FakeIrisXEAcceleratorUserClient, target);
    return uc ? uc->methodFenceTest(args) : kIOReturnBadArgument;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodGetCaps(IOExternalMethodArguments* args) {
    if (!args || !args->structureOutput) return kIOReturnBadArgument;

    if (args->structureOutputSize == sizeof(FXE_VersionInfo)) {
        FXE_VersionInfo out = {};
        out.abiMajor = kFakeIrisXE_ABI_Major;
        out.abiMinor = kFakeIrisXE_ABI_Minor;
        out.kextVersion = kFakeIrisXE_KextVersion_u32;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        FXE_LOG("[UC][GetVersion] abi=%u.%u kext=0x%08X", out.abiMajor, out.abiMinor, out.kextVersion);
        return kIOReturnSuccess;
    }

    if (args->structureOutputSize < sizeof(XEAccelCaps)) return kIOReturnMessageTooLarge;
    XEAccelCaps caps = {};
    fOwner->getCaps(caps);
    bcopy(&caps, args->structureOutput, sizeof(caps));
    args->structureOutputSize = sizeof(caps);
    FXE_LOG("[UC][GetCaps] version=%u metal=%u", caps.version, caps.metalSupported);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodCreateContext(IOExternalMethodArguments* args) {
    if (!args || !args->structureInput || !args->structureOutput) return kIOReturnBadArgument;
    if (args->structureInputSize != sizeof(XECreateCtxIn) || args->structureOutputSize < sizeof(XECreateCtxOut)) {
        return kIOReturnBadArgument;
    }

    const XECreateCtxIn* in = (const XECreateCtxIn*)args->structureInput;
    XECreateCtxOut out = {};
    out.ctxId = fOwner->createContext(in->sharedGPUPtr, in->flags);
    if (!out.ctxId) return kIOReturnNoMemory;

    bcopy(&out, args->structureOutput, sizeof(out));
    args->structureOutputSize = sizeof(out);
    FXE_LOG("[UC][CreateContext] ctx=%u flags=0x%08X", out.ctxId, in->flags);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodDestroyContext(IOExternalMethodArguments* args) {
    if (!args || args->scalarInputCount < 1) return kIOReturnBadArgument;
    uint32_t ctxId = (uint32_t)args->scalarInput[0];
    bool ok = fOwner->destroyContext(ctxId);
    FXE_LOG("[UC][DestroyContext] ctx=%u rc=%s", ctxId, ok ? "OK" : "NOT_FOUND");
    return ok ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodBindSurface(IOExternalMethodArguments* args) {
    if (!args || !args->structureInput || !args->structureOutput) return kIOReturnBadArgument;

    if (args->structureInputSize == sizeof(XEBindSurfaceIn) &&
        args->structureOutputSize >= sizeof(XEBindSurfaceOut)) {
        const XEBindSurfaceIn* in = (const XEBindSurfaceIn*)args->structureInput;
        XEBindSurfaceOut out = {};
        IOReturn kr = fOwner->bindSurface(in->ctxId, *in, out);
        if (kr != kIOReturnSuccess) return kr;

        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        FXE_LOG("[UC][BindSurfaceLegacy] ctx=%u ioSurfaceID=%u status=0x%X",
                in->ctxId, in->ioSurfaceID, out.status);
        return kIOReturnSuccess;
    }

    if (args->structureInputSize == sizeof(FXE_BindSurface_In) &&
        args->structureOutputSize >= sizeof(FXE_BindSurface_Out)) {
        const FXE_BindSurface_In* in = (const FXE_BindSurface_In*)args->structureInput;
        FXE_BindSurface_Out out = {};

        if (!IOSurfaceKextAvailable()) {
            out.rc = FXE_ENOSUPPORT;
            bcopy(&out, args->structureOutput, sizeof(out));
            args->structureOutputSize = sizeof(out);
            FXE_LOG("[UC][BindSurface] unsupported: IOSurface symbols unavailable");
            return kIOReturnUnsupported;
        }
        if (in->surfaceId == 0) {
            out.rc = FXE_EINVAL;
            bcopy(&out, args->structureOutput, sizeof(out));
            args->structureOutputSize = sizeof(out);
            FXE_LOG("[UC][BindSurface] invalid surfaceId=0");
            return kIOReturnBadArgument;
        }

        IOSurfaceRef surf = IOSurfaceLookup(in->surfaceId);
        if (!surf) {
            out.rc = FXE_ENOENT;
            bcopy(&out, args->structureOutput, sizeof(out));
            args->structureOutputSize = sizeof(out);
            FXE_LOG("[UC][BindSurface] lookup failed id=%u", in->surfaceId);
            return kIOReturnNotFound;
        }

        const uint32_t width = (uint32_t)IOSurfaceGetWidth(surf);
        const uint32_t height = (uint32_t)IOSurfaceGetHeight(surf);
        const uint32_t stride = (uint32_t)IOSurfaceGetBytesPerRow(surf);
        const uint32_t pixelFormat = 0;

        IOSurfaceRelease(surf);

        if (!width || !height || !stride) {
            out.rc = FXE_EINVAL;
            bcopy(&out, args->structureOutput, sizeof(out));
            args->structureOutputSize = sizeof(out);
            FXE_LOG("[UC][BindSurface] invalid metadata id=%u w=%u h=%u stride=%u",
                    in->surfaceId, width, height, stride);
            return kIOReturnBadArgument;
        }

        const uint64_t handle = makeSurfaceHandle(in->surfaceId);
        if (!storeSurfaceBinding(handle, in->surfaceId, width, height, pixelFormat, stride, in->flags)) {
            out.rc = FXE_EINTERNAL;
            bcopy(&out, args->structureOutput, sizeof(out));
            args->structureOutputSize = sizeof(out);
            FXE_LOG("[UC][BindSurface] failed to store binding handle=0x%llx",
                    (unsigned long long)handle);
            return kIOReturnError;
        }

        out.surfaceHandle = handle;
        out.width = width;
        out.height = height;
        out.pixelFormat = pixelFormat;
        out.stride = stride;
        out.rc = FXE_OK;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);

        FXE_LOG("[UC][BindSurface] id=%u handle=0x%llx w=%u h=%u stride=%u rc=0x%X",
                in->surfaceId,
                (unsigned long long)out.surfaceHandle,
                out.width,
                out.height,
                out.stride,
                out.rc);
        return kIOReturnSuccess;
    }

    return kIOReturnBadArgument;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodPresent(IOExternalMethodArguments* args) {
    if (!args) return kIOReturnBadArgument;

    if (args->scalarInputCount >= 1 && args->structureInputSize == 0) {
        const uint32_t ctxId = (uint32_t)args->scalarInput[0];
        FXE_LOG("[UC][PresentLegacy] intent ctx=%u", ctxId);
        IOReturn kr = fOwner->flush(ctxId);
        FXE_LOG("[UC][PresentLegacy] ack ctx=%u kr=0x%X", ctxId, kr);
        return kr;
    }

    if (!args->structureInput || !args->structureOutput ||
        args->structureInputSize != sizeof(FXE_Present_In) ||
        args->structureOutputSize < sizeof(FXE_Present_Out)) {
        return kIOReturnBadArgument;
    }

    const FXE_Present_In* in = (const FXE_Present_In*)args->structureInput;
    FXE_Present_Out out = {};

    uint32_t surfaceId = 0;
    if (!getSurfaceBinding(in->surfaceHandle, &surfaceId, nullptr, nullptr, nullptr, nullptr, nullptr)) {
        out.rc = FXE_ENOENT;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        FXE_LOG("[UC][Present] intent frameId=%llu handle=0x%llx rc=0x%X (missing handle)",
                (unsigned long long)in->frameId,
                (unsigned long long)in->surfaceHandle,
                out.rc);
        return kIOReturnNotFound;
    }

    if (!IOSurfaceKextAvailable()) {
        out.rc = FXE_ENOSUPPORT;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        FXE_LOG("[UC][Present] intent frameId=%llu handle=0x%llx rc=0x%X (iosurface unavailable)",
                (unsigned long long)in->frameId,
                (unsigned long long)in->surfaceHandle,
                out.rc);
        return kIOReturnUnsupported;
    }

    IOSurfaceRef surf = IOSurfaceLookup(surfaceId);
    if (!surf) {
        out.rc = FXE_ENOENT;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        FXE_LOG("[UC][Present] intent frameId=%llu handle=0x%llx rc=0x%X (surface vanished id=%u)",
                (unsigned long long)in->frameId,
                (unsigned long long)in->surfaceHandle,
                out.rc,
                surfaceId);
        return kIOReturnNotFound;
    }
    IOSurfaceRelease(surf);

    IOLockLock(fSurfaceLock);
    const uint64_t completion = ++fCompletionCounter;
    IOLockUnlock(fSurfaceLock);

    FXE_LOG("[UC][Present] intent frameId=%llu handle=0x%llx", (unsigned long long)in->frameId,
            (unsigned long long)in->surfaceHandle);
    FXE_LOG("[UC][Present] queued frameId=%llu completion=%llu", (unsigned long long)in->frameId,
            (unsigned long long)completion);

    (void)fOwner->flush(0);

    out.completionValue = completion;
    out.rc = FXE_OK;
    bcopy(&out, args->structureOutput, sizeof(out));
    args->structureOutputSize = sizeof(out);
    FXE_LOG("[UC][Present] ack frameId=%llu completion=%llu rc=0x%X",
            (unsigned long long)in->frameId,
            (unsigned long long)out.completionValue,
            out.rc);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodFenceTest(IOExternalMethodArguments* args) {
    uint32_t engine = 0;
    uint32_t timeoutMs = 2000;
    FXE_FenceTest_Out out = {};
    bool hasOut = false;

    if (args && args->structureInput && args->structureOutput &&
        args->structureInputSize == sizeof(FXE_FenceTest_In) &&
        args->structureOutputSize >= sizeof(FXE_FenceTest_Out)) {
        const FXE_FenceTest_In* in = (const FXE_FenceTest_In*)args->structureInput;
        engine = in->engine;
        timeoutMs = in->timeoutMs ? in->timeoutMs : 2000;
        hasOut = true;
    } else if (args && (args->structureInputSize != 0 || args->structureOutputSize != 0)) {
        return kIOReturnBadArgument;
    }

    FXE_LOG("[UC][FenceTest] submit engine=%u timeoutMs=%u", engine, timeoutMs);

    FakeIrisXEFramebuffer* fb = fOwner ? fOwner->getFramebufferOwner() : nullptr;
    if (!fb || !IsRangeReady(fOwner)) {
        out.pass = 0;
        out.reason = FXE_ENOTREADY;
        out.rc = FXE_ENOTREADY;
        if (hasOut) {
            bcopy(&out, args->structureOutput, sizeof(out));
            args->structureOutputSize = sizeof(out);
        }
        FXE_LOG("[UC][FenceTest] result=NOT_READY rc=0x%X", out.rc);
        return kIOReturnNotReady;
    }

    FakeIrisXEGEM* batchGem = fb->createTinyBatchGem();
    if (!batchGem) {
        out.pass = 0;
        out.reason = FXE_EINTERNAL;
        out.rc = FXE_EINTERNAL;
        if (hasOut) {
            bcopy(&out, args->structureOutput, sizeof(out));
            args->structureOutputSize = sizeof(out);
        }
        FXE_LOG("[UC][FenceTest] result=FAIL rc=0x%X (batch alloc)", out.rc);
        return kIOReturnNoMemory;
    }

    const bool ok = fOwner->fExeclistFromFB->submitBatchWithExeclist(
        fb,
        batchGem,
        0,
        fOwner->fRcsRingFromFB,
        timeoutMs);
    batchGem->release();

    out.pass = ok ? 1 : 0;
    out.reason = ok ? FXE_OK : FXE_ETIMEOUT;
    out.rc = out.reason;

    if (hasOut) {
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
    }

    FXE_LOG("[UC][FenceTest] wait result=%s rc=0x%X", ok ? "PASS" : "FAIL", out.rc);
    return ok ? kIOReturnSuccess : kIOReturnTimeout;
}

IOReturn FakeIrisXEAcceleratorUserClient::externalMethod(uint32_t selector,
                                                         IOExternalMethodArguments* args,
                                                         IOExternalMethodDispatch*,
                                                         OSObject*,
                                                         void*) {
    if (!fOwner) return kIOReturnNotReady;

    if (selector >= 8 || sDispatchTable[selector].function == nullptr) {
        FXE_LOG("[UC] unsupported selector=%u", selector);
        return kIOReturnUnsupported;
    }

    uint64_t t0 = mach_absolute_time();
    FXE_PHASE("UC", 200 + selector, "selector=%u inSz=%u outSz=%u scalarIn=%u",
              selector,
              args ? (uint32_t)args->structureInputSize : 0,
              args ? (uint32_t)args->structureOutputSize : 0,
              args ? (uint32_t)args->scalarInputCount : 0);

    IOReturn kr = sDispatchTable[selector].function(this, nullptr, args);

    uint64_t t1 = mach_absolute_time();
    FXE_LOG("[UC][selector=%u] rc=0x%X dur=%llu", selector, kr,
            (unsigned long long)(t1 - t0));
    return kr;
}
