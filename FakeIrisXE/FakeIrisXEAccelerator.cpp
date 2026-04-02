#include "FakeIrisXEAccelerator.hpp"
#include "FakeIrisXEAccelShared.h"
#include "FakeIrisXEFramebuffer.hpp"
#include "FakeIrisXEFenceRetirement.hpp"
#include "FakeIrisXEIOSurfaceManager.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>



// cheap forward-declare (only if header isn't available)
typedef struct __IOSurface * IOSurfaceRef;
extern "C" IOSurfaceRef IOSurfaceLookup(uint32_t);
extern "C" int IOSurfaceLock(IOSurfaceRef, uint32_t, void *);
extern "C" int IOSurfaceUnlock(IOSurfaceRef, uint32_t, void *);
extern "C" void *IOSurfaceGetBaseAddress(IOSurfaceRef);
extern "C" size_t IOSurfaceGetBytesPerRow(IOSurfaceRef);
extern "C" size_t IOSurfaceGetWidth(IOSurfaceRef);
extern "C" size_t IOSurfaceGetHeight(IOSurfaceRef);
extern "C" void IOSurfaceRelease(IOSurfaceRef);





#define LOG(fmt, ...) IOLog("(FakeIrisXEFramebuffer) [Accel] " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(FakeIrisXEAccelerator, IOService)


static uint32_t readFenceSeqnoFromFramebuffer(void* ctx)
{
    FakeIrisXEAccelerator* accel = static_cast<FakeIrisXEAccelerator*>(ctx);
    if (!accel) return 0;

    FakeIrisXEFramebuffer* fb = accel->framebuffer();
    if (!fb) return 0;

    return fb->readCompletedFenceSeq();
}

#pragma mark - Init / Probe

bool FakeIrisXEAccelerator::init(OSDictionary* dict) {
    if (!IOService::init(dict)) return false;
    LOG("init");

    fFB        = nullptr;
    fHdr       = nullptr;
    fRingBase  = nullptr;
    fSharedMem = nullptr;
    fPixels    = nullptr;
    fWL        = nullptr;
    fTimer     = nullptr;
    fenceManager = nullptr;
    fFenceSeqno = 0;
    fSurfaceMgr = nullptr;
    fContexts  = OSArray::withCapacity(8);
    fCtxLock   = IOLockAlloc();
    fNextCtxId = 1;

    return true;
}

IOService* FakeIrisXEAccelerator::probe(IOService* provider, SInt32* score) {
    LOG("probe()");
    if (score) *score += 500;
    return this;
}



#pragma mark - Start

bool FakeIrisXEAccelerator::start(IOService* provider) {
    LOG("start() attaching to framebuffer");

    if (!IOService::start(provider)) {
        LOG("IOService::start failed");
        return false;
    }

    fFB = OSDynamicCast(FakeIrisXEFramebuffer, provider);
    if (!fFB) {
        LOG("provider is not FakeIrisXEFramebuffer");
        return false;
    }
    fFramebuffer = fFB;

    if (!fWL) {
        fWL = getWorkLoop();
        if (fWL) fWL->retain();
    }

    if (!fenceManager) {
        fenceManager = FakeIrisXEFenceManager::create(fWL);
        if (fenceManager) {
            fenceManager->setSeqnoReader(readFenceSeqnoFromFramebuffer, this);
            IOLog("(FakeIrisXEFramebuffer) [Accel] fence manager initialized\n");
        }
    }

    if (!fSurfaceMgr) {
        fSurfaceMgr = FakeIrisXEIOSurfaceManager::create();
        if (fSurfaceMgr) {
            IOLog("(FakeIrisXEFramebuffer) [Accel] IOSurface manager initialized\n");
        }
    }

    // V148: Comprehensive startup diagnostics
    IOLog("(FakeIrisXEAccelerator)[V148] ============================================\n");
    IOLog("(FakeIrisXEAccelerator)[V148] FAKEIRISXE METAL ACCELERATOR STARTING\n");
    IOLog("(FakeIrisXEAccelerator)[V148] ============================================\n");
    
    // Keep accelerator-facing readiness conservative until there is real
    // fence-backed execution proof. The service can exist for diagnostics
    // without advertising that Metal/IOGPU are usable.
    setProperty("MetalSupported", kOSBooleanFalse);
    setProperty("MetalPlugin", kOSBooleanFalse);
    setProperty("IOAccelReady", kOSBooleanFalse);
    setProperty("IOGPU", kOSBooleanFalse);
    setProperty("FakeIrisXEAcceleratorAttached", kOSBooleanTrue);
    setProperty("FakeIrisXEAcceleratorLinked", kOSBooleanFalse);

    IOLog("(FakeIrisXEAccelerator)[V148] Properties set:\n");
    IOLog("(FakeIrisXEAccelerator)[V148]   MetalSupported = false (pending)\n");
    IOLog("(FakeIrisXEAccelerator)[V148]   MetalPlugin = false (pending)\n");
    IOLog("(FakeIrisXEAccelerator)[V148]   IOAccelReady = false (pending)\n");
    IOLog("(FakeIrisXEAccelerator)[V148]   IOGPU = false (pending)\n");

    // framebuffer basics
    fW      = fFB->getWidth();
    fH      = fFB->getHeight();
    fStride = fFB->getStride();
    fPixels = fFB->getFramebufferKernelPtr();

    IOLog("(FakeIrisXEAccelerator)[V148] Framebuffer info:\n");
    IOLog("(FakeIrisXEAccelerator)[V148]   Resolution: %ux%u\n", fW, fH);
    IOLog("(FakeIrisXEAccelerator)[V148]   Stride: %u bytes\n", fStride);
    IOLog("(FakeIrisXEAccelerator)[V148] ============================================\n");

    /*
    // timer (inside start)
    if (!fWL) {
        fWL = getWorkLoop();
        if (!fWL) fWL = IOWorkLoop::workLoop();
        if (fWL) fWL->retain();
    }

    if (!createAndArmTimer(this, fWL, fTimer, 16)) {
        IOLog("(FakeIrisXEFramebuffer) [Accel] start(): failed to create timer\n");
    } else {
        IOLog("(FakeIrisXEFramebuffer) [Accel] start(): timer created\n");
    }
*/
    
    

    if (fFB->getExeclist() && fFB->getRcsRing()) {
        linkFromFramebuffer(fFB);
    } else {
        setProperty("FakeIrisXELinkPending", kOSBooleanTrue);
        IOLog("(FakeIrisXEFramebuffer) [Accel] deferring link until execlist/ring are ready\n");
    }

    registerService(kIOServiceSynchronous);
    LOG("started; waiting for accelerator user client methods");

    return true;
}

#pragma mark - Stop

void FakeIrisXEAccelerator::stop(IOService* provider) {
    LOG("stop");

    if (fenceManager) {
        fenceManager->release();
        fenceManager = nullptr;
    }

    if (fSurfaceMgr) {
        fSurfaceMgr->release();
        fSurfaceMgr = nullptr;
    }

    if (fTimer) {
        fTimer->cancelTimeout();
        if (fWL) fWL->removeEventSource(fTimer);
        fTimer->release();
        fTimer = nullptr;
    }

    if (fWL) {
        fWL->release();
        fWL = nullptr;
    }

    if (fSharedMem) {
        fSharedMem->release();
        fSharedMem = nullptr;
        fHdr = nullptr;
        fRingBase = nullptr;
    }

    if (fContexts) { fContexts->release(); fContexts = nullptr; }
    if (fCtxLock) { IOLockFree(fCtxLock); fCtxLock = nullptr; }

    fFB = nullptr;
    fFramebuffer = nullptr;
    IOService::stop(provider);
}

uint32_t FakeIrisXEAccelerator::nextSeqNo()
{
    return (uint32_t)__sync_add_and_fetch(&fFenceSeqno, 1);
}

uint32_t FakeIrisXEAccelerator::submitBatchWithFence(FakeIrisXEGEM* batchGem, uint32_t flags)
{
    (void)flags;

    FakeIrisXEFramebuffer* fb = getFramebufferOwner();
    if (!fb || !batchGem) {
        return 0;
    }

    return fb->appendFenceAndSubmit(batchGem, 0, 0);
}



#pragma mark - attachShared (UserClient provides ring)

bool FakeIrisXEAccelerator::attachShared(IOBufferMemoryDescriptor* page) {
    if (!page) return false;

    if (fSharedMem) { fSharedMem->release(); }

    page->retain();
    fSharedMem = page;

    void* base = fSharedMem->getBytesNoCopy();
    if (!base) {
        LOG("attachShared: null base");
        return false;
    }

    volatile XEHdr* hdr = reinterpret_cast<volatile XEHdr*>(base);
    if (hdr->magic != XE_MAGIC || hdr->version != XE_VERSION) {
        LOG("attachShared: BAD HEADER (magic=0x%08x ver=%u)", hdr->magic, hdr->version);
        return false;
    }

    fHdr = hdr;
    fRingBase = reinterpret_cast<uint8_t*>(base) + sizeof(XEHdr);

    LOG("attachShared: OK (magic=0x%08x cap=%u)", hdr->magic, hdr->capacity);

    // accelerate polling to 5ms once ring is live
    if (fTimer) fTimer->setTimeoutMS(5);

    return true;
}

#pragma mark - Contexts

FakeIrisXEAccelerator::XEContext* FakeIrisXEAccelerator::lookupContext(uint32_t ctxId)
{
    if (!fContexts) return nullptr;

    for (unsigned i = 0; i < fContexts->getCount(); ++i)
    {
        OSData* d = OSDynamicCast(OSData, fContexts->getObject(i));
        if (!d) continue;

        XEContext* ctx = (XEContext*)d->getBytesNoCopy();
        if (ctx && ctx->ctxId == ctxId)
            return ctx;
    }
    return nullptr;
}


uint32_t FakeIrisXEAccelerator::createContext(uint64_t sharedPtr, uint32_t flags)
{
    XEContext ctx{};
    ctx.ctxId = fNextCtxId++;
    ctx.active = true;
    ctx.sharedGPUPtr = sharedPtr;

    // Wrap context struct in OSData
    OSData* data = OSData::withBytes(&ctx, sizeof(ctx));
    if (!data) return 0;

    fContexts->setObject(data);
    data->release(); // OSArray retains it

    LOG("createContext ctxId=%u", ctx.ctxId);
    return ctx.ctxId;
}



#pragma mark - Poll Ring

// in FakeIrisXEAccelerator.cpp
#define MAX_PROC_PER_TICK 4   // small, safe
#define POLL_MS 16

void FakeIrisXEAccelerator::pollRing(IOTimerEventSource* sender)
{
    if (!fHdr || !fRingBase) {
        if (sender) sender->setTimeoutMS(250);
        return;
    }

    // Reentrancy guard: if already processing, just reschedule and return.
    if (OSCompareAndSwap(false, true, (volatile int*)&fPollActive) == false) {
        // couldn't swap (already true) -> someone else processing
        if (sender) sender->setTimeoutMS(POLL_MS);
        return;
    }

    // Snapshot head/tail/cap quickly
    uint32_t head = fHdr->head;
    uint32_t tail = fHdr->tail;
    uint32_t cap  = fHdr->capacity;

    // Nothing to do
    if (head == tail) {
        fPollActive = false;
        if (sender) sender->setTimeoutMS(POLL_MS);
        return;
    }

    IOLog("(FakeIrisXEFramebuffer) [Accel] pollRing(): head=%u tail=%u cap=%u\n", head, tail, cap);

    uint32_t processed = 0;
    while (tail != head && processed < MAX_PROC_PER_TICK) {
        // read header safely (handle wrap)
        XECmd cmd;
        uint32_t hdr_off = tail;
        if (hdr_off + sizeof(cmd) <= cap) {
            memcpy(&cmd, fRingBase + hdr_off, sizeof(cmd));
        } else {
            uint32_t first = cap - hdr_off;
            memcpy(&cmd, fRingBase + hdr_off, first);
            memcpy(((uint8_t*)&cmd) + first, fRingBase, sizeof(cmd) - first);
        }

        if (cmd.bytes > cap) {
            IOLog("(FakeIrisXEFramebuffer) [Accel] pollRing: invalid cmd.bytes=%u > cap\n", cmd.bytes);
            break;
        }

        uint32_t total = xe_align(sizeof(XECmd) + cmd.bytes);
        uint32_t payload_off = (tail + sizeof(XECmd)) % cap;

        // small local buffer to copy payload (prevents reading ring while userspace writes)
        uint8_t payloadBuf[256]; // ensure your protocol limits < sizeof(payloadBuf)
        if (cmd.bytes > sizeof(payloadBuf)) {
            IOLog("(FakeIrisXEFramebuffer) [Accel] pollRing: cmd too large %u\n", cmd.bytes);
            break;
        }

        if (payload_off + cmd.bytes <= cap) {
            memcpy(payloadBuf, fRingBase + payload_off, cmd.bytes);
        } else {
            uint32_t first = cap - payload_off;
            memcpy(payloadBuf, fRingBase + payload_off, first);
            memcpy(payloadBuf + first, fRingBase, cmd.bytes - first);
        }

        // Minimal logging (do NOT hex-dump the whole payload here)
        IOLog("(FakeIrisXEFramebuffer) [Accel] pollRing: opcode=%u bytes=%u ctx=%u\n",
              cmd.opcode, cmd.bytes, cmd.ctxId);

        // Call processor but avoid heavy/blocking ops here
        // processCommand should be lightweight (see next section)
        processCommand(cmd, payloadBuf, cmd.bytes);

        // advance tail and publish
        tail = (tail + total) % cap;
        fHdr->tail = tail;
        OSSynchronizeIO();

        ++processed;
    }

    // If more work remains, reschedule quickly; otherwise use normal poll interval
    head = fHdr->head;
    if (tail != head) {
        if (sender) sender->setTimeoutMS(POLL_MS);
    } else {
        if (sender) sender->setTimeoutMS(50);
    }

    fPollActive = false; // release guard
}




void FakeIrisXEAccelerator::processCommand(const XECmd &cmd, const void* payload, uint32_t payloadBytes)
{
    IOLog("(FakeIrisXEFramebuffer) [Accel] processCommand: opcode=%u bytes=%u ctx=%u\n",
          cmd.opcode, payloadBytes, cmd.ctxId);

    switch (cmd.opcode) {
        
        
        case XE_CMD_CLEAR:
            if (payloadBytes >= 4) {
                uint32_t color;
                memcpy(&color, payload, 4);
                // perform only memory writes here (fast)
                if (fPixels && fStride) {
                    // Simplified: fill first pixel block (keep cheap)
                    uint32_t *pix = reinterpret_cast<uint32_t*>(fPixels);
                    for (size_t i=0; i < (fW * fH) && i < 4096; ++i) pix[i] = color;
                }
                // Mark that a flush is required; let the framebuffer do actual flush on its workloop
                fNeedFlush = true;
            }
            break;
        
            
            
        
        case XE_CMD_RECT:
            if (payloadBytes < sizeof(XERectPayload)) {
                IOLog("(FakeIrisXEFramebuffer) [Accel] RECT: invalid payload (%u bytes)\n",
                      payloadBytes);
                break;
            }

            {
                XERectPayload p = {};
                memcpy(&p, payload, sizeof(p));

                // Clamp region safely
                uint32_t x0 = MIN(p.x, fW);
                uint32_t y0 = MIN(p.y, fH);
                uint32_t x1 = MIN(p.x + p.w, fW);
                uint32_t y1 = MIN(p.y + p.h, fH);
                uint32_t width  = (x1 > x0) ? (x1 - x0) : 0;
                uint32_t height = (y1 > y0) ? (y1 - y0) : 0;

                IOLog("(FakeIrisXEFramebuffer) [Accel] RECT %u x %u at (%u,%u)\n",
                      width, height, x0, y0);

                if (width == 0 || height == 0 || !fPixels || fStride == 0)
                    break;

                // Write only a SAFE amount of pixels per timer tick.
                // Avoid touching entire screen → no freezes.
                const size_t MAX_PER_TICK = 250000; // ~1MB, safe
                size_t count = 0;

                for (uint32_t yy = y0; yy < y1; ++yy) {
                    uint32_t* row = reinterpret_cast<uint32_t*>(
                        reinterpret_cast<uint8_t*>(fPixels) + yy * fStride
                    );
                    for (uint32_t xx = x0; xx < x1; ++xx) {
                        row[xx] = p.colorARGB;

                        if (++count >= MAX_PER_TICK) {
                            // Don’t freeze system — stop early
                            yy = y1;
                            xx = x1;
                            break;
                        }
                    }
                }

                // request flush later
                fNeedFlush = true;
            }
            break;
            
            
        case XE_CMD_PRESENT:
        {
            IOLockLock(fCtxLock);
            XEContext* ctx = lookupContext(cmd.ctxId);

            if (!ctx) {
                IOLockUnlock(fCtxLock);
                IOLog("(FakeIrisXEFramebuffer) [Accel] PRESENT: ctx %u not found\n", cmd.ctxId);
                break;
            }

            if (!ctx->surfCPU || ctx->surfRowBytes == 0 ||
                ctx->surfWidth == 0 || ctx->surfHeight == 0)
            {
                IOLockUnlock(fCtxLock);
                IOLog("(FakeIrisXEFramebuffer) [Accel] PRESENT: invalid surface for ctx %u\n", cmd.ctxId);
                break;
            }

            uint8_t* srcBase = (uint8_t*)ctx->surfCPU;
            uint32_t srcRB   = ctx->surfRowBytes;
            uint32_t srcW    = ctx->surfWidth;
            uint32_t srcH    = ctx->surfHeight;

            IOLockUnlock(fCtxLock);

            if (!fPixels || !fStride) {
                IOLog("(FakeIrisXEFramebuffer) [Accel] PRESENT: framebuffer pixels missing\n");
                break;
            }

            // Clip copy area to framebuffer bounds
            uint32_t copyW = MIN(fW, srcW);
            uint32_t copyH = MIN(fH, srcH);

            uint8_t* dstBase = (uint8_t*)fPixels;
            uint32_t dstRB   = fStride;

            // ARGB8888 fast memcpy per row
            for (uint32_t y = 0; y < copyH; ++y) {
                memcpy(dstBase + y * dstRB,
                       srcBase + y * srcRB,
                       copyW * 4 /* bytes per pixel */);
            }

            // Request a flush, but do NOT block in timer thread
            fNeedFlush = true;

            IOLog("(FakeIrisXEFramebuffer) [Accel] PRESENT OK ctx=%u (%ux%u)\n",
                  cmd.ctxId, copyW, copyH);
            break;
        }

            
            
        default:
            IOLog("(FakeIrisXEFramebuffer) [Accel] unknown opcode %u\n", cmd.opcode);
    }
    
}




#pragma mark - Primitive ops

void FakeIrisXEAccelerator::cmdClear(uint32_t argb) {
    uint8_t* base = (uint8_t*)fPixels;
    for (uint32_t y = 0; y < fH; ++y) {
        uint32_t* row = (uint32_t*)(base + y * fStride);
        for (uint32_t x = 0; x < fW; ++x) row[x] = argb;
    }
}



void FakeIrisXEAccelerator::cmdRect(const XERectPayload& p) {
    uint32_t x0 = (p.x < fW ? p.x : fW);
    uint32_t y0 = (p.y < fH ? p.y : fH);
    uint32_t x1 = (x0 + p.w > fW ? fW : x0 + p.w);
    uint32_t y1 = (y0 + p.h > fH ? fH : y0 + p.h);

    uint8_t* base = (uint8_t*)fPixels + y0 * fStride + x0 * 4;
    for (uint32_t row = 0; row < (y1 - y0); ++row) {
        uint32_t* r = (uint32_t*)(base + row * fStride);
        for (uint32_t x = 0; x < (x1 - x0); ++x) r[x] = p.colorARGB;
    }
}



void FakeIrisXEAccelerator::cmdCopy(const XECopyPayload& p) {
    uint32_t w = p.w, h = p.h;

    if (p.sx + w > fW) w = fW - p.sx;
    if (p.sy + h > fH) h = fH - p.sy;
    if (p.dx + w > fW) w = fW - p.dx;
    if (p.dy + h > fH) h = fH - p.dy;

    uint8_t* base = (uint8_t*)fPixels;
    for (uint32_t row = 0; row < h; ++row) {
        uint8_t* src = base + (p.sy + row) * fStride + p.sx * 4;
        uint8_t* dst = base + (p.dy + row) * fStride + p.dx * 4;
        bcopy(src, dst, w * 4);
    }
}


// Replace current bindSurface implementation with this (in FakeIrisXEAccelerator.cpp)
IOReturn FakeIrisXEAccelerator::bindSurface(uint32_t ctxId, const XEBindSurfaceIn& in, XEBindSurfaceOut& out)
{
    if (!fCtxLock) return kIOReturnNoResources;

    uint64_t gpuAddr = in.gpuAddr;
    FakeIrisXEGEM* surfaceGem = nullptr;
    const uint32_t surfaceKey = in.ioSurfaceID ? in.ioSurfaceID : in.surfaceID;
    if (surfaceKey && fSurfaceMgr) {
        surfaceGem = fSurfaceMgr->getSurfaceGem(surfaceKey);
        if (surfaceGem && !gpuAddr && fFB) {
            gpuAddr = surfaceGem->gpuAddress();
            if (!gpuAddr) {
                gpuAddr = fFB->ggttMap(surfaceGem);
            }
        }
    }

    IOLockLock(fCtxLock);
    XEContext* ctx = lookupContext(ctxId);
    if (!ctx) {
        IOLockUnlock(fCtxLock);
        if (surfaceGem) surfaceGem->release();
        return kIOReturnNotFound;
    }

    // Store metadata reported by user-space
    ctx->hasSurface       = true;
    ctx->surfWidth        = in.width;
    ctx->surfHeight       = in.height;
    ctx->surfRowBytes     = in.bytesPerRow;
    ctx->surfPixelFormat  = in.pixelFormat;
    ctx->surfIOSurfaceID  = in.ioSurfaceID;
    ctx->surfID           = in.surfaceID;

    // IMPORTANT: user-space must pass a pointer that's already mapped into the client task
    // (for testing we accept that pointer value and store it).
    // We save as void* kernel-side, but it points into the client's address space.
    ctx->surfCPU = reinterpret_cast<void*>((uintptr_t)in.cpuPtr);
    if (!ctx->surfCPU && surfaceGem && surfaceGem->memoryDescriptor()) {
        ctx->surfCPU = surfaceGem->memoryDescriptor()->getBytesNoCopy();
    }

    IOLockUnlock(fCtxLock);

    out.gpuAddr = gpuAddr;
    out.status  = gpuAddr ? kIOReturnSuccess : kIOReturnUnsupported;

    IOLog("(FakeIrisXEFramebuffer) [Accel] BindSurface: ctx=%u iosurf=%u cpuPtr=%p %ux%u stride=%u fmt=0x%08x\n",
          ctxId, in.ioSurfaceID, ctx->surfCPU, in.width, in.height, in.bytesPerRow, in.pixelFormat);
    IOLog("(FakeIrisXEFramebuffer) [Accel] BindSurface: gpuAddr=0x%llx status=0x%x key=%u\n",
          (unsigned long long)gpuAddr,
          out.status,
          surfaceKey);

    if (surfaceGem) {
        surfaceGem->release();
    }

    return out.status;
}


// Start worker loop / timer (idempotent)
void FakeIrisXEAccelerator::startWorkerLoop()
{
    if (!fWL) {
        fWL = getWorkLoop();
        if (!fWL) fWL = IOWorkLoop::workLoop();
        if (!fWL) {
            IOLog("(FakeIrisXEFramebuffer) [Accel] startWorkerLoop: no workloop\n");
            return;
        }
        fWL->retain();
    }

    if (!createAndArmTimer(this, fWL, fTimer, 16)) {
        IOLog("(FakeIrisXEFramebuffer) [Accel] startWorkerLoop: failed to create/arm timer\n");
        return;
    }

    IOLog("(FakeIrisXEFramebuffer) [Accel] startWorkerLoop: timer armed\n");
}







// Fill capabilities
void FakeIrisXEAccelerator::getCaps(XEAccelCaps& out)
{
    bzero(&out, sizeof(out));
    out.version = XE_VERSION;
    out.metalSupported = 0; // flip to 1 if you wire Metal later
    out.reserved0 = out.reserved1 = 0;
}

// Flush -> call FB flush if present
IOReturn FakeIrisXEAccelerator::flush(uint32_t ctxId)
{
    if (fFB) {
        fFB->flushDisplay();
        return kIOReturnSuccess;
    }
    return kIOReturnNotReady;
}

// Bind an IOSurface id to a context (simple bookkeeping)
IOReturn FakeIrisXEAccelerator::bindSurfaceToContext(uint32_t ctxId, uint32_t surfID)
{
    if (!fCtxLock) return kIOReturnNoResources;
    IOLockLock(fCtxLock);
    XEContext* ctx = lookupContext(ctxId);
    if (!ctx) {
        IOLockUnlock(fCtxLock);
        IOLog("(FakeIrisXEFramebuffer) [Accel] bindSurfaceToContext: ctx %u not found\n", ctxId);
        return kIOReturnNotFound;
    }
    ctx->surfID = surfID;
    // leave surfCPU / mapping to a later IOSurface path
    ctx->hasSurface = true;
    IOLockUnlock(fCtxLock);
    IOLog("(FakeIrisXEFramebuffer) [Accel] bindSurfaceToContext: ctx=%u iosurf=%u\n", ctxId, surfID);
    return kIOReturnSuccess;
}



// --------- Helpers: init/free contexts ----------
bool FakeIrisXEAccelerator::initContexts()
{
    contextsLock = IOLockAlloc();
    if (!contextsLock) return false;
    contexts = OSArray::withCapacity(8);
    if (!contexts) {
        IOLockFree(contextsLock);
        contextsLock = nullptr;
        return false;
    }
    nextCtxId = 1;
    return true;
}

void FakeIrisXEAccelerator::freeContexts()
{
    if (!contexts) return;
    IOLockLock(contextsLock);
    unsigned count = contexts->getCount();
    for (unsigned i = 0; i < count; ++i) {
        OSData *d = OSDynamicCast(OSData, contexts->getObject(i));
        if (!d) continue;
        XECtx *c = (XECtx*)d->getBytesNoCopy();
        if (c) {
            IOFree(c, sizeof(XECtx));
        }
    }
    contexts->flushCollection();
    IOLockUnlock(contextsLock);
    contexts->release();
    contexts = nullptr;
    if (contextsLock) {
        IOLockFree(contextsLock);
        contextsLock = nullptr;
    }
}

// call initContexts() from your start() after basic init
// call freeContexts() from stop()

// ---------- create / destroy / find ----------
uint32_t FakeIrisXEAccelerator::createContext()
{
    if (!contexts) return 0;
    XECtx *c = (XECtx*)IOMallocZero(sizeof(XECtx));
    if (!c) return 0;
    IOLockLock(contextsLock);
    c->ctxId = nextCtxId++;
    c->alive = true;
    c->surf_vaddr = 0;
    c->surf_bytes = 0;
    c->surf_rowbytes = 0;
    c->surf_w = 0;
    c->surf_h = 0;
    OSData *d = OSData::withBytesNoCopy(c, sizeof(XECtx)); // contexts will own pointer
    if (d) {
        contexts->setObject(d);
        d->release();
    } else {
        IOFree(c, sizeof(XECtx));
        IOLockUnlock(contextsLock);
        return 0;
    }
    IOLockUnlock(contextsLock);
    IOLog("(FakeIrisXEFramebuffer) [Accel] createContext -> %u\n", c->ctxId);
    return c->ctxId;
}

bool FakeIrisXEAccelerator::destroyContext(uint32_t ctxId)
{
    if (!contexts) return false;
    IOLockLock(contextsLock);
    for (unsigned i = 0; i < contexts->getCount(); ++i) {
        OSData *d = OSDynamicCast(OSData, contexts->getObject(i));
        if (!d) continue;
        XECtx *c = (XECtx*)d->getBytesNoCopy();
        if (c && c->ctxId == ctxId) {
            // clear any bound mapping info but don't free user memory
            c->surf_vaddr = 0;
            c->surf_bytes = 0;
            c->surf_rowbytes = 0;
            c->surf_w = 0;
            c->surf_h = 0;
            contexts->removeObject(i);
            IOFree(c, sizeof(XECtx));
            IOLockUnlock(contextsLock);
            IOLog("(FakeIrisXEFramebuffer) [Accel] destroyContext %u\n", ctxId);
            return true;
        }
    }
    IOLockUnlock(contextsLock);
    return false;
}

XECtx* FakeIrisXEAccelerator::findCtx(uint32_t ctxId)
{
    if (!contexts) return nullptr;
    XECtx *res = nullptr;
    IOLockLock(contextsLock);
    for (unsigned i = 0; i < contexts->getCount(); ++i) {
        OSData *d = OSDynamicCast(OSData, contexts->getObject(i));
        if (!d) continue;
        XECtx *c = (XECtx*)d->getBytesNoCopy();
        if (c && c->ctxId == ctxId && c->alive) {
            res = c;
            break;
        }
    }
    IOLockUnlock(contextsLock);
    return res;
}

// ---------- bind surface: user mapped fallback ----------
bool FakeIrisXEAccelerator::bindSurface_UserMapped(uint32_t ctxId,
                                                   const void* userPtr,
                                                   size_t bytes,
                                                   uint32_t rowbytes,
                                                   uint32_t w,
                                                   uint32_t h)
{
    XECtx *c = findCtx(ctxId);
    if (!c) { IOLog("(FakeIrisXEFramebuffer) [Accel] bindSurface: no ctx %u\n", ctxId); return false; }

    // The user must have already mapped the memory into kernel addressable memory via clientMemory mapping.
    // We accept the userPtr as kernel-visible pointer. Validate basic fields.
    if (!userPtr || bytes == 0 || rowbytes == 0 || w == 0 || h == 0) {
        IOLog("(FakeIrisXEFramebuffer) [Accel] bindSurface: bad args\n");
        return false;
    }

    // store mapping
    c->surf_vaddr = (uintptr_t)userPtr;
    c->surf_bytes = bytes;
    c->surf_rowbytes = rowbytes;
    c->surf_w = w;
    c->surf_h = h;

    IOLog("(FakeIrisXEFramebuffer) [Accel] bindSurface_UserMapped ctx=%u vaddr=%p bytes=%zu rowbytes=%u %ux%u\n",
          ctxId, (void*)c->surf_vaddr, c->surf_bytes, c->surf_rowbytes, c->surf_w, c->surf_h);

    return true;
}

// ---------- present: copy into framebuffer and schedule flush ----------
bool FakeIrisXEAccelerator::presentContext(uint32_t ctxId)
{
    XECtx *c = findCtx(ctxId);
    if (!c) { IOLog("(FakeIrisXEFramebuffer) [Accel] present: no ctx %u\n", ctxId); return false; }

    if (!c->surf_vaddr || c->surf_bytes == 0) {
        IOLog("(FakeIrisXEFramebuffer) [Accel] present: no surface bound for ctx %u\n", ctxId);
        return false;
    }

    // validate we have framebuffer pointers available
    if (!fPixels || fStride == 0 || fW == 0 || fH == 0) {
        IOLog("(FakeIrisXEFramebuffer) [Accel] present: framebuffer not ready\n");
        return false;
    }

    // Perform safe line-by-line copy (clamped)
    const uint8_t *srcBase = (const uint8_t*)(uintptr_t)c->surf_vaddr;
    uint8_t *dstBase = (uint8_t*)fPixels;

    uint32_t copy_w = (c->surf_w < fW) ? c->surf_w : fW;
    uint32_t copy_h = (c->surf_h < fH) ? c->surf_h : fH;

    // protect against overflow: ensure rowbytes * copy_h <= surf_bytes
    uint64_t required = (uint64_t)c->surf_rowbytes * (uint64_t)copy_h;
    if (required > c->surf_bytes) {
        IOLog("(FakeIrisXEFramebuffer) [Accel] present: surface size mismatch (need %llu bytes have %zu)\n",
              (unsigned long long)required, c->surf_bytes);
        // clamp height to available rows
        uint32_t max_h = (uint32_t)(c->surf_bytes / c->surf_rowbytes);
        if (max_h < copy_h) copy_h = max_h;
    }

    // Copy each row - assume 4 bytes/pixel (ARGB8888) — adjust if you support other formats
    for (uint32_t y = 0; y < copy_h; ++y) {
        const uint8_t *src = srcBase + (size_t)y * c->surf_rowbytes;
        uint8_t *dst = dstBase + (size_t)y * fStride;
        // copy copy_w * 4 bytes, but ensure not to exceed row sizes:
        size_t bytes_to_copy = (size_t)copy_w * 4;
        // clamp to the minimum of src row and dst row
        if (bytes_to_copy > c->surf_rowbytes) bytes_to_copy = c->surf_rowbytes;
        if (bytes_to_copy > fStride) bytes_to_copy = fStride;
        memcpy(dst, src, bytes_to_copy);        // NOTE: some kext environments / sandbox may require memory barriers; memcpy is usually fine
    }

    // mark flush required and request framebuffer schedule
    fNeedFlush = true;

    // marshal flush to framebuffer workloop via command gate (non-blocking)
    if (fFB && fFB->commandGate) {
        fFB->commandGate->runAction(&FakeIrisXEFramebuffer::staticFlushAction);
    } else {
        if (fFB) fFB->flushDisplay();
    }


    IOLog("(FakeIrisXEFramebuffer) [Accel] presentContext ctx=%u copied %u x %u, scheduled flush\n", ctxId, copy_w, copy_h);
    return true;
}


bool FakeIrisXEAccelerator::submitGpuBatchForCtx(uint32_t ctxId,
                                                 FakeIrisXEGEM* batchGem,
                                                 uint32_t priority)
{
    // V148: Add Metal submission diagnostics
    static uint64_t submitCount = 0;
    submitCount++;
    
    if (!fFB || !fFB->fExeclist || !batchGem)
        return false;

    FakeIrisXEExeclist* ex = fFB->fExeclist;

    FakeIrisXEExeclist::XEHWContext* hw = ex->lookupHwContext(ctxId);
    if (!hw) {
        hw = ex->createHwContextFor(ctxId, priority);
        if (!hw) {
            IOLog("(FakeIrisXEFramebuffer) [Accel][V148] submitGpuBatchForCtx: createHwContextFor FAILED ctx=%u\n", ctxId);
            return false;
        }
        IOLog("(FakeIrisXEFramebuffer) [Accel][V148] Created new HW context ctx=%u prio=%u\n", ctxId, priority);
    }

    // V148: Log first few submissions for diagnostics
    if (submitCount <= 5) {
        IOLog("(FakeIrisXEFramebuffer) [Accel][V148] submitGpuBatch ctx=%u batch=%p seq=%llu\n", 
              ctxId, batchGem, submitCount);
    }
    
    bool result = ex->submitForContext(hw, batchGem);
    
    if (submitCount <= 5 && !result) {
        IOLog("(FakeIrisXEFramebuffer) [Accel][V148] submitForContext FAILED ctx=%u\n", ctxId);
    }
    
    return result;
}


void FakeIrisXEAccelerator::linkFromFramebuffer(FakeIrisXEFramebuffer* fb)
{
    if (!fb) {
        IOLog("(FakeIrisXEFramebuffer) [Accel] linkFromFramebuffer: null framebuffer\n");
        return;
    }

    fFB = fb;
    fFramebuffer = fb;
    uint64_t accelRegistryID = getRegistryEntryID();
    fExeclistFromFB = fb->getExeclist();
    fRcsRingFromFB  = fb->getRcsRing();
    
    // V148: Comprehensive Metal Diagnostics
    IOLog("(FakeIrisXEFramebuffer) [Accel][V148] ============================================\n");
    IOLog("(FakeIrisXEFramebuffer) [Accel][V148] METAL DIAGNOSTICS - Execlist Status\n");
    IOLog("(FakeIrisXEFramebuffer) [Accel][V148] ============================================\n");
    IOLog("(FakeIrisXEFramebuffer) [Accel][V148] RegistryID: 0x%llx\n", accelRegistryID);
    IOLog("(FakeIrisXEFramebuffer) [Accel][V148] Execlist ptr: %p\n", fExeclistFromFB);
    IOLog("(FakeIrisXEFramebuffer) [Accel][V148] RCS Ring ptr: %p\n", fRcsRingFromFB);
    
    if (fExeclistFromFB) {
        IOLog("(FakeIrisXEFramebuffer) [Accel][V148] Execlist: AVAILABLE ✅\n");
    } else {
        IOLog("(FakeIrisXEFramebuffer) [Accel][V148] Execlist: NOT AVAILABLE ❌\n");
    }
    
    if (fRcsRingFromFB) {
        IOLog("(FakeIrisXEFramebuffer) [Accel][V148] RCS Ring: AVAILABLE ✅\n");
    } else {
        IOLog("(FakeIrisXEFramebuffer) [Accel][V148] RCS Ring: NOT AVAILABLE ❌\n");
    }
    
    // Check framebuffer state
    IOLog("(FakeIrisXEFramebuffer) [Accel][V148] FB Dimensions: %ux%u\n", fb->getWidth(), fb->getHeight());
    
    IOLog("(FakeIrisXEFramebuffer) [Accel][V148] ============================================\n");
    
    IOLog("(FakeIrisXEFramebuffer) [Accel] link debug: Exec=%p Ring=%p\n", fExeclistFromFB, fRcsRingFromFB);

    if (!fExeclistFromFB || !fRcsRingFromFB || !fb->isCommandSubmissionReady())
    {
        setProperty("FakeIrisXELinkPending", kOSBooleanTrue);
        setProperty("FakeIrisXEAcceleratorLinked", kOSBooleanFalse);
        setProperty("IOAccelReady", kOSBooleanFalse);
        setProperty("MetalSupported", kOSBooleanFalse);
        setProperty("MetalPlugin", kOSBooleanFalse);
        setProperty("IOGPU", kOSBooleanFalse);
        IOLog("(FakeIrisXEFramebuffer) [Accel] link pending: missing RING/EXECLIST or framebuffer execution not ready\n");
        return;
    }

    setProperty("FakeIrisXELinkPending", kOSBooleanFalse);
    setProperty("FakeIrisXEAcceleratorLinked", kOSBooleanTrue);
    setProperty("IOAccelReady", kOSBooleanFalse);
    setProperty("MetalSupported", kOSBooleanFalse);
    setProperty("MetalPlugin", kOSBooleanFalse);
    setProperty("IOGPU", kOSBooleanFalse);
    setProperty("IOAcceleratorRegistryID", accelRegistryID, 64);
    fFB->setProperty("FakeIrisXEAcceleratorLinked", kOSBooleanTrue);
    fFB->setProperty("IOFBAccelerator", kOSBooleanFalse);
    fFB->setProperty("IOFBAcceleratorRegistryID", accelRegistryID, 64);
    fFB->setProperty("IOAccelServiceRegistryID", accelRegistryID, 64);

    IOLog("(FakeIrisXEFramebuffer) [Accel] link complete - diagnostic only, acceleration still unproven\n");
}
