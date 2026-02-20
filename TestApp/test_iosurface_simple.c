#include <stdio.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

int main() {
    printf("Testing IOSurface creation with SIP disabled...\n");
    
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    
    int32_t w = 640, h = 480, bpr = 640*4;
    CFNumberRef width = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &w);
    CFNumberRef height = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &h);
    CFNumberRef rowBytes = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpr);
    
    CFDictionarySetValue(props, CFSTR("Width"), width);
    CFDictionarySetValue(props, CFSTR("Height"), height);
    CFDictionarySetValue(props, CFSTR("BytesPerRow"), rowBytes);
    CFDictionarySetValue(props, CFSTR("PixelFormat"), CFSTR("BGRA"));
    
    printf("Creating IOSurface...\n");
    IOSurfaceRef surface = IOSurfaceCreate(props);
    
    CFRelease(width);
    CFRelease(height);
    CFRelease(rowBytes);
    CFRelease(props);
    
    if (surface) {
        uint32_t surfaceID = IOSurfaceGetID(surface);
        printf("✅ SUCCESS! IOSurface created: ID=%u\n", surfaceID);
        CFRelease(surface);
        return 0;
    } else {
        printf("❌ Failed to create IOSurface (returned NULL)\n");
        printf("   This may be due to missing entitlements or other restrictions\n");
        return 1;
    }
}
