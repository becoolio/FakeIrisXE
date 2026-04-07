#ifndef FAKE_IRIS_XE_FENCE_MANAGER_HPP
#define FAKE_IRIS_XE_FENCE_MANAGER_HPP

#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSObject.h>

// V295: Enhanced from IntelFence
class FakeIrisXEFence : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEFence);

public:
    static FakeIrisXEFence* create(uint32_t id);
    virtual bool init() override;
    virtual void free() override;

    bool wait(uint32_t timeoutMs);
    void signal();
    bool isSignaled() const;
    void reset();

    uint32_t getId() const { return fenceId; }
    uint64_t getSignalTime() const { return signalTime; }
    void setSeqno(uint32_t seqno) { this->seqno = seqno; }
    uint32_t getSeqno() const { return seqno; }
    void setEngineId(uint32_t engine) { this->engineId = engine; }
    uint32_t getEngineId() const { return engineId; }

private:
    uint32_t    fenceId;
    bool        signaled;
    uint32_t    seqno;
    uint32_t    engineId;
    uint64_t    signalTime;
    IOLock*     fenceLock;
};

class FakeIrisXEFenceManager : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEFenceManager);

public:
    static FakeIrisXEFenceManager* create(IOWorkLoop* wl);
    void setSeqnoReader(uint32_t (*readSeqno)(void*), void* ctx);

    uint64_t allocFence(uint32_t expectedSeqno);
    IOReturn waitFence(uint64_t fenceId, uint32_t timeoutMs);
    bool isFenceSignaled(uint64_t fenceId);
    void free() override;

    // V295: Enhanced fence management
    FakeIrisXEFence* createFence(uint32_t id);
    IOReturn signalFence(uint64_t fenceId);
    void cleanupFence(uint64_t fenceId);
    void dumpFenceStatus();

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

// Helper to create FakeIrisXEFence
FakeIrisXEFence* FakeIrisXEFenceCreate(uint32_t id);

#endif
