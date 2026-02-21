#ifndef FAKE_IRIS_XE_ACCEL_USERCLIENT_HPP
#define FAKE_IRIS_XE_ACCEL_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>

#include "FXE_SurfaceStore.hpp"

class FakeIrisXEAccelerator;

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

    static IOReturn sGetCaps(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sCreateContext(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sAttachShared(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sBindSurface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sPresent(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sFenceTest(OSObject* target, void* ref, IOExternalMethodArguments* args);

    
    
    
private:
    FakeIrisXEAccelerator* fOwner;
    task_t fTask;

    IOReturn methodGetCaps(IOExternalMethodArguments* args);
    IOReturn methodCreateContext(IOExternalMethodArguments* args);
    IOReturn methodAttachShared(IOExternalMethodArguments* args);
    IOReturn methodBindSurface(IOExternalMethodArguments* args);
    IOReturn methodPresent(IOExternalMethodArguments* args);
    IOReturn methodFenceTest(IOExternalMethodArguments* args);

    FXE_SurfaceStore fSurfaceStore;
    bool mIOSurfaceEnabled = false;
    uint64_t mCompletionCounter = 0;
    uint32_t mNextCtxId = 1;

    static const IOExternalMethodDispatch sDispatchTable[8];
    
    
    

};

#endif
