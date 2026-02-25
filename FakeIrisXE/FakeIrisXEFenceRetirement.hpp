#ifndef FAKE_IRIS_XE_FENCE_MANAGER_HPP
#define FAKE_IRIS_XE_FENCE_MANAGER_HPP

#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSObject.h>

class FakeIrisXEFenceManager : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEFenceManager);

public:
    static FakeIrisXEFenceManager* create(IOWorkLoop* wl);
    void setSeqnoReader(uint32_t (*readSeqno)(void*), void* ctx);

    uint64_t allocFence(uint32_t expectedSeqno);
    IOReturn waitFence(uint64_t fenceId, uint32_t timeoutMs);
    bool isFenceSignaled(uint64_t fenceId);
    void free() override;

private:
    IOWorkLoop* fWL;
    IOTimerEventSource* fTimer;
    IOLock* fLock;
    uint32_t (*fSeqnoReader)(void*);
    void* fSeqnoReaderCtx;
    uint32_t fCompletedSeqno;
    uint64_t fNextFenceId;
    OSArray* fFences;
};

#endif
