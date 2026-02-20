//
// FakeIrisXE V144 - End-to-End Present Test with Correct IOSurface
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

// Method selectors
enum {
    kFakeIris_Method_GetCaps = 0,
    kFakeIris_Method_CreateContext = 1,
    kFakeIris_Method_DestroyContext = 2,
    kFakeIris_Method_BindSurfaceUserMapped = 3,
    kFakeIris_Method_PresentContext = 4,
};

#define XE_CMD_PRESENT 1

struct XEHdr {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t reserved[3];
};

struct XECmd {
    uint32_t cmd;
    uint32_t ctxId;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
};

struct XEAccelCaps {
    uint32_t version;
    uint32_t metalSupported;
    uint32_t reserved[2];
};

struct XECreateCtxIn {
    uint32_t flags;
    uint32_t pad;
    uint64_t sharedGPUPtr;
};

struct XECreateCtxOut {
    uint32_t ctxId;
    uint32_t pad;
};

struct XEBindSurfaceIn {
    uint32_t ctxId;
    uint32_t ioSurfaceID;
    uint32_t width;
    uint32_t height;
    uint32_t bytesPerRow;
    uint32_t surfaceID;
    void* cpuPtr;
    uint64_t gpuAddr;
    uint32_t pixelFormat;
    int valid;
};

struct XEBindSurfaceOut {
    uint64_t gpuAddr;
    uint32_t status;
    uint32_t reserved;
};

static io_connect_t gConnection = MACH_PORT_NULL;
static mach_vm_address_t gRingAddr = 0;

static uint32_t fourcc(const char a, const char b, const char c, const char d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

int openConnection() {
    io_iterator_t iterator = MACH_PORT_NULL;
    io_service_t service = MACH_PORT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("FakeIrisXEAccelerator"), &iterator);
    if (kr != KERN_SUCCESS) return -1;
    service = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (service == MACH_PORT_NULL) return -1;
    kr = IOServiceOpen(service, mach_task_self(), 0, &gConnection);
    IOObjectRelease(service);
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

int mapSharedRing() {
    mach_vm_size_t size = 0;
    kern_return_t kr = IOConnectMapMemory(gConnection, 0, mach_task_self(),
                                          &gRingAddr, &size, kIOMapAnywhere);
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

void unmapSharedRing() {
    if (gRingAddr) {
        IOConnectUnmapMemory(gConnection, 0, mach_task_self(), gRingAddr);
        gRingAddr = 0;
    }
}

void closeConnection() {
    unmapSharedRing();
    if (gConnection != MACH_PORT_NULL) {
        IOServiceClose(gConnection);
        gConnection = MACH_PORT_NULL;
    }
}

bool submitCommand(uint32_t cmd, uint32_t ctxId) {
    if (!gRingAddr) return false;
    struct XEHdr* hdr = (struct XEHdr*)gRingAddr;
    struct XECmd* cmds = (struct XECmd*)(gRingAddr + sizeof(struct XEHdr));
    
    uint32_t tail = hdr->tail;
    uint32_t head = hdr->head;
    uint32_t capacity = (hdr->capacity / sizeof(struct XECmd));
    uint32_t nextTail = (tail + 1) % capacity;
    if (nextTail == head) return false;
    
    cmds[tail].cmd = cmd;
    cmds[tail].ctxId = ctxId;
    __sync_synchronize();
    hdr->tail = nextTail;
    return true;
}

IOSurfaceRef createTestSurface(int width, int height) {
    int bpr = width * 4;
    uint32_t fmt = fourcc('B','G','R','A');

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFNumberRef nW = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &width);
    CFNumberRef nH = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &height);
    CFNumberRef nBPR = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpr);
    CFNumberRef nFMT = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fmt);

    CFDictionarySetValue(props, kIOSurfaceWidth, nW);
    CFDictionarySetValue(props, kIOSurfaceHeight, nH);
    CFDictionarySetValue(props, kIOSurfaceBytesPerRow, nBPR);
    CFDictionarySetValue(props, kIOSurfacePixelFormat, nFMT);

    IOSurfaceRef surface = IOSurfaceCreate(props);

    CFRelease(nW); CFRelease(nH); CFRelease(nBPR); CFRelease(nFMT);
    CFRelease(props);

    return surface;
}

void fillTestPattern(IOSurfaceRef surface, int width, int height) {
    IOSurfaceLock(surface, 0, NULL);
    uint8_t* pixels = (uint8_t*)IOSurfaceGetBaseAddress(surface);
    if (pixels) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint32_t* pixel = (uint32_t*)(pixels + y * width * 4 + x * 4);
                // Gradient pattern
                uint8_t b = (x * 255) / width;
                uint8_t g = (y * 255) / height;
                uint8_t r = 128;
                uint8_t a = 255;
                *pixel = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        printf("   Filled surface with gradient pattern\n");
    }
    IOSurfaceUnlock(surface, 0, NULL);
}

int main() {
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║   FakeIrisXE V144 - End-to-End Present Test       ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n\n");

    // Step 1: Connect
    printf("[1/8] Connecting to accelerator...\n");
    if (openConnection() != 0) {
        printf("   ❌ Failed\n");
        return 1;
    }
    printf("   ✅ Connected\n");

    // Step 2: Map ring
    printf("[2/8] Mapping shared ring...\n");
    if (mapSharedRing() != 0) {
        printf("   ❌ Failed\n");
        closeConnection();
        return 1;
    }
    printf("   ✅ Ring mapped\n");

    // Step 3: Create IOSurface
    printf("[3/8] Creating IOSurface...\n");
    int width = 640, height = 480;
    IOSurfaceRef surface = createTestSurface(width, height);
    if (!surface) {
        printf("   ❌ IOSurfaceCreate failed\n");
        closeConnection();
        return 1;
    }
    uint32_t surfaceID = IOSurfaceGetID(surface);
    printf("   ✅ Created IOSurface ID=%u (%dx%d)\n", surfaceID, width, height);

    // Step 4: Fill with pattern
    printf("[4/8] Filling surface with test pattern...\n");
    fillTestPattern(surface, width, height);

    // Step 5: Create context
    printf("[5/8] Creating context...\n");
    struct XECreateCtxIn ctxIn = {0};
    struct XECreateCtxOut ctxOut;
    size_t outSize = sizeof(ctxOut);
    kern_return_t kr = IOConnectCallStructMethod(gConnection, kFakeIris_Method_CreateContext,
                                                  &ctxIn, sizeof(ctxIn), &ctxOut, &outSize);
    if (kr != KERN_SUCCESS) {
        printf("   ❌ Failed: 0x%x\n", kr);
        CFRelease(surface);
        closeConnection();
        return 1;
    }
    uint32_t ctxId = ctxOut.ctxId;
    printf("   ✅ Context ID=%u\n", ctxId);

    // Step 6: Bind surface
    printf("[6/8] Binding IOSurface to context...\n");
    struct XEBindSurfaceIn bindIn = {
        .ctxId = ctxId,
        .ioSurfaceID = surfaceID,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .bytesPerRow = (uint32_t)(width * 4),
        .surfaceID = surfaceID,
        .cpuPtr = NULL,
        .gpuAddr = 0,
        .pixelFormat = fourcc('B','G','R','A'),
        .valid = 1
    };
    struct XEBindSurfaceOut bindOut;
    size_t bindOutSize = sizeof(bindOut);
    kr = IOConnectCallStructMethod(gConnection, kFakeIris_Method_BindSurfaceUserMapped,
                                    &bindIn, sizeof(bindIn), &bindOut, &bindOutSize);
    if (kr != KERN_SUCCESS) {
        printf("   ❌ Failed: 0x%x\n", kr);
        IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                                  (uint64_t[]){ctxId}, 1, NULL, NULL);
        CFRelease(surface);
        closeConnection();
        return 1;
    }
    printf("   ✅ Bound (status=%u)\n", bindOut.status);

    // Step 7: Submit PRESENT command
    printf("[7/8] Submitting PRESENT command...\n");
    if (submitCommand(XE_CMD_PRESENT, ctxId)) {
        printf("   ✅ Command submitted\n");
        usleep(100000);
        struct XEHdr* hdr = (struct XEHdr*)gRingAddr;
        printf("   Ring state: head=%u, tail=%u\n", hdr->head, hdr->tail);
    } else {
        printf("   ⚠️ Failed to submit\n");
    }

    // Step 8: Present
    printf("[8/8] Calling PresentContext...\n");
    kr = IOConnectCallScalarMethod(gConnection, kFakeIris_Method_PresentContext,
                                    (uint64_t[]){ctxId}, 1, NULL, NULL);
    if (kr == KERN_SUCCESS) {
        printf("   ✅ Present called\n");
    } else {
        printf("   ⚠️ Present returned: 0x%x\n", kr);
    }

    printf("\n🎉 V144 Test Complete!\n");
    printf("   Look for gradient on screen!\n");

    // Cleanup
    IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                              (uint64_t[]){ctxId}, 1, NULL, NULL);
    CFRelease(surface);
    closeConnection();

    return 0;
}
