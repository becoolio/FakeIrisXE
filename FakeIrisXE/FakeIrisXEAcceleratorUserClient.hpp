#ifndef FAKE_IRIS_XE_ACCEL_USERCLIENT_HPP
#define FAKE_IRIS_XE_ACCEL_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>

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

    static IOReturn sGetCaps(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sCreateContext(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sDestroyContext(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sBindSurface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sPresent(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn sFenceTest(OSObject* target, void* ref, IOExternalMethodArguments* args);

    
    
    
private:
    FakeIrisXEAccelerator* fOwner;
    task_t fTask;

    // GEM
    GEMHandleTable* fHandleTable = nullptr;
    uint32_t fLastRequestedGemHandle = 0;

    uint32_t createGemAndRegister(uint64_t size, uint32_t flags);
    bool destroyGemHandle(uint32_t handle);
    IOReturn pinGemHandle(uint32_t handle, uint64_t* outGpuAddr);
    bool unpinGemHandle(uint32_t handle);
    IOReturn getPhysPagesForHandle(uint32_t handle, void* outBuf, size_t* outSize);

    IOReturn methodGetCaps(IOExternalMethodArguments* args);
    IOReturn methodCreateContext(IOExternalMethodArguments* args);
    IOReturn methodDestroyContext(IOExternalMethodArguments* args);
    IOReturn methodBindSurface(IOExternalMethodArguments* args);
    IOReturn methodPresent(IOExternalMethodArguments* args);
    IOReturn methodFenceTest(IOExternalMethodArguments* args);

    void clearSurfaceBindings();
    uint64_t makeSurfaceHandle(uint32_t surfaceId);
    bool storeSurfaceBinding(uint64_t handle,
                             uint32_t surfaceId,
                             uint32_t width,
                             uint32_t height,
                             uint32_t pixelFormat,
                             uint32_t stride,
                             uint32_t flags);
    bool getSurfaceBinding(uint64_t handle,
                           uint32_t* surfaceId,
                           uint32_t* width,
                           uint32_t* height,
                           uint32_t* pixelFormat,
                           uint32_t* stride,
                           uint32_t* flags);

    OSDictionary* fSurfaceBindings = nullptr;
    IOLock* fSurfaceLock = nullptr;
    uint32_t fSurfaceSalt = 1;
    uint64_t fCompletionCounter = 0;

    static const IOExternalMethodDispatch sDispatchTable[8];
    
    
    

};

#endif
