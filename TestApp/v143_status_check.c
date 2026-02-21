#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "fakeirisxe_user_shared.h"

static io_connect_t gConnection = MACH_PORT_NULL;
static mach_vm_address_t gRingAddr = 0;
static volatile XEHdr* gRingHdr = NULL;
static uint8_t* gRingBase = NULL;

static int openConnection(void) {
    kern_return_t kr;
    io_iterator_t iterator = MACH_PORT_NULL;
    io_service_t service = MACH_PORT_NULL;

    kr = IOServiceGetMatchingServices(kIOMainPortDefault,
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

static void unmapSharedRing(void) {
    if (gRingAddr) {
        IOConnectUnmapMemory(gConnection, 0, mach_task_self(), gRingAddr);
    }
    gRingAddr = 0;
    gRingHdr = NULL;
    gRingBase = NULL;
}

static void closeConnection(void) {
    unmapSharedRing();
    if (gConnection != MACH_PORT_NULL) {
        IOServiceClose(gConnection);
        gConnection = MACH_PORT_NULL;
    }
}

static bool submitCommand(uint32_t opcode, uint32_t ctxId) {
    if (!gRingHdr || !gRingBase) return false;
    return xe_ring_submit_cmd(gRingHdr, gRingBase, opcode, ctxId, NULL, 0);
}

static IOSurfaceRef createIOSurfaceIfAllowed(void) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    int32_t w = 640;
    int32_t h = 480;
    int32_t bpr = w * 4;
    uint32_t pf = 0x42475241u;
    CFNumberRef width = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &w);
    CFNumberRef height = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &h);
    CFNumberRef rowBytes = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpr);
    CFNumberRef pixelFormat = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);

    CFDictionarySetValue(props, kIOSurfaceWidth, width);
    CFDictionarySetValue(props, kIOSurfaceHeight, height);
    CFDictionarySetValue(props, kIOSurfaceBytesPerRow, rowBytes);
    CFDictionarySetValue(props, kIOSurfacePixelFormat, pixelFormat);

    IOSurfaceRef surface = IOSurfaceCreate(props);

    CFRelease(width);
    CFRelease(height);
    CFRelease(rowBytes);
    CFRelease(pixelFormat);
    CFRelease(props);
    return surface;
}

int main(void) {
    printf("FakeIrisXE V143 - Rendering pipeline status\n\n");

    printf("[1/6] Connecting to FakeIrisXEAccelerator...\n");
    if (openConnection() != 0) {
        printf("  FAIL\n");
        return 1;
    }
    printf("  OK\n");

    printf("[2/6] Mapping shared ring...\n");
    if (mapSharedRing() != 0) {
        printf("  FAIL\n");
        closeConnection();
        return 1;
    }
    printf("  OK addr=0x%llx magic=0x%08x capacity=%u bytes\n",
           (unsigned long long)gRingAddr, gRingHdr->magic, gRingHdr->capacity);

    printf("[3/6] Getting accelerator capabilities...\n");
    XEAccelCaps caps;
    size_t capsSize = sizeof(caps);
    kern_return_t kr = IOConnectCallStructMethod(gConnection,
                                                 kFakeIris_Method_GetCaps,
                                                 NULL,
                                                 0,
                                                 &caps,
                                                 &capsSize);
    if (kr == KERN_SUCCESS) {
        printf("  OK version=%u metal=%u\n", caps.version, caps.metalSupported);
    } else {
        printf("  FAIL 0x%x\n", kr);
    }

    printf("[4/6] Creating context...\n");
    XECreateCtxIn ctxIn = {0};
    XECreateCtxOut ctxOut;
    size_t ctxOutSize = sizeof(ctxOut);
    kr = IOConnectCallStructMethod(gConnection,
                                   kFakeIris_Method_CreateContext,
                                   &ctxIn,
                                   sizeof(ctxIn),
                                   &ctxOut,
                                   &ctxOutSize);
    if (kr != KERN_SUCCESS) {
        printf("  FAIL 0x%x\n", kr);
        closeConnection();
        return 1;
    }
    uint32_t ctxId = ctxOut.ctxId;
    printf("  OK ctx=%u\n", ctxId);

    printf("[5/6] Creating IOSurface (may be blocked by policy)...\n");
    IOSurfaceRef surface = createIOSurfaceIfAllowed();
    if (surface) {
        uint32_t surfaceID = IOSurfaceGetID(surface);
        size_t rowBytes = IOSurfaceGetBytesPerRow(surface);
        size_t width = IOSurfaceGetWidth(surface);
        size_t height = IOSurfaceGetHeight(surface);

        XEBindSurfaceIn bindIn = {
            .ctxId = ctxId,
            .ioSurfaceID = surfaceID,
            .width = (uint32_t)width,
            .height = (uint32_t)height,
            .bytesPerRow = (uint32_t)rowBytes,
            .surfaceID = surfaceID,
            .cpuPtr = NULL,
            .gpuAddr = 0,
            .pixelFormat = 0x42475241u,
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
        if (kr == KERN_SUCCESS) {
            printf("  OK bound surface id=%u status=%u\n", surfaceID, bindOut.status);

            printf("[6/6] Submitting PRESENT command...\n");
            if (submitCommand(XE_CMD_PRESENT, ctxId)) {
                usleep(100000);
                printf("  OK ring head=%u tail=%u\n", gRingHdr->head, gRingHdr->tail);
                (void)IOConnectCallScalarMethod(gConnection,
                                                kFakeIris_Method_PresentContext,
                                                (uint64_t[]){ctxId},
                                                1,
                                                NULL,
                                                NULL);
            } else {
                printf("  FAIL ring submit\n");
            }
        } else {
            printf("  FAIL bind 0x%x\n", kr);
        }
        CFRelease(surface);
    } else {
        printf("  BLOCKED (expected on some configurations)\n");
        printf("[6/6] Skipped PRESENT submit because no IOSurface\n");
    }

    (void)IOConnectCallScalarMethod(gConnection,
                                    kFakeIris_Method_DestroyContext,
                                    (uint64_t[]){ctxId},
                                    1,
                                    NULL,
                                    NULL);
    closeConnection();

    printf("\nV143 test complete\n");
    return 0;
}
