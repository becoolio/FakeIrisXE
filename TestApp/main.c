//
// FakeIrisXE Test App
// Tests the UserClient API for the FakeIrisXE kext
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include "fakeirisxe_user_shared.h"

// Global connection
static io_connect_t gConnection = MACH_PORT_NULL;

// Helper: Open connection to FakeIrisXEAccelerator
int openConnection() {
    kern_return_t kr;
    io_iterator_t iterator = MACH_PORT_NULL;
    io_service_t service = MACH_PORT_NULL;
    
    // Find the accelerator service
    kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                      IOServiceMatching("FakeIrisXEAccelerator"),
                                      &iterator);
    if (kr != KERN_SUCCESS) {
        printf("❌ Failed to find FakeIrisXEAccelerator service\n");
        return -1;
    }
    
    service = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    
    if (service == MACH_PORT_NULL) {
        printf("❌ FakeIrisXEAccelerator not found\n");
        return -1;
    }
    
    // Open connection
    kr = IOServiceOpen(service, mach_task_self(), 0, &gConnection);
    IOObjectRelease(service);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ Failed to open connection: 0x%x\n", kr);
        return -1;
    }
    
    printf("✅ Connected to FakeIrisXEAccelerator\n");
    return 0;
}

// Helper: Close connection
void closeConnection() {
    if (gConnection != MACH_PORT_NULL) {
        IOServiceClose(gConnection);
        gConnection = MACH_PORT_NULL;
    }
}

// Test 1: GetCaps
int testGetCaps() {
    printf("\n=== Test: GetCaps ===\n");
    
    XEAccelCaps caps;
    size_t capsSize = sizeof(caps);
    
    kern_return_t kr = IOConnectCallStructMethod(gConnection,
                                                  kFakeIris_Method_GetCaps,
                                                  NULL, 0,
                                                  &caps, &capsSize);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ GetCaps failed: 0x%x\n", kr);
        return -1;
    }
    
    printf("✅ GetCaps success:\n");
    printf("   Version: %u\n", caps.version);
    printf("   Metal Supported: %u\n", caps.metalSupported);
    
    return 0;
}

// Test 2: Create/Destroy Context
int testCreateDestroyContext() {
    printf("\n=== Test: Create/Destroy Context ===\n");
    
    XECreateCtxIn in = {
        .flags = 0,
        .pad = 0,
        .sharedGPUPtr = 0
    };
    XECreateCtxOut out;
    size_t outSize = sizeof(out);
    
    // Create context
    kern_return_t kr = IOConnectCallStructMethod(gConnection,
                                                  kFakeIris_Method_CreateContext,
                                                  &in, sizeof(in),
                                                  &out, &outSize);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ CreateContext failed: 0x%x\n", kr);
        return -1;
    }
    
    printf("✅ CreateContext success: ctxId=%u\n", out.ctxId);
    
    // Destroy context
    uint64_t ctxId = out.ctxId;
    kr = IOConnectCallScalarMethod(gConnection,
                                   kFakeIris_Method_DestroyContext,
                                   &ctxId, 1,
                                   NULL, NULL);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ DestroyContext failed: 0x%x\n", kr);
        return -1;
    }
    
    printf("✅ DestroyContext success\n");
    return 0;
}

// Test 3: BindSurface with IOSurface
int testBindSurface() {
    printf("\n=== Test: BindSurface with IOSurface ===\n");
    
    // First create a context
    XECreateCtxIn ctxIn = {0};
    XECreateCtxOut ctxOut;
    size_t ctxOutSize = sizeof(ctxOut);
    
    kern_return_t kr = IOConnectCallStructMethod(gConnection,
                                                  kFakeIris_Method_CreateContext,
                                                  &ctxIn, sizeof(ctxIn),
                                                  &ctxOut, &ctxOutSize);
    if (kr != KERN_SUCCESS) {
        printf("❌ CreateContext failed: 0x%x\n", kr);
        return -1;
    }
    
    uint32_t ctxId = ctxOut.ctxId;
    printf("✅ Created context: ctxId=%u\n", ctxId);
    
    // Create an IOSurface
    uint32_t width = 640;
    uint32_t height = 480;
    uint32_t bytesPerRow = width * 4; // ARGB8888
    
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    
    int32_t widthVal = width;
    int32_t heightVal = height;
    int32_t bprVal = bytesPerRow;
    CFNumberRef w = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &widthVal);
    CFNumberRef h = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &heightVal);
    CFNumberRef bpr = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bprVal);
    CFStringRef pixelFormat = CFSTR("BGRA");
    
    CFDictionarySetValue(properties, CFSTR("Width"), w);
    CFDictionarySetValue(properties, CFSTR("Height"), h);
    CFDictionarySetValue(properties, CFSTR("BytesPerRow"), bpr);
    CFDictionarySetValue(properties, CFSTR("PixelFormat"), pixelFormat);
    
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(w);
    CFRelease(h);
    CFRelease(bpr);
    CFRelease(properties);
    
    if (!surface) {
        printf("❌ Failed to create IOSurface\n");
        IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                                  (uint64_t[]){ctxId}, 1, NULL, NULL);
        return -1;
    }
    
    uint32_t surfaceID = IOSurfaceGetID(surface);
    printf("✅ Created IOSurface: ID=%u, %ux%u\n", surfaceID, width, height);
    
    // Bind surface to context
    XEBindSurfaceIn bindIn = {
        .ctxId = ctxId,
        .ioSurfaceID = surfaceID,
        .width = width,
        .height = height,
        .bytesPerRow = bytesPerRow,
        .surfaceID = surfaceID,
        .cpuPtr = NULL,
        .gpuAddr = 0,
        .pixelFormat = 0x42475241, // 'BGRA'
        .valid = true
    };
    XEBindSurfaceOut bindOut;
    size_t bindOutSize = sizeof(bindOut);
    
    kr = IOConnectCallStructMethod(gConnection,
                                   kFakeIris_Method_BindSurfaceUserMapped,
                                   &bindIn, sizeof(bindIn),
                                   &bindOut, &bindOutSize);
    
    // Release IOSurface (kernel now has reference)
    CFRelease(surface);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ BindSurface failed: 0x%x\n", kr);
        IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                                  (uint64_t[]){ctxId}, 1, NULL, NULL);
        return -1;
    }
    
    printf("✅ BindSurface success: status=%u\n", bindOut.status);
    
    // Destroy context
    IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                              (uint64_t[]){ctxId}, 1, NULL, NULL);
    printf("✅ Destroyed context\n");
    
    return 0;
}

// Test 4: Present
int testPresent() {
    printf("\n=== Test: Present ===\n");
    
    // Create context with bound surface
    XECreateCtxIn ctxIn = {0};
    XECreateCtxOut ctxOut;
    size_t ctxOutSize = sizeof(ctxOut);
    
    kern_return_t kr = IOConnectCallStructMethod(gConnection,
                                                  kFakeIris_Method_CreateContext,
                                                  &ctxIn, sizeof(ctxIn),
                                                  &ctxOut, &ctxOutSize);
    if (kr != KERN_SUCCESS) {
        printf("❌ CreateContext failed: 0x%x\n", kr);
        return -1;
    }
    
    uint32_t ctxId = ctxOut.ctxId;
    
    // Create and bind IOSurface
    uint32_t width = 640;
    uint32_t height = 480;
    
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    
    int32_t widthVal = width;
    int32_t heightVal = height;
    int32_t bprVal = width * 4;
    CFNumberRef w = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &widthVal);
    CFNumberRef h = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &heightVal);
    CFNumberRef bpr = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bprVal);
    CFStringRef pixelFormat = CFSTR("BGRA");
    
    CFDictionarySetValue(properties, CFSTR("Width"), w);
    CFDictionarySetValue(properties, CFSTR("Height"), h);
    CFDictionarySetValue(properties, CFSTR("BytesPerRow"), bpr);
    CFDictionarySetValue(properties, CFSTR("PixelFormat"), pixelFormat);
    
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(w);
    CFRelease(h);
    CFRelease(bpr);
    CFRelease(properties);
    
    if (!surface) {
        printf("❌ Failed to create IOSurface\n");
        IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                                  (uint64_t[]){ctxId}, 1, NULL, NULL);
        return -1;
    }
    
    uint32_t surfaceID = IOSurfaceGetID(surface);
    
    XEBindSurfaceIn bindIn = {
        .ctxId = ctxId,
        .ioSurfaceID = surfaceID,
        .width = width,
        .height = height,
        .bytesPerRow = width * 4,
        .surfaceID = surfaceID,
        .cpuPtr = NULL,
        .gpuAddr = 0,
        .pixelFormat = 0x42475241,
        .valid = true
    };
    XEBindSurfaceOut bindOut;
    size_t bindOutSize = sizeof(bindOut);
    
    kr = IOConnectCallStructMethod(gConnection,
                                   kFakeIris_Method_BindSurfaceUserMapped,
                                   &bindIn, sizeof(bindIn),
                                   &bindOut, &bindOutSize);
    
    CFRelease(surface);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ BindSurface failed: 0x%x\n", kr);
        IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                                  (uint64_t[]){ctxId}, 1, NULL, NULL);
        return -1;
    }
    
    printf("✅ Bound IOSurface to context\n");
    
    // Test Present
    uint64_t presentCtx = ctxId;
    kr = IOConnectCallScalarMethod(gConnection,
                                   kFakeIris_Method_PresentContext,
                                   &presentCtx, 1,
                                   NULL, NULL);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ Present failed: 0x%x\n", kr);
    } else {
        printf("✅ Present success\n");
    }
    
    // Cleanup
    IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                              (uint64_t[]){ctxId}, 1, NULL, NULL);
    
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

// Test 5: Shared Ring Mapping
int testSharedRing() {
    printf("\n=== Test: Shared Ring Mapping ===\n");
    
    // Map shared ring (type 0)
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    
    kern_return_t kr = IOConnectMapMemory(gConnection,
                                          0,  // type 0 = shared ring
                                          mach_task_self(),
                                          &address,
                                          &size,
                                          kIOMapAnywhere);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ MapMemory failed: 0x%x\n", kr);
        return -1;
    }
    
    printf("✅ Shared ring mapped:\n");
    printf("   Address: 0x%llx\n", (unsigned long long)address);
    printf("   Size: %llu bytes\n", (unsigned long long)size);
    
    // Read ring header
    XEHdr* hdr = (XEHdr*)address;
    printf("   Magic: 0x%08x (expected: 0x%08x)\n", hdr->magic, XE_MAGIC);
    printf("   Version: %u\n", hdr->version);
    printf("   Capacity: %u\n", hdr->capacity);
    printf("   Head: %u, Tail: %u\n", hdr->head, hdr->tail);
    
    // Unmap
    kr = IOConnectUnmapMemory(gConnection, 0, mach_task_self(), address);
    if (kr != KERN_SUCCESS) {
        printf("❌ UnmapMemory failed: 0x%x\n", kr);
        return -1;
    }
    
    printf("✅ Unmapped shared ring\n");
    return 0;
}

// Print usage
void printUsage(const char* prog) {
    printf("Usage: %s [test_name]\n", prog);
    printf("\nTests:\n");
    printf("  caps      - Test GetCaps\n");
    printf("  context   - Test Create/DestroyContext\n");
    printf("  surface   - Test BindSurface with IOSurface\n");
    printf("  present   - Test Present\n");
    printf("  ring      - Test shared ring mapping\n");
    printf("  all       - Run all tests (default)\n");
}

int main(int argc, const char* argv[]) {
    printf("╔════════════════════════════════════════╗\n");
    printf("║   FakeIrisXE UserClient API Test App   ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    // Parse arguments
    const char* test = "all";
    if (argc > 1) {
        test = argv[1];
    }
    
    if (strcmp(test, "-h") == 0 || strcmp(test, "--help") == 0) {
        printUsage(argv[0]);
        return 0;
    }
    
    // Open connection
    if (openConnection() != 0) {
        printf("\n❌ Failed to connect to FakeIrisXEAccelerator\n");
        printf("Make sure the kext is loaded: kextstat | grep FakeIris\n");
        return 1;
    }
    
    int result = 0;
    
    // Run tests
    if (strcmp(test, "all") == 0 || strcmp(test, "caps") == 0) {
        if (testGetCaps() != 0) result = 1;
    }
    
    if (strcmp(test, "all") == 0 || strcmp(test, "context") == 0) {
        if (testCreateDestroyContext() != 0) result = 1;
    }
    
    if (strcmp(test, "all") == 0 || strcmp(test, "surface") == 0) {
        if (testBindSurface() != 0) result = 1;
    }
    
    if (strcmp(test, "all") == 0 || strcmp(test, "present") == 0) {
        if (testPresent() != 0) result = 1;
    }
    
    if (strcmp(test, "all") == 0 || strcmp(test, "ring") == 0) {
        if (testSharedRing() != 0) result = 1;
    }
    
    // Cleanup
    closeConnection();
    
    printf("\n═══════════════════════════════════════════\n");
    if (result == 0) {
        printf("✅ All tests passed!\n");
    } else {
        printf("❌ Some tests failed\n");
    }
    printf("═══════════════════════════════════════════\n");
    
    return result;
}
