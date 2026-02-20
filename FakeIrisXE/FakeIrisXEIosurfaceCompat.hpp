#pragma once
#include <stdint.h>
#include <stddef.h>

typedef void* IOSurfaceRef;

// IOSurface function pointers - will be resolved at runtime
// We use function pointers instead of weak_import to avoid kext linking issues
extern "C" {
typedef IOSurfaceRef (*IOSurfaceLookup_t)(uint32_t);
typedef int         (*IOSurfaceLock_t)(IOSurfaceRef, uint32_t, uint32_t*);
typedef int         (*IOSurfaceUnlock_t)(IOSurfaceRef, uint32_t, uint32_t*);
typedef void*       (*IOSurfaceGetBaseAddress_t)(IOSurfaceRef);
typedef size_t      (*IOSurfaceGetAllocSize_t)(IOSurfaceRef);
typedef size_t      (*IOSurfaceGetBytesPerRow_t)(IOSurfaceRef);
typedef size_t      (*IOSurfaceGetWidth_t)(IOSurfaceRef);
typedef size_t      (*IOSurfaceGetHeight_t)(IOSurfaceRef);
typedef void        (*IOSurfaceRelease_t)(IOSurfaceRef);
}

// Global function pointers (initialized to nullptr)
extern IOSurfaceLookup_t         g_IOSurfaceLookup;
extern IOSurfaceLock_t           g_IOSurfaceLock;
extern IOSurfaceUnlock_t         g_IOSurfaceUnlock;
extern IOSurfaceGetBaseAddress_t g_IOSurfaceGetBaseAddress;
extern IOSurfaceGetBytesPerRow_t g_IOSurfaceGetBytesPerRow;
extern IOSurfaceGetWidth_t       g_IOSurfaceGetWidth;
extern IOSurfaceGetHeight_t      g_IOSurfaceGetHeight;
extern IOSurfaceRelease_t        g_IOSurfaceRelease;

// Function to initialize IOSurface symbols
bool InitIOSurfaceSymbols();

// Helper to check if IOSurface is available
static inline bool IOSurfaceKextAvailable() {
    return g_IOSurfaceLookup != nullptr;
}

// userland uses 0x1 for read-only; keep it consistent
static const uint32_t kIOSurfaceLockReadOnlyCompat = 0x1;

// Wrapper functions that check availability
static inline IOSurfaceRef IOSurfaceLookup(uint32_t id) {
    return g_IOSurfaceLookup ? g_IOSurfaceLookup(id) : nullptr;
}

static inline int IOSurfaceLock(IOSurfaceRef ref, uint32_t flags, uint32_t* seed) {
    return g_IOSurfaceLock ? g_IOSurfaceLock(ref, flags, seed) : -1;
}

static inline int IOSurfaceUnlock(IOSurfaceRef ref, uint32_t flags, uint32_t* seed) {
    return g_IOSurfaceUnlock ? g_IOSurfaceUnlock(ref, flags, seed) : -1;
}

static inline void* IOSurfaceGetBaseAddress(IOSurfaceRef ref) {
    return g_IOSurfaceGetBaseAddress ? g_IOSurfaceGetBaseAddress(ref) : nullptr;
}

static inline size_t IOSurfaceGetBytesPerRow(IOSurfaceRef ref) {
    return g_IOSurfaceGetBytesPerRow ? g_IOSurfaceGetBytesPerRow(ref) : 0;
}

static inline size_t IOSurfaceGetWidth(IOSurfaceRef ref) {
    return g_IOSurfaceGetWidth ? g_IOSurfaceGetWidth(ref) : 0;
}

static inline size_t IOSurfaceGetHeight(IOSurfaceRef ref) {
    return g_IOSurfaceGetHeight ? g_IOSurfaceGetHeight(ref) : 0;
}

static inline void IOSurfaceRelease(IOSurfaceRef ref) {
    if (g_IOSurfaceRelease) g_IOSurfaceRelease(ref);
}
