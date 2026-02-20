//
// FakeIrisXE V142 - Rendering Pipeline Test
// Privileged test app with IOSurface entitlements
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

// Method selectors (must match FakeIrisXEAccelShared.h)
enum {
    kFakeIris_Method_GetCaps                = 0,
    kFakeIris_Method_CreateContext          = 1,
    kFakeIris_Method_DestroyContext         = 2,
    kFakeIris_Method_BindSurfaceUserMapped  = 3,
    kFakeIris_Method_PresentContext         = 4,
    kFakeIris_Method_SubmitExeclistFenceTest = 7,
};

// Ring command structures (must match kernel)
#define XE_MAGIC        0x53524558  // 'XERS'
#define XE_VERSION      1
#define XE_PAGE         4096
#define XE_CMD_NOP      0
#define XE_CMD_PRESENT  1
#define XE_CMD_BATCH    2

struct XEHdr {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t head;      // kernel writes
    uint32_t tail;      // user writes
    uint32_t reserved[3];
};

struct XECmd {
    uint32_t cmd;       // XE_CMD_*
    uint32_t ctxId;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
};

// Capability struct
struct XEAccelCaps {
    uint32_t version;
    uint32_t metalSupported;
    uint32_t reserved0;
    uint32_t reserved1;
};

// Context creation structs
struct XECreateCtxIn {
    uint32_t flags;
    uint32_t pad;
    uint64_t sharedGPUPtr;
};

struct XECreateCtxOut {
    uint32_t ctxId;
    uint32_t pad;
};

// Surface binding structs
struct XEBindSurfaceIn {
    uint32_t ctxId;
    uint32_t ioSurfaceID;
    uint32_t width;
    uint32_t height;
    uint32_t bytesPerRow;
    uint32_t surfaceID;
    void*    cpuPtr;
    uint64_t gpuAddr;
    uint32_t pixelFormat;
    int      valid;
};

struct XEBindSurfaceOut {
    uint64_t gpuAddr;
    uint32_t status;
    uint32_t reserved;
};

// Global connection
static io_connect_t gConnection = MACH_PORT_NULL;
static mach_vm_address_t gRingAddr = 0;
static struct XEHdr* gRingHdr = NULL;
static struct XECmd* gRingCmds = NULL;

// Helper: Open connection
int openConnection() {
    kern_return_t kr;
    io_iterator_t iterator = MACH_PORT_NULL;
    io_service_t service = MACH_PORT_NULL;
    
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
    
    kr = IOServiceOpen(service, mach_task_self(), 0, &gConnection);
    IOObjectRelease(service);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ Failed to open connection: 0x%x\n", kr);
        return -1;
    }
    
    printf("✅ Connected to FakeIrisXEAccelerator\n");
    return 0;
}

// Helper: Map shared ring
int mapSharedRing() {
    mach_vm_size_t size = 0;
    
    kern_return_t kr = IOConnectMapMemory(gConnection,
                                          0,  // type 0 = shared ring
                                          mach_task_self(),
                                          &gRingAddr,
                                          &size,
                                          kIOMapAnywhere);
    
    if (kr != KERN_SUCCESS) {
        printf("❌ Failed to map shared ring: 0x%x\n", kr);
        return -1;
    }
    
    gRingHdr = (struct XEHdr*)gRingAddr;
    gRingCmds = (struct XECmd*)(gRingAddr + sizeof(struct XEHdr));
    
    printf("✅ Shared ring mapped:\n");
    printf("   Address: 0x%llx\n", (unsigned long long)gRingAddr);
    printf("   Size: %llu bytes\n", (unsigned long long)size);
    printf("   Magic: 0x%08x %s\n", gRingHdr->magic, 
           gRingHdr->magic == XE_MAGIC ? "✓" : "✗");
    printf("   Version: %u\n", gRingHdr->version);
    printf("   Capacity: %u commands\n", gRingHdr->capacity / sizeof(struct XECmd));
    
    return 0;
}

// Helper: Unmap shared ring
void unmapSharedRing() {
    if (gRingAddr) {
        IOConnectUnmapMemory(gConnection, 0, mach_task_self(), gRingAddr);
        gRingAddr = 0;
        gRingHdr = NULL;
        gRingCmds = NULL;
    }
}

// Helper: Submit command to ring
bool submitCommand(uint32_t cmd, uint32_t ctxId, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (!gRingHdr || !gRingCmds) return false;
    
    uint32_t tail = gRingHdr->tail;
    uint32_t head = gRingHdr->head;
    uint32_t capacity = (gRingHdr->capacity / sizeof(struct XECmd));
    
    // Check if ring is full
    uint32_t nextTail = (tail + 1) % capacity;
    if (nextTail == head) {
        printf("⚠️ Ring full, cannot submit command\n");
        return false;
    }
    
    // Write command
    struct XECmd* cmdPtr = &gRingCmds[tail];
    cmdPtr->cmd = cmd;
    cmdPtr->ctxId = ctxId;
    cmdPtr->arg0 = arg0;
    cmdPtr->arg1 = arg1;
    cmdPtr->arg2 = arg2;
    cmdPtr->arg3 = arg3;
    
    // Memory barrier
    __sync_synchronize();
    
    // Update tail
    gRingHdr->tail = nextTail;
    
    return true;
}

// Test: Full Rendering Pipeline
int testRenderingPipeline() {
    printf("\n=== V142: Full Rendering Pipeline Test ===\n");
    
    // Step 1: Get capabilities
    struct XEAccelCaps caps;
    size_t capsSize = sizeof(caps);
    kern_return_t kr = IOConnectCallStructMethod(gConnection,
                                                  kFakeIris_Method_GetCaps,
                                                  NULL, 0,
                                                  &caps, &capsSize);
    if (kr != KERN_SUCCESS) {
        printf("❌ GetCaps failed: 0x%x\n", kr);
        return -1;
    }
    printf("✅ Accelerator caps: version=%u, metal=%u\n", caps.version, caps.metalSupported);
    
    // Step 2: Create IOSurface (BGRA 640x480)
    uint32_t width = 640;
    uint32_t height = 480;
    uint32_t bytesPerRow = width * 4;
    
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
    CFRelease(pixelFormat);
    CFRelease(properties);
    
    if (!surface) {
        printf("❌ Failed to create IOSurface\n");
        return -1;
    }
    
    uint32_t surfaceID = IOSurfaceGetID(surface);
    printf("✅ Created IOSurface: ID=%u, %ux%u\n", surfaceID, width, height);
    
    // Step 3: Lock and fill surface with test pattern
    IOSurfaceLock(surface, 0, NULL);
    uint8_t* pixels = (uint8_t*)IOSurfaceGetBaseAddress(surface);
    if (pixels) {
        // Fill with gradient pattern
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t* pixel = (uint32_t*)(pixels + y * bytesPerRow + x * 4);
                // BGRA format
                uint8_t b = (x * 255) / width;
                uint8_t g = (y * 255) / height;
                uint8_t r = 128;
                uint8_t a = 255;
                *pixel = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        printf("✅ Filled IOSurface with gradient pattern\n");
    }
    IOSurfaceUnlock(surface, 0, NULL);
    
    // Step 4: Create context
    struct XECreateCtxIn ctxIn = {0};
    struct XECreateCtxOut ctxOut;
    size_t ctxOutSize = sizeof(ctxOut);
    
    kr = IOConnectCallStructMethod(gConnection,
                                   kFakeIris_Method_CreateContext,
                                   &ctxIn, sizeof(ctxIn),
                                   &ctxOut, &ctxOutSize);
    if (kr != KERN_SUCCESS) {
        printf("❌ CreateContext failed: 0x%x\n", kr);
        CFRelease(surface);
        return -1;
    }
    
    uint32_t ctxId = ctxOut.ctxId;
    printf("✅ Created context: ctxId=%u\n", ctxId);
    
    // Step 5: Bind surface to context
    struct XEBindSurfaceIn bindIn = {
        .ctxId = ctxId,
        .ioSurfaceID = surfaceID,
        .width = width,
        .height = height,
        .bytesPerRow = bytesPerRow,
        .surfaceID = surfaceID,
        .cpuPtr = NULL,
        .gpuAddr = 0,
        .pixelFormat = 0x42475241, // 'BGRA'
        .valid = 1
    };
    struct XEBindSurfaceOut bindOut;
    size_t bindOutSize = sizeof(bindOut);
    
    kr = IOConnectCallStructMethod(gConnection,
                                   kFakeIris_Method_BindSurfaceUserMapped,
                                   &bindIn, sizeof(bindIn),
                                   &bindOut, &bindOutSize);
    if (kr != KERN_SUCCESS) {
        printf("❌ BindSurface failed: 0x%x\n", kr);
        // Destroy context
        IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                                  (uint64_t[]){ctxId}, 1, NULL, NULL);
        CFRelease(surface);
        return -1;
    }
    printf("✅ Bound IOSurface to context: status=%u\n", bindOut.status);
    
    // Step 6: Submit PRESENT command via shared ring
    printf("📤 Submitting PRESENT command to ring...\n");
    if (submitCommand(XE_CMD_PRESENT, ctxId, 0, 0, 0, 0)) {
        printf("✅ Command submitted to ring\n");
        
        // Wait a bit for kernel to process
        usleep(100000); // 100ms
        
        printf("   Ring status: head=%u, tail=%u\n", gRingHdr->head, gRingHdr->tail);
    } else {
        printf("❌ Failed to submit command\n");
    }
    
    // Step 7: Present via UserClient (triggers flush)
    kr = IOConnectCallScalarMethod(gConnection,
                                   kFakeIris_Method_PresentContext,
                                   (uint64_t[]){ctxId}, 1,
                                   NULL, NULL);
    if (kr != KERN_SUCCESS) {
        printf("⚠️ Present returned: 0x%x (may be expected)\n", kr);
    } else {
        printf("✅ Present called successfully\n");
    }
    
    // Step 8: Cleanup
    printf("🧹 Cleaning up...\n");
    
    // Unbind surface (implicit in destroy)
    // Destroy context
    kr = IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                                   (uint64_t[]){ctxId}, 1, NULL, NULL);
    if (kr == KERN_SUCCESS) {
        printf("✅ Destroyed context\n");
    } else {
        printf("⚠️ DestroyContext returned: 0x%x\n", kr);
    }
    
    // Release IOSurface
    CFRelease(surface);
    printf("✅ Released IOSurface\n");
    
    printf("\n🎉 V142 Rendering Pipeline Test Complete!\n");
    printf("   If the gradient appeared on screen, the pipeline works!\n");
    
    return 0;
}

void closeConnection() {
    unmapSharedRing();
    if (gConnection != MACH_PORT_NULL) {
        IOServiceClose(gConnection);
        gConnection = MACH_PORT_NULL;
    }
}

int main(int argc, const char* argv[]) {
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║   FakeIrisXE V142 - Rendering Pipeline Test       ║\n");
    printf("║   Tests full render: IOSurface → Ring → Display   ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n");
    
    // Open connection
    if (openConnection() != 0) {
        printf("\n❌ Failed to connect\n");
        return 1;
    }
    
    // Map shared ring
    if (mapSharedRing() != 0) {
        printf("\n❌ Failed to map ring\n");
        closeConnection();
        return 1;
    }
    
    // Run rendering test
    int result = testRenderingPipeline();
    
    // Cleanup
    closeConnection();
    
    printf("\n═══════════════════════════════════════════\n");
    if (result == 0) {
        printf("✅ V142 Test passed! Check screen for gradient.\n");
    } else {
        printf("❌ V142 Test failed\n");
    }
    printf("═══════════════════════════════════════════\n");
    
    return result;
}
