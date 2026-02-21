#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "fakeirisxe_user_shared.h"

#define WIDTH 640
#define HEIGHT 480
#define BPP 4

static io_connect_t gConnection = MACH_PORT_NULL;
static mach_vm_address_t gRingAddr = 0;
static mach_vm_address_t gPixelBufferAddr = 0;
static volatile XEHdr* gRingHdr = NULL;
static uint8_t* gRingBase = NULL;

static uint32_t fourcc(char a, char b, char c, char d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

static int openConnection(void) {
    io_iterator_t iterator = MACH_PORT_NULL;
    io_service_t service = MACH_PORT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    IOServiceMatching("FakeIrisXEAccelerator"),
                                                    &iterator);
    if (kr != KERN_SUCCESS) return -1;

    service = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (service == MACH_PORT_NULL) return -1;

    kr = IOServiceOpen(service, mach_task_self(), 0, &gConnection);
    IOObjectRelease(service);
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

static int mapSharedRing(void) {
    mach_vm_size_t size = 0;
    kern_return_t kr = IOConnectMapMemory(gConnection, 0, mach_task_self(),
                                          &gRingAddr, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) return -1;

    gRingHdr = (volatile XEHdr*)gRingAddr;
    gRingBase = (uint8_t*)(gRingAddr + sizeof(XEHdr));
    return 0;
}

static int mapPixelBuffer(void) {
    mach_vm_size_t size = 0;
    kern_return_t kr = IOConnectMapMemory(gConnection, 1, mach_task_self(),
                                          &gPixelBufferAddr, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) return -1;

    printf("  pixel buffer mapped: %llu bytes at 0x%llx\n",
           (unsigned long long)size,
           (unsigned long long)gPixelBufferAddr);
    return 0;
}

static void closeConnection(void) {
    if (gPixelBufferAddr) {
        IOConnectUnmapMemory(gConnection, 1, mach_task_self(), gPixelBufferAddr);
    }
    if (gRingAddr) {
        IOConnectUnmapMemory(gConnection, 0, mach_task_self(), gRingAddr);
    }

    gPixelBufferAddr = 0;
    gRingAddr = 0;
    gRingHdr = NULL;
    gRingBase = NULL;

    if (gConnection != MACH_PORT_NULL) {
        IOServiceClose(gConnection);
        gConnection = MACH_PORT_NULL;
    }
}

static bool submitPresentCommand(uint32_t ctxId) {
    if (!gRingHdr || !gRingBase) return false;
    return xe_ring_submit_cmd(gRingHdr, gRingBase, XE_CMD_PRESENT, ctxId, NULL, 0);
}

static void fillGradient(void) {
    if (!gPixelBufferAddr) return;

    uint8_t* pixels = (uint8_t*)gPixelBufferAddr;
    const uint32_t rowBytes = WIDTH * BPP;

    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t* pixel = (uint32_t*)(pixels + (size_t)y * rowBytes + (size_t)x * BPP);
            uint8_t b = (uint8_t)((x * 255) / WIDTH);
            uint8_t g = (uint8_t)((y * 255) / HEIGHT);
            uint8_t r = 128;
            uint8_t a = 255;
            *pixel = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

int main(void) {
    printf("FakeIrisXE V145 - Shared memory rendering test\n\n");

    printf("[1/8] Connecting to accelerator...\n");
    if (openConnection() != 0) {
        printf("  FAIL\n");
        return 1;
    }
    printf("  OK\n");

    printf("[2/8] Mapping shared ring...\n");
    if (mapSharedRing() != 0) {
        printf("  FAIL\n");
        closeConnection();
        return 1;
    }
    printf("  OK magic=0x%08x capacity=%u bytes\n", gRingHdr->magic, gRingHdr->capacity);

    printf("[3/8] Mapping pixel buffer (memory type 1)...\n");
    if (mapPixelBuffer() != 0) {
        printf("  FAIL\n");
        closeConnection();
        return 1;
    }
    printf("  OK\n");

    printf("[4/8] Filling pixel buffer with gradient...\n");
    fillGradient();
    printf("  OK\n");

    printf("[5/8] Creating context...\n");
    XECreateCtxIn ctxIn = {0};
    XECreateCtxOut ctxOut;
    size_t outSize = sizeof(ctxOut);
    kern_return_t kr = IOConnectCallStructMethod(gConnection,
                                                 kFakeIris_Method_CreateContext,
                                                 &ctxIn,
                                                 sizeof(ctxIn),
                                                 &ctxOut,
                                                 &outSize);
    if (kr != KERN_SUCCESS) {
        printf("  FAIL 0x%x\n", kr);
        closeConnection();
        return 1;
    }
    uint32_t ctxId = ctxOut.ctxId;
    printf("  OK ctx=%u\n", ctxId);

    printf("[6/8] Binding shared buffer metadata to context...\n");
    XEBindSurfaceIn bindIn = {
        .ctxId = ctxId,
        .ioSurfaceID = 0,
        .width = WIDTH,
        .height = HEIGHT,
        .bytesPerRow = WIDTH * BPP,
        .surfaceID = 0,
        .cpuPtr = (void*)(uintptr_t)gPixelBufferAddr,
        .gpuAddr = 0,
        .pixelFormat = fourcc('B', 'G', 'R', 'A'),
        .valid = true,
    };
    XEBindSurfaceOut bindOut;
    size_t bindOutSize = sizeof(bindOut);
    kr = IOConnectCallStructMethod(gConnection,
                                   kFakeIris_Method_BindSurfaceUserMapped,
                                   &bindIn,
                                   sizeof(bindIn),
                                   &bindOut,
                                   &bindOutSize);
    if (kr != KERN_SUCCESS) {
        printf("  FAIL 0x%x\n", kr);
        (void)IOConnectCallScalarMethod(gConnection,
                                        kFakeIris_Method_DestroyContext,
                                        (uint64_t[]){ctxId},
                                        1,
                                        NULL,
                                        NULL);
        closeConnection();
        return 1;
    }
    printf("  OK status=%u\n", bindOut.status);

    printf("[7/8] Submitting PRESENT command...\n");
    if (submitPresentCommand(ctxId)) {
        usleep(100000);
        printf("  OK ring head=%u tail=%u\n", gRingHdr->head, gRingHdr->tail);
    } else {
        printf("  FAIL\n");
    }

    printf("[8/8] Calling PresentContext...\n");
    kr = IOConnectCallScalarMethod(gConnection,
                                   kFakeIris_Method_PresentContext,
                                   (uint64_t[]){ctxId},
                                   1,
                                   NULL,
                                   NULL);
    if (kr == KERN_SUCCESS) {
        printf("  OK\n");
    } else {
        printf("  WARN 0x%x\n", kr);
    }

    (void)IOConnectCallScalarMethod(gConnection,
                                    kFakeIris_Method_DestroyContext,
                                    (uint64_t[]){ctxId},
                                    1,
                                    NULL,
                                    NULL);
    closeConnection();

    printf("\nV145 test complete\n");
    return 0;
}
