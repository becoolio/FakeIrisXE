#ifndef FAKE_IRIS_XE_IOSURFACE_MANAGER_HPP
#define FAKE_IRIS_XE_IOSURFACE_MANAGER_HPP

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSObject.h>
#include "FakeIrisXESurface.hpp"

#define kIOSurfacePixelFormatBGRA8        'BGRA'
#define kIOSurfacePixelFormatRGBA8        'RGBA'
#define kIOSurfacePixelFormatYUV420       '420v'
#define kIOSurfacePixelFormatYUV422       '422v'
#define kIOSurfacePixelFormatRGB565       'R565'

#define kIOSurfaceBacked       (1 << 0)
#define kIOSurfaceGlobal       (1 << 1)
#define kIOSurfacePurgeable    (1 << 2)
#define kIOSurfaceDisplayable  (1 << 3)
#define kIOSurfaceCompressable (1 << 4)

#define MAX_IOSURFACES              4096
#define IOSURFACE_HASH_BUCKETS        256
#define IOSURFACE_MAX_SIZE           (64 * 1024 * 1024)

struct IOSurfaceStatistics {
    uint64_t totalSurfacesCreated;
    uint64_t totalSurfacesDestroyed;
    uint64_t activeSurfaces;
    uint64_t peakActiveSurfaces;
    uint64_t totalMemoryAllocated;
    uint64_t currentMemoryUsage;
    uint64_t peakMemoryUsage;
    uint64_t lookupHits;
    uint64_t lookupMisses;
    float hitRatio;
    uint64_t bgra8Surfaces;
    uint64_t yuv420Surfaces;
    uint64_t otherSurfaces;
};

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
    IOReturn getSurfaceInfo(uint32_t surfID, FakeIrisXESurfaceInfo* outInfo, uint64_t* outGpuAddr);
    FakeIrisXEGEM* getSurfaceGem(uint32_t surfID);

    // V260: Surface iteration and statistics
    uint32_t getSurfaceCount();
    bool enumerateSurfaces(uint32_t* surfIDs, uint32_t maxCount, uint32_t* outCount);
    uint64_t getTotalSurfaceMemory();
    uint64_t getSurfaceGpuMemorySize(uint32_t surfID);

    // V260: Surface validation and properties
    bool isSurfaceValid(uint32_t surfID) const;
    bool isSurfaceMapped(uint32_t surfID) const;
    uint32_t getSurfacePixelFormat(uint32_t surfID);
    uint32_t getSurfaceWidth(uint32_t surfID);
    uint32_t getSurfaceHeight(uint32_t surfID);
    uint64_t getSurfaceStride(uint32_t surfID);

    // V260: Surface management operations
    IOReturn validateSurface(uint32_t surfID, uint32_t width, uint32_t height, uint32_t format);
    IOReturn invalidateSurface(uint32_t surfID);
    IOReturn flushSurfaceCache(uint32_t surfID);
    IOReturn trimSurface(uint32_t surfID);

    // V260: Compression support
    bool isSurfaceCompressed(uint32_t surfID) const;
    IOReturn setSurfaceCompression(uint32_t surfID, bool enable);
    uint32_t getSurfaceCompressionMode(uint32_t surfID) const;

    // V260: Display and scanout support
    IOReturn setSurfaceDisplay(uint32_t surfID, uint32_t pipe, uint32_t plane);
    IOReturn clearSurfaceDisplay(uint32_t surfID);
    bool isSurfaceDisplayEnabled(uint32_t surfID) const;

    // V260: Cursor surface support
    IOReturn setCursorSurface(uint32_t surfID, int32_t x, int32_t y);
    IOReturn updateCursorPosition(int32_t x, int32_t y);
    bool isCursorSurfaceActive() const;
    void disableCursorSurface();

    // V260: YUV plane management
    IOReturn setYUVPlanes(uint32_t surfID, uint32_t yPlane, uint32_t uPlane, uint32_t vPlane);
    IOReturn getYUVPlanes(uint32_t surfID, uint32_t* yPlane, uint32_t* uPlane, uint32_t* vPlane);

    // V260: Memory management
    IOReturn pinSurfaceMemory(uint32_t surfID, uint64_t* outGpuAddr);
    IOReturn unpinSurfaceMemory(uint32_t surfID);
    bool isSurfacePinned(uint32_t surfID) const;

    // V260: Fence management
    IOReturn attachFence(uint32_t surfID, uint32_t fenceId);
    IOReturn waitForFence(uint32_t surfID, uint32_t timeoutMs);
    bool isFenceSignaled(uint32_t fenceId) const;

    // V260: Statistics and diagnostics
    void dumpSurfaceInfo(uint32_t surfID);
    void dumpAllSurfaceInfo();
    uint32_t getActiveSurfaceCount();
    uint64_t getPeakMemoryUsage();
    void resetStatistics();

    // V280: Enhanced IOSurface (from AppleIntelTGLIOSurfaceManager)
    IOReturn createSurfaceEx(uint32_t width, uint32_t height, uint32_t pixelFormat, uint32_t flags, uint32_t* outSurfID);
    IOReturn getSurfaceProperties(uint32_t surfID, uint32_t* width, uint32_t* height, uint32_t* format, uint64_t* size);
    IOReturn setSurfaceProperties(uint32_t surfID, uint32_t width, uint32_t height, uint32_t format);
    bool lockSurface(uint32_t surfID, uint32_t lockType, uint32_t timeoutMs);
    IOReturn unlockSurface(uint32_t surfID);
    bool isSurfaceInUse(uint32_t surfID) const;
    IOReturn markForDisplay(uint32_t surfID, uint32_t displayID);
    IOReturn removeFromDisplay(uint32_t surfID);
    uint32_t getDisplayableSurfaces(uint32_t* surfIDs, uint32_t maxCount);
    IOReturn setAsFramebuffer(uint32_t surfID);
    IOReturn getFramebuffer(uint32_t* outSurfID);
    IOReturn compressSurface(uint32_t surfID);
    IOReturn decompressSurface(uint32_t surfID);
    void getStatistics(IOSurfaceStatistics* stats);
    void printStatistics();

private:
    OSDictionary* fMap;
    IOLock* fLock;
    uint32_t fNextSurfaceID;
    uint32_t fCursorSurfaceId;
    int32_t fCursorX;
    int32_t fCursorY;
    uint64_t fTotalAllocatedMemory;
    uint64_t fPeakMemoryUsage;
    uint32_t fFramebufferSurfaceID;
    bool fFramebufferSet;
    IOSurfaceStatistics fStats;
    IOLock* fStatsLock;
};

#endif
