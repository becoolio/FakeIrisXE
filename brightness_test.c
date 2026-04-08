#include <stdio.h>
#include <stdlib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <percent>\n", argv[0]);
        printf("Tests: 25, 50, 75, 100\n");
        return 1;
    }
    
    int percent = atoi(argv[1]);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("FakeIrisXEFramebuffer"));
    
    if (!service) {
        service = IOServiceGetMatchingService(kIOMainPortDefault,
            IOServiceMatching("IOFramebuffer"));
    }
    
    if (!service) {
        printf("No framebuffer service found\n");
        return 1;
    }
    
    printf("Found service: 0x%x\n", service);
    
    // Create dictionary with brightness property
    CFDictionaryRef dict = CFDictionaryCreate(kCFAllocatorDefault,
        (const void*[]){ CFSTR("brightness") },
        (const void*[]){ CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &percent) },
        1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    
    kern_return_t ret = IORegistryEntrySetCFProperty(service, dict);
    printf("Set brightness %d%% -> 0x%x\n", percent, ret);
    CFRelease(dict);
    
    // Also try vblm
    uint32_t vblm = (percent * 65535) / 100;
    dict = CFDictionaryCreate(kCFAllocatorDefault,
        (const void*[]){ CFSTR("vblm") },
        (const void*[]){ CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vblm) },
        1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    ret = IORegistryEntrySetCFProperty(service, dict);
    printf("Set vblm %u -> 0x%x\n", vblm, ret);
    CFRelease(dict);
    
    IOObjectRelease(service);
    return 0;
}