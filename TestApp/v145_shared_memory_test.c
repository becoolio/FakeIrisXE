//
// FakeIrisXE V145 - Shared Memory Rendering Test
// Uses type=1 pixel buffer instead of IOSurface
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
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
#define WIDTH 640
#define HEIGHT 480
#define BPP 4
#define BUFFER_SIZE (WIDTH * HEIGHT * BPP)

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
static mach_vm_address_t gPixelBufferAddr = 0;

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

int mapPixelBuffer() {
    mach_vm_size_t size = 0;
    kern_return_t kr = IOConnectMapMemory(gConnection, 1, mach_task_self(),
                                          &gPixelBufferAddr, &size, kIOMapAnywhere);
    if (kr == KERN_SUCCESS) {
        printf("   Pixel buffer mapped: %llu bytes at 0x%llx\n", 
               (unsigned long long)size, (unsigned long long)gPixelBufferAddr);
        return 0;
    }
    return -1;
}

void unmapAll() {
    if (gPixelBufferAddr) {
        IOConnectUnmapMemory(gConnection, 1, mach_task_self(), gPixelBufferAddr);
        gPixelBufferAddr = 0;
    }
    if (gRingAddr) {
        IOConnectUnmapMemory(gConnection, 0, mach_task_self(), gRingAddr);
        gRingAddr = 0;
    }
}

void closeConnection() {
    unmapAll();
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

void fillGradient() {
    if (!gPixelBufferAddr) return;
    
    uint8_t* pixels = (uint8_t*)gPixelBufferAddr;
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            uint32_t* pixel = (uint32_t*)(pixels + y * WIDTH * 4 + x * 4);
            // BGRA gradient
            uint8_t b = (x * 255) / WIDTH;
            uint8_t g = (y * 255) / HEIGHT;
            uint8_t r = 128;
            uint8_t a = 255;
            *pixel = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    printf("   Filled pixel buffer with gradient\n");
}

int main() {
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║   FakeIrisXE V145 - Shared Memory Rendering       ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n\n");

    // Step 1: Connect
    printf("[1/7] Connecting to accelerator...\n");
    if (openConnection() != 0) {
        printf("   ❌ Failed\n");
        return 1;
    }
    printf("   ✅ Connected\n");

    // Step 2: Map shared ring
    printf("[2/7] Mapping shared ring...\n");
    if (mapSharedRing() != 0) {
        printf("   ❌ Failed\n");
        closeConnection();
        return 1;
    }
    printf("   ✅ Ring mapped\n");

    // Step 3: Map pixel buffer (V145 - type=1)
    printf("[3/7] Mapping pixel buffer (type=1)...\n");
    if (mapPixelBuffer() != 0) {
        printf("   ❌ Failed\n");
        closeConnection();
        return 1;
    }
    printf("   ✅ Pixel buffer mapped\n");

    // Step 4: Fill pixel buffer with gradient
    printf("[4/7] Filling pixel buffer...\n");
    fillGradient();

    // Step 5: Create context
    printf("[5/7] Creating context...\n");
    struct XECreateCtxIn ctxIn = {0};
    struct XECreateCtxOut ctxOut;
    size_t outSize = sizeof(ctxOut);
    kern_return_t kr = IOConnectCallStructMethod(gConnection, kFakeIris_Method_CreateContext,
                                                  &ctxIn, sizeof(ctxIn), &ctxOut, &outSize);
    if (kr != KERN_SUCCESS) {
        printf("   ❌ Failed: 0x%x\n", kr);
        closeConnection();
        return 1;
    }
    uint32_t ctxId = ctxOut.ctxId;
    printf("   ✅ Context ID=%u\n", ctxId);

    // Step 6: Submit PRESENT command
    printf("[6/7] Submitting PRESENT command...\n");
    if (submitCommand(XE_CMD_PRESENT, ctxId)) {
        printf("   ✅ Command submitted\n");
        usleep(100000);
        struct XEHdr* hdr = (struct XEHdr*)gRingAddr;
        printf("   Ring state: head=%u, tail=%u\n", hdr->head, hdr->tail);
    } else {
        printf("   ⚠️ Failed to submit\n");
    }

    // Step 7: Present
    printf("[7/7] Calling PresentContext...\n");
    kr = IOConnectCallScalarMethod(gConnection, kFakeIris_Method_PresentContext,
                                    (uint64_t[]){ctxId}, 1, NULL, NULL);
    if (kr == KERN_SUCCESS) {
        printf("   ✅ Present called\n");
    } else {
        printf("   ⚠️ Present returned: 0x%x\n", kr);
    }

    printf("\n🎉 V145 Test Complete!\n");
    printf("   GRADIENT SHOULD BE VISIBLE ON SCREEN!\n");
    printf("   (If you see the gradient, shared memory rendering works!)\n");

    // Cleanup
    IOConnectCallScalarMethod(gConnection, kFakeIris_Method_DestroyContext,
                              (uint64_t[]){ctxId}, 1, NULL, NULL);
    closeConnection();

    return 0;
}
