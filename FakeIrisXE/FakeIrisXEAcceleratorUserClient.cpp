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

static bool IsRangeReady(const FakeIrisXEAccelerator* owner) {
    return owner && owner->fExeclistFromFB && owner->fRcsRingFromFB;
}

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(FakeIrisXEAcceleratorUserClient, IOUserClient)

const IOExternalMethodDispatch FakeIrisXEAcceleratorUserClient::sDispatchTable[8] = {
    {&FakeIrisXEAcceleratorUserClient::sGetCaps, 0, 0, 0, sizeof(FXE_VersionInfo)},
    {&FakeIrisXEAcceleratorUserClient::sCreateContext, 0, sizeof(FXE_CreateCtx_In), 0, sizeof(FXE_CreateCtx_Out)},
    {&FakeIrisXEAcceleratorUserClient::sDestroyContext, 0, sizeof(FXE_AttachShared_In), 0, sizeof(FXE_AttachShared_Out)},
    {&FakeIrisXEAcceleratorUserClient::sBindSurface, 0, sizeof(FXE_BindSurface_In), 0, sizeof(FXE_BindSurface_Out)},
    {&FakeIrisXEAcceleratorUserClient::sPresent, 0, sizeof(FXE_Present_In), 0, sizeof(FXE_Present_Out)},
    {nullptr, 0, 0, 0, 0},
    {nullptr, 0, 0, 0, 0},
    {&FakeIrisXEAcceleratorUserClient::sFenceTest, 0, sizeof(FXE_FenceTest_In), 0, sizeof(FXE_FenceTest_Out)},
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
    mCompletionCounter = 0;
    mNextCtxId = 1;
    mIOSurfaceEnabled = InitIOSurfaceSymbols();

    if (!fHandleTable || !fSurfaceStore.init()) {
        FXE_LOG("[UC] allocation failure handleTable=%p storeReady=%u",
                fHandleTable,
                fSurfaceStore.isReady() ? 1u : 0u);
        return false;
    }

    if (mIOSurfaceEnabled) {
        FXE_LOG("[IOSurface] ENABLED lookupOK=1");
    } else {
        FXE_LOG("[IOSurface] DISABLED lookupOK=0 IOSURF=0");
    }

    const uint32_t gtReady = IsRangeReady(fOwner) ? 1u : 0u;
    const uint32_t gucState = 0u;
    FXE_SUMMARY("IOSURF=%u UC=1 GT=%u GUC=%u err=0x%X",
                mIOSurfaceEnabled ? 1u : 0u,
                gtReady,
                gucState,
                0u);

    fOwner->setProperty("FakeIrisXEUCReady", kOSBooleanTrue);
    FXE_PHASE("UC", 101, "start ready owner=%p", fOwner);
    return true;
}

void FakeIrisXEAcceleratorUserClient::stop(IOService* provider) {
    FXE_PHASE("UC", 900, "stop enter provider=%p", provider);

    fSurfaceStore.clearAll();
    fSurfaceStore.free();

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
    fSurfaceStore.clearAll();
    terminate();
    return kIOReturnSuccess;
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
    if (!args || !args->structureOutput || args->structureOutputSize != sizeof(FXE_VersionInfo)) {
        return kIOReturnBadArgument;
    }

    FXE_VersionInfo out = {};
    out.abiMajor = FXE_ABI_MAJOR;
    out.abiMinor = FXE_ABI_MINOR;
    out.kextVersionPacked = FXE_KEXT_VERSION_PACKED;
    out.features = mIOSurfaceEnabled ? 0x1u : 0u;

    bcopy(&out, args->structureOutput, sizeof(out));
    args->structureOutputSize = sizeof(out);
    FXE_LOG("[UC][GetVersion] abi=%u.%u kext=0x%08X features=0x%X",
            out.abiMajor,
            out.abiMinor,
            out.kextVersionPacked,
            out.features);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodCreateContext(IOExternalMethodArguments* args) {
    if (!args || !args->structureInput || !args->structureOutput ||
        args->structureInputSize != sizeof(FXE_CreateCtx_In) ||
        args->structureOutputSize != sizeof(FXE_CreateCtx_Out)) {
        return kIOReturnBadArgument;
    }

    const FXE_CreateCtx_In* in = (const FXE_CreateCtx_In*)args->structureInput;
    FXE_CreateCtx_Out out = {};
    out.ctxId = fOwner ? fOwner->createContext(0, in->flags) : mNextCtxId++;
    out.rc = out.ctxId ? FXE_OK : FXE_EINTERNAL;

    bcopy(&out, args->structureOutput, sizeof(out));
    args->structureOutputSize = sizeof(out);
    FXE_LOG("[UC][CreateCtx] ctx=%u flags=0x%08X rc=0x%X", out.ctxId, in->flags, out.rc);
    return out.rc == FXE_OK ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodDestroyContext(IOExternalMethodArguments* args) {
    if (!args || !args->structureInput || !args->structureOutput ||
        args->structureInputSize != sizeof(FXE_AttachShared_In) ||
        args->structureOutputSize != sizeof(FXE_AttachShared_Out)) {
        return kIOReturnBadArgument;
    }

    FXE_AttachShared_Out out = {};
    out.rc = FXE_OK;
    out.magic = 0x53524558u;
    out.cap = 4064u;
    bcopy(&out, args->structureOutput, sizeof(out));
    args->structureOutputSize = sizeof(out);
    FXE_LOG("[UC][AttachShared] rc=0x%X magic=0x%X cap=%u", out.rc, out.magic, out.cap);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodBindSurface(IOExternalMethodArguments* args) {
    if (!args || !args->structureInput || !args->structureOutput ||
        args->structureInputSize != sizeof(FXE_BindSurface_In) ||
        args->structureOutputSize != sizeof(FXE_BindSurface_Out)) {
        return kIOReturnBadArgument;
    }

    const FXE_BindSurface_In* in = (const FXE_BindSurface_In*)args->structureInput;
    FXE_BindSurface_Out out = {};

    if (!mIOSurfaceEnabled) {
        out.rc = FXE_ENOSUPPORT;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return kIOReturnUnsupported;
    }

    FXE_SurfaceEntry meta = {};
    uint64_t handle = 0;
    if (!fSurfaceStore.bind(in->surfaceId, in->flags, &handle, &meta)) {
        out.rc = FXE_ENOENT;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return kIOReturnNotFound;
    }

    out.surfaceHandle = handle;
    out.width = meta.width;
    out.height = meta.height;
    out.pixelFormat = meta.pixelFormat;
    out.stride = meta.stride;
    out.rc = FXE_OK;

    bcopy(&out, args->structureOutput, sizeof(out));
    args->structureOutputSize = sizeof(out);

    FXE_LOG("[UC][BindSurface] id=%u handle=0x%llX %ux%u fmt=0x%X bpr=%u rc=0x%X",
            in->surfaceId,
            (unsigned long long)out.surfaceHandle,
            out.width,
            out.height,
            out.pixelFormat,
            out.stride,
            out.rc);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodPresent(IOExternalMethodArguments* args) {
    if (!args || !args->structureInput || !args->structureOutput ||
        args->structureInputSize != sizeof(FXE_Present_In) ||
        args->structureOutputSize != sizeof(FXE_Present_Out)) {
        return kIOReturnBadArgument;
    }

    const FXE_Present_In* in = (const FXE_Present_In*)args->structureInput;
    FXE_Present_Out out = {};

    if (!mIOSurfaceEnabled) {
        out.rc = FXE_ENOSUPPORT;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return kIOReturnUnsupported;
    }

    FXE_SurfaceEntry meta = {};
    if (!fSurfaceStore.lookup(in->surfaceHandle, &meta)) {
        out.rc = FXE_ENOENT;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return kIOReturnNotFound;
    }

    IOSurfaceRef surf = IOSurfaceLookup(meta.surfaceId);
    if (!surf) {
        out.rc = FXE_ENOENT;
        bcopy(&out, args->structureOutput, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return kIOReturnNotFound;
    }
    IOSurfaceRelease(surf);

    const uint64_t completion = ++mCompletionCounter;

    out.completionValue = completion;
    out.rc = FXE_OK;
    bcopy(&out, args->structureOutput, sizeof(out));
    args->structureOutputSize = sizeof(out);
    FXE_LOG("[UC][Present] handle=0x%llX frameId=%llu -> completion=%llu rc=0x%X",
            (unsigned long long)in->surfaceHandle,
            (unsigned long long)in->frameId,
            (unsigned long long)out.completionValue,
            out.rc);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::methodFenceTest(IOExternalMethodArguments* args) {
    if (!args || !args->structureInput || !args->structureOutput ||
        args->structureInputSize != sizeof(FXE_FenceTest_In) ||
        args->structureOutputSize != sizeof(FXE_FenceTest_Out)) {
        return kIOReturnBadArgument;
    }

    const FXE_FenceTest_In* in = (const FXE_FenceTest_In*)args->structureInput;
    FXE_FenceTest_Out out = {};
    out.pass = 0;
    out.reason = FXE_ENOTREADY;
    out.rc = FXE_ENOTREADY;
    bcopy(&out, args->structureOutput, sizeof(out));
    args->structureOutputSize = sizeof(out);

    if (!IsRangeReady(fOwner)) {
        FXE_LOG("[UC][FenceTest] GT not ready; engine=%u -> NOT_READY", in->engine);
        return kIOReturnNotReady;
    }

    FXE_LOG("[UC][FenceTest] submission path not wired; engine=%u -> NOT_READY", in->engine);
    return kIOReturnNotReady;
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

    FXE_LOG("[UC] externalMethod selector=%u inSz=%u outSz=%u",
            selector,
            args ? (unsigned)args->structureInputSize : 0u,
            args ? (unsigned)args->structureOutputSize : 0u);

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
