#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>

#include <stdio.h>
#include <string.h>

#include "fakeirisxe_user_shared.h"

static io_connect_t OpenService(const char* className) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(className));
    if (!service) {
        return MACH_PORT_NULL;
    }

    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);
    return (kr == KERN_SUCCESS) ? conn : MACH_PORT_NULL;
}

static kern_return_t CallStruct(io_connect_t c,
                                uint32_t sel,
                                const void* in,
                                size_t inSz,
                                void* out,
                                size_t* outSz) {
    return IOConnectCallStructMethod(c, sel, in, inSz, out, outSz);
}

static IOSurfaceRef CreateTestSurface(size_t w, size_t h) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!dict) {
        return nullptr;
    }

    int32_t width = (int32_t)w;
    int32_t height = (int32_t)h;
    int32_t bpe = 4;
    int32_t bpr = (int32_t)(w * (size_t)bpe);
    int32_t pf = (int32_t)'BGRA';

    CFNumberRef nW = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &width);
    CFNumberRef nH = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &height);
    CFNumberRef nBPE = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpe);
    CFNumberRef nBPR = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpr);
    CFNumberRef nPF = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);

    if (!nW || !nH || !nBPE || !nBPR || !nPF) {
        if (nW) CFRelease(nW);
        if (nH) CFRelease(nH);
        if (nBPE) CFRelease(nBPE);
        if (nBPR) CFRelease(nBPR);
        if (nPF) CFRelease(nPF);
        CFRelease(dict);
        return nullptr;
    }

    CFDictionarySetValue(dict, kIOSurfaceWidth, nW);
    CFDictionarySetValue(dict, kIOSurfaceHeight, nH);
    CFDictionarySetValue(dict, kIOSurfaceBytesPerElement, nBPE);
    CFDictionarySetValue(dict, kIOSurfaceBytesPerRow, nBPR);
    CFDictionarySetValue(dict, kIOSurfacePixelFormat, nPF);

    IOSurfaceRef surface = IOSurfaceCreate(dict);
    CFRelease(nW);
    CFRelease(nH);
    CFRelease(nBPE);
    CFRelease(nBPR);
    CFRelease(nPF);
    CFRelease(dict);
    return surface;
}

int main() {
    io_connect_t conn = OpenService("FakeIrisXEAccelerator");
    if (conn == MACH_PORT_NULL) {
        printf("{\"step\":\"Open\",\"ok\":false,\"err\":\"service_not_found\"}\n");
        return 1;
    }
    printf("{\"step\":\"Open\",\"ok\":true}\n");

    FXE_VersionInfo ver = {};
    size_t outSz0 = sizeof(ver);
    kern_return_t kr = CallStruct(conn, FXE_SEL_GET_VERSION, nullptr, 0, &ver, &outSz0);
    printf("{\"step\":\"GetVersion\",\"kr\":%d,\"abiMajor\":%u,\"abiMinor\":%u,\"kextVersion\":%u,\"features\":%u}\n",
           kr,
           ver.abiMajor,
           ver.abiMinor,
           ver.kextVersionPacked,
           ver.features);

    FXE_CreateCtx_In createIn = {};
    FXE_CreateCtx_Out createOut = {};
    size_t outSz1 = sizeof(createOut);
    kr = CallStruct(conn, FXE_SEL_CREATE_CTX, &createIn, sizeof(createIn), &createOut, &outSz1);
    printf("{\"step\":\"CreateCtx\",\"kr\":%d,\"ctxId\":%u,\"rc\":%u}\n", kr, createOut.ctxId, createOut.rc);

    FXE_AttachShared_In attachIn = {};
    FXE_AttachShared_Out attachOut = {};
    size_t outSz2 = sizeof(attachOut);
    kr = CallStruct(conn, FXE_SEL_ATTACH_SHARED, &attachIn, sizeof(attachIn), &attachOut, &outSz2);
    printf("{\"step\":\"AttachShared\",\"kr\":%d,\"rc\":%u,\"magic\":%u,\"cap\":%u}\n",
           kr,
           attachOut.rc,
           attachOut.magic,
           attachOut.cap);

    IOSurfaceRef surf = CreateTestSurface(128, 128);
    if (!surf) {
        printf("{\"step\":\"IOSurfaceCreate\",\"ok\":false}\n");
        IOServiceClose(conn);
        return 1;
    }

    uint32_t sid = IOSurfaceGetID(surf);
    printf("{\"step\":\"IOSurfaceCreate\",\"ok\":true,\"id\":%u}\n", sid);

    FXE_BindSurface_In bindIn = {sid, 0};
    FXE_BindSurface_Out bindOut = {};
    size_t outSz3 = sizeof(bindOut);
    kr = CallStruct(conn, FXE_SEL_BIND_SURFACE, &bindIn, sizeof(bindIn), &bindOut, &outSz3);
    printf("{\"step\":\"BindSurface\",\"kr\":%d,\"rc\":%u,\"handle\":\"0x%llX\",\"w\":%u,\"h\":%u,\"fmt\":%u,\"stride\":%u}\n",
           kr,
           bindOut.rc,
           (unsigned long long)bindOut.surfaceHandle,
           bindOut.width,
           bindOut.height,
           bindOut.pixelFormat,
           bindOut.stride);

    FXE_Present_In presentIn = {};
    presentIn.surfaceHandle = bindOut.surfaceHandle;
    presentIn.frameId = 1;
    size_t outSz4 = sizeof(FXE_Present_Out);
    FXE_Present_Out presentOut = {};
    kr = CallStruct(conn, FXE_SEL_PRESENT, &presentIn, sizeof(presentIn), &presentOut, &outSz4);
    printf("{\"step\":\"Present\",\"kr\":%d,\"rc\":%u,\"completion\":%llu}\n",
           kr,
           presentOut.rc,
           (unsigned long long)presentOut.completionValue);

    FXE_FenceTest_In fenceIn = {0, 2000};
    FXE_FenceTest_Out fenceOut = {};
    size_t outSz7 = sizeof(fenceOut);
    kr = CallStruct(conn, FXE_SEL_FENCE_TEST, &fenceIn, sizeof(fenceIn), &fenceOut, &outSz7);
    printf("{\"step\":\"FenceTest\",\"kr\":%d,\"rc\":%u,\"pass\":%u,\"reason\":%u}\n",
           kr,
           fenceOut.rc,
           fenceOut.pass,
           fenceOut.reason);

    CFRelease(surf);
    IOServiceClose(conn);
    return 0;
}
