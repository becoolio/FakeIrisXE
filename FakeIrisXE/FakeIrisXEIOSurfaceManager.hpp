#ifndef FAKE_IRIS_XE_IOSURFACE_MANAGER_HPP
#define FAKE_IRIS_XE_IOSURFACE_MANAGER_HPP

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSObject.h>
#include "FakeIrisXESurface.hpp"

class FakeIrisXEIOSurfaceManager : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEIOSurfaceManager);

public:
    static FakeIrisXEIOSurfaceManager* create();
    bool init() override;
    void free() override;

    IOReturn createSurface(uint32_t surfID, FakeIrisXEGEM* gem, const FakeIrisXESurfaceInfo& info);
    IOReturn retainSurface(uint32_t surfID);
    IOReturn releaseSurface(uint32_t surfID);
    IOReturn destroySurface(uint32_t surfID, bool force = false);

    IOReturn mapSurfaceToTask(uint32_t surfID, task_t task, IOMemoryDescriptor** outDesc, uint64_t* outAddr);
    IOReturn unmapSurfaceFromTask(uint32_t surfID, IOMemoryMap* map);

private:
    OSDictionary* fMap;
    IOLock* fLock;
};

#endif
