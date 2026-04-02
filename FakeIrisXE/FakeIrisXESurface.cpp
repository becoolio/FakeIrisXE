#include "FakeIrisXESurface.hpp"
#include "FakeIrisXEGEM.hpp"

OSDefineMetaClassAndStructors(FakeIrisXESurface, OSObject);

FakeIrisXESurface* FakeIrisXESurface::create(uint32_t surfID, FakeIrisXEGEM* gem, const FakeIrisXESurfaceInfo& info) {
    if (!gem || surfID == 0) return nullptr;
    auto* s = OSTypeAlloc(FakeIrisXESurface);
    if (!s || !s->init()) { s->release(); return nullptr; }
    s->fGem = gem;
    gem->retain();
    s->fInfo = info;
    s->fDesc = gem->memoryDescriptor();
    s->fDesc->retain();
    s->fRefCount = 1;
    s->fMapCount = 0;
    return s;
}

void FakeIrisXESurface::free() {
    if (fDesc) { fDesc->release(); fDesc = nullptr; }
    if (fGem)  { fGem->release();  fGem = nullptr; }
    OSObject::free();
}

uint64_t FakeIrisXESurface::getGpuAddress() const
{
    return fGem ? fGem->gpuAddress() : 0;
}

void* FakeIrisXESurface::getCpuAddress() const
{
    return fDesc ? fDesc->getBytesNoCopy() : nullptr;
}

IOReturn FakeIrisXESurface::mapToTask(task_t task, IOMemoryDescriptor** outDesc, uint64_t* outAddr) {
    if (!task || !outDesc || !outAddr) return kIOReturnBadArgument;
    *outDesc = nullptr;
    *outAddr = 0;

    IOReturn ret = fDesc->prepare();
    if (ret != kIOReturnSuccess) return ret;

    fMapCount++;
    *outDesc = fDesc;
    fDesc->retain();

    // The caller owns user-task mapping details; we only provide the descriptor.
    *outAddr = 0;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXESurface::unmapFromTask(IOMemoryMap* map) {
    (void)map;
    if (fMapCount > 0) fMapCount--;
    fDesc->complete();
    return kIOReturnSuccess;
}
