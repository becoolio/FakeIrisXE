/* #ifndef _FAKE_IRIS_XE_FRAMEBUFFER_HPP_ */
/* #define _FAKE_IRIS_XE_FRAMEBUFFER_HPP_ */
#pragma once


#include <IOKit/acpi/IOACPIPlatformDevice.h>

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h> // for IOPCIDevice
#include <IOKit/IOBufferMemoryDescriptor.h> // for IODeviceMemory
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <libkern/OSAtomic.h>
// OR (for newer versions)
#include <os/atomic.h>

#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXEExeclist.hpp"

#include "FakeIrisXERing.h"

#include "FakeIrisXEBacklight.hpp" // include so the compiler sees the type

#include <IOKit/IOLocks.h>










class FakeIrisXEFramebuffer : public IOFramebuffer

{
    OSDeclareDefaultStructors(FakeIrisXEFramebuffer)
    

private:
    

    
    IOInterruptEventSource* vsyncSource;
    IOTimerEventSource* vsyncTimer = nullptr;
    IODisplayModeID supportedModes[1] = {0};
    IOTimerEventSource* displayInjectTimer = nullptr;
    IOWorkLoop* workLoop = nullptr;

    volatile bool driverActive;
        IOLock* timerLock;
    
    // Cursor state
    SInt32 cursorX, cursorY;
    bool cursorVisible;
    
    // Color management
    IOColorEntry* clutTable;
    void* gammaTable;
    size_t gammaTableSize;
    
    // Interrupt handling
    struct InterruptInfo {
        IOSelect type;
        IOFBInterruptProc proc;
        void* ref;
    };
    OSArray* interruptList;
    
    void activatePowerAndController();
    
    bool initPowerManagement();

    volatile void *  wsFrontBuffer = nullptr;

      // simple gamma cache
      struct { bool set=false; UInt16 table[256*3]; } gamma{};

      // helpers
      void waitVBlank();       // simple vblank poll
    
    
    void* fFB;
    
    
    
protected:
 
    IOPhysicalAddress bar0Phys = 0;

    
    
    IOMemoryMap* bar0Map;

    enum {
        kConnectionEnable              = 0x656E6162, // 'enab'
        kConnectionDisable             = 0x646E626C, // 'dnbl'
        kConnectionHandleDisplayConfig = 0x68646463, // 'hddc'
        kConnectionFlush               = 0x666C6773, // 'flgs'
        kConnectionLinkChanged         = 0x6C636863, // 'lchc'
        kConnectionSupportsFastSwitch  = 0x73777368, // 'swsh'
        kConnectionAssign              = 0x61736E73, // 'asns'
        kConnectionEnableAudio         = 0x656E6175, // 'enau'
    };

    
    
    IOMemoryMap* vramMap;
    IOMemoryMap* mmioMap;
    IOBufferMemoryDescriptor* vramMemory;
        IOPCIDevice* pciDevice;
        volatile UInt8* mmioBase;
        IODisplayModeID currentMode;
        IOIndex currentDepth;
        size_t vramSize;
    
    IOMemoryDescriptor* framebufferSurface;

    IOBufferMemoryDescriptor* cursorMemory;

    
         
    IOIndex currentConnection;
      bool displayOnline;
    bool fullyInitialized = false;
    IOLock* powerLock;
    bool shuttingDown = false;


    
    
    
    
    
 public:
    
    
    // Safe MMIO access methods
       uint32_t safeMMIORead(uint32_t offset) {
           if (!mmioBase || offset >= mmioMap->getLength()) {
               IOLog("Invalid MMIO read at 0x%X\n", offset);
               return 0xFFFFFFFF;
           }
           return *(volatile uint32_t*)(mmioBase + offset);
       }
       
       void safeMMIOWrite(uint32_t offset, uint32_t value) {
           if (!mmioBase || offset >= mmioMap->getLength()) {
               IOLog("Invalid MMIO write at 0x%X\n", offset);
               return;
           }
           *(volatile uint32_t*)(mmioBase + offset) = value;
                  #ifdef OSMemoryBarrier
                  OSMemoryBarrier();
                  #else
                  __asm__ volatile("mfence" ::: "memory"); // x86 specific
                  #endif

       }
    
    
    
    
    
    bool fNeedFlush = false;   // <-- REQUIRED (you were missing this)

    
    IOCommandGate* commandGate;

    void scheduleFlushFromAccelerator(); // called from accelerator
       static IOReturn staticFlushAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
        
    
    bool mapFramebufferIntoGGTT();

    
    static constexpr uint32_t H_ACTIVE = 1920;
    static constexpr uint32_t V_ACTIVE = 1080;

    uint32_t getWidth()  const { return H_ACTIVE; }
     uint32_t getHeight() const { return V_ACTIVE; }
    uint32_t getStride() { return 7680; }

    
    
    
 
    
     uint64_t getFramebufferPhysAddr() const {
         return framebufferMemory ? framebufferMemory->getPhysicalAddress() : 0;
     }
    

    
    
//   uint64_t fFBPhys{0};

  
    
    void*    getFramebufferKernelPtr() const; // from IOBufferMemoryDescriptor::getBytesNoCopy()

    
    
    
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    virtual bool           init(OSDictionary* dict) override;
    bool   setupWorkLoop();


    virtual void           free() override;
    virtual void startIOFB();

    
    
    bool controllerEnabled = false;

    bool displayPublished = false; // ✅ Member variable
    
    void disableController();
    
    
    enum {
        kPowerStateOff = 0,
        kPowerStateOn,
        kNumPowerStates
    };

    static IOPMPowerState powerStates[kNumPowerStates];


    
    
    virtual IOService *probe(IOService *provider, SInt32 *score) override;

    
    bool getIsUsable() const;
    
    
    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService* whatDevice) override;

    
    virtual const char* getPixelFormats() override;
    
    virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    virtual IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
   
 
    
    virtual UInt32 getConnectionCount() override;
    
    virtual IOReturn getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t *value) override;



    virtual IOReturn createSharedCursor(IOIndex connectIndex, int version) ;
    virtual IOReturn setBounds(IOIndex index, IOGBounds *bounds) ;
        
    
    virtual IOReturn getDisplayModes(IODisplayModeID* allDisplayModes) override;

    
    virtual IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    

    virtual IOReturn getFramebufferOffsetForX_Y(IOPixelAperture aperture, SInt32 x, SInt32 y, UInt32 *offset) ;
    
    virtual IOReturn clientMemoryForType(UInt32 type,
                                             UInt32* flags,
                                             IOMemoryDescriptor** memory) ;

    virtual IOReturn enableController() override;
    

   virtual IOReturn getPixelInformation(IODisplayModeID displayMode,
                                 IOIndex depth,
                                 IOPixelAperture aperture,
                                 IOPixelInformation* info) override;

   virtual IOReturn getCurrentDisplayMode(IODisplayModeID* displayMode, IOIndex* depth) override;

    

    virtual IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t value) override;
    
    virtual IOReturn flushDisplay(void) ;
    
    
    virtual void deliverFramebufferNotification(IOIndex index, UInt32 event, void* info);

    
     IOReturn setNumberOfDisplays(UInt32 count) ;
    
    
    
    virtual IODeviceMemory* getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;

    virtual IOReturn getApertureRange(IOSelect aperture,
                                      IOPhysicalAddress *phys,
                                      IOByteCount *length);

    
    
    
    
    
    virtual IOIndex getAperture() const ;

    
    
    virtual IOItemCount getDisplayModeCount(void) override;

    
        virtual IOReturn  getAttribute(IOSelect attr, uintptr_t *value) override;
    
        virtual IOReturn  setAttribute(IOSelect attribute, uintptr_t value) override;
        
        // Optional but recommended
        virtual IOReturn       registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc, void* ref, void** interruptRef) ;
    
        virtual IOReturn       unregisterInterrupt(void* interruptRef) override;
        virtual IOReturn       setCursorImage(void* cursorImage) override;
        virtual IOReturn       setCursorState(SInt32 x, SInt32 y, bool visible) override;

   virtual IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode,
                                                IOTimingInformation* info)override;

    virtual IOReturn setGammaTable(UInt32 channelCount,
                                                      UInt32 dataCount,
                                                      UInt32 dataWidth,
                                                      void* data)override;
            
    
    
        
    
    virtual  IOReturn getGammaTable(UInt32 channelCount,
                                                  UInt32* dataCount,
                                                  UInt32* dataWidth,
                                                         void** data);

    // Essential methods for IOFramebufferUserClient to function
    virtual IOReturn getAttributeForIndex(IOSelect attribute, UInt32 index, UInt32* value) ;

   
 
    // in class FakeIrisXEFramebuffer : public IOService / IOFramebuffer...
    IOTimerEventSource* fVBlankTimer = nullptr;
    IOWorkLoop* fWorkLoop = nullptr;   // likely already present
    
    void vblankTick(IOTimerEventSource* sender);

    
 
    void*    getFB() const { return kernelFBPtr; }
    size_t   getFBSize() const { return kernelFBSize; }
    uint64_t getFBPhysAddr() const { return kernelFBPhys; }



    FakeIrisXEExeclist* getExeclist() const { return fExeclist; }
    FakeIrisXERing* getRcsRing() const { return fRcsRing; }
    
    
    
    IOBufferMemoryDescriptor* framebufferMemory;
    void* kernelFBPtr = nullptr;
    IOPhysicalAddress kernelFBPhys = 0;
    IOByteCount kernelFBSize = 0;
    IOMemoryMap* framebufferMap = nullptr;

    bool initGuCSystem();

    
    IOReturn performFlushNow();
    static IOReturn staticPerformFlush(OSObject *owner,
                                       void *arg0, void *arg1,
                                       void *arg2, void *arg3);

    
    bool makeUsable();
    
        static IOReturn staticStopAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
        void performSafeStop(); // actual cleanup executed on workloop/gated thread

    
    IOMemoryDescriptor* gttMemoryDescriptor;
      IOMemoryMap* gttMemoryMap;
      IOMemoryMap* ggttMemoryMap;
    

    uint32_t fbGGTTOffset = 0x00000800;

    
 virtual   IOReturn getNotificationSemaphore(IOSelect event,semaphore **sem)override;
    
   virtual IOReturn setCLUTWithEntries(IOColorEntry *entries, SInt32 index,
 SInt32 numEntries, IOOptionBits options);
  
    
    IOReturn setBackingStoreState(IODisplayModeID mode, IOOptionBits options);
    
    IOReturn setStartupDisplayMode(IODisplayModeID mode, IOIndex depth)override;
    
    
    
   virtual IOReturn newUserClient(task_t owningTask,
                                                  void* securityID,
                                                  UInt32 type,
                                                  OSDictionary* properties,
                                                  IOUserClient **handler)override;
    
    
    IOReturn waitForAcknowledge(IOIndex connect, UInt32 type, void *info);
    
    
    bool gpuPowerOn();

    
    bool waitForExeclistEvent(uint32_t timeoutMs);
    void* fSleepToken = (void*)0x12345678; // any unique pointer
    FakeIrisXERing* fRcsRing;   // render ring
    FakeIrisXEGEM* createTinyBatchGem();

    
    // GGTT mapping area (aperture)
    volatile uint32_t* fGGTT = nullptr;
    uint64_t fGGTTSize = 0;       // bytes
    uint64_t fGGTTBaseGPU = 0;    // start VA

    // Simple bump allocator state
    uint64_t fNextGGTTOffset = 0;
    
    // V90: Helper functions for GEM/GGTT management
    FakeIrisXEGEM* createGEMObject(size_t size);
    uint64_t mapGEMToGGTT(FakeIrisXEGEM* gem);
    void unmapGEMFromGGTT(uint64_t gpuAddr);
    
    // ============================================
    // V90: IOAccelerator Hooks for WindowServer
    // ============================================
    
    // Surface management for IOSurface integration
    IOReturn createSurface(uint32_t width, uint32_t height, uint32_t format, 
                           uint64_t* surfaceIdOut, uint64_t* gpuAddrOut);
    IOReturn destroySurface(uint64_t surfaceId);
    IOReturn getSurfaceInfo(uint64_t surfaceId, uint32_t* width, uint32_t* height, 
                           uint32_t* format, uint64_t* gpuAddr);
    
    // 2D Blit/Copy operations for compositing
    IOReturn blitSurface(uint64_t srcSurfaceId, uint64_t dstSurfaceId,
                         uint32_t srcX, uint32_t srcY, uint32_t dstX, uint32_t dstY,
                         uint32_t width, uint32_t height);
    IOReturn copyToFramebuffer(uint64_t surfaceId, uint32_t x, uint32_t y);
    IOReturn fillRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
    
    // Command buffer submission for WindowServer
    IOReturn submit2DCommandBuffer(void* commands, size_t size);
    IOReturn submitBlitCommand(uint32_t opcode, void* data, size_t size);
    
    // Surface cache (simple implementation)
    static constexpr uint32_t kMaxSurfaces = 16;
    struct SurfaceInfo {
        uint64_t id = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
        uint64_t gpuAddress = 0;
        FakeIrisXEGEM* gemObj = nullptr;
        bool inUse = false;
    };
    SurfaceInfo fSurfaces[kMaxSurfaces];
    uint64_t fNextSurfaceId = 1;
    
    // V90 diagnostic counters
    uint32_t fV90BlitCount = 0;
    uint32_t fV90SurfaceCount = 0;
    
    // ============================================
    // V91: 2D Blit Command Builders (Intel PRM Vol 10)
    // ============================================
    
    // XY_SRC_COPY_BLT - Copy rectangular region
    IOReturn submitBlitXY_SRC_COPY(SurfaceInfo* srcSurf, SurfaceInfo* dstSurf,
                                   uint32_t srcX, uint32_t srcY,
                                   uint32_t dstX, uint32_t dstY,
                                   uint32_t width, uint32_t height);
    
    // XY_COLOR_BLT - Fill rectangle with solid color
    IOReturn submitBlitXY_COLOR_BLT(SurfaceInfo* dstSurf,
                                    uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height,
                                    uint32_t color);
    
    // V91 diagnostic counters
    uint32_t fV91BlitSubmitCount = 0;
    uint32_t fV91BlitCompleteCount = 0;
    
    // ============================================
    // V92: Debug Infrastructure & Advanced Features
    // ============================================
    
    // Debug diagnostics - Priority 2 requirements
    void runV92Diagnostics();  // Comprehensive boot diagnostics
    void checkKextLoading();   // Verify kext loaded correctly
    void checkWindowServerConnection();  // Check WS integration
    void checkGPUStatus();     // Verify GPU responding
    void dumpSystemState();    // Full state dump for debugging
    
    // V92 diagnostic properties (exposed to user space)
    OSDictionary* getDiagnosticsReport();
    
    // V92: XY_COLOR_BLT full implementation
    struct XY_COLOR_BLT_CMD;
    IOReturn submitBlitXY_COLOR_BLT_Full(SurfaceInfo* dstSurf,
                                         uint32_t x, uint32_t y,
                                         uint32_t width, uint32_t height,
                                         uint32_t color);
    
    // V92: XY_SETUP_CLIP_BLT for clipping support
    struct XY_SETUP_CLIP_BLT_CMD;
    IOReturn submitBlitXY_SETUP_CLIP(SurfaceInfo* surf,
                                     uint32_t left, uint32_t top,
                                     uint32_t right, uint32_t bottom);
    bool fClipEnabled = false;
    uint32_t fClipLeft = 0, fClipTop = 0, fClipRight = 0, fClipBottom = 0;
    
    // V92: Batch chaining for multiple blits
    static constexpr uint32_t kMaxBatchBlits = 8;
    struct BatchBlitEntry {
        uint64_t srcSurfaceId = 0;
        uint64_t dstSurfaceId = 0;
        uint32_t srcX = 0, srcY = 0;
        uint32_t dstX = 0, dstY = 0;
        uint32_t width = 0, height = 0;
        bool isFill = false;
        uint32_t fillColor = 0;
    };
    
    IOReturn submitBatchBlits(BatchBlitEntry* entries, uint32_t count);
    IOReturn buildBatchCommandBuffer(BatchBlitEntry* entries, uint32_t count,
                                     FakeIrisXEGEM** batchGemOut, uint32_t* seqNumOut);
    
    // V92 counters
    uint32_t fV92ClipCount = 0;
    uint32_t fV92BatchCount = 0;
    uint32_t fV92ColorBlitCount = 0;
    uint64_t fV92LastDiagnosticTime = 0;
    
    // Debug state tracking
    bool fV92DiagnosticsRun = false;
    uint32_t fV92LastError = 0;
    char fV92LastErrorString[256];
    
    // ============================================
    // V93: Display Verification & Integration Testing
    // ============================================
    
    // Display pipe verification (Intel PRM Vol 12)
    void verifyDisplayPipeState();           // Verify pipe/transcoder/DDI status
    bool isPipeAEnabled();                   // Check PIPECONF_A
    bool isTranscoderAEnabled();             // Check TRANS_CONF_A
    bool isDDIAEnabled();                    // Check DDI_BUF_CTL_A
    void logDisplayRegisters();              // Log all display registers
    
    // WindowServer integration tracking
    void trackWindowServerBlit(uint32_t width, uint32_t height, bool isFill);
    uint32_t getWindowServerBlitCount() { return fV93WindowServerBlitCount; }
    bool isWindowServerActive() { return fV93WindowServerBlitCount > 0; }
    
    // GPU activity monitoring
    void trackGPUCommandSubmitted();
    void trackGPUCommandCompleted(uint32_t seqNum);
    void updateGPUPerformanceStats(uint64_t submitTime, uint64_t completeTime);
    
    // V93: Real-time diagnostic report
    OSDictionary* getV93StatusReport();
    void printV93Summary();
    
    // V93 counters
    uint32_t fV93WindowServerBlitCount = 0;
    uint32_t fV93CommandsSubmitted = 0;
    uint32_t fV93CommandsCompleted = 0;
    uint32_t fV93DisplayVerificationFailures = 0;
    
    // V93 timing
    uint64_t fV93FirstBlitTime = 0;
    uint64_t fV93LastBlitTime = 0;
    uint64_t fV93TotalBlitTime = 0;
    
    // V93 state
    bool fV93DisplayVerified = false;
    bool fV93WindowServerConnected = false;
    uint64_t fV93BootTime = 0;

    
  
    uint64_t ggttMap(FakeIrisXEGEM* gem);
    uint64_t ggttMapAtOrAbove(FakeIrisXEGEM* gem, uint64_t minOffset);  // V140: Map at or above minimum offset
    void ggttUnmap(uint64_t gpuAddr, uint32_t pages);

    // ===========================
    // RCS Ring + GGTT + BAR0
    // ===========================
    volatile uint32_t* fBar0 = nullptr;       // MMIO BAR0 virtual mapping
    FakeIrisXERing*    fRingRCS = nullptr;    // Render Command Streamer ring

    uint64_t fGGTTBase = 0;                   // Base GGTT physical address
    uint32_t fGTTMMIOOffset = 0;              // from config space

    // Temporary batch GEM for testing
    FakeIrisXEGEM* batchGem = nullptr;

    FakeIrisXEGEM*    fFenceGEM = nullptr;
    uint64_t          fRingGpuVA = 0;             // GPU VA of ring buffer (GGTT)
    size_t            fRingSize = 0;              // bytes
    uint32_t fFenceSeq;
    FakeIrisXEGEM* fRingGem = nullptr;  // <--- Add this

    
    

    
    FakeIrisXERing* createRcsRing(size_t bytes);

    uint32_t submitBatch(FakeIrisXEGEM* batchGem, size_t batchOffsetBytes, size_t batchSizeBytes);
    
    
    
    
    uint32_t appendFenceAndSubmit(FakeIrisXEGEM* userBatchGem, size_t userBatchOffsetBytes, size_t userBatchSizeBytes);
   
    void handleInterrupt(IOInterruptEventSource* src, int count);
    bool addPendingSubmission(uint32_t seq, FakeIrisXEGEM* master, FakeIrisXEGEM* tail);
    bool completePendingSubmission(uint32_t seq);
    void cleanupAllPendingSubmissions();

    bool setBacklightPercent(uint32_t percent);
    
    uint32_t getBacklightPercent();

    void initBacklightHardware();

    FakeIrisXEGEM* createSimpleUserBatch();
    
    void dumpIRQAndRingRegsSafe();
    
    void enableRcsInterruptsSafely();

    
        bool forcewakeRenderHold(uint32_t timeoutMs = 2000);   // request & wait for FW ack
        void forcewakeRenderRelease();                         // drop FW
        void ensureEngineInterrupts();                         // minimal IER for engine
    
    // V43: GuC submission diagnostics
    bool diagnoseGuCSubmissionFailure();
    bool testGuCCommandExecution();
    bool programMOCS();
   
    FakeIrisXEExeclist* fExeclist = nullptr;

    
   
protected:
   
    void* gttVa = nullptr;
     IOVirtualAddress gttVA = 0;
     volatile uint64_t* ggttMMIO = nullptr;
    IOBufferMemoryDescriptor* textureMemory;
    size_t textureMemorySize;

    
    
    
    static void handleInterruptTrampoline(OSObject *owner, IOInterruptEventSource *src, int count) {
        FakeIrisXEFramebuffer *self = OSDynamicCast(FakeIrisXEFramebuffer, owner);
        if (!self) return;
        self->handleInterrupt(src, count);
    }

    
    

private:
    IOInterruptEventSource* fInterruptSource = nullptr;

    // simple struct to keep pending submissions if you want cleanup:
    struct Submission {
        uint32_t seq;
        FakeIrisXEGEM* masterGem;
        FakeIrisXEGEM* tailGem;
        // add timestamp, owner, etc.
    };
    OSArray* fPendingSubmissions = nullptr; // array of Submission objects or custom wrapper
    IOLock* fPendingLock = nullptr;

    IOCommandGate*  fCmdGate          = nullptr;
   
    FakeIrisXEBacklight* fBacklight = nullptr;
    
    
    
    
    // Firmware data storage
     
      bool fGuCEnabled;
      
      // GuC manager instance (if you create one)
      class FakeIrisXEGuC* fGuC;
    


};






/* #endif  _FAKE_IRIS_XE_FRAMEBUFFER_HPP_ */
