#ifndef FAKE_IRIS_XE_ACCEL_USERCLIENT_HPP
#define FAKE_IRIS_XE_ACCEL_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <IOKit/IOLib.h>
#include "FakeIrisXEAccelShared.h"

class FakeIrisXEAccelerator;
class FakeIrisXEGEM;
class GEMHandleTable;   // GLOBAL forward declaration

class FakeIrisXEAcceleratorUserClient : public IOUserClient {
    OSDeclareDefaultStructors(FakeIrisXEAcceleratorUserClient);

public:
    bool initWithTask(task_t owningTask, void* securityID, UInt32 type) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;

    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments* args,
                            IOExternalMethodDispatch* dispatch,
                            OSObject* target, void* reference) override;

    IOReturn clientMemoryForType(UInt32 type, UInt32 *flags, IOMemoryDescriptor **memory) override;

private:
    task_t fTask {nullptr};
    FakeIrisXEAccelerator* fOwner {nullptr};
    OSDictionary* fMemTypeToHandle {nullptr};
    IOLock* fMemBindLock {nullptr};
    OSDictionary* fSurfaceRegistry {nullptr};
    IOLock* fSurfaceLock {nullptr};

    // GEM
    GEMHandleTable* fHandleTable = nullptr;

    uint32_t createGemAndRegister(uint64_t size, uint32_t flags);
    bool destroyGemHandle(uint32_t handle);
    IOReturn pinGemHandle(uint32_t handle, uint64_t* outGpuAddr);
    bool unpinGemHandle(uint32_t handle);
    IOReturn getPhysPagesForHandle(uint32_t handle, void* outBuf, size_t* outSize);

    // Memory-type binding (new per guide)
    void setMemBinding(UInt32 type, uint32_t handle);
    uint32_t getMemBinding(UInt32 type);
    IOReturn clearMemBindings();

    // IOSurface registry helpers
    IOReturn registerSurface(uint32_t surfID, uint32_t handle,
                             uint32_t w, uint32_t h,
                             uint32_t rowBytes, uint32_t pixFmt);
    IOReturn unregisterSurface(uint32_t surfID);
    IOReturn getSurfaceInfo(uint32_t surfID, void* out, size_t *outSize);

    // Utilities
    static const OSSymbol* keyForUInt32(UInt32 v);
};

#endif
