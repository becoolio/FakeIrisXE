#include "FakeIrisXEIosurfaceCompat.hpp"
#include <IOKit/IOLib.h>

extern "C" {
IOSurfaceRef IOSurfaceLookupWeak(uint32_t) asm("_IOSurfaceLookup") __attribute__((weak_import));
int IOSurfaceLockWeak(IOSurfaceRef, uint32_t, uint32_t*) asm("_IOSurfaceLock") __attribute__((weak_import));
int IOSurfaceUnlockWeak(IOSurfaceRef, uint32_t, uint32_t*) asm("_IOSurfaceUnlock") __attribute__((weak_import));
void* IOSurfaceGetBaseAddressWeak(IOSurfaceRef) asm("_IOSurfaceGetBaseAddress") __attribute__((weak_import));
size_t IOSurfaceGetBytesPerRowWeak(IOSurfaceRef) asm("_IOSurfaceGetBytesPerRow") __attribute__((weak_import));
size_t IOSurfaceGetWidthWeak(IOSurfaceRef) asm("_IOSurfaceGetWidth") __attribute__((weak_import));
size_t IOSurfaceGetHeightWeak(IOSurfaceRef) asm("_IOSurfaceGetHeight") __attribute__((weak_import));
void IOSurfaceReleaseWeak(IOSurfaceRef) asm("_IOSurfaceRelease") __attribute__((weak_import));
}

// Global function pointers (initialized to nullptr)
IOSurfaceLookup_t         g_IOSurfaceLookup = nullptr;
IOSurfaceLock_t           g_IOSurfaceLock = nullptr;
IOSurfaceUnlock_t         g_IOSurfaceUnlock = nullptr;
IOSurfaceGetBaseAddress_t g_IOSurfaceGetBaseAddress = nullptr;
IOSurfaceGetBytesPerRow_t g_IOSurfaceGetBytesPerRow = nullptr;
IOSurfaceGetWidth_t       g_IOSurfaceGetWidth = nullptr;
IOSurfaceGetHeight_t      g_IOSurfaceGetHeight = nullptr;
IOSurfaceRelease_t        g_IOSurfaceRelease = nullptr;

bool InitIOSurfaceSymbols() {
    g_IOSurfaceLookup = IOSurfaceLookupWeak;
    g_IOSurfaceLock = IOSurfaceLockWeak;
    g_IOSurfaceUnlock = IOSurfaceUnlockWeak;
    g_IOSurfaceGetBaseAddress = IOSurfaceGetBaseAddressWeak;
    g_IOSurfaceGetBytesPerRow = IOSurfaceGetBytesPerRowWeak;
    g_IOSurfaceGetWidth = IOSurfaceGetWidthWeak;
    g_IOSurfaceGetHeight = IOSurfaceGetHeightWeak;
    g_IOSurfaceRelease = IOSurfaceReleaseWeak;

    const bool available = g_IOSurfaceLookup && g_IOSurfaceGetBytesPerRow &&
                           g_IOSurfaceGetWidth && g_IOSurfaceGetHeight &&
                           g_IOSurfaceRelease;

    if (!available) {
        IOLog("FakeIrisXE: [IOSurface] DISABLED lookupOK=0 IOSURF=0\n");
        return false;
    }

    IOSurfaceRef probe = IOSurfaceLookup(0xFFFFFFFFu);
    if (probe) {
        IOSurfaceRelease(probe);
    }

    IOLog("FakeIrisXE: [IOSurface] ENABLED lookupOK=1\n");
    return true;
}
