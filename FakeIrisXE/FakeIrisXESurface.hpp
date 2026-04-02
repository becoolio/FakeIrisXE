#ifndef FAKE_IRIS_XE_SURFACE_HPP
#define FAKE_IRIS_XE_SURFACE_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSDictionary.h>
#include "FakeIrisXEAccelShared.h"

class FakeIrisXEGEM;

class FakeIrisXESurface : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXESurface);

public:
    static FakeIrisXESurface* create(uint32_t surfID, FakeIrisXEGEM* gem, const FakeIrisXESurfaceInfo& info);
    void free() override;

    uint32_t getID() const { return fInfo.ioSurfaceID; }
    const FakeIrisXESurfaceInfo& getInfo() const { return fInfo; }
    FakeIrisXEGEM* getGem() const { return fGem; }
    uint64_t getGpuAddress() const;
    void* getCpuAddress() const;

    void retainClient() { fRefCount++; }
    bool releaseClient() { return (--fRefCount == 0); }

    IOReturn mapToTask(task_t task, IOMemoryDescriptor** outDesc, uint64_t* outAddr);
    IOReturn unmapFromTask(IOMemoryMap* map);

private:
    uint32_t fRefCount;
    FakeIrisXESurfaceInfo fInfo;
    FakeIrisXEGEM* fGem;
    IOBufferMemoryDescriptor* fDesc;
    uint32_t fMapCount;
};

#endif
