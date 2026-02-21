#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>

#include "fakeirisxe_user_shared.h"

static io_connect_t open_connection(void) {
    io_iterator_t iterator = MACH_PORT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    IOServiceMatching("FakeIrisXEAccelerator"),
                                                    &iterator);
    if (kr != KERN_SUCCESS) {
        return MACH_PORT_NULL;
    }

    io_service_t service = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (service == MACH_PORT_NULL) {
        return MACH_PORT_NULL;
    }

    io_connect_t conn = MACH_PORT_NULL;
    kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);
    return (kr == KERN_SUCCESS) ? conn : MACH_PORT_NULL;
}

static IOSurfaceRef create_surface(uint32_t width, uint32_t height) {
    int w = (int)width;
    int h = (int)height;
    int bpr = (int)(width * 4);
    uint32_t fmt = 0x42475241u;

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!props) return NULL;

    CFNumberRef nW = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &w);
    CFNumberRef nH = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &h);
    CFNumberRef nBPR = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpr);
    CFNumberRef nFmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fmt);

    CFDictionarySetValue(props, kIOSurfaceWidth, nW);
    CFDictionarySetValue(props, kIOSurfaceHeight, nH);
    CFDictionarySetValue(props, kIOSurfaceBytesPerRow, nBPR);
    CFDictionarySetValue(props, kIOSurfacePixelFormat, nFmt);

    IOSurfaceRef surface = IOSurfaceCreate(props);

    CFRelease(nW);
    CFRelease(nH);
    CFRelease(nBPR);
    CFRelease(nFmt);
    CFRelease(props);
    return surface;
}

int main(void) {
    io_connect_t conn = open_connection();
    if (conn == MACH_PORT_NULL) {
        printf("{\"step\":\"Connect\",\"ok\":false}\n");
        return 1;
    }
    printf("{\"step\":\"Connect\",\"ok\":true}\n");

    FXE_VersionInfo ver = {0};
    size_t verOutSize = sizeof(ver);
    kern_return_t kr = IOConnectCallStructMethod(conn,
                                                 FXE_SEL_GET_VERSION,
                                                 NULL,
                                                 0,
                                                 &ver,
                                                 &verOutSize);
    printf("{\"step\":\"GetVersion\",\"kr\":%d,\"abiMajor\":%u,\"abiMinor\":%u,\"kextVersion\":%u,\"features\":%u}\n",
           kr,
           ver.abiMajor,
           ver.abiMinor,
           ver.kextVersionPacked,
           ver.features);

    IOSurfaceRef surface = create_surface(640, 480);
    uint64_t boundHandle = 0;
    if (surface) {
        uint32_t surfaceID = IOSurfaceGetID(surface);
        FXE_BindSurface_In in = {
            .surfaceId = surfaceID,
            .flags = 0,
        };
        FXE_BindSurface_Out out = {0};
        size_t outSize = sizeof(out);
        kr = IOConnectCallStructMethod(conn,
                                       FXE_SEL_BIND_SURFACE,
                                       &in,
                                       sizeof(in),
                                       &out,
                                       &outSize);

        boundHandle = out.surfaceHandle;
        printf("{\"step\":\"BindSurface\",\"kr\":%d,\"rc\":%u,\"surfaceId\":%u,\"handle\":\"0x%llx\",\"w\":%u,\"h\":%u,\"fmt\":%u,\"stride\":%u}\n",
               kr,
               out.rc,
               surfaceID,
               (unsigned long long)out.surfaceHandle,
               out.width,
               out.height,
               out.pixelFormat,
               out.stride);
    } else {
        printf("{\"step\":\"BindSurface\",\"kr\":-1,\"rc\":%u,\"error\":\"IOSurfaceCreate failed\"}\n", FXE_ENOENT);
    }

    FXE_Present_In pIn = {
        .surfaceHandle = boundHandle,
        .frameId = 1,
        .flags = 0,
    };
    FXE_Present_Out pOut = {0};
    size_t pOutSize = sizeof(pOut);
    kr = IOConnectCallStructMethod(conn,
                                   FXE_SEL_PRESENT,
                                   &pIn,
                                   sizeof(pIn),
                                   &pOut,
                                   &pOutSize);
    printf("{\"step\":\"Present\",\"kr\":%d,\"rc\":%u,\"completion\":%llu}\n",
           kr,
           pOut.rc,
           (unsigned long long)pOut.completionValue);

    FXE_FenceTest_In fIn = {
        .engine = 0,
        .timeoutMs = 2000,
    };
    FXE_FenceTest_Out fOut = {0};
    size_t fOutSize = sizeof(fOut);
    kr = IOConnectCallStructMethod(conn,
                                   FXE_SEL_FENCE_TEST,
                                   &fIn,
                                   sizeof(fIn),
                                   &fOut,
                                   &fOutSize);
    printf("{\"step\":\"FenceTest\",\"kr\":%d,\"rc\":%u,\"pass\":%u,\"reason\":%u}\n",
           kr,
           fOut.rc,
           fOut.pass,
           fOut.reason);

    if (surface) {
        CFRelease(surface);
    }
    IOServiceClose(conn);
    return 0;
}
