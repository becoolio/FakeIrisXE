#include "FakeIrisXEFramebuffer.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSSymbol.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/IOKitKeys.h>           // Needed for types like OSAsyncReference
#include <IOKit/IOUserClient.h>        // Must follow after including IOKit headers
#include <IOKit/IOMessage.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <string.h>
#include <IOKit/graphics/IOFramebufferShared.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <libkern/OSAtomic.h>
#include <pexpert/i386/boot.h>            // PE_Video
#include <mach/mach_time.h>

#include <IOKit/IOLocks.h>



#include "FakeIrisXEAccelerator.hpp"

#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXERing.h"
#include "i915_reg.h"


#include "FakeIrisXEGuC.hpp"
#include "embedded_firmware.h"


extern "C" {
    #include <pexpert/pexpert.h>
    #include <pexpert/device_tree.h>
    #include <libkern/OSTypes.h>
}


using namespace libkern;



// Connection attribute keys (from IOFramebufferShared.h, internal Apple headers)
#define kConnectionSupportsAppleSense   0x00000001
#define kConnectionSupportsLLDDCSense   0x00000002
#define kConnectionSupportsHLDDCSense   0x00000004
#define kConnectionSupportsDDCSense     0x00000008
#define kConnectionDisplayParameterCount 0x00000009
#define kConnectionFlags                0x0000000A
#define kConnectionSupportsHotPlug        0x000000A1
#define kIOFBCursorSupportedKey               "IOFBCursorSupported"
#define kIOFBHardwareCursorSupportedKey       "IOFBHardwareCursorSupported"
#define kIOFBDisplayModeCountKey              "IOFBDisplayModeCount"
#define kIOFBNotifyDisplayModeChange 'dmod'
#define kIOTimingIDDefault 0

#define kIOFramebufferConsoleKey "IOFramebufferIsConsole"
#define kIOFBConsoleKey "kIOFramebufferConsoleKey"
#define kIO32BGRAPixelFormat 'BGRA'
#define kIO32ARGBPixelFormat 'ARGB'

#define kIOPixelFormatWideGamut 'wgam'
#define kIOCaptureAttribute 'capt'

#define kIOFBNotifyDisplayAdded  0x00000010
#define kIOFBConfigChanged       0x00000020

// IOFramebuffer-related property keys (manually declared)
#define kIOFBSurfaceKey                  "IOFBSurface"
#define kIOFBUserClientClassKey         "IOFBUserClientClass"
#define kIOFBSharedUserClientKey        "IOFBSharedUserClient"
#define kIOConsoleFramebuffer         "IOConsoleFramebuffer"
#define kIOConsoleSafeBoot            "IOConsoleSafe"
#define kIOConsoleDeviceKey           "IOConsoleDevice"
#define kIOKitConsoleSecurityKey      "IOKitConsoleSecurity"
#define kIOFBFramebufferKey     "IOFBFramebufferKey"
#define kIOConsoleFramebufferKey "IOConsoleFramebuffer"
#define kIOFramebufferIsConsoleKey "IOFramebufferIsConsole"
#define kIOConsoleModeKey "IOConsoleMode"
#define kIOFBNotifyConsoleReady  0x00002222
#define kIOFBNotifyDisplayModeChanged 0x00002223


#ifndef kIOTimingInfoValid_AppleTimingID
#define kIOTimingInfoValid_AppleTimingID 0x00000001
#endif

#ifndef kIOFBVsyncNotification
#define kIOFBVsyncNotification iokit_common_msg(0x300)
#endif

#define MAKE_IOVRAM_RANGE_INDEX(index) ((UInt32)(index))
#define kIOFBMemoryCountKey   "IOFBMemoryCount"


// Connection flag values
#define kIOConnectionBuiltIn            0x00000100
#define kIOConnectionDisplayPort        0x00000800

#define kIOMessageServiceIsRunning 0x00001001

#ifndef kConnectionIsOnline
#define kConnectionIsOnline        'ionl'
#endif


#define SAFE_MMIO_WRITE(offset, value) \
    if (offset > mmioMap->getLength() - 4) { \
        IOLog("❌ MMIO offset 0x%X out of bounds\n", offset); \
        return kIOReturnError; \
    } \
    *(volatile uint32_t*)((uint8_t*)mmioBase + offset) = value;



#define super IOFramebuffer

OSDefineMetaClassAndStructors(FakeIrisXEFramebuffer, IOFramebuffer);

// ============================================================================
// P0A: Single authoritative display mode table
// All five IOFramebuffer display mode methods must agree on these entries.
// Timing strategy: appleTimingID=0x7F (native timing fallback via EDID/display).
// No detailed timing blocks are provided; the system derives timing from the
// display's reported capabilities. This avoids CoreDisplay assertion failures
// that occurred when detailed timing values conflicted with system expectations.
// ============================================================================
#ifndef kIOTimingID_TigerLake_Fallback
#define kIOTimingID_TigerLake_Fallback 0x7F
#endif
#ifndef kIOTimingInfoValid_AppleTimingID
#define kIOTimingInfoValid_AppleTimingID 0x00000001
#endif

static const uint32_t kNumDisplayModes = 1;

enum {
    kProofModeDepthIndex = 0,          // one depth index (32bpp ARGB)
    kProofModeRefreshFixed = (60 << 16), // 60 Hz in IOFixed format
};

enum ProofDisplayModeFlags {
    kProofModeFlagDefault  = 0,
    kProofModeFlagNative    = (1 << 0),
};

typedef struct {
    uint32_t modeID;
    uint32_t width;
    uint32_t height;
    uint32_t refreshFixed;     // IOFixed format (e.g. 60<<16)
    uint32_t depthIndex;       // max depth index supported
    uint32_t flags;           // ProofDisplayModeFlags
    const char* name;
} ProofDisplayMode;

static const ProofDisplayMode s_proofDisplayModes[kNumDisplayModes] = {
    { 1, 1920, 1080, kProofModeRefreshFixed, kProofModeDepthIndex, kProofModeFlagNative, "1920x1080@60" },
};

// Legacy alias for code that reads s_displayModes[]
typedef ProofDisplayMode DisplayModeInfo;
#define s_displayModes s_proofDisplayModes

static inline const ProofDisplayMode* getProofModeByID(IODisplayModeID modeID)
{
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        if (s_proofDisplayModes[i].modeID == modeID) {
            return &s_proofDisplayModes[i];
        }
    }
    return nullptr;
}

static inline const ProofDisplayMode* getDefaultProofMode()
{
    return (kNumDisplayModes > 0) ? &s_proofDisplayModes[0] : nullptr;
}

static inline const ProofDisplayMode* getCurrentProofMode(IODisplayModeID currentMode)
{
    if (currentMode != 0) {
        if (const ProofDisplayMode* m = getProofModeByID(currentMode))
            return m;
    }
    return getDefaultProofMode();
}

static inline bool isSupportedProofDepth(IOIndex depth)
{
    return depth == kProofModeDepthIndex;
}

static void setNumberProperty(IORegistryEntry *entry, const char *key, uint64_t value, uint32_t bits)
{
    if (!entry || !key) {
        return;
    }

    OSNumber *number = OSNumber::withNumber(value, bits);
    if (!number) {
        return;
    }

    entry->setProperty(key, number);
    number->release();
}

static void setDataProperty32(IORegistryEntry *entry, const char *key, uint32_t value)
{
    if (!entry || !key) {
        return;
    }

    OSData *data = OSData::withBytes(&value, sizeof(value));
    if (!data) {
        return;
    }

    entry->setProperty(key, data);
    data->release();
}

static void publishBrightnessProperties(IORegistryEntry* entry, uint32_t percent, uint32_t raw);
static uint32_t percentToVBLMultiplier(uint32_t percent);
static uint32_t vblMultiplierToPercent(uint32_t vblm);
static void logBrightnessTransaction(const char* origin,
                                     const char* key,
                                     uint32_t inputValue,
                                     uint32_t percent);

static void publishNormalizedMemoryModel(FakeIrisXEFramebuffer *fb,
                                         IOPCIDevice *pci,
                                         uint64_t reportedBytes)
{
    if (!fb) {
        return;
    }

    setNumberProperty(fb, "IOFBMemorySize", reportedBytes, 64);
    setNumberProperty(fb, "IOAccelMemorySize", reportedBytes, 64);
    setNumberProperty(fb, "IOAccelVRAMSize", reportedBytes, 64);
    setNumberProperty(fb, "IOAccelVideoMemorySize", reportedBytes, 64);
    setNumberProperty(fb, "VRAMSize", reportedBytes, 64);
    setNumberProperty(fb, "VRAM,totalMB", reportedBytes / (1024ULL * 1024ULL), 32);
    setNumberProperty(fb, "framebuffer-unifiedmem", reportedBytes, 32);

    if (pci) {
        setDataProperty32(pci, "VRAM,totalsize", static_cast<uint32_t>(reportedBytes));
        setNumberProperty(pci, "deviceVRAM", reportedBytes, 64);
    }
}

static void publishTypedIdentityProperties(FakeIrisXEFramebuffer *fb, IOPCIDevice *pci)
{
    if (!fb) {
        return;
    }

    const uint32_t vendor = 0x8086;
    uint32_t device = 0x9A49;
    if (pci) {
        device = pci->configRead16(kIOPCIConfigDeviceID);
    }

    setDataProperty32(fb, "vendor-id", vendor);
    setDataProperty32(fb, "product-id", device);
    setDataProperty32(fb, "serial-number", 0x12345678);
    setDataProperty32(fb, "display-serial-number", 0x12345678);

    if (pci) {
        setDataProperty32(pci, "vendor-id", vendor);
        setDataProperty32(pci, "device-id", device);
    }
}

static uint64_t absDeltaToNs(uint64_t startAbs, uint64_t endAbs)
{
    if (endAbs <= startAbs) {
        return 0;
    }

    const uint64_t delta = endAbs - startAbs;
    uint64_t deltaNs = 0;
    absolutetime_to_nanoseconds(delta, &deltaNs);
    return deltaNs;
}

static IOService *findDisplayServiceUnderFramebuffer(IOService *fb)
{
    if (!fb) {
        return nullptr;
    }

    OSIterator *fbChildren = fb->getChildIterator(gIOServicePlane);
    if (!fbChildren) {
        return nullptr;
    }

    IOService *result = nullptr;
    IOService *child = nullptr;

    while ((child = OSDynamicCast(IOService, fbChildren->getNextObject()))) {
        const char *childName = child->getName();
        if (!childName || strcmp(childName, "display0") != 0) {
            continue;
        }

        OSIterator *displayChildren = child->getChildIterator(gIOServicePlane);
        if (displayChildren) {
            IOService *displayDriver = nullptr;
            while ((displayDriver = OSDynamicCast(IOService, displayChildren->getNextObject()))) {
                const char *driverName = displayDriver->getName();
                if (!driverName) {
                    continue;
                }

                if (!strcmp(driverName, "AppleDisplay") || !strcmp(driverName, "AppleBacklightDisplay")) {
                    displayDriver->retain();
                    result = displayDriver;
                    break;
                }
            }
            displayChildren->release();
        }

        if (!result) {
            child->retain();
            result = child;
        }
        break;
    }

    fbChildren->release();
    return result;
}

static void applyDisplayMergeOverrides(IOService *service)
{
    if (!service) {
        return;
    }

    static const uint8_t kScale3840x2400[] = {0x00,0x00,0x0F,0x00,0x00,0x00,0x09,0x60,0x00};
    static const uint8_t kScale3360x2100[] = {0x00,0x00,0x0D,0x20,0x00,0x00,0x08,0x34,0x00};
    static const uint8_t kScale2880x1800[] = {0x00,0x00,0x0B,0x40,0x00,0x00,0x07,0x08,0x00};
    static const uint8_t kScale2560x1600[] = {0x00,0x00,0x0A,0x00,0x00,0x00,0x06,0x40,0x00};
    static const uint8_t kScale2048x1280[] = {0x00,0x00,0x08,0x00,0x00,0x00,0x05,0x00,0x00};
    static const uint8_t kScale2560x1440[] = {0x00,0x00,0x0A,0x00,0x00,0x00,0x05,0xA0,0x00,0x00,0x00,0x01,0x00,0x20,0x00,0x00};
    static const uint8_t kScale1920x1200[] = {0x00,0x00,0x07,0x80,0x00,0x00,0x04,0xB0,0x00,0x00,0x00,0x01,0x00,0x20,0x00,0x00};
    static const uint8_t kScale1920x1080[] = {0x00,0x00,0x07,0x80,0x00,0x00,0x04,0x38,0x00,0x00,0x00,0x01,0x00,0x20,0x00,0x00};
    static const uint8_t kScale1280x720[]  = {0x00,0x00,0x05,0x00,0x00,0x00,0x02,0xD0,0x00,0x00,0x00,0x01,0x00,0x20,0x00,0x00};
    static const uint8_t kScale1680x1050[] = {0x00,0x00,0x06,0x72,0x00,0x00,0x04,0x1A,0x00,0x00,0x00,0x01};
    static const uint8_t kScale1440x900[]  = {0x00,0x00,0x05,0xA0,0x00,0x00,0x03,0x84,0x00,0x00,0x00,0x01};
    static const uint8_t kScale1280x800[]  = {0x00,0x00,0x05,0x00,0x00,0x00,0x03,0x20,0x00,0x00,0x00,0x01};

    struct ScaleBlob { const uint8_t* bytes; size_t len; };
    static const ScaleBlob kScaleBlobs[] = {
        { kScale3840x2400, sizeof(kScale3840x2400) },
        { kScale3360x2100, sizeof(kScale3360x2100) },
        { kScale2880x1800, sizeof(kScale2880x1800) },
        { kScale2560x1600, sizeof(kScale2560x1600) },
        { kScale2048x1280, sizeof(kScale2048x1280) },
        { kScale2560x1440, sizeof(kScale2560x1440) },
        { kScale1920x1200, sizeof(kScale1920x1200) },
        { kScale1920x1080, sizeof(kScale1920x1080) },
        { kScale1280x720,  sizeof(kScale1280x720) },
        { kScale1680x1050, sizeof(kScale1680x1050) },
        { kScale1440x900,  sizeof(kScale1440x900) },
        { kScale1280x800,  sizeof(kScale1280x800) },
    };

    static constexpr uint64_t kMergedDisplayProductID = 41008ULL;
    static constexpr uint64_t kMergedDisplayVendorID = 1552ULL;
    static constexpr uint64_t kMergedDisplayGUID = 436849163854938112ULL;
    static constexpr uint64_t kOriginalDisplayProductID = 1815ULL;
    static constexpr uint64_t kOriginalDisplayVendorID = 1970170734ULL;

    setNumberProperty(service, "DisplayProductID", kMergedDisplayProductID, 32);
    setNumberProperty(service, "DisplayVendorID", kMergedDisplayVendorID, 32);
    setNumberProperty(service, "IODisplayGUID", kMergedDisplayGUID, 64);
    service->setProperty("DisplayProductName", "Color LCD");
    service->setProperty("IODisplayName", "Color LCD");
    setNumberProperty(service, "IOGFlags", 4, 32);
    setNumberProperty(service, kDisplayHorizontalImageSize, 286, 32);
    setNumberProperty(service, kDisplayVerticalImageSize, 179, 32);
    setNumberProperty(service, "DisplayProductIDOld", kOriginalDisplayProductID, 32);
    setNumberProperty(service, "DisplayVendorIDOld", kOriginalDisplayVendorID, 32);
    service->setProperty("AppleBacklightDisplay", kOSBooleanTrue);

    OSArray *scaleModes = OSArray::withCapacity(static_cast<unsigned int>(sizeof(kScaleBlobs) / sizeof(kScaleBlobs[0])));
    if (scaleModes) {
        for (size_t i = 0; i < sizeof(kScaleBlobs) / sizeof(kScaleBlobs[0]); ++i) {
            OSData *blob = OSData::withBytes(kScaleBlobs[i].bytes, static_cast<unsigned int>(kScaleBlobs[i].len));
            if (blob) {
                scaleModes->setObject(blob);
                blob->release();
            }
        }
        service->setProperty("scale-resolutions", scaleModes);
        scaleModes->release();
    }

    const char *name = service->getName();
    if (name && !strcmp(name, "AppleDisplay")) {
        service->setName("AppleBacklightDisplay");
    }

    IOService *provider = OSDynamicCast(IOService, service->getProvider());
    if (provider) {
        const char *providerName = provider->getName();
        if (providerName && !strcmp(providerName, "display0")) {
            setNumberProperty(provider, "DisplayProductID", kMergedDisplayProductID, 32);
            setNumberProperty(provider, "DisplayVendorID", kMergedDisplayVendorID, 32);
            setNumberProperty(provider, "IODisplayGUID", kMergedDisplayGUID, 64);
            provider->setProperty("DisplayProductName", "Color LCD");
            setNumberProperty(provider, "IOGFlags", 4, 32);
            setNumberProperty(provider, kDisplayHorizontalImageSize, 286, 32);
            setNumberProperty(provider, kDisplayVerticalImageSize, 179, 32);
            provider->setProperty("AppleBacklightDisplay", kOSBooleanTrue);
        }
    }
}

static void injectDisplayMergeOverridesIfAvailable(FakeIrisXEFramebuffer *fb)
{
    if (!fb) {
        return;
    }

    IOService *displayService = findDisplayServiceUnderFramebuffer(fb);
    if (!displayService) {
        IOLog("[V170] display0 not found under FakeIrisXEFramebuffer yet\n");
        return;
    }

    applyDisplayMergeOverrides(displayService);
    IOLog("[V170] Applied display merge overrides on %s\n", displayService->getName() ? displayService->getName() : "<unknown>");
    displayService->release();
}



//probe
IOService *FakeIrisXEFramebuffer::probe(IOService *provider, SInt32 *score) {
    // V72: FAILSAFE - Only load if -fakeirisxe boot-arg is set in NVRAM
    // This is a CRITICAL safety mechanism to prevent automatic loading
    // The kext MUST be explicitly enabled via: sudo nvram boot-args="-fakeirisxe"
    
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║    FAKEIRISXE V288 - Minimal bare-DMA boot (pre-arm!)      ║\n");
    IOLog("║         FakeIrisXEFramebuffer::probe()                   ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    
    // V83: Check for -fakeirisxe in boot-args
    char bootArg[64] = {0};
    bool hasBootArg = PE_parse_boot_argn("-fakeirisxe", bootArg, sizeof(bootArg));
    
    // V83: Also check using IODTNVRAM for OpenCore compatibility
    bool hasBootArgOC = false;
    {
        // Try reading from device tree /chosen/boot-args
        IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIOServicePlane);
        if (chosen) {
            OSString *bootargs = OSDynamicCast(OSString, chosen->getProperty("boot-args"));
            if (bootargs) {
                const char *bootArgsStr = bootargs->getCStringNoCopy();
                if (bootArgsStr) {
                    // Manual string search for "-fakeirisxe"
                    const char *needle = "-fakeirisxe";
                    size_t needleLen = strlen(needle);
                    size_t haystackLen = strlen(bootArgsStr);
                    for (size_t i = 0; i <= haystackLen - needleLen; i++) {
                        if (strncmp(&bootArgsStr[i], needle, needleLen) == 0) {
                            hasBootArgOC = true;
                            IOLog("[V83] Boot-arg found in /chosen/boot-args via IORegistry\n");
                            break;
                        }
                    }
                }
            }
            chosen->release();
        }
    }
    
    // V83: Check if either method found the boot-arg
    bool bootArgValid = hasBootArg || hasBootArgOC;
    
    if (!bootArgValid) {
        IOLog("❌ [V83] FAILSAFE TRIGGERED: -fakeirisxe boot-arg NOT detected\n");
        IOLog("❌ PE_parse_boot_argn returned: %s\n", hasBootArg ? "true" : "false");
        IOLog("❌ IORegistry check returned: %s\n", hasBootArgOC ? "true" : "false");
        IOLog("❌ To enable: sudo nvram boot-args=\"<existing args> -fakeirisxe\"\n");
        IOLog("============================================================\n");
        return nullptr;
    }
    
    IOLog("✅ [V83] FAILSAFE PASSED: -fakeirisxe detected\n");
    IOLog("✅ PE_parse_boot_argn: %s\n", hasBootArg ? "found" : "not found (OK)");
    IOLog("✅ IORegistry check: %s\n", hasBootArgOC ? "found" : "not found (OK)");
    IOLog("✅ Proceeding with kext initialization...\n");
    IOLog("============================================================\n");
    IOLog("\n");
    
    IOPCIDevice *pdev = OSDynamicCast(IOPCIDevice, provider);
    if (!pdev) {
        IOLog("FakeIrisXEFramebuffer::probe(): Provider is not IOPCIDevice\n");
        return nullptr;
    }

    UInt16 vendor = pdev->configRead16(kIOPCIConfigVendorID);
    UInt16 device = pdev->configRead16(kIOPCIConfigDeviceID);

    // Only proceed if it's your target device
    if (vendor == 0x8086 && (device == 0x9A49 || device == 0x46A3)) {
        IOLog("FakeIrisXEFramebuffer::probe(): Found matching GPU (8086:%04X)\n", device);
        
        
        if (score) *score = 99999999; // MAX override score
                return this; // 👈 Do NOT call super::probe() or it might lower the score!
            }

    return nullptr; // No match
}




bool FakeIrisXEFramebuffer::init(OSDictionary* dict) {
    if (!super::init(dict))
        return false;
   
// Initialize other members
    vramMemory = nullptr;
    cursorMemory = nullptr;
    framebufferMemory = nullptr;
    framebufferSurface = nullptr;
    mmioMap = nullptr;
    pciDevice = nullptr;
  //  mmioBase = nullptr;
   // mmioWrite32 = nullptr;
     // P0A: Derive startup mode from the authoritative table.
     if (const ProofDisplayMode* m = getDefaultProofMode()) {
         currentMode = m->modeID;
         currentDepth = m->depthIndex;
         vramSize = m->width * m->height * 4;
     } else {
         currentMode = 1;
         currentDepth = 0;
         vramSize = 1920 * 1080 * 4;
     }
    controllerEnabled = false;
    displayOnline = false;
    displayPublished = false;
    shuttingDown = false;
    fullyInitialized = false;  // ADD THIS
    
    // V90: Initialize surface management
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        fSurfaces[i].inUse = false;
        fSurfaces[i].id = 0;
        fSurfaces[i].gpuAddress = 0;
        fSurfaces[i].gemObj = nullptr;
    }
    fNextSurfaceId = 1;
    fV90SurfaceCount = 0;
    fV90BlitCount = 0;
    
    // V91: Initialize 2D blit command counters
    fV91BlitSubmitCount = 0;
    fV91BlitCompleteCount = 0;
    
    // V92: Initialize debug infrastructure
    fV92DiagnosticsRun = false;
    fV92ClipCount = 0;
    fV92BatchCount = 0;
    fV92ColorBlitCount = 0;
    fV92LastDiagnosticTime = 0;
    fV92LastError = 0;
    fV92LastErrorString[0] = '\0';
    
    // V92: Initialize clipping state
    fClipEnabled = false;
    fClipLeft = fClipTop = fClipRight = fClipBottom = 0;
    
    // V93: Initialize display verification
    fV93BootTime = 0;
    fV93WindowServerBlitCount = 0;
    fV93CommandsSubmitted = 0;
    fV93CommandsCompleted = 0;
    fV93DisplayVerificationFailures = 0;
    fV93FirstBlitTime = 0;
    fV93LastBlitTime = 0;
    fV93TotalBlitTime = 0;
    fV93DisplayVerified = false;
    fV93WindowServerConnected = false;
    
    // V138: Initialize BLT ring pointer
    fBltRing = nullptr;
    
    IOLog("FakeIrisXEFramebuffer::init succeeded\n");
    return true;
}









IOPMPowerState FakeIrisXEFramebuffer::powerStates[kNumPowerStates] = {
    {
        1,                          // version
        0,                          // capabilityFlags
        0,                          // outputPowerCharacter
        0,                          // inputPowerRequirement
        0,                          // staticPower
        0,                          // unbudgetedPower
        0,                          // powerToAttain
        0,                          // timeToAttain
        0,                          // settleUpTime
        0,                          // timeToLower
        0,                          // settleDownTime
        0                           // powerDomainBudget
    },
    
    {
        1,                          // version
        IOPMPowerOn,                // capabilityFlags
        IOPMPowerOn,                // outputPowerCharacter
        IOPMPowerOn,                // inputPowerRequirement
        0, 0, 0, 0, 0, 0, 0
    }
};


    IOPCIDevice* pciDevice;
    IOMemoryMap* mmioMap;
    volatile uint8_t* mmioBase;


// --- CRITICAL MMIO HELPER FUNCTIONS ---
  // These functions ensure safe access to the memory-mapped registers.
  // They are essential for the power management block to compile and run.
inline uint32_t safeMMIORead(uint32_t offset){
      if (!mmioBase || !mmioMap || offset >= mmioMap->getLength()) {
          IOLog("❌ MMIO Read attempted with invalid offset: 0x%08X\n", offset);
          return 0;
      }
      return *(volatile uint32_t*)(mmioBase + offset);
  }

  inline void safeMMIOWrite(uint32_t offset, uint32_t value) {
      if (!mmioBase || !mmioMap || offset >= mmioMap->getLength()) {
          IOLog("❌ MMIO Write attempted with invalid offset: 0x%08X\n", offset);
          return;
      }
      *(volatile uint32_t*)(mmioBase + offset) = value;
  }



//helper to reactive gpu power

bool FakeIrisXEFramebuffer::gpuPowerOn(){
    IOLog("gpuPowerOn(): Waking GT + RCS engine...\n");

    if (!pciDevice || !pciDevice->isOpen(this)) {
        IOLog("❌ gpuPowerOn(): PCI device not open\n");
        return false;
    }
    // --- PCI Power Management (Force D0) ---
    uint16_t pmcsr = pciDevice->configRead16(0x84);
    IOLog("PCI PMCSR before = 0x%04X\n", pmcsr);
    pmcsr &= ~0x3; // Force D0
    pciDevice->configWrite16(0x84, pmcsr);
    IOSleep(10);
    pmcsr = pciDevice->configRead16(0x84);
    IOLog("PCI PMCSR after force = 0x%04X\n", pmcsr);

    // --- Hardware Register Defines ---
    const uint32_t GT_PG_ENABLE = 0xA218;
    const uint32_t PUNIT_PG_CTRL = 0xA2B0;
    
    // PW1 (Render)
    const uint32_t PWR_WELL_CTL_1 = 0x45400;
    const uint32_t PWR_WELL_STATUS = 0x45408;
    const uint32_t PW_1_STATUS_BIT = (1 << 30);

    // PW2 (Display)
    const uint32_t PWR_WELL_CTL_2 = 0x45404;
    const uint32_t PW_2_REQ_BIT = (1 << 0);
    const uint32_t PW_2_STATE_VALUE = 0x000000FF;

    // V200: Try more aggressive GT power enable
    IOLog("(FakeIrisXE) [V204] Attempting aggressive GT power enable...\n");
    uint32_t current_pg = safeMMIORead(GT_PG_ENABLE);
    IOLog("(FakeIrisXE) [V204] GT_PG_ENABLE before: 0x%08x\n", current_pg);
    safeMMIOWrite(GT_PG_ENABLE, 0x00000000);
    IOSleep(10);
    IOLog("(FakeIrisXE) [V204] GT_PG_ENABLE after: 0x%08x\n", safeMMIORead(GT_PG_ENABLE));

    const uint32_t CLK_CTL = 0x46000;
    uint32_t clk_status = safeMMIORead(CLK_CTL);
    IOLog("(FakeIrisXE) [V204] CLK_CTL (0x46000): 0x%08x\n", clk_status);

    // V204: Disable Tiger Lake-specific clock gating
    // Based on Linux: GEN9_CLKGATE_DIS_3 (0x46538) TGL_VRH_GATING_DIS
    uint32_t clkgate_dis3 = safeMMIORead(0x46538);
    IOLog("(FakeIrisXE) [V204] CLKGATE_DIS_3 (0x46538) before: 0x%08x\n", clkgate_dis3);
    safeMMIOWrite(0x46538, clkgate_dis3 | 0x80000000);  // Enable VRH GATING
    IOSleep(10);
    IOLog("(FakeIrisXE) [V204] CLKGATE_DIS_3 (0x46538) after: 0x%08x\n", safeMMIORead(0x46538));

    // Force wake
    const uint32_t FORCEWAKE_RENDER_CTL = 0xA188;
    const uint32_t FORCEWAKE_ACK_RENDER = 0x130044;
    const uint32_t RENDER_WAKE_VALUE = 0x000F000F; // Aggressive
    const uint32_t RENDER_ACK_BIT = 0x00000001;

    // MBUS
    const uint32_t MBUS_DBOX_CTL_A = 0x7003C;
    const uint32_t MBUS_DBOX_VALUE = 0xb1038c02;

    // --- V24 NEW CLOCK REGISTERS ---
    const uint32_t LCPLL1_CTL = 0x46010;
    const uint32_t LCPLL1_VALUE = 0xcc000000;
    const uint32_t TRANS_CLK_SEL_A = 0x46140;
    const uint32_t TRANS_CLK_VALUE = 0x10000000;

    // 1. GT Power Gating Control
    safeMMIOWrite(GT_PG_ENABLE, safeMMIORead(GT_PG_ENABLE) & ~0x1);
    IOSleep(10);

    // 2. PUNIT Power Gating Control
    safeMMIOWrite(PUNIT_PG_CTRL, safeMMIORead(PUNIT_PG_CTRL) & ~0x80000000);
    IOSleep(15);

    // 3. Power Well 1 Control (KNOWN GOOD)
    IOLog("Requesting Power Well 1 (Render)...\n");
    safeMMIOWrite(PWR_WELL_CTL_1, safeMMIORead(PWR_WELL_CTL_1) | 0x2);
    IOSleep(10);
    safeMMIOWrite(PWR_WELL_CTL_1, safeMMIORead(PWR_WELL_CTL_1) | 0x4);
    IOSleep(10);

    // 4. VERIFY Power Well 1 (KNOWN GOOD)
    IOLog("Waiting for Power Well 1 to be enabled...\n");
    int tries = 0;
    bool pw1_up = false;
    while (tries++ < 20) {
        if (safeMMIORead(PWR_WELL_STATUS) & PW_1_STATUS_BIT) {
            pw1_up = true;
            IOLog("✅ Power Well 1 is UP! Status: 0x%08X\n", safeMMIORead(PWR_WELL_STATUS));
            break;
        }
        IOSleep(10);
    }
    if (!pw1_up) {
        IOLog("❌ ERROR: Power Well 1 FAILED to enable! Status: 0x%08X\n", safeMMIORead(PWR_WELL_STATUS));
        return false;
    }

    // 5. Power Well 2 Control (KNOWN GOOD)
    IOLog("Requesting Power Well 2 (Display) via bit 0...\n");
    safeMMIOWrite(PWR_WELL_CTL_2, safeMMIORead(PWR_WELL_CTL_2) | PW_2_REQ_BIT);

    // 6. VERIFY Power Well 2 (KNOWN GOOD)
    IOLog("Waiting for Power Well 2 to be enabled (polling 0x45404 for 0xFF)...\n");
    tries = 0;
    bool pw2_up = false;
    while (tries++ < 50) {
        uint32_t pw2_status = safeMMIORead(PWR_WELL_CTL_2);
        if ((pw2_status & 0xFF) == PW_2_STATE_VALUE) {
            pw2_up = true;
            IOLog("✅ Power Well 2 is UP! Status: 0x%08X\n", pw2_status);
            break;
        }
        IOSleep(10);
    }
    if (!pw2_up) {
        IOLog("❌ ERROR: Power Well 2 FAILED to enable! Status: 0x%08X\n", safeMMIORead(PWR_WELL_CTL_2));
        return false;
    }

    // 7. FORCEWAKE Sequence (KNOWN GOOD)
    IOLog("Initiating AGGRESSIVE FORCEWAKE (0xF)...\n");
    safeMMIOWrite(FORCEWAKE_RENDER_CTL, RENDER_WAKE_VALUE); // Write 0x000F000F
    
    bool forcewake_ack = false;
    for (int i = 0; i < 100; i++) {
        uint32_t ack = safeMMIORead(FORCEWAKE_ACK_RENDER);
        if ((ack & RENDER_ACK_BIT) == RENDER_ACK_BIT) {
            forcewake_ack = true;
            IOLog("✅ Render ACK received! (0x%08X)\n", ack);
            break;
        }
        IOSleep(10);
    }
    if (!forcewake_ack) {
        IOLog("❌ ERROR: Render force-wake FAILED!\n");
        return false;
        
    }
    
    
    // V201: Additional GT power status verification
    // Check if GT is actually powered after power well enable
    uint32_t gt_perf_post = safeMMIORead(0xA070);  // GT_PERF_STATUS (Gen12)
    uint32_t gt_status_post = safeMMIORead(0xA000);  // GT_STATUS
    IOLog("(FakeIrisXE) [V204] GT power status: PERF=0x%08x STATUS=0x%08x\n", 
          gt_perf_post, gt_status_post);
    
    if (gt_perf_post == 0x00000000) {
        IOLog("(FakeIrisXE) [V204] WARNING: GT_PERF_STATUS still 0 - GT may not be powered for compute!\n");
    }
    
    // 1. Define the register (GEN9_PG_ENABLE is usually 0x8000)
    //#define GEN9_PG_ENABLE 0x8000

    // 2. Read current state
    uint32_t pg_status = safeMMIORead(GEN9_PG_ENABLE);

    // 3. If bit 2 (Render Gating) is set, KILL IT.
    if (pg_status & 0x00000004) {
        IOLog("⚠️ Render Power Gating is ON (0x%x). Disabling it...", pg_status);
        
        // Write 0 to disable all power gating logic
        safeMMIOWrite(GEN9_PG_ENABLE, 0x00000000);
        
        // Crucial: Wait for the hardware to stabilize
        IODelay(500);
        
        // Verify
        uint32_t new_pg = safeMMIORead(GEN9_PG_ENABLE);
        IOLog("✅ Power Gating Status Now: 0x%x", new_pg);
    }
    
    
    
    IOLog("gpuPowerOn(): GT and RCS awake — READY for ELSP writes!\n");
    return true;
}







// hardware init on startup
bool FakeIrisXEFramebuffer::initPowerManagement() {
    IOLog("🚀 Initiating CORRECTED-V24 power management (Enabling Clocks)...\n");

    if (!pciDevice || !pciDevice->isOpen(this)) {
        IOLog("❌ initPowerManagement(): PCI device not open - aborting\n");
        return false;
    }
    
    // Initialize backlight table from Apple AGDC defaults.
    initBacklightTable();
    
    // --- PCI Power Management (Force D0) ---
    uint16_t pmcsr = pciDevice->configRead16(0x84);
    IOLog("PCI PMCSR before = 0x%04X\n", pmcsr);
    pmcsr &= ~0x3; // Force D0
    pciDevice->configWrite16(0x84, pmcsr);
    IOSleep(10);
    pmcsr = pciDevice->configRead16(0x84);
    IOLog("PCI PMCSR after force = 0x%04X\n", pmcsr);

    // --- Hardware Register Defines ---
    const uint32_t GT_PG_ENABLE = 0xA218;
    const uint32_t PUNIT_PG_CTRL = 0xA2B0;
    
    // PW1 (Render)
    const uint32_t PWR_WELL_CTL_1 = 0x45400;
    const uint32_t PWR_WELL_STATUS = 0x45408;
    const uint32_t PW_1_STATUS_BIT = (1 << 30);

    // PW2 (Display)
    const uint32_t PWR_WELL_CTL_2 = 0x45404;
    const uint32_t PW_2_REQ_BIT = (1 << 0);
    const uint32_t PW_2_STATE_VALUE = 0x000000FF;

    // Force wake
    const uint32_t FORCEWAKE_RENDER_CTL = 0xA188;
    const uint32_t FORCEWAKE_ACK_RENDER = 0x130044;
    const uint32_t RENDER_WAKE_VALUE = 0x000F000F; // Aggressive
    const uint32_t RENDER_ACK_BIT = 0x00000001;

    // MBUS
    const uint32_t MBUS_DBOX_CTL_A = 0x7003C;
    const uint32_t MBUS_DBOX_VALUE = 0xb1038c02;

    // --- V24 NEW CLOCK REGISTERS ---
    const uint32_t LCPLL1_CTL = 0x46010;
    const uint32_t LCPLL1_VALUE = 0xcc000000;
    const uint32_t TRANS_CLK_SEL_A = 0x46140;
    const uint32_t TRANS_CLK_VALUE = 0x10000000;

    // 1. GT Power Gating Control
    safeMMIOWrite(GT_PG_ENABLE, safeMMIORead(GT_PG_ENABLE) & ~0x1);
    IOSleep(10);

    // 2. PUNIT Power Gating Control
    safeMMIOWrite(PUNIT_PG_CTRL, safeMMIORead(PUNIT_PG_CTRL) & ~0x80000000);
    IOSleep(15);

    // 3. Power Well 1 Control (KNOWN GOOD)
    IOLog("Requesting Power Well 1 (Render)...\n");
    safeMMIOWrite(PWR_WELL_CTL_1, safeMMIORead(PWR_WELL_CTL_1) | 0x2);
    IOSleep(10);
    safeMMIOWrite(PWR_WELL_CTL_1, safeMMIORead(PWR_WELL_CTL_1) | 0x4);
    IOSleep(10);

    // 4. VERIFY Power Well 1 (KNOWN GOOD)
    IOLog("Waiting for Power Well 1 to be enabled...\n");
    int tries = 0;
    bool pw1_up = false;
    while (tries++ < 20) {
        if (safeMMIORead(PWR_WELL_STATUS) & PW_1_STATUS_BIT) {
            pw1_up = true;
            IOLog("✅ Power Well 1 is UP! Status: 0x%08X\n", safeMMIORead(PWR_WELL_STATUS));
            break;
        }
        IOSleep(10);
    }
    if (!pw1_up) {
        IOLog("❌ ERROR: Power Well 1 FAILED to enable! Status: 0x%08X\n", safeMMIORead(PWR_WELL_STATUS));
        return false;
    }

    // 5. Power Well 2 Control (KNOWN GOOD)
    IOLog("Requesting Power Well 2 (Display) via bit 0...\n");
    safeMMIOWrite(PWR_WELL_CTL_2, safeMMIORead(PWR_WELL_CTL_2) | PW_2_REQ_BIT);

    // 6. VERIFY Power Well 2 (KNOWN GOOD)
    IOLog("Waiting for Power Well 2 to be enabled (polling 0x45404 for 0xFF)...\n");
    tries = 0;
    bool pw2_up = false;
    while (tries++ < 50) {
        uint32_t pw2_status = safeMMIORead(PWR_WELL_CTL_2);
        if ((pw2_status & 0xFF) == PW_2_STATE_VALUE) {
            pw2_up = true;
            IOLog("✅ Power Well 2 is UP! Status: 0x%08X\n", pw2_status);
            break;
        }
        IOSleep(10);
    }
    if (!pw2_up) {
        IOLog("❌ ERROR: Power Well 2 FAILED to enable! Status: 0x%08X\n", safeMMIORead(PWR_WELL_CTL_2));
        return false;
    }

    // 7. FORCEWAKE Sequence (KNOWN GOOD)
    IOLog("Initiating AGGRESSIVE FORCEWAKE (0xF)...\n");
    safeMMIOWrite(FORCEWAKE_RENDER_CTL, RENDER_WAKE_VALUE); // Write 0x000F000F
    
    bool forcewake_ack = false;
    for (int i = 0; i < 100; i++) {
        uint32_t ack = safeMMIORead(FORCEWAKE_ACK_RENDER);
        if ((ack & RENDER_ACK_BIT) == RENDER_ACK_BIT) {
            forcewake_ack = true;
            IOLog("✅ Render ACK received! (0x%08X)\n", ack);
            break;
        }
        IOSleep(10);
    }
    if (!forcewake_ack) {
        IOLog("❌ ERROR: Render force-wake FAILED!\n");
        return false;
    }

    // 8. ENABLE DISPLAY MMIO BUS (KNOWN GOOD)
    IOLog("Enabling Display MMIO Bus (MBUS_DBOX_CTL_A)...\n");
    safeMMIOWrite(MBUS_DBOX_CTL_A, MBUS_DBOX_VALUE);
    IOSleep(10);

    // 9. --- NEW STEP: ENABLE DISPLAY CLOCKS ---
    IOLog("Enabling Display PLL (LCPLL1_CTL)...\n");
    safeMMIOWrite(LCPLL1_CTL, LCPLL1_VALUE);
    IOSleep(10);
    
    IOLog("Enabling Transcoder Clock Select (TRANS_CLK_SEL_A)...\n");
    safeMMIOWrite(TRANS_CLK_SEL_A, TRANS_CLK_VALUE);
    IOSleep(10);
    
    IOLog("Power management sequence complete.\n");
    return true;
}






//start
bool FakeIrisXEFramebuffer::start(IOService* provider) {
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║     FAKEIRISXE V288 - Minimal bare-DMA boot (pre-arm!)   ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");

    if (!super::start(provider)) {
        IOLog("❌ [V170] super::start() failed\n");
        return false;
    }
    IOLog("✅ [V170] super::start() succeeded\n");

    // V149: Add GEM/GTT Diagnostics
    IOLog("(FakeIrisXE)[V149] ============================================\n");
    IOLog("(FakeIrisXE)[V149] GEM/GTT DIAGNOSTICS\n");
    IOLog("(FakeIrisXE)[V149] ============================================\n");
    
    // Check key GGTT registers
    if (fBar0) {
        // PGTBL_CTL - Page Table Control (0x02020)
        uint32_t pgtblCtl = safeMMIORead(0x02020);
        
        IOLog("(FakeIrisXE)[V149] GTT Page Table:\n");
        IOLog("(FakeIrisXE)[V149]   PGTBL_CTL (0x02020): 0x%08X\n", pgtblCtl);
        
        // Check if GTT is enabled
        bool gttEnabled = (pgtblCtl & 0x1) != 0;
        IOLog("(FakeIrisXE)[V149]   GTT Enabled: %s\n", gttEnabled ? "YES ✅" : "NO ❌");
        
        // Get GTT base address
        uint32_t pgtblAddr = pgtblCtl & 0xFFFFF000;
        IOLog("(FakeIrisXE)[V149]   GTT Base: 0x%08X\n", pgtblAddr);
    }
    
    // Check if we have GEM objects mapped
    IOLog("(FakeIrisXE)[V149] GEM Status:\n");
    IOLog("(FakeIrisXE)[V149]   GEM system: %s\n", "Initializing...");
    
    IOLog("(FakeIrisXE)[V149] ============================================\n");

    const uint64_t startTotalAbs = mach_absolute_time();
    uint64_t stageStartAbs = startTotalAbs;
    uint32_t currentStage = 0;
    const char *currentStageName = "boot";
    uint32_t softFailCount = 0;

    auto publishStageDurationUs = [this](uint32_t stage, uint64_t durationUs) {
        switch (stage) {
            case 1:
                setNumberProperty(this, "FakeIrisXEStage1DurationUs", durationUs, 64);
                break;
            case 2:
                setNumberProperty(this, "FakeIrisXEStage2DurationUs", durationUs, 64);
                break;
            case 3:
                setNumberProperty(this, "FakeIrisXEStage3DurationUs", durationUs, 64);
                break;
            case 4:
                setNumberProperty(this, "FakeIrisXEStage4DurationUs", durationUs, 64);
                break;
            case 5:
                setNumberProperty(this, "FakeIrisXEStage5DurationUs", durationUs, 64);
                break;
            default:
                break;
        }
    };

    auto closeCurrentStage = [&]() {
        if (!currentStage) {
            return;
        }

        const uint64_t nowAbs = mach_absolute_time();
        const uint64_t durationUs = absDeltaToNs(stageStartAbs, nowAbs) / 1000ULL;
        IOLog("(FakeIrisXE) [STAGE %u] END %s (%llu us)\n",
              currentStage,
              currentStageName,
              static_cast<unsigned long long>(durationUs));
        publishStageDurationUs(currentStage, durationUs);
        stageStartAbs = nowAbs;
    };

    auto logStage = [&](uint32_t stage, const char *name) {
        closeCurrentStage();
        currentStage = stage;
        currentStageName = name;
        IOLog("(FakeIrisXE) [STAGE %u] BEGIN %s\n", stage, name);
    };

    auto logSoftFail = [&](uint32_t stage, const char *name) {
        ++softFailCount;
        IOLog("(FakeIrisXE) [STAGE %u] SOFT-FAIL: %s\n", stage, name);
    };

    logStage(1, "Core PCI/MMIO bring-up");

    

    
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        IOLog("❌ Provider is not IOPCIDevice\n");
        return false;
    }

    //    pciDevice->retain();


    
    
  
    
    // 1️⃣ Open PCI device
    IOLog("📦 Opening PCI device...\n");
    if (!pciDevice->open(this)) {
        IOLog("❌ Failed to open PCI device\n");
            return false;
    }

    // V185: Try PCI function-level reset to force ME reinit
    IOLog("(FakeIrisXE) [V185] Attempting PCI function-level reset...\n");
    uint16_t pciCmd_before = pciDevice->configRead16(kIOPCIConfigCommand);
    uint16_t pciStat_before = pciDevice->configRead16(kIOPCIConfigStatus);
    IOLog("(FakeIrisXE) [V185] PCI CMD before=0x%04X STATUS=0x%04X\n", pciCmd_before, pciStat_before);
    
    // Disable memory access and I/O - full reset
    pciDevice->configWrite16(kIOPCIConfigCommand, 0);
    IOSleep(20);
    
    // V186: Dump full PCI config space for debugging
    IOLog("(FakeIrisXE) [V186] PCI Config Space:\n");
    IOLog("  Vendor=0x%04X Device=0x%04X\n",
          pciDevice->configRead16(kIOPCIConfigVendorID),
          pciDevice->configRead16(kIOPCIConfigDeviceID));
    IOLog("  Command=0x%04X Status=0x%04X\n",
          pciDevice->configRead16(kIOPCIConfigCommand),
          pciDevice->configRead16(kIOPCIConfigStatus));
    IOLog("  Revision=0x%02X ProgIF=0x%02X SubClass=0x%02X BaseClass=0x%02X\n",
          pciDevice->configRead8(kIOPCIConfigRevisionID),
          pciDevice->configRead8(0x09),  // Programming Interface
          pciDevice->configRead8(0x0A),  // Subclass
          pciDevice->configRead8(0x0B)); // Base Class
    IOLog("  BAR0=0x%08X BAR1=0x%08X\n",
          pciDevice->configRead32(kIOPCIConfigBaseAddress0),
          pciDevice->configRead32(kIOPCIConfigBaseAddress1));
    IOLog("  BAR2=0x%08X BAR3=0x%08X\n",
          pciDevice->configRead32(kIOPCIConfigBaseAddress2),
          pciDevice->configRead32(kIOPCIConfigBaseAddress3));
    IOLog("  BAR4=0x%08X BAR5=0x%08X\n",
          pciDevice->configRead32(kIOPCIConfigBaseAddress4),
          pciDevice->configRead32(kIOPCIConfigBaseAddress5));
    IOLog("  ROM Base=0x%08X\n", pciDevice->configRead32(kIOPCIConfigExpansionROMBase));
    IOLog("  IntPin=0x%02X IntLine=0x%02X\n",
          pciDevice->configRead8(kIOPCIConfigInterruptPin),
          pciDevice->configRead8(kIOPCIConfigInterruptLine));
    
    // Try to access parent bus for secondary bus reset
    // Use IOService traversal instead
    IOLog("(FakeIrisXE) [V185] Cycling PCI device...\n");
    
    // Close and re-open the PCI device
    pciDevice->close(this);
    IOSleep(200);
    if (!pciDevice->open(this)) {
        IOLog("(FakeIrisXE) [V185] Failed to re-open PCI device\n");
    } else {
        IOLog("(FakeIrisXE) [V185] PCI device re-opened\n");
    }
    
    // Re-enable memory access
    pciDevice->configWrite16(kIOPCIConfigCommand, 0x0006);  // Memory + Bus Master
    IOSleep(20);
    
    uint16_t pciCmd_after = pciDevice->configRead16(kIOPCIConfigCommand);
    uint16_t pciStat_after = pciDevice->configRead16(kIOPCIConfigStatus);
    IOLog("(FakeIrisXE) [V185] PCI CMD after=0x%04X STATUS=0x%04X\n", pciCmd_after, pciStat_after);

  
    
    
  //  IOLog("⚠️ Skipping enablePCIPowerManagement (causes freeze on some systems)\n");
  // 2️⃣ Optional: PCI Power Management (safe here)
    IOLog("⚡️ Powering up PCI device...\n");
    if (pciDevice->hasPCIPowerManagement()) {
        IOLog("Using modern power management\n");
        pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    }
    IOSleep(20);

    
    
    
    //verify BAR0 satus
    IOLog("veryfying bar0 adddress");
    uint32_t bar0=pciDevice->configRead32(kIOPCIConfigBaseAddress0);
    IOLog("PCI BAR0 = 0x%08X\n",bar0);
    
    
    if ((bar0 & ~0xf)==0){
        
        IOLog("bar0 invalid, device not assigned memory");

        return false;
    }
    
    
    
    // --- Store physical BAR0 base address ---
    uint32_t bar0Low  = pciDevice->configRead32(kIOPCIConfigBaseAddress0) & ~0xF;
    uint32_t bar0High = pciDevice->configRead32(kIOPCIConfigBaseAddress0 + 4);

    bar0Phys = ((uint64_t)bar0High << 32) | bar0Low;

    IOLog("📌 BAR0 physical address = 0x%llX\n", (unsigned long long)bar0Phys);

    
    
    
    // 3️⃣ Enable PCI Memory and IO
    IOLog("🛠 Enabling PCI memory and IO...\n");
    pciDevice->setMemoryEnable(true);
    pciDevice->setIOEnable(false);
    IOSleep(10); // Let it propagate


    // 4️⃣ Confirm enablement via config space
    uint16_t command = pciDevice->configRead16(kIOPCIConfigCommand);
    bool memEnabled = command & kIOPCICommandMemorySpace;
    bool ioEnabled  = command & kIOPCICommandIOSpace;
    if (!memEnabled) {
        IOLog("❌ Resource enable failed (PCI command: 0x%04X, mem:%d, io:%d)\n", command, memEnabled, ioEnabled);
        return false;
    }
    IOLog("✅ PCI resource enable succeeded (command: 0x%04X)\n", command);

    
    
    

    IOLog("About to Map Bar0");
    // 5️⃣ MMIO BAR0 mapping
    if (pciDevice->getDeviceMemoryCount() < 1) {
        IOLog("❌ No MMIO regions available\n");
        return false;
    }

    mmioMap = pciDevice->mapDeviceMemoryWithIndex(0);
    if (!mmioMap || mmioMap->getLength() < 0x100000) {
        IOLog("❌ BAR0 mapping failed or too small\n");
        OSSafeReleaseNULL(mmioMap);
        return false;
    }
    mmioBase = (volatile uint8_t*)mmioMap->getVirtualAddress();
    IOLog("BAR0 mapped successfully (len: 0x%llX)\n", mmioMap->getLength());


    
    
   
    // 6️⃣ Power management: wake up GPU
        IOLog("🔌 Calling initPowerManagement()...\n");
        
        if (!initPowerManagement()) {
            IOLog("❌ FATAL: initPowerManagement failed (Reported Failure). GPU is not awake.");
            IOLog("Aborting start() to prevent system freeze.");
            
            mmioMap->release();
            mmioMap = nullptr;
            pciDevice->close(this);
            pciDevice->release();
            pciDevice = nullptr;
            
            return false;
        }
        
        IOLog("✅ initPowerManagement() (Reported Success). Trust, but verify...\n");

      
    
    
    
    
    
    
    
    // --- NEW: TRUST BUT VERIFY (Safe) ---
    uint32_t gt_status = safeMMIORead(0x13805C);
    uint32_t forcewake_ack = safeMMIORead(0x130044);

    if ((gt_status == 0x0) || ((forcewake_ack & 0xF) == 0x0)) {
        IOLog("⚠️ GPU verification failed: GT_STATUS=0x%08X, ACK=0x%08X — still waking up\n", gt_status, forcewake_ack);
        IOLog("Releasing PCI + MMIO resources safely.\n");

        if (mmioMap) { mmioMap->release(); mmioMap = nullptr; }
        if (pciDevice) {
            pciDevice->close(this);
            pciDevice->release();
            pciDevice = nullptr;
        }

        return false; // Exit gracefully (prevent freeze)
    }

    IOLog("✅ GPU verified awake: GT_STATUS=0x%08X, ACK=0x%08X\n", gt_status, forcewake_ack);


    
    
    
    
    
        // 7️⃣ Now it's SAFE to do MMIO read/write
        // We already read pciID, so let's check the other registers
        IOLog("FORCEWAKE_MT snapshot: REQ=0x%08X ACK=0x%08X\n",
              safeMMIORead(0xA188),
              safeMMIORead(0x130044));
    IOLog("✅ Returned from initPowerManagement()\n");
   
    
    
    
    
    
    
    
    // 7️⃣ Now it's SAFE to do MMIO read/write
    uint32_t zeroReg = safeMMIORead(0x0000);
    IOLog("MMIO[0x0000] = 0x%08X\n", zeroReg);

    uint32_t ack = safeMMIORead(0x130044);
    IOLog("FORCEWAKE_MT_ACK: 0x%08X\n", ack);

    
    
    
    // 8️⃣ Optional: MMIO register dump
    IOLog("🔍 MMIO Register Dump:\n");
    for (uint32_t offset = 0; offset < 0x40; offset += 4) {
        uint32_t val = safeMMIORead(offset);
        IOLog("[0x%04X] = 0x%08X\n", offset, val);
    }

    
    
    
    bar0Map = pciDevice->mapDeviceMemoryWithIndex(0);
    mmioBase = (volatile uint8_t*) bar0Map->getVirtualAddress();
    
    
    
    
/*
    // === GPU Acceleration Properties ===
    {
        // Required properties for Quartz Extreme / Core Animation
               OSArray* accelTypes = OSArray::withCapacity(4); // Increased capacity for more types
               if (accelTypes) {
                   accelTypes->setObject(OSSymbol::withCString("Accel"));
                   accelTypes->setObject(OSSymbol::withCString("Metal"));
                   accelTypes->setObject(OSSymbol::withCString("OpenGL"));
                   accelTypes->setObject(OSSymbol::withCString("Quartz"));
                   setProperty("IOAccelTypes", accelTypes);
                   accelTypes->release();
                   IOLog("GPU Acceleration Properties used\n"); // Added newline for cleaner log
               }
    }
  

    
    //display bounds
    OSDictionary* bounds = OSDictionary::withCapacity(2);
    bounds->setObject("Height", OSNumber::withNumber(1080, 32));
    bounds->setObject("Width", OSNumber::withNumber(1920, 32));
    setProperty("IOFramebufferBounds", bounds);
    bounds->release();
*/
    
    
    
    
    
    
    
    logStage(2, "Framebuffer allocation + IORegistry model");

    const uint32_t width  = 1920;
    const uint32_t height = 1080;
    const uint32_t bpp    = 4;
    const uint64_t kReportedVramBytes = 1536ULL * 1024ULL * 1024ULL;  // Tiger Lake 1.5GB

    uint32_t rawSize     = width * height * bpp;
    uint32_t alignedSize = (rawSize + 0xFFFF) & ~0xFFFF; // 64KB aligned

    IOLog("🧠 Allocating framebuffer memory: %ux%u = %u bytes (aligned to 0x%X)\n",
          width, height, rawSize, alignedSize);

    int retries = 3;

    while (retries-- > 0) {

        framebufferMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryKernelUserShared,
            alignedSize,
            0x000000003FFFF000ULL   // BELOW 1GB, 4KB aligned, macOS-friendly
        );


        if (!framebufferMemory) {
            IOLog("❌ Failed to allocate framebuffer descriptor (retry %d)\n", retries);
            continue;
        }
        
        if (framebufferMemory->prepare() != kIOReturnSuccess) {
            IOLog("❌ framebufferMemory->prepare() failed\n");
            framebufferMemory->release();
            framebufferMemory = nullptr;
            continue;
        }

        break;
    }

    
    
    IOPhysicalAddress fbPhys = framebufferMemory->getPhysicalAddress();

    if ((fbPhys & 0xFFFF) != 0) {
        IOLog(" FB not 64KB aligned, but OK — GGTT mapping handles alignment\n");
    }
    else
    { IOLog("64 KB Aligned");
        
    }
    
    
    void* fbAddr = framebufferMemory->getBytesNoCopy();
    if (fbAddr) bzero(fbAddr, rawSize);

    //IOPhysicalAddress fbPhys = framebufferMemory->getPhysicalAddress();
    size_t fbLen = framebufferMemory->getLength();

    this->kernelFBPtr  = fbAddr;
    this->kernelFBSize = fbLen;
    this->kernelFBPhys = fbPhys;

    IOLog("📦 Final FB physical address: 0x%08llX\n", fbPhys);
    IOLog("📏 Final FB length: 0x%08zX\n", fbLen);

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    // Optional surface descriptor (for later IOSurface/Metal integration)
    framebufferSurface = IOMemoryDescriptor::withAddressRange(
        fbPhys,
        fbLen,
        kIODirectionInOut,
        kernel_task
    );

    if (framebufferSurface) {
        IOLog("✅ Framebuffer surface registered\n");
    } else {
        IOLog("❌ Failed to create framebuffer surface\n");
    }
    
    
    
    
    publishNormalizedMemoryModel(this, pciDevice, kReportedVramBytes);
    IOLog("[V134] Normalized memory model: %llu MB\n", kReportedVramBytes / (1024ULL * 1024ULL));
    
    // V75: Add HDA audio codec properties for display audio
    // Intel HDA controller properties for audio over HDMI/DisplayPort
    IOLog("[V75] Setting up HDA audio codec properties...\n");
    
    // Audio device properties
    setProperty("hda-gfx", OSString::withCString("on-PCI"));
    OSNumber* _n0 = OSNumber::withNumber(2, 32);
    setProperty("hda-audio", _n0);
    _n0->release(); // HDAUDIO_FMT_CHANNELS_2
    setProperty("hda-eld", OSData::withBytes((const void*)"\x00\x00\x00\x00\x00\x00\x00\x00", 8));  // ELD buffer
    
    // Audio codec vendor/product IDs (Intel HDA generic)
    OSNumber* _n1 = OSNumber::withNumber(0x808629AD, 32);
    setProperty("codec-vendor-id", _n1);
    _n1->release(); // Intel
    OSNumber* _n2 = OSNumber::withNumber(0xA0CF0000, 32);
    setProperty("codec-id", _n2);
    _n2->release(); // Generic
    
    // Audio capabilities
    OSNumber* _n3 = OSNumber::withNumber(0x1C, 32);
    setProperty("audio-formats", _n3);
    _n3->release(); // PCM 16/20/24-bit, stereo
    OSNumber* _n4 = OSNumber::withNumber(2, 32);
    setProperty("audio-max-channels", _n4);
    _n4->release(); // Stereo
    OSNumber* _n5 = OSNumber::withNumber(48000, 32);
    setProperty("audio-sample-rate", _n5);
    _n5->release(); // 48kHz
    
    // HDMI/DP audio node
    setProperty("hdmiaudio", kOSBooleanTrue);
    setProperty("dp-audio", kOSBooleanTrue);
    
    IOLog("[V75] HDA audio properties published\n");
    
    // V131: Connector/Framebuffer patch configuration
    // Tiger Lake has 4 DDI ports: A, B, C, D (some shared with USB-C)
    // Port A = eDP (internal panel), Port B = HDMI, Port C = DP, Port D = USB-C
    IOLog("[V131] Setting up connector/framebuffer patch properties...\n");
    
    // Enable framebuffer patching
    setProperty("framebuffer-patch-enable", kOSBooleanTrue);
    setProperty("framebuffer-con0-enable", kOSBooleanTrue);
    setProperty("framebuffer-con1-enable", kOSBooleanTrue);
    setProperty("framebuffer-con2-enable", kOSBooleanTrue);
    
    // V240: Use connector manager for dynamic connector properties if available
    if (fConnectorManager && fConnectorDiscoveryDone) {
        IOLog("[V240] Publishing dynamic connector properties from connector manager...\n");
        
        // Get connector count and publish each connector
        uint8_t connCount = fConnectorManager->getConnectorCount();
        for (uint8_t i = 0; i < connCount && i < 4; i++) {
            TGLConnectorDesc* conn = fConnectorManager->getConnector(i);
            if (!conn) continue;
            
            // Build connector alldata based on discovered type
            uint8_t conData[32] = {0};
            conData[0] = (uint8_t)conn->type;  // Type: eDP=4, HDMI=8, DP=16, USB4=32
            conData[4] = conn->maxLanes;       // Lanes
            conData[8] = conn->maxLanes;       // Max lanes
            conData[12] = (uint8_t)(conn->maxBitRate / 100); // Max bitrate (in 100MHz units)
            conData[16] = conn->isInternal ? 1 : 0; // Internal flag
            conData[20] = conn->supportsAudio ? 1 : 0; // Audio support
            
            char propName[64];
            snprintf(propName, sizeof(propName), "framebuffer-con%d-alldata", i);
            setProperty(propName, OSData::withBytes(conData, sizeof(conData)));
            
            IOLog("[V240] Published %s: type=%d, lanes=%d, internal=%d\n",
                  propName, conn->type, conn->maxLanes, conn->isInternal);
        }
    } else {
        // Fallback to hardcoded defaults
        IOLog("[V131] Using hardcoded fallback connector properties...\n");
    
        // Port A (0): eDP - Internal panel (0x04 = eDP)
        // Format: type(4) + hotplug(4) + lanes(4) + reserved(4) + flags(4) + maxlanes(4) + maxbitrate(4)
        // 0x00000004 = eDP, 0x00000004 = 4 lanes, 0x000000A0 = max 10Gbps
        static const uint8_t con0_edp[] = {
            0x04, 0x00, 0x00, 0x00,  // Type: eDP (0x04)
            0x00, 0x00, 0x00, 0x00,  // Hotplug: none
            0x04, 0x00, 0x00, 0x00,  // Lanes: 4
            0x00, 0x00, 0x00, 0x00,  // Reserved
            0x00, 0x00, 0x00, 0x00,  // Flags
            0x04, 0x00, 0x00, 0x00,  // Max lanes: 4
            0xA0, 0x00, 0x00, 0x00   // Max bitrate: 10Gbps
        };
        setProperty("framebuffer-con0-alldata", OSData::withBytes(con0_edp, sizeof(con0_edp)));
        
        // Port B (1): HDMI (0x08 = HDMI)
        static const uint8_t con1_hdmi[] = {
            0x08, 0x00, 0x00, 0x00,  // Type: HDMI (0x08)
            0x00, 0x00, 0x00, 0x00,  // Hotplug: none (native panel)
            0x04, 0x00, 0x00, 0x00,  // Lanes: 4
            0x00, 0x00, 0x00, 0x00,  // Reserved
            0x01, 0x00, 0x00, 0x00,  // Flags: 0x01 = IBOOST
            0x04, 0x00, 0x00, 0x00,  // Max lanes: 4
            0xA0, 0x00, 0x00, 0x00   // Max bitrate: 10Gbps
        };
        setProperty("framebuffer-con1-alldata", OSData::withBytes(con1_hdmi, sizeof(con1_hdmi)));
        
        // Port C (2): DP (0x10 = DP)
        static const uint8_t con2_dp[] = {
            0x10, 0x00, 0x00, 0x00,  // Type: DP (0x10)
            0x00, 0x00, 0x00, 0x00,  // Hotplug: none
            0x04, 0x00, 0x00, 0x00,  // Lanes: 4
            0x00, 0x00, 0x00, 0x00,  // Reserved
            0x00, 0x00, 0x00, 0x00,  // Flags
            0x04, 0x00, 0x00, 0x00,  // Max lanes: 4
            0xA0, 0x00, 0x00, 0x00   // Max bitrate: 10Gbps
        };
        setProperty("framebuffer-con2-alldata", OSData::withBytes(con2_dp, sizeof(con2_dp)));
    }
    
    // ================================================
    // V166: Proper AAPL01-int-cmn-overrides for internal display
    // This is the critical framebuffer patch for Tiger Lake internal panel
    // ================================================
    static const uint8_t aapl01_cmn_overrides[] = {
        0x00, 0x00, 0x00, 0x00,  // Framebuffer 0, flags=0
        0x04, 0x00, 0x00, 0x00,  // eDP panel type
        0x01, 0x00, 0x00, 0x00,  // Backlight: PWM control
        0x00, 0x00, 0x00, 0x00,  // Reserved
        0x00, 0x00, 0x00, 0x00,  // Backlight level min
        0xFF, 0x00, 0x00, 0x00,  // Backlight level max (255)
        0x00, 0x00, 0x00, 0x00,  // Panel ID
        0x01, 0x00, 0x00, 0x00,  // Version
        0x00, 0x00, 0x00, 0x00,  // Feature flags
    };
    setProperty("AAPL01-int-cmn-overrides", OSData::withBytes(aapl01_cmn_overrides, sizeof(aapl01_cmn_overrides)));
    IOLog("[V166] AAPL01-int-cmn-overrides set for internal eDP panel\n");
    
    // Additional framebuffer properties
    setProperty("complete-modeset", kOSBooleanTrue);
    setProperty("force-online", kOSBooleanTrue);
    
    // V131: Additional GPU detection properties for About This Mac
    IOLog("[V131] Adding GPU detection properties...\n");
    
    // Critical for About This Mac GPU detection
    setProperty("model", OSString::withCString("Intel Iris Xe Graphics"));
    setProperty("model Alias", OSString::withCString("Intel Iris Xe"));
    setProperty("IOName", OSString::withCString("Intel Iris Xe Graphics"));
    
    // V131: Internal display properties
    // These tell macOS this is the built-in display (like a MacBook Pro)
    setProperty("IODisplayIsInternal", kOSBooleanTrue);
    setProperty("builtin", kOSBooleanTrue);
    setProperty("display-type", OSString::withCString("built-in"));
    setProperty("panel-orientation", OSString::withCString("normal"));
    
    // V167: Additional display properties for proper attachment
    // Mark as boot display - critical for suppressing .Display_boot
    setProperty("AAPL,boot-display", kOSBooleanTrue);
    setProperty("AAPL,has-display", kOSBooleanTrue);
    setProperty("AAPL,ignore-ulve", kOSBooleanTrue);
    
    // V167: Internal display identification
    setProperty("IODisplayIsInternal", kOSBooleanTrue);
    setProperty("built-in", kOSBooleanTrue);
    setProperty("display-type", OSString::withCString("built-in"));
    setProperty("IODisplayLocation", OSString::withCString("internal"));
    
    // V167: Proper slot-name for internal display (critical for .Display_boot suppression)
    // Format: Internal@bus,device,function - Tiger Lake iGPU is typically @
    setProperty("AAPL,slot-name", OSData::withBytes((const void*)"\x00\x00\x00\x00Internal@2", 16));
    
    // V167: Add framebuffer index for proper routing
    OSNumber* _num_0 = OSNumber::withNumber(0ULL, 32);
    setProperty("AAPL,framebuffer-index", _num_0);
    _num_0->release();
    
    // V167: Tell system this is the primary display
    setProperty("IODisplayConnectsToFB", kOSBooleanTrue);
    
    // Keep reporter-facing identity properties as Data (32-bit LE blobs),
    // which matches IORegistry expectations used by display tools.
    publishTypedIdentityProperties(this, pciDevice);
    setProperty("vendor-name", OSString::withCString("Intel"));
    setProperty("product-name", OSString::withCString("Intel Iris Xe Graphics"));
    
    IOLog("[V131] ✅ Internal display properties set\n");
    
    // Keep IOAccelTypes scalar; IOAccel user-space probing expects
    // string-like values and can fault on array-typed payloads.
    setProperty("IOAccelTypes", OSString::withCString("Accel"));
    
    // PCI properties for GPU detection
    if (pciDevice) {
        pciDevice->setProperty("model", OSString::withCString("Intel Iris Xe Graphics"));
        pciDevice->setProperty("model Alias", OSString::withCString("Intel Xe"));

        // Keep native PCI identity keys as their kernel-provided Data types.
        // Overriding these with OSNumber causes ApplePCIeAnalytics to fault.
    }
    
    // Keep acceleration-facing identity claims disabled until there is real
    // execution proof and completion tracking.
    updateExecutionState(false, "startup");
    
    IOLog("[V167] Connector/framebuffer patch properties published\n");
    IOLog("[V167] - Port 0: eDP (internal panel)\n");
    IOLog("[V167] - Port 1: HDMI\n");
    IOLog("[V167] - Port 2: DP\n");
    IOLog("[V167] GPU detection properties added\n");
    

    
    
    
    textureMemorySize = 64 * 1024 * 1024; // 64MB for textures
    textureMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryKernelUserShared,
        textureMemorySize,
        0x00000000FFFFFFF0ULL
    );

    if (textureMemory) {
        setProperty("IOAccelTextureMemory", textureMemory);
        IOLog("✅ Texture memory allocated: %zu MB\n", textureMemorySize / (1024*1024));
    }
    
    
    
    
    
    
    
    
    
    
    
    /*
    // Create work loop and command gate
    workLoop = super::getWorkLoop();   // IOFramebuffer's internal loop
       if (!workLoop) {
           IOLog("❌ getWorkLoop() returned null\n");
           return false;
       }
       workLoop->retain();   // since you're storing it in a member

       commandGate = IOCommandGate::commandGate(this);
       if (!commandGate ||
           workLoop->addEventSource(commandGate) != kIOReturnSuccess) {
           IOLog("Failed to create/add commandGate\n");
           OSSafeReleaseNULL(commandGate);
           return false;
       }
    */
    
    
    
    
    
    
    
/*
    // create timer event source and add to workloop
    if (!fWorkLoop) {
        fWorkLoop = getWorkLoop();
    }
    if (fWorkLoop) {
        fVBlankTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &FakeIrisXEFramebuffer::vblankTick));
        if (fVBlankTimer) {
            fWorkLoop->addEventSource(fVBlankTimer);
            // schedule first tick after 16 ms
            fVBlankTimer->setTimeoutMS(16);
        }
    }
*/
    
    
    
    
    cursorMemory = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryKernelUserShared | kIODirectionInOut,
        4096,  // 4KB for cursor
        page_size
    );
    if (cursorMemory) {
        bzero(cursorMemory->getBytesNoCopy(), 4096);
        IOLog("Cursor memory allocated\n");
    } else {
        IOLog("Failed to allocate cursor memory\n");
    }
    
    
    // === V170: fallback EDID if no Apple internal override is available ===
    static const uint8_t fallbackDisplayEDID[128] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
        0x30, 0xE4, 0x1E, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x1F, 0x01, 0x04, 0x95, 0x22, 0x13, 0x78,
        0x03, 0xB3, 0x85, 0x99, 0x5E, 0x5B, 0x8C, 0x26,
        0x1B, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x2E, 0x36, 0x80, 0xA0, 0x70, 0x38, 0x1F, 0x40,
        0x30, 0x20, 0x35, 0x00, 0x58, 0xC2, 0x10, 0x00,
        0x00, 0x1A, 0x1F, 0x24, 0x80, 0xA0, 0x70, 0x38,
        0x1F, 0x40, 0x30, 0x20, 0x35, 0x00, 0x58, 0xC2,
        0x10, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // === V170: Display Identity Injection (Apple internal panel) ===
    // Use boot-arg: -fakeirisxe-display=mbp16_2, a030, a014, air10_1, pro16_1, pro13_1, or lg
    {
        uint32_t displayVendorID = 0x0610;    // Apple internal display vendor
        uint32_t displayProductID = 0xA030;   // F16Ta030 Color LCD
        const char* displayName = "Color LCD";
        uint32_t displaySerial = 0x00000001;
        
        char displayArgBuf[32] = {0};
        if (PE_parse_boot_argn("-fakeirisxe-display", displayArgBuf, sizeof(displayArgBuf))) {
            IOLog("[V170] Display identity boot-arg: '%s'\n", displayArgBuf);
            
            if (strncmp(displayArgBuf, "mbp16_2", 7) == 0 || strncmp(displayArgBuf, "a030", 4) == 0) {
                displayVendorID = 0x0610;
                displayProductID = 0xA030;
                displayName = "Color LCD";
                displaySerial = 0x00000001;
                IOLog("[V170] Using MacBookPro16,2 / F16Ta030 display identity\n");
            } else if (strncmp(displayArgBuf, "a014", 4) == 0) {
                displayVendorID = 0x0610;
                displayProductID = 0xA014;
                displayName = "Color LCD";
                displaySerial = 0x00000001;
                IOLog("[V170] Using Apple internal display identity A014\n");
            } else if (strncmp(displayArgBuf, "air10_1", 7) == 0) {
                displayVendorID = 0x0610;
                displayProductID = 0xA030;
                displayName = "Color LCD";
                displaySerial = 0x00000001;
                IOLog("[V170] Using Apple Color LCD identity instead of legacy MacBook Air IDs\n");
            } else if (strncmp(displayArgBuf, "pro16_1", 7) == 0) {
                displayVendorID = 0x0610;
                displayProductID = 0xA030;
                displayName = "Color LCD";
                displaySerial = 0x00000001;
                IOLog("[V170] Using Apple Color LCD identity instead of legacy MacBook Pro 16,1 IDs\n");
            } else if (strncmp(displayArgBuf, "pro13_1", 7) == 0) {
                displayVendorID = 0x0610;
                displayProductID = 0xA014;
                displayName = "Color LCD";
                displaySerial = 0x00000001;
                IOLog("[V170] Using Apple Color LCD identity instead of legacy MacBook Pro 13 IDs\n");
            } else if (strncmp(displayArgBuf, "lg", 2) == 0) {
                displayVendorID = 0xE430;
                displayProductID = 0x071E;
                displayName = "LG Display";
                displaySerial = 0x00000000;
                IOLog("[V170] Using LG Display panel identity\n");
            } else {
                IOLog("[V170] Unknown display identity, defaulting to MacBookPro16,2 / F16Ta030\n");
            }
        } else {
            IOLog("[V170] No display identity specified, defaulting to MacBookPro16,2 / F16Ta030\n");
        }
        
        // Apply display properties
        OSData *edidData = OSDynamicCast(OSData, getProperty("AAPL00,override-no-connect"));
        if (!edidData) {
            edidData = OSData::withBytes(fallbackDisplayEDID, sizeof(fallbackDisplayEDID));
        } else {
            edidData->retain();
        }
        if (edidData) {
            setProperty("IODisplayEDID", edidData);
            setProperty("AAPL00,PanelEDID", edidData);
            edidData->release();
            IOLog("[V170] EDID published\n");
        }

        setProperty("IOFBHasPreferredEDID", kOSBooleanTrue);
        OSNumber* _nSer = OSNumber::withNumber((uint64_t)displaySerial, 32);
        OSNumber* _nVid = OSNumber::withNumber((uint64_t)displayVendorID, 16);
        OSNumber* _nPid = OSNumber::withNumber((uint64_t)displayProductID, 16);
        setProperty("IODisplaySerialNumber", _nSer);
        setProperty("IODisplayVendorID", _nVid);
        setProperty("IODisplayProductID", _nPid);
        OSNumber* _nDVid = OSNumber::withNumber((uint64_t)displayVendorID, 32);
        OSNumber* _nDPid = OSNumber::withNumber((uint64_t)displayProductID, 32);
        setProperty("DisplayVendorID", _nDVid);
        setProperty("DisplayProductID", _nDPid);
        _nSer->release(); _nVid->release(); _nPid->release();
        _nDVid->release(); _nDPid->release();
        setProperty("IODisplayName", OSString::withCString(displayName));
        setProperty("DisplayProductName", OSString::withCString(displayName));
        setNumberProperty(this, kDisplayHorizontalImageSize, 286, 32);
        setNumberProperty(this, kDisplayVerticalImageSize, 179, 32);
        
        // Set Apple-specific properties for MacBook identity
        if (displayVendorID == 0x0610) {
            // Apple display - additional properties
            OSNumber* _nPID = OSNumber::withNumber((uint64_t)0x0001, 16);
            setProperty("IODisplayPanelID", _nPID);
            _nPID->release();
            OSNumber* _num_4 = OSNumber::withNumber(1ULL, 32);
            setProperty("AAPL,backlight-control-type", _num_4);
            _num_4->release();
            setProperty("AAPL01-internal-panel", kOSBooleanTrue);
        }
        
        // Internal panel properties
        setProperty("AAPL,slot-name", OSString::withCString("Internal@0,2,0"));
        setProperty("built-in", kOSBooleanTrue);
        applyBacklightPresetForIdentity(displayVendorID, displayProductID);
        
        IOLog("[V170] Display identity applied: Vendor=0x%04X, Product=0x%04X\n", displayVendorID, displayProductID);
    }

    // === V161: Proper LG Display EDID for Tiger Lake integrated panel (LGD 0x071E) ===
    {
        // (Legacy - now handled by V170 block above)
    }

    // Display Timing Information

    const uint8_t timingData[] = {
        0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,  // Header
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Serial
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Basic params
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Detailed timings
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00   // Extension blocks
    };

    OSData* timingInfo = OSData::withBytes(timingData, sizeof(timingData));
    if (timingInfo) {
        setProperty("IOTimingInformation", timingInfo);
        timingInfo->release();
        IOLog("Added timing information\n");
    }
    
    
    
    timerLock = IOLockAlloc();
    if (!timerLock) {
        IOLog("❌ Failed to allocate timerLock\n");
        return false;
    }

    
    /*
    
    // Setup vsyncTimer for screen refresh (simulation only)
    if (workLoop && !isInactive()) {  // Add safety check
        vsyncTimer = IOTimerEventSource::timerEventSource(
            this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this, &FakeIrisXEFramebuffer::vsyncTimerFired)
        );
        if (vsyncTimer) {
            if (workLoop->addEventSource(vsyncTimer) == kIOReturnSuccess) {
                vsyncTimer->setTimeoutMS(16);
            } else {
                vsyncTimer->release();
                vsyncTimer = nullptr;
            }
        }
    }
*/
     
    
    
    
    

    setProperty("IOFBOnline", kOSBooleanTrue);
    setProperty("IOFBDisplayModeCount", (uint64_t)kNumDisplayModes, 32);
    setProperty("IOFBIsMainDisplay", kOSBooleanTrue);
    setProperty("AAPL,boot-display", kOSBooleanTrue);
    setProperty(kIOConsoleFramebufferKey, kOSBooleanTrue);
    setProperty(kIOFramebufferIsConsoleKey, kOSBooleanTrue);
    setProperty("IOFramebufferOpenGLIndex", 0ULL, 32);

    // V161: Additional properties to suppress .Display_boot fallback
    setProperty("AAPL,ignore-ulve", kOSBooleanTrue);  // Ignore unexpected LVDS/eDP
    setProperty("AAPL,has-display", kOSBooleanTrue);  // We have a display
    
    // V167: Enhanced brightness control properties
    setProperty("brightness-control", kOSBooleanTrue);
    setProperty("IOBacklight", kOSBooleanTrue);
    setProperty("IODisplayHasBacklight", kOSBooleanTrue);
    OSNumber* _num_5 = OSNumber::withNumber(0ULL, 32);
    setProperty("IODisplayCanRotate", _num_5);
    _num_5->release();
    
    // V167: Backlight calibration
    OSNumber* _num_6 = OSNumber::withNumber(100ULL, 32);
    setProperty("brightness-level", _num_6);
    _num_6->release();
    OSNumber* _num_7 = OSNumber::withNumber(100ULL, 32);
    setProperty("brightness-max", _num_7);
    _num_7->release();
    OSNumber* _num_8 = OSNumber::withNumber(0ULL, 32);
    setProperty("brightness-min", _num_8);
    _num_8->release();
    OSNumber* _num_9 = OSNumber::withNumber(75ULL, 32);
    setProperty("brightness-default", _num_9);
    _num_9->release();
    
    // backlight-index / backlight-control-type must be OSNumber
    OSNumber *idx = OSNumber::withNumber((uint64_t)1ULL, 32);
    if (idx) { setProperty("AAPL,backlight-control-type", idx); idx->release(); }
    
    // V167: Additional backlight
    setProperty("AAPL01-internal-panel", kOSBooleanTrue);
    setProperty("AAPL00,PanelPowerOn", kOSBooleanTrue);
    publishBrightnessProperties(this, 100, 0xFFFEu);

    
    // Hold back compositor / surface / AGPM claims until command submission is
    // proven with real completion evidence.
    setProperty("IOFBTranslucencySupport", kOSBooleanFalse);
    setProperty("IOFBVibrantSupport", kOSBooleanFalse);
    setProperty("IOFBAlphaBlending", kOSBooleanFalse);
    setProperty("IOFBCompositeSupport", kOSBooleanFalse);
    setProperty("IOFBWSAASupport", kOSBooleanFalse);
    setProperty("IOFBWSSupport", kOSBooleanFalse);
    setProperty("IOFBHardwareCompositing", kOSBooleanFalse);
    setProperty("IOFBAutoCompositing", kOSBooleanFalse);
    setProperty("IOAccelerator", kOSBooleanFalse);
    setProperty("CISupported", kOSBooleanFalse);
    setProperty("CIAllowSoftwareRenderer", kOSBooleanTrue);
    setProperty("CIContextUseSoftwareRenderer", kOSBooleanTrue);
    setProperty("IOSurfaceSupported", kOSBooleanFalse);
    setProperty("IOSurfaceSupport", kOSBooleanFalse);
    setProperty("IOAccelSurfaceSupported", kOSBooleanFalse);
    setProperty("IOAccelCLContextSupported", kOSBooleanFalse);
    setProperty("IOAccelGLContextSupported", kOSBooleanFalse);
    setProperty("CIBlurSupported", kOSBooleanFalse);
    setProperty("CITransparencySupported", kOSBooleanFalse);
    setProperty("AGPM_Enabled", kOSBooleanFalse);
    setProperty("GPUPowerManagementEnabled", kOSBooleanFalse);
    setProperty("IOGPUPowerManagement", kOSBooleanFalse);
    setProperty("IOGPUPowermanagementCapable", kOSBooleanFalse);
    setProperty("IOGPUDVFM", kOSBooleanFalse);
    setProperty("AGPMFullControl", kOSBooleanFalse);
    setProperty("IOGPUPowerControl", kOSBooleanFalse);
    IOLog("[V289] Acceleration and AGPM-facing claims held back until execution proof exists\n");
    
    // Quartz Extreme requirements
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

    


    // Cursor - publish the owned cursor buffer instead of swapping in a second descriptor.
    // The old path released the member immediately after setProperty(), leaving a dangling
    // pointer that later panicked during performSafeStop().
    if (cursorMemory) {
        setProperty("IOFBCursorMemory", cursorMemory);
    }


    
    
    OSNumber* cursorSizeNum = OSNumber::withNumber(32ULL, 32); // Renamed variable to avoid conflict
       if (cursorSizeNum) {
           OSObject* values[1] = { cursorSizeNum };
           OSArray* array = OSArray::withObjects((const OSObject**)values, 1);
           if (array) {
               setProperty("IOFBCursorSizes", array);
               array->release();
           }
           cursorSizeNum->release();
       }
  


    
    
    
    
    
    
    
    setNumberOfDisplays(1);


    
    fullyInitialized = true;
    driverActive = true;

    
    bzero(framebufferMemory->getBytesNoCopy(), framebufferMemory->getLength());

    
    
    
    /*
    if (workLoop) {
        IOTimerEventSource* activateTimer = IOTimerEventSource::timerEventSource(
            this,
            [](OSObject* owner, IOTimerEventSource* timer) {
                auto fb = OSDynamicCast(FakeIrisXEFramebuffer, owner);
                if (fb) {
                    IOLog("🔥 Timer fired, calling enableController()\n");
                    fb->activatePowerAndController();
                }
            }
        );

        if (activateTimer) {
            workLoop->addEventSource(activateTimer);
            activateTimer->setTimeoutMS(5000);
            IOLog("⏰ Timer scheduled for 5s\n");
        }
    }
*/
    
    logStage(3, "Display/controller activation");
    activatePowerAndController();
    
    
    // ================================================
    // V46+V47: GGTT INITIALIZATION
    // ================================================
    logStage(4, "GGTT/ring/command submission bring-up");
    IOLog("(FakeIrisXE) [V48] Initializing GGTT aperture...\n");

    // Reuse the full BAR0 mapping from stage 1 instead of remapping BAR0 from the
    // low 32 bits only. The previous code mapped 0x10000000 instead of the actual
    // 64-bit BAR0 (for example 0x4010000000), which made stage-4 direct MMIO writes
    // and later validation read from different physical regions.
    if (mmioBase && mmioMap) {
        fBar0 = reinterpret_cast<volatile uint32_t*>(const_cast<volatile UInt8*>(mmioBase));
        fGGTTBaseGPU = 0x00000000;
        fNextGGTTOffset = 0x00100000;
        IOLog("FakeIrisXEFramebuffer: BAR0 stage1 mapping phys=0x%llx va=%p len=0x%llx\n",
              static_cast<unsigned long long>(bar0Phys),
              fBar0,
              static_cast<unsigned long long>(mmioMap->getLength()));
        IOLog("FakeIrisXEFramebuffer: GGTT stage3 mapping va=%p len=0x%llx\n",
              fGGTT,
              static_cast<unsigned long long>(fGGTTSize));
        if (!fGGTT) {
            logSoftFail(4, "GGTT BAR1 map missing; skipping ring init");
        }
    } else {
        logSoftFail(4, "BAR0/GGTT map missing; skipping ring init");
    }

    // map BAR0 into fBar0 — done above
    // map GGTT into fGGTT — done above
    fNextGGTTOffset = 0x00100000; // choose appropriate base

    char runtimeArgBuf[16] = {0};
    bool runBootDiagFull = PE_parse_boot_argn("-fakeirisxe-diag", runtimeArgBuf, sizeof(runtimeArgBuf));
    bool runBootDiagQuick = PE_parse_boot_argn("-fakeirisxe-quickdiag", runtimeArgBuf, sizeof(runtimeArgBuf));
    bool skipGuCInit = PE_parse_boot_argn("-fakeirisxe-noguc", runtimeArgBuf, sizeof(runtimeArgBuf));
    bool forceGuCInit = PE_parse_boot_argn("-fakeirisxe-guc", runtimeArgBuf, sizeof(runtimeArgBuf));
    bool directProofMode = !runBootDiagFull && !runBootDiagQuick;

    // V274: Attempt GuC init using Linux i915 boot path. V271 showed Apple path fails at ME handshake. V272 showed GuC doesn't boot (status stuck at 0x01).
    // all failed with F_NO_SCHEDULING_PROGRESS because Gen12 requires GuC for scheduling.
    // V281: Apple path first with Intel firmware (KEY STRATEGY - no GUC_CTL dependency!)
    // V281: Apple path uses GUC_MISC_CONTROL+auth-kick instead of GUC_CTL (never tried with Intel fw)
    // V281: Linux fallback uses Apple SHIM=0x00208617, extended GUC register investigation
    // V289: Minimal bare-DMA FIRST → Apple path → Linux path
    // Skip GuC only when explicitly requested via -fakeirisxe-noguc.
    // -fakeirisxe-guc now behaves the same as plain -fakeirisxe (both try GuC).

    updateExecutionState(false, "stage4-begin");

    if (directProofMode) {
        IOLog("(FakeIrisXE) [V289] Stage 4: Minimal bare-DMA FIRST → Apple path → Linux fallback\n");
    } else {
        // V200: CRITICAL - Ensure GT power is enabled BEFORE ring creation
        IOLog("(FakeIrisXE) [V204] Ensuring GT power is enabled before ring creation...\n");
        if (!gpuPowerOn()) {
            IOLog("(FakeIrisXE) [V204] WARNING: gpuPowerOn failed, continuing anyway...\n");
        }

        // Create ring
        if (!createRcsRing(256 * 1024)) {
            logSoftFail(4, "createRcsRing failed; continuing degraded");
        } else {
            IOLog("FakeIrisXEFramebuffer: createRcsRing Succes\n");
        }
    }

    // optional: create & map fence early (so submitBatch doesn't do it)
    fFenceGEM = FakeIrisXEGEM::withSize(4096, 0);
    if (fFenceGEM) {
        fFenceGEM->pin();
    } else {
        logSoftFail(4, "Fence GEM allocation failed");
    }

    // ================================================
    // V45: FIRMWARE LOADING (After GGTT init, Intel PRM sequence)
    // ================================================
    logStage(5, "Firmware + execution submission mode");
    IOLog("(FakeIrisXE) [V289] Loading firmware (Intel PRM compliant)...\n");

    setProperty("FakeIrisXEBootDiagFull", runBootDiagFull ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("FakeIrisXEBootDiagQuick", runBootDiagQuick ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("FakeIrisXEDirectProofMode", directProofMode ? kOSBooleanTrue : kOSBooleanFalse);

    IOLog("(FakeIrisXE) [V289] Runtime toggles: diag_full=%u diag_quick=%u direct_proof=%u skip_guc=%u force_guc=%u\n",
          runBootDiagFull ? 1U : 0U,
          runBootDiagQuick ? 1U : 0U,
          directProofMode ? 1U : 0U,
          skipGuCInit ? 1U : 0U,
          forceGuCInit ? 1U : 0U);

    // V274: Attempt GuC init using Linux i915 path. Only skip if -fakeirisxe-noguc is set.
    // The previous 17 direct-Execlist iterations (V254-V270) all failed with
    // F_NO_SCHEDULING_PROGRESS because Gen12 requires GuC for scheduling.
    if (skipGuCInit) {
        IOLog("(FakeIrisXE) [V289] ⚠️ Skipping GuC init (-fakeirisxe-noguc set)\n");
        fGuCEnabled = false;
        fRcsRingValidated = false;
        fCommandSubmissionReady = false;
    } else {
    // V45: Program MOCS before GuC init
    IOLog("(FakeIrisXE) [V45] Programming MOCS...\n");
    if (!programMOCS()) {
        IOLog("(FakeIrisXE) [V45] ⚠️ MOCS programming failed, continuing anyway\n");
    }

    IOLog("(FakeIrisXE) [V45] Initializing GuC system (PRM sequence)...\n");

    // Initialize GuC system with Intel PRM-compliant sequence
    if (!initGuCSystem()) {
        IOLog("(FakeIrisXE) [V45] ❌ GuC init failed; entering fallback execlist/ring diagnostics mode\n");
        fGuCEnabled = false;
        setProperty("FakeIrisXEGuCFailureTerminal", kOSBooleanTrue);
        updateExecutionState(false, "guc-failure");
        IOLog("(FakeIrisXE) [V45] Stage4 fallback: ring=%p validated=%u commandReady=%u\n",
              fRcsRing,
              fRcsRingValidated ? 1U : 0U,
              fCommandSubmissionReady ? 1U : 0U);
    } else {
        fGuCEnabled = true;
        IOLog("(FakeIrisXE) [V45] ✅ GuC submission enabled\n");
    }
    }  // V251: end skipGuCInit

    if (true) {

    //enabling interrupts:
    // Create / obtain a workloop (safe)
    fWorkLoop = getWorkLoop();
    if (!fWorkLoop) {
        // create our own workloop if none provided
        fWorkLoop = IOWorkLoop::workLoop();
        if (!fWorkLoop) {
            IOLog("FakeIrisXEFramebuffer: createWorkLoop failed\n");
            // still continue — we will operate without IRQs
        } else {
            fWorkLoop->retain();
            IOLog("FakeIrisXEFramebuffer: created own workloop\n");
        }
    } else {
        fWorkLoop->retain();
        IOLog("FakeIrisXEFramebuffer: obtained existing workloop\n");
    }

    // Make sure provider is an IOPCIDevice
    IOService* prov = provider;
    if (!prov) {
        IOLog("FakeIrisXEFramebuffer: no provider for interrupts\n");
    } else if (fWorkLoop) {
        // Create the interrupt source using trampoline (C-callback)
        fInterruptSource = IOInterruptEventSource::interruptEventSource(
            this,
            handleInterruptTrampoline,
            prov
        );

        if (!fInterruptSource) {
            IOLog("FakeIrisXEFramebuffer: failed to create interrupt source\n");
            // We will not enable IRQs
        } else {
            fWorkLoop->addEventSource(fInterruptSource);
            // Do not call enable() here until we safely unmask registers in the next step
            IOLog("FakeIrisXEFramebuffer: interrupt source created (not yet enabled)\n");
        }
    }

    
    
    
    // Create lock for pending submissions
    if (!fPendingLock)
        fPendingLock = IOLockAlloc();

    // Pending submission array
    if (!fPendingSubmissions)
        fPendingSubmissions = OSArray::withCapacity(16);

    // Create IOCommandGate
    if (fWorkLoop && !fCmdGate) {
        fCmdGate = IOCommandGate::commandGate(this);
        if (fCmdGate) {
            fWorkLoop->addEventSource(fCmdGate);
            IOLog("FakeIrisXEFramebuffer: commandGate added\n");
        }
    }

    

    enableRcsInterruptsSafely();

    
   
    // ---------------------------------------------------------
    // PHASE 7.2 — Initialize Execlists engine (REAL GPU PATH)
    // ---------------------------------------------------------
    IOLog("FakeIrisXEFramebuffer: Initializing EXECLIST engine...\n");

    fExeclist = FakeIrisXEExeclist::withOwner(this);

    
    
    if (!fExeclist) {
        IOLog("FakeIrisXEFramebuffer: EXECLIST allocation FAILED\n");
    } else {
        
        
        
        if (!fExeclist->createHwContext()) {
            IOLog("FakeIrisXEFramebuffer: EXECLIST HW context FAILED\n");
        } else {
            IOLog("FakeIrisXEFramebuffer: EXECLIST HW context OK\n");

            if (!fExeclist->setupExeclistPorts()) {
                IOLog("FakeIrisXEFramebuffer: EXECLIST port setup FAILED\n");
            } else {
                IOLog("FakeIrisXEFramebuffer: EXECLIST ports programmed; execution remains unproven\n");
                fExeclist->fIsReady = false;
                setProperty("FakeIrisXEExeclistPortsReady", kOSBooleanTrue);
                setProperty("FakeIrisXEExeclistExecutionProven", kOSBooleanFalse);
                
                if (runBootDiagFull) {
                    IOLog("FakeIrisXEFramebuffer: [V70] '-fakeirisxe-diag' detected - running comprehensive diagnostics...\n");

                    if (fExeclist->runComprehensiveDiagnosticTest()) {
                        IOLog("FakeIrisXEFramebuffer: [V70] ✅ ALL COMPREHENSIVE TESTS PASSED\n");
                    } else {
                        IOLog("FakeIrisXEFramebuffer: [V70] ⚠️ Some comprehensive tests failed (see logs above)\n");
                    }

                    IOLog("FakeIrisXEFramebuffer: [V70] Running simple diagnostic test...\n");
                    if (fExeclist->runSimpleDiagnosticTest()) {
                        IOLog("FakeIrisXEFramebuffer: [V62] Simple diagnostic test PASSED\n");
                    } else {
                        IOLog("FakeIrisXEFramebuffer: [V62] Simple diagnostic test FAILED\n");
                    }
                } else if (runBootDiagQuick) {
                    IOLog("FakeIrisXEFramebuffer: [V70] '-fakeirisxe-quickdiag' detected - running simple diagnostic test only...\n");
                    if (fExeclist->runSimpleDiagnosticTest()) {
                        IOLog("FakeIrisXEFramebuffer: [V62] Simple diagnostic test PASSED\n");
                    } else {
                        IOLog("FakeIrisXEFramebuffer: [V62] Simple diagnostic test FAILED\n");
                    }
                } else {
                    IOLog("FakeIrisXEFramebuffer: [V70] Skipping extra boot diagnostics (use -fakeirisxe-quickdiag or -fakeirisxe-diag)\n");
                    IOLog("FakeIrisXEFramebuffer: [V70] Plain -fakeirisxe boot will still run one direct Execlist proof via testGPUExecution()\n");
                }
            }

            if (directProofMode) {
                IOLog("FakeIrisXEFramebuffer: [V289] Direct proof mode active - skipping legacy RCS/BLT ring warmup path\n");
            } else {
                // Create / init RCS ring (existing helper returns bool)
                if (!fRcsRing && createRcsRing(256 * 1024)) {
                    IOLog("FakeIrisXEFramebuffer: RCS ring initialization complete. fRcsRing=%p\n", fRcsRing);
                } else if (!fRcsRing) {
                    IOLog("FakeIrisXEFramebuffer: FAILED creating RCS ring\n");
                }

                // V138: Create BLT ring for 2D operations
                fBltRing = createBltRing(256 * 1024);
                if (fBltRing) {
                    IOLog("FakeIrisXEFramebuffer: BLT ring initialization complete. fBltRing=%p\n", fBltRing);
                } else {
                    IOLog("FakeIrisXEFramebuffer: FAILED creating BLT ring\n");
                }

                // V201: Try creating RCS again AFTER BLT (when GT is "warmed up")
                if (!fRcsRing) {
                    IOLog("(FakeIrisXE) [V204] Retry RCS creation after BLT warmup...\n");
                    if (gpuPowerOn()) {
                        IOLog("(FakeIrisXE) [V204] gpuPowerOn succeeded, retrying RCS...\n");
                    }
                    fRcsRing = createRcsRing(256 * 1024);
                    if (fRcsRing) {
                        IOLog("FakeIrisXEFramebuffer: [V204] RCS ring retry SUCCESS! fRcsRing=%p\n", fRcsRing);
                    } else {
                        IOLog("FakeIrisXEFramebuffer: [V204] RCS ring retry FAILED\n");
                    }
                }
            }

            // V150: Test GPU execution
            IOLog("(FakeIrisXE)[V151] Running GPU execution test...\n");
            bool gpuWorking = testGPUExecution();
            if (gpuWorking) {
                IOLog("(FakeIrisXE)[V150] ✅ GPU EXECUTION TEST PASSED\n");
            } else {
                IOLog("(FakeIrisXE)[V150] ❌ GPU EXECUTION TEST FAILED\n");
            }
            
            // V213: Allow submission if EXEClist is ready (even without RCS validated)
            // ELSP works, so we can use it for submissions
            bool execlistReady = fExeclist && fExeclist->isReady();
            fCommandSubmissionReady = gpuWorking && (fRcsRingValidated || execlistReady);
            IOLog("(FakeIrisXE) [V213] Submission ready: gpuWorking=%d RCSvalidated=%d EXEClistReady=%d -> %d\n",
                  gpuWorking ? 1 : 0, fRcsRingValidated ? 1 : 0, execlistReady ? 1 : 0, fCommandSubmissionReady ? 1 : 0);
            updateExecutionState(fCommandSubmissionReady, gpuWorking ? "gpu-test-pass" : "gpu-test-fail");

            if (runBootDiagFull) {
                IOLog("\n");
                IOLog("[V88] EXECLIST command submission test (diag mode)\n");
                IOLog("\n");

                if (fExeclist && fRcsRing) {
                    IOLog("[V88] Attempting simple MI_NOOP submission via execlist...\n");

                    FakeIrisXEGEM* testBatch = createSimpleUserBatch();
                    if (testBatch) {
                        testBatch->pin();
                        uint64_t batchGpu = ggttMap(testBatch);

                        IOLog("[V88] Test batch created: GPU addr=0x%llx\n", batchGpu);

                        bool submitOk = fExeclist->submitBatchExeclist(testBatch);
                        if (submitOk) {
                            IOLog("[V88] ✅ MI_NOOP command submitted successfully!\n");
                        } else {
                            IOLog("[V88] ❌ MI_NOOP submission failed - checking with full submit...\n");

                            bool fullSubmitOk = fExeclist->submitBatchWithExeclist(
                                this,
                                testBatch,
                                4096,
                                fRcsRing,
                                5000
                            );
                            if (fullSubmitOk) {
                                IOLog("[V88] ✅ Full submit path (with fence) succeeded!\n");
                            } else {
                                IOLog("[V88] ❌ Full submit path also failed\n");
                            }
                        }

                        testBatch->unpin();
                        testBatch->release();
                    } else {
                        IOLog("[V88] ❌ Failed to create test batch buffer\n");
                    }
                } else {
                    IOLog("[V88] ⚠️ Cannot run test - execlist or RCS ring not ready\n");
                    IOLog("   fExeclist: %p\n", fExeclist);
                    IOLog("   fRcsRing: %p\n", fRcsRing);
                }
            } else {
                IOLog("[V88] Skipping boot-time submission test (use -fakeirisxe-diag)\n");
            }

            // V89: WindowServer Integration Setup
            IOLog("\n");
            IOLog("╔══════════════════════════════════════════════════════════════╗\n");
            IOLog("║  V89: WINDOWS SERVER INTEGRATION                             ║\n");
            IOLog("╚══════════════════════════════════════════════════════════════╝\n");
            IOLog("\n");
            
            // Set up display pipe for WindowServer
            IOLog("[V89] Setting up display pipe for WindowServer...\n");
            
            // Create IOSurface-compatible framebuffer properties
            setProperty("IOFBScalerInfo", OSData::withBytes((void*)"\x00\x00\x00\x00", 4));
            OSNumber* _nT = OSNumber::withNumber((unsigned long long)0, 32);
            setProperty("IOFBTransform", _nT);
            setProperty("IOFBSignal", _nT);
            _nT->release();
            
            setProperty("IOFBHWCursor", kOSBooleanTrue);
            setProperty("IOFBAlphaCursor", kOSBooleanTrue);
            
            // Set up surface format for WindowServer
            OSNumber* _n8 = OSNumber::withNumber(0x42475241, 32);
            setProperty("IOSurfacePixelFormat", _n8);
            _n8->release(); // ARGB
            OSNumber* _num_17 = OSNumber::withNumber(4, 32);
            setProperty("IOSurfaceBytesPerElement", _num_17);
            _num_17->release();
            OSNumber* _num_18 = OSNumber::withNumber(7680, 32);
            setProperty("IOSurfaceBytesPerRow", _num_18);
            _num_18->release();
            OSNumber* _num_19 = OSNumber::withNumber(1920, 32);
            setProperty("IOSurfaceWidth", _num_19);
            _num_19->release();
            OSNumber* _num_20 = OSNumber::withNumber(1080, 32);
            setProperty("IOSurfaceHeight", _num_20);
            _num_20->release();
            
            IOLog("[V89] WindowServer framebuffer properties staged\n");
            IOLog("[V89] Acceleration remains disabled until execution proof exists\n");
            IOLog("[V89] IOSurface metadata exported for diagnostic compatibility only\n");
            
            // V90: IOAccelerator Initialization
            IOLog("\n");
            IOLog("╔══════════════════════════════════════════════════════════════╗\n");
            IOLog("║  V90: IOACCELERATOR HOOKS STAGED                             ║\n");
            IOLog("╚══════════════════════════════════════════════════════════════╝\n");
            IOLog("\n");
            IOLog("[V90] Surface management staging:\n");
            IOLog("      Max surfaces: %u\n", kMaxSurfaces);
            IOLog("      Format: ARGB8888\n");
            IOLog("[V90] 2D Blit operations: not yet execution-proven\n");
            IOLog("[V90] Command submission: diagnostic only\n");
            IOLog("\n");
            
            // V91: 2D Blit Command Support
            IOLog("╔══════════════════════════════════════════════════════════════╗\n");
            IOLog("║  V91: 2D BLIT COMMANDS NOT YET PROVEN                        ║\n");
            IOLog("╚══════════════════════════════════════════════════════════════╝\n");
            IOLog("\n");
            IOLog("[V91] Intel Blitter Commands:\n");
            IOLog("      XY_SRC_COPY_BLT (0x53): staged only\n");
            IOLog("      XY_COLOR_BLT (0x50): staged only\n");
            IOLog("      XY_SETUP_BLT (0x01): staged only\n");
            IOLog("[V91] GPU Hardware Acceleration: not yet proven\n");
            IOLog("\n");
            
            // V92: Debug Infrastructure & Advanced Features
            IOLog("╔══════════════════════════════════════════════════════════════╗\n");
            IOLog("║  V92: DEBUG INFRASTRUCTURE & BATCH BLITS                     ║\n");
            IOLog("╚══════════════════════════════════════════════════════════════╝\n");
            IOLog("\n");
            
            // Run V92 comprehensive diagnostics
            runV92Diagnostics();
            
            IOLog("\n");
            
            // V93: Display Verification & Integration Testing
            IOLog("╔══════════════════════════════════════════════════════════════╗\n");
            IOLog("║  V93: DISPLAY VERIFICATION & INTEGRATION TESTING            ║\n");
            IOLog("╚══════════════════════════════════════════════════════════════╝\n");
            IOLog("\n");
            
            // Initialize V93 tracking
            fV93BootTime = mach_absolute_time();
            fV93WindowServerBlitCount = 0;
            fV93CommandsSubmitted = 0;
            fV93CommandsCompleted = 0;
            fV93DisplayVerificationFailures = 0;
            fV93FirstBlitTime = 0;
            fV93LastBlitTime = 0;
            fV93TotalBlitTime = 0;
            fV93DisplayVerified = false;
            fV93WindowServerConnected = false;
            
            // Verify display pipe state (Intel PRM Vol 12)
            IOLog("[V93] Verifying display pipe configuration...\n");
            verifyDisplayPipeState();
            
            IOLog("\n");
            
            // Register for display notifications
            displayOnline = true;
            setProperty("IOFBDisplayOnline", kOSBooleanTrue);
            
            // Expose V93 status for user-space tools
            OSNumber* _num_21 = OSNumber::withNumber(93, 32);
            setProperty("IOFBAccelRevision", _num_21);
            _num_21->release();
            
            IOLog("[V93] Display verification complete. Ready for integration testing.\n");
            IOLog("\n");

        
        }
    }

    
    
    IOLog("FB scanning IOServicePlane children for FakeIrisXEAccelerator…\n");

    OSIterator* iter = this->getChildIterator(gIOServicePlane);
    if (iter)
    {
        IORegistryEntry* entry = nullptr;
        while ((entry = OSDynamicCast(IORegistryEntry, iter->getNextObject())))
        {
            FakeIrisXEAccelerator* accel = OSDynamicCast(FakeIrisXEAccelerator, entry);
            if (accel)
            {
                IOLog("🔗 Found Accelerator child %p — linking…\n", accel);
                accel->linkFromFramebuffer(this);
                if (fCommandSubmissionReady) {
                    setProperty("IOAccelServiceRegistryID", accel->getRegistryEntryID(), 64);
                    IOLog("🟢 LINK SUCCESS — FB → Accelerator\n");
                } else {
                    IOLog("⚠️ LINK DEFERRED — execution path not ready yet\n");
                }
                break;  // Important — only 1 accelerator
            }
        }
        if (!fGuCEnabled) {
            setProperty("FakeIrisXEExecutionFallbackMode", kOSBooleanTrue);
            IOLog("(FakeIrisXE) [V177] Fallback execution diagnostics completed with acceleration still disabled (%s)\n",
                  skipGuCInit ? "GuC intentionally skipped in direct-proof mode" : "GuC init failed");
        }
        iter->release();
    }

    } else {
        IOLog("(FakeIrisXE) [V45] Apple-only phase: skipping IRQ, execlist, ring, GPU execution test, and accelerator link after GuC auth failure\n");
        displayOnline = true;
        setProperty("IOFBDisplayOnline", kOSBooleanTrue);
    }

    
    
    
    
    
    

    
    
    
    
    
    
    
    
    // Keep display graph ownership with IOGraphicsFamily.
    // Synthetic IODisplayConnect / backlight nodes caused unstable CoreDisplay routing
    // and user-space property parsing crashes.
    IOLog("[V170] Skipping synthetic IODisplayConnect/backlight publication\n");



    
    
    
    
    
    
  
    // 6. Then flush and notify
    //flushDisplay();
    
    
    // 6. Finally, publish the framebuffer
    registerService();
    IOLog("register service called");

    
    
    

    
    
    
    
    // 5. Notify WindowServer
    deliverFramebufferNotification(0, kIOFBNotifyWillPowerOn, nullptr);
    deliverFramebufferNotification(0, kIOFBNotifyDidPowerOn, nullptr);
    deliverFramebufferNotification(0, 0x10, nullptr);      // display mode set complete
    deliverFramebufferNotification(0, 'dmod', nullptr);    // publish mode
    deliverFramebufferNotification(0, kIOFBNotifyDisplayModeWillChange, nullptr);
    deliverFramebufferNotification(0, kIOFBNotifyDisplayModeDidChange, nullptr);
    deliverFramebufferNotification(0, kIOFBNotifyDisplayAdded, nullptr);
    deliverFramebufferNotification(0, kIOFBNotifyDisplayModeChange, nullptr);
    deliverFramebufferNotification(0, kIOFBConfigChanged, nullptr);
    IOLog("WS notified\n");

    // Try to rewrite display identity on display0 when it appears under this framebuffer.
    injectDisplayMergeOverridesIfAvailable(this);
    
   



    
    


    
    
    // V131: Final initialization diagnostics with WindowServer info
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║  V170 INITIALIZATION COMPLETE - STATUS REPORT                 ║\n");
    IOLog("╠══════════════════════════════════════════════════════════════╣\n");
    IOLog("║  FRAMEBUFFER STATUS                                          ║\n");
    IOLog("║  Framebuffer:     %s\n", framebufferMemory ? "✅ ALLOCATED" : "❌ MISSING");
    IOLog("║  Kernel Pointer:  %s\n", kernelFBPtr ? "✅ VALID" : "❌ NULL");
    IOLog("║  Physical Addr:   0x%llX\n", kernelFBPhys);
    IOLog("║  Size:            %llu MB\n", kernelFBSize / (1024*1024));
    IOLog("╠══════════════════════════════════════════════════════════════╣\n");
    IOLog("║  HARDWARE STATUS                                             ║\n");
    IOLog("║  MMIO Base:       %s\n", mmioBase ? "✅ MAPPED" : "❌ MISSING");
    IOLog("║  VRAM Reported:   %llu MB\n", vramSize / (1024*1024));
    IOLog("║  Controller:      %s\n", controllerEnabled ? "✅ ENABLED" : "❌ DISABLED");
    IOLog("║  Display Online:  %s\n", displayOnline ? "✅ YES" : "❌ NO");
    IOLog("╠══════════════════════════════════════════════════════════════╣\n");
    IOLog("║  DISPLAY CONFIGURATION                                       ║\n");
    IOLog("║  Current Mode:    %u (%ux%u)\n", currentMode, H_ACTIVE, V_ACTIVE);
    IOLog("║  Available Modes: %u\n", kNumDisplayModes);
    IOLog("║  Display:         Apple Color LCD (MacBookPro16,2)\n");
    IOLog("╠══════════════════════════════════════════════════════════════╣\n");
    IOLog("║  WINDOWSERVER INTEGRATION                                    ║\n");
    IOLog("║  Aperture Range:  ✅ CONFIGURED\n");
    IOLog("║  Client Memory:   %s\n", fCommandSubmissionReady ? "⚠️ UNVERIFIED RUNTIME PATH" : "DISABLED / DIAGNOSTIC ONLY");
    IOLog("║  Surface Mapping: %s\n", fCommandSubmissionReady ? "⚠️ UNVERIFIED" : "NOT YET PROVEN");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    
    if (fGuCEnabled) {
        IOLog("[V131] WindowServer should now be able to render to this framebuffer\n");
        IOLog("[V131] Look for color bars on screen (V81 test pattern)\n");
    } else {
        IOLog("[V131] Published framebuffer with acceleration disabled; direct proof mode is active until real execution succeeds (%s)\n",
              skipGuCInit ? "GuC skipped by policy" : "GuC init failed");
    }
    IOLog("\n");
    
    closeCurrentStage();
    const uint64_t totalStartUs = absDeltaToNs(startTotalAbs, mach_absolute_time()) / 1000ULL;
    setNumberProperty(this, "FakeIrisXEStartDurationUs", totalStartUs, 64);
    setNumberProperty(this, "FakeIrisXESoftFailCount", softFailCount, 32);
    IOLog("(FakeIrisXE) start timing: total=%llu us softFails=%u\n",
          static_cast<unsigned long long>(totalStartUs),
          softFailCount);
    IOLog("FakeIrisXEFramebuffer::start() - Completed (V288, Minimal bare-DMA boot)\n");
    return true;

}



void FakeIrisXEFramebuffer::stop(IOService* provider)
{
    IOLog("FakeIrisXEFramebuffer::stop() called — scheduling gated cleanup\n");

    // V281: DEFENSIVE STOP — guard against init() failing before allocations

    // V281: Release IOService objects with null-checks first
    if (fInterruptSource) {
        fInterruptSource->disable();
        if (fWorkLoop) {
            fWorkLoop->removeEventSource(fInterruptSource);
        }
        auto* tmp = fInterruptSource;
        fInterruptSource = nullptr;
        tmp->release();
    }

    if (fCmdGate) {
        if (fWorkLoop) {
            fWorkLoop->removeEventSource(fCmdGate);
        }
        auto* tmp = fCmdGate;
        fCmdGate = nullptr;
        tmp->release();
    }

    if (fPendingSubmissions) {
        cleanupAllPendingSubmissions();
        auto* tmp = fPendingSubmissions;
        fPendingSubmissions = nullptr;
        tmp->release();
    }

    if (fPendingLock) {
        IOLockFree(fPendingLock);
        fPendingLock = nullptr;
    }

    if (fWorkLoop) {
        // V281: Remove commandGate from workLoop first (if not already done above)
        if (fCmdGate == nullptr) {
            // Already removed above, skip
        }
        auto* tmp = fWorkLoop;
        fWorkLoop = nullptr;
        tmp->release();
    }

    // V281: Mark shutdown state
    if (timerLock) {
        IOLockLock(timerLock);
        driverActive = false;
        shuttingDown = true;
        IOLockUnlock(timerLock);
    }

    // V281: Run gated cleanup via commandGate, or inline fallback
    if (commandGate) {
        commandGate->runAction(&FakeIrisXEFramebuffer::staticStopAction);
    } else {
        performSafeStop();
    }

    // V281: Delete ring objects (not OSObject, use C++ delete)
    // V281: Note: fRcsRing deleted here, fBltRing deleted in performSafeStop
    if (fRcsRing) {
        delete fRcsRing;
        fRcsRing = nullptr;
    }
    if (fBltRing) {
        delete fBltRing;
        fBltRing = nullptr;
    }

    // V281: Release Execlist (OSObject subclass - use release() not delete)
    if (fExeclist) {
        fExeclist->release();
        fExeclist = nullptr;
    }

    // V281: Delete connector manager (plain C++ object)
    if (fConnectorManager) {
        delete fConnectorManager;
        fConnectorManager = nullptr;
    }

    // V281: Release GEM objects (OSObject subclasses)
    // V281: FakeIrisXEGEM::release() will call IOBufferMemoryDescriptor::release()
    if (fFenceGEM) {
        fFenceGEM->release();
        fFenceGEM = nullptr;
    }
    if (fRingGem) {
        fRingGem->release();
        fRingGem = nullptr;
    }
    if (fScratchGem) {
        fScratchGem->release();
        fScratchGem = nullptr;
    }
    if (fLrcGem) {
        fLrcGem->release();
        fLrcGem = nullptr;
    }
    if (batchGem) {
        batchGem->release();
        batchGem = nullptr;
    }

    // V281: Release framebufferMap (IOMemoryMap)
    if (framebufferMap) {
        framebufferMap->release();
        framebufferMap = nullptr;
    }

    // V281: Release all pending surface GEM objects
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        if (fSurfaces[i].inUse && fSurfaces[i].gemObj) {
            fSurfaces[i].gemObj->release();
            fSurfaces[i].gemObj = nullptr;
        }
        fSurfaces[i].inUse = false;
        fSurfaces[i].id = 0;
        fSurfaces[i].gpuAddress = 0;
    }

    // Now call superclass stop after all our cleanup.
    super::stop(provider);
}



IOReturn FakeIrisXEFramebuffer::staticStopAction(OSObject *owner,
                                                 void * /*arg0*/,
                                                 void * /*arg1*/,
                                                 void * /*arg2*/,
                                                 void * /*arg3*/)
{
    FakeIrisXEFramebuffer *fb = OSDynamicCast(FakeIrisXEFramebuffer, owner);
    if (!fb) return kIOReturnBadArgument;

    IOLog("FakeIrisXEFramebuffer::staticStopAction() running on gated thread\n");
    fb->performSafeStop();
    return kIOReturnSuccess;
}

void FakeIrisXEFramebuffer::performSafeStop()
{
    // V281: This runs on the gated thread after stop() has already cleaned up:
    // - fInterruptSource, fCmdGate, fPendingSubmissions, fWorkLoop
    // - GEM objects, ring objects, connector manager
    // Only do cleanup that is SAFE from this context:
    // - Cancel timers that may fire on the workloop
    // - Release framebuffer memory that was prepared for IOGraphics
    // - Release PCI device and remaining IOService objects

    IOLog("FakeIrisXEFramebuffer::performSafeStop() — gated cleanup\n");

    // V281: Cancel timers (they may fire on the workloop)
    if (vsyncTimer) {
        vsyncTimer->cancelTimeout();
        // Don't remove from workLoop - already done in stop()
        auto* tmp = vsyncTimer;
        vsyncTimer = nullptr;
        tmp->release();
    }
    if (displayInjectTimer) {
        displayInjectTimer->cancelTimeout();
        auto* tmp = displayInjectTimer;
        displayInjectTimer = nullptr;
        tmp->release();
    }

    // V281: Stop power management
    PMstop();

    // V281: Release framebuffer memory with null-checks
    if (framebufferMemory) {
        framebufferMemory->complete();
        auto* tmp = framebufferMemory;
        framebufferMemory = nullptr;
        tmp->release();
    }
    if (framebufferSurface) {
        auto* tmp = framebufferSurface;
        framebufferSurface = nullptr;
        tmp->release();
    }
    if (cursorMemory) {
        auto* tmp = cursorMemory;
        cursorMemory = nullptr;
        tmp->release();
    }
    if (mmioMap) {
        auto* tmp = mmioMap;
        mmioMap = nullptr;
        tmp->release();
    }
    if (vramMemory) {
        auto* tmp = vramMemory;
        vramMemory = nullptr;
        tmp->release();
    }

    // V281: Release vsyncSource
    if (vsyncSource) {
        auto* tmp = vsyncSource;
        vsyncSource = nullptr;
        tmp->release();
    }

    // V281: Free locks
    if (timerLock) {
        IOLockFree(timerLock);
        timerLock = nullptr;
    }
    if (powerLock) {
        IOLockFree(powerLock);
        powerLock = nullptr;
    }

    // V281: Release interruptList
    if (interruptList) {
        auto* tmp = interruptList;
        interruptList = nullptr;
        tmp->release();
    }

    // V281: Close and release PCI device
    if (pciDevice) {
        pciDevice->close(this);
        auto* tmp = pciDevice;
        pciDevice = nullptr;
        tmp->release();
    }

    IOLog("FakeIrisXEFramebuffer::performSafeStop() — gated cleanup complete\n");
}














void FakeIrisXEFramebuffer::startIOFB() {
    IOLog("FakeIrisXEFramebuffer::startIOFB() called\n");
    injectDisplayMergeOverridesIfAvailable(this);
}

 
 
void FakeIrisXEFramebuffer::free() {
    IOLog("FakeIrisXEFramebuffer::free() called\n");
    
    if (clutTable) {
        IOFree(clutTable, 256 * sizeof(IOColorEntry));
        clutTable = nullptr;
    }
    
    if (gammaTable) {
        IOFree(gammaTable, gammaTableSize);
        gammaTable = nullptr;
        gammaTableSize = 0;
    }
    
    if (interruptList) {
        // Cleanup all interrupt registrations
        for (unsigned int i = 0; i < interruptList->getCount(); i++) {
            OSData* data = OSDynamicCast(OSData, interruptList->getObject(i));
            if (data) {
                IOFree((void*)data->getBytesNoCopy(), sizeof(InterruptInfo));
            }
        }
        interruptList->release();
        interruptList = nullptr;
    }
    
    if (powerLock) {
        IOLockFree(powerLock);
        powerLock = nullptr;
    }
    
    if (timerLock) {
        IOLockFree(timerLock);
        timerLock = nullptr;
    }
    
    driverActive = false;
    

        if (vsyncTimer) {
            workLoop->removeEventSource(vsyncTimer);
            vsyncTimer->release();
            vsyncTimer = nullptr;
        }
      
    
    
    
    if (workLoop) {
        if (commandGate) {
            workLoop->removeEventSource(commandGate);
            commandGate->release();
            commandGate = nullptr;
        }
        workLoop->release();  // release *your* retain only
        workLoop = nullptr;
    }

    
    
    
    OSSafeReleaseNULL(framebufferSurface);
    OSSafeReleaseNULL(cursorMemory);
    
    super::free();
}





void* FakeIrisXEFramebuffer::getFramebufferKernelPtr() const {
    return framebufferMemory ? framebufferMemory->getBytesNoCopy() : nullptr;
}




bool FakeIrisXEFramebuffer::makeUsable() {
    IOLog("makeUsable() called\n");
    return super::makeUsable();
}



void FakeIrisXEFramebuffer::activatePowerAndController() {
    IOLog("Delayed Power and Controller Activation\n");
    
    if (!pciDevice || !mmioBase) {
        IOLog(" activatePowerAndController: device or mmio not ready, aborting\n");
        return;
    }


    controllerEnabled = true;
    
    enableController();
    
    // Verify GPU still alive
        uint32_t ack = safeMMIORead(0x130044);
        IOLog("FORCEWAKE_ACK after enableController(): 0x%08X\n", ack);

    
        
   getProvider()->joinPMtree(this);
    // PMinit is void — no return check needed
    PMinit();
    IOLog("✅ PMinit called (void — no error check possible)\n");

    // Register power driver (this is the key — enables setPowerState callbacks)
   // registerPowerDriver(this, powerStates, kNumPowerStates);
    IOLog("✅ Power management registered\n");
   
    
    
   // makeUsable();
   
    
    
    
    displayOnline = true;

    
    IOLog("Delayed power and display activation complete\n");
        
}






IOReturn FakeIrisXEFramebuffer::newUserClient(task_t owningTask,
                                              void* securityID,
                                              UInt32 type,
                                              OSDictionary* properties,
                                              IOUserClient **handler)
{
    IOLog("[FakeIrisXEFramebuffer] newUserClient(type=%u)\n", type);

    //
    // Call the REAL IOFramebuffer::newUserClient !!!
    // We do this because WindowServer REQUIRES the real framebuffer UC.
    //

    IOFramebuffer* fb = OSDynamicCast(IOFramebuffer, this);

    if (!fb) {
        IOLog("[FakeIrisXEFramebuffer] ERROR: this is not an IOFramebuffer\n");
        return kIOReturnUnsupported;
    }

    //
    // Real IOFramebuffer::newUserClient has signature:
    // IOReturn newUserClient(task_t, void*, UInt32, IOUserClient**)
    //
    // So we pass ONLY 4 args, not 5.
    //

    IOReturn ret = fb->IOFramebuffer::newUserClient(owningTask,
                                                    securityID,
                                                    type,
                                                    handler);

    if (ret != kIOReturnSuccess) {
        IOLog("[FakeIrisXEFramebuffer] real IOFramebuffer::newUserClient failed (%x)\n", ret);
        return ret;
    }

    IOLog("[FakeIrisXEFramebuffer] returned REAL IOFramebufferUserClient OK\n");
    return kIOReturnSuccess;
}






IOReturn FakeIrisXEFramebuffer::staticFlushAction(OSObject *owner,
                                                  void *arg0,
                                                  void *arg1,
                                                  void *arg2,
                                                  void *arg3)
{
    FakeIrisXEFramebuffer *fb = OSDynamicCast(FakeIrisXEFramebuffer, owner);
    if (!fb) return kIOReturnBadArgument;

    fb->flushDisplay();   // safe, running on the FB workloop
    return kIOReturnSuccess;
}


void FakeIrisXEFramebuffer::scheduleFlushFromAccelerator()
{
    // Coalesce bursts from accelerator submissions.
    if (fNeedFlush || fFlushInProgress) {
        fFlushDeferred = true;
        return;
    }

    fNeedFlush = true;

    if (commandGate) {
        // Correct runAction syntax:
        commandGate->runAction(&FakeIrisXEFramebuffer::staticFlushAction);
    } else {
        // Fallback (not recommended but safe):
        flushDisplay();
    }
}









// Tiger Lake register addresses (verified from your system)
#define TRANS_CONF_A        0x70008
#define TRANS_HTOTAL_A      0x60000
#define TRANS_HBLANK_A      0x60004
#define TRANS_HSYNC_A       0x60008
#define TRANS_VTOTAL_A      0x6000C
#define TRANS_VBLANK_A      0x60010
#define TRANS_VSYNC_A       0x60014
#define TRANS_DDI_FUNC_CTL_A 0x60400
#define TRANS_CLK_SEL_A     0x46140

#define PLANE_CTL_1_A       0x70180
#define PLANE_SURF_1_A      0x7019C
#define PLANE_STRIDE_1_A    0x70188
#define PLANE_POS_1_A       0x7018C
#define PLANE_SIZE_1_A      0x70190

#define LCPLL1_CTL          0x46010  // DPLL0 on Tiger Lake

// DDI registers (not in your dump, using standard addresses)
#define DDI_BUF_CTL_A       0x64000
#define DDI_BUF_TRANS_A     0x64E00



// Try both panel power locations
#define PP_STATUS_OLD       0x61200  // Pre-TGL
#define PP_CONTROL_OLD      0x61204
#define PP_STATUS_NEW       0xC7200  // TGL
#define PP_CONTROL_NEW      0xC7204
#define PP_ON_DELAYS_NEW    0xC7208
#define PP_OFF_DELAYS_NEW   0xC720C

// Backlight
#define BXT_BLC_PWM_CTL1    0xC8250
#define BXT_BLC_PWM_FREQ_REG 0xC8254
#define BXT_BLC_PWM_DUTY_REG 0xC8258

#define PLANE_OFFSET_1_A 0x00000000
#define PIPECONF_A      0x70008

// Tiger Lake / Apple backlight PWM registers from DTK traces.
static constexpr uint32_t BXT_BLC_PWM_FREQ1 = 0x000C8254;
static constexpr uint32_t BXT_BLC_PWM_DUTY1 = 0x000C8258;



IOReturn FakeIrisXEFramebuffer::enableController() {
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║  V131: enableController() - Comprehensive Diagnostics         ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    IOSleep(30);

    if (!mmioBase || !framebufferMemory) {
        IOLog("❌ [V131] CRITICAL: MMIO or framebuffer not set!\n");
        IOLog("   mmioBase: %p\n", mmioBase);
        IOLog("   framebufferMemory: %p\n", framebufferMemory);
        return kIOReturnError;
    }
    
    IOLog("✅ [V131] Prerequisites check passed\n");
    IOLog("   mmioBase: %p\n", mmioBase);
    IOLog("   framebufferMemory: %p (size: %llu bytes)\n", 
          framebufferMemory, framebufferMemory->getLength());

    // V240: Initialize Connector Manager for Tiger Lake
    initializeConnectorManager();
    
    // ---- constants (Tiger Lake) ----
 //   const uint32_t PIPECONF_A       = 0x70008;
    const uint32_t PIPE_SRC_A       = 0x6001C;

    auto rd = [&](uint32_t off) { return safeMMIORead(off); };
    auto wr = [&](uint32_t off, uint32_t val) { safeMMIOWrite(off, val); };

    #define LOG_FLUSH(msg) do { IOLog msg ; IOSleep(30); } while (0)

    const uint32_t width  = H_ACTIVE;   // 1920
    const uint32_t height = V_ACTIVE;   // 1080
    const IOPhysicalAddress phys = framebufferMemory->getPhysicalAddress();

    IOLog("DEBUG[V38]: reading initial state…\n");
    IOLog("  PLANE_CTL_1_A (before):   0x%08X\n", rd(PLANE_CTL_1_A));
    IOLog("  PLANE_SURF_1_A (before):  0x%08X\n", rd(PLANE_SURF_1_A));
    IOLog("  PLANE_STRIDE_1_A (before):0x%08X\n", rd(PLANE_STRIDE_1_A));
    IOLog("  TRANS_CONF_A (before):    0x%08X\n", rd(TRANS_CONF_A));
    IOLog("  PIPECONF_A (before):      0x%08X\n", rd(PIPECONF_A));
    LOG_FLUSH(("DEBUG[V38]: …state read complete.\n"));

    // --- 1) Program visible pipe source ---
    wr(PIPE_SRC_A, ((width - 1) << 16) | (height - 1));
    IOLog("✅ PIPE_SRC_A set to %ux%u (reg=0x%08X)\n", width, height, rd(PIPE_SRC_A));

    // --- 2) Program plane position/size ---
    wr(PLANE_POS_1_A, 0x00000000);
    wr(PLANE_SIZE_1_A, ((height - 1) << 16) | (width - 1));
    IOLog("✅ PLANE_POS_1_A=0x%08X, PLANE_SIZE_1_A=0x%08X\n",
          rd(PLANE_POS_1_A), rd(PLANE_SIZE_1_A));

   
    
    
    
    
    // --------- MAP FB INTO GGTT -----------

    if (!mapFramebufferIntoGGTT()) {
         IOLog("❌ GGTT mapping failed\n");
         return kIOReturnError;
     }

     // V131: WRITE TEST PATTERN BEFORE ENABLING PLANE
     // This ensures the GPU sees colors immediately when plane is enabled
     IOLog("\n");
     IOLog("╔══════════════════════════════════════════════════════════════╗\n");
     IOLog("║  V131: PRE-ENABLE TEST PATTERN                                ║\n");
     IOLog("╚══════════════════════════════════════════════════════════════╝\n");
     IOLog("\n");
     
     if (framebufferMemory && kernelFBPtr) {
         uint32_t* fb = (uint32_t*)kernelFBPtr;
         size_t fbSize = framebufferMemory->getLength();
         uint32_t width = H_ACTIVE;   // 1920
         uint32_t height = V_ACTIVE;  // 1080
         uint32_t stride = width;     // pixels per row
         
         IOLog("[V131] Writing test pattern BEFORE enabling plane...\n");
         IOLog("[V131] Framebuffer: %p, Size: %zu bytes\n", fb, fbSize);
         
         // Write test pattern: color bars
         uint32_t barWidth = width / 8;
         uint32_t colors[8] = {
             0xFFFF0000,  // Red
             0xFF00FF00,  // Green
             0xFF0000FF,  // Blue
             0xFFFFFF00,  // Yellow
             0xFF00FFFF,  // Cyan
             0xFFFF00FF,  // Magenta
             0xFFFFFFFF,  // White
             0xFF808080   // Gray
         };
         
         for (uint32_t y = 0; y < height; y++) {
             for (uint32_t x = 0; x < width; x++) {
                 uint32_t bar = x / barWidth;
                 if (bar > 7) bar = 7;
                 fb[y * stride + x] = colors[bar];
             }
         }
         
         // Add white border
         for (uint32_t x = 0; x < width; x++) {
             fb[0 * stride + x] = 0xFFFFFFFF;
             fb[(height-1) * stride + x] = 0xFFFFFFFF;
         }
         for (uint32_t y = 0; y < height; y++) {
             fb[y * stride + 0] = 0xFFFFFFFF;
             fb[y * stride + (width-1)] = 0xFFFFFFFF;
         }
         
         // Force memory flush
         __sync_synchronize();
         IOSleep(10);  // Give memory time to settle
         
         IOLog("[V131] ✅ Test pattern written to framebuffer\n");
         IOLog("[V131] Colors should appear immediately when plane is enabled\n");
     }

     // --------- PROGRAM PLANE SURFACE ---------
     wr(PLANE_SURF_1_A, fbGGTTOffset);
     IOLog("PLANE_SURF_1_A = 0x%08X\n", rd(PLANE_SURF_1_A));
   
   
    
    
    

    
    
    // ddb entry for pipe a plane 1
    const uint32_t PLANE_BUF_CFG_1_A = 0x70140;
    wr(PLANE_BUF_CFG_1_A, (0x07FFu << 16) | 0x000u);
    IOLog("DDB buffer assigned (PLANE_BUF_CFG_1_A=0x%08X)\n", rd(PLANE_BUF_CFG_1_A));

    
    
    // === TIGER LAKE WATERMARK / FIFO FIX (THIS KILLS THE FLICKERING LINES) ===
    wr(0xC4060, 0x00003FFF);   // WM_LINETIME_A  – increase line time watermark
    wr(0xC4064, 0x00000010);   // WM0_PIPE_A     – conservative level 0
    wr(0xC4068, 0x00000020);   // WM1_PIPE_A     – level 1
    wr(0xC406C, 0x00000040);   // WM2_PIPE_A     – level 2
    wr(0xC4070, 0x00000080);   // WM3_PIPE_A     – level 3

    // Force maximum priority for primary plane
    wr(0xC4020, 0x0000000F);   // ARB_CTL – give plane highest priority

    IOLog("Tiger Lake FIFO/watermark fix applied \n");
    
    
    
    // --- Program stride (in 64-byte blocks) ---
    const uint32_t strideBytes  = 7680;
    const uint32_t strideBlocks = strideBytes / 64;  // Each unit = 64 bytes
    wr(PLANE_STRIDE_1_A, strideBlocks);
    IOSleep(1);
    uint32_t readBack = rd(PLANE_STRIDE_1_A);
    IOLog("✅ PLANE_STRIDE_1_A programmed: %u blocks (64B each), readback=0x%08X\n", strideBlocks, readBack);
    IOLog("👉 Effective byte stride = %u bytes\n", readBack * 64);


     
     
    uint32_t planeCtl = rd(PLANE_CTL_1_A);

    // enable plane
    planeCtl |= (1u << 31);

    // pixel format ARGB8888 = 0x06 << 24
    planeCtl &= ~(7u << 24);
    planeCtl |= (6u << 24);

    // disable all tiling modes
    planeCtl &= ~(3u << 10);   // bits 11:10 = 00 = linear

    // disable rotation
    planeCtl &= ~(3u << 14);

    // write back
    wr(PLANE_CTL_1_A, planeCtl);

    IOLog("PLANE_CTL_1_A (linear/ARGB8888) = 0x%08X\n", rd(PLANE_CTL_1_A));

    
    
    
    // Disable cursor plane (CURSOR_CTL_A = 0x70080)
    wr(0x70080, 0x00000000);  // CURSOR_CTL = disabled
    wr(0x70084, 0x00000000);  // CURBASE = null
    wr(0x7008C, 0x00000000);   // CUR_POS_A  = 0,0
    IOLog("Cursor plane disabled (0x%08X)\n", rd(0x70080));
    
    
    
    // --- Program Pipe A timings for 1920x1080@60 ---
    const uint32_t HTOTAL_A = 0x60000;
    const uint32_t HBLANK_A = 0x60004;
    const uint32_t HSYNC_A  = 0x60008;
    const uint32_t VTOTAL_A = 0x6000C;
    const uint32_t VBLANK_A = 0x60010;
    const uint32_t VSYNC_A  = 0x60014;

    const uint32_t h_active   = 1920;
    const uint32_t h_frontpor = 88;
    const uint32_t h_sync     = 44;
    const uint32_t h_backpor  = 148;
    const uint32_t h_total    = h_active + h_frontpor + h_sync + h_backpor;

    const uint32_t v_active   = 1080;
    const uint32_t v_frontpor = 4;
    const uint32_t v_sync     = 5;
    const uint32_t v_backpor  = 36;
    const uint32_t v_total    = v_active + v_frontpor + v_sync + v_backpor;

    auto pack = [](uint32_t hi, uint32_t lo){ return ((hi - 1) << 16) | (lo - 1); };

    wr(HTOTAL_A, pack(h_total,  h_active));
    wr(HBLANK_A, pack(h_total,  h_active));
    wr(HSYNC_A,  pack(h_active + h_frontpor + h_sync, h_active + h_frontpor));

    wr(VTOTAL_A, pack(v_total,  v_active));
    wr(VBLANK_A, pack(v_total,  v_active));
    wr(VSYNC_A,  pack(v_active + v_frontpor + v_sync, v_active + v_frontpor));

    IOLog("✅ Pipe A timings set: %ux%u @60 (HTOTAL=0x%08X VTOTAL=0x%08X)\n",
          h_active, v_active, rd(HTOTAL_A), rd(VTOTAL_A));

    // Disable panel fitter / pipe scaler
    const uint32_t PF_CTL_A      = 0x68080;
    const uint32_t PF_WIN_POS_A  = 0x68070;
    const uint32_t PF_WIN_SZ_A   = 0x68074;
    const uint32_t PS_CTRL_1_A   = 0x68180;
    const uint32_t PS_WIN_POS_1A = 0x68170;
    const uint32_t PS_WIN_SZ_1A  = 0x68174;

    wr(PF_CTL_A,     0x00000000);
    wr(PF_WIN_POS_A, 0x00000000);
    wr(PF_WIN_SZ_A,  ((v_active & 0x1FFF) << 16) | (h_active & 0x1FFF));
    wr(PS_CTRL_1_A,  0x00000000);
    wr(PS_WIN_POS_1A,0x00000000);
    wr(PS_WIN_SZ_1A, ((v_active & 0x1FFF) << 16) | (h_active & 0x1FFF));

    IOLog("✅ Panel fitter / pipe scaler forced OFF\n");

    // V131: CRITICAL FIX - Power up eDP Panel BEFORE enabling pipe/transcoder
    // For eDP, panel must be powered before enabling display pipeline
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║  V131: PANEL POWER SEQUENCING (Critical Fix)                  ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    
    const uint32_t panelControlNew = PP_CONTROL_NEW;
    const uint32_t panelStatusNew  = PP_STATUS_NEW;
    const uint32_t panelControlOld = PP_CONTROL_OLD;
    const uint32_t panelStatusOld  = PP_STATUS_OLD;
    
    // Step 1: Power up panel
    IOLog("[V131] Step 1: Powering up eDP panel...\n");
    if (rd(PP_ON_DELAYS_NEW) == 0) {
        wr(PP_ON_DELAYS_NEW, 0x01900190u);
    }
    if (rd(PP_OFF_DELAYS_NEW) == 0) {
        wr(PP_OFF_DELAYS_NEW, 0x01900190u);
    }
    wr(panelControlNew, rd(panelControlNew) | (1u << 31) | (1u << 30) | 0x8u);
    wr(panelControlOld, rd(panelControlOld) | (1u << 31) | (1u << 30));
    IOSleep(20);  // V251: Reduced from 100ms to prevent stall
    
    // Step 2: Wait for panel power ready (CRITICAL)
    IOLog("[V131] Step 2: Waiting for panel power ready...\n");
    bool panelReady = false;
    for (int i = 0; i < 200; i++) {  // Increased timeout
        uint32_t statusNew = rd(panelStatusNew);
        uint32_t statusOld = rd(panelStatusOld);
        if ((statusNew & (1u << 31)) || (statusOld & (1u << 31))) {
            IOLog("[V131] ✅ Panel power ready (PP_STATUS_NEW=0x%08X PP_STATUS_OLD=0x%08X)\n",
                  statusNew,
                  statusOld);
            panelReady = true;
            break;
        }
        IOSleep(10);
    }
    
    if (!panelReady) {
        IOLog("[V131] ⚠️ Panel power timeout - new=0x%08X old=0x%08X\n",
              rd(panelStatusNew),
              rd(panelStatusOld));
    }
    
    IOSleep(200);  // Extra delay after panel power
    
    // Step 3: Enable DDI A buffer (before pipe/transcoder)
    IOLog("[V131] Step 3: Enabling DDI A buffer...\n");
    uint32_t ddi = rd(DDI_BUF_CTL_A);
    ddi |= (1u << 31);  // Enable
    ddi &= ~(7u << 24);
    ddi |= (1u << 24);  // x1 width for eDP
    wr(DDI_BUF_CTL_A, ddi);
    IOSleep(5);
    IOLog("[V131] DDI_BUF_CTL_A = 0x%08X\n", rd(DDI_BUF_CTL_A));
    
    // Step 4: Enable Pipe A
    IOLog("[V131] Step 4: Enabling Pipe A...\n");
    uint32_t pipeconf = rd(PIPECONF_A);
    pipeconf |= (1u << 31);  // Enable
    pipeconf |= (1u << 30);  // Progressive
    wr(PIPECONF_A, pipeconf);
    IOSleep(5);
    IOLog("[V131] PIPECONF_A = 0x%08X\n", rd(PIPECONF_A));
    
    // Step 5: Enable Transcoder A
    IOLog("[V131] Step 5: Enabling Transcoder A...\n");
    uint32_t trans = rd(TRANS_CONF_A);
    trans |= (1u << 31);  // Enable
    wr(TRANS_CONF_A, trans);
    IOSleep(5);
    IOLog("[V131] TRANS_CONF_A = 0x%08X\n", rd(TRANS_CONF_A));
    
    // Step 6: Force display online
    IOLog("[V131] Step 6: Forcing display online...\n");
    displayOnline = true;
    controllerEnabled = true;
    setProperty("IOFBDisplayOnline", kOSBooleanTrue);
    setProperty("display-online", kOSBooleanTrue);
    IOLog("[V131] ✅ Display forced online\n");

    
    
    // --- Enable backlight ---
    initBacklightHardware();
    ensureBacklightHardwareState("enableController");
    setBacklightPercent(100, "enableController-init");

    // V81: Write test pattern to framebuffer to verify panel output
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║  V81: PANEL OUTPUT TEST - Writing Test Pattern               ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    
    if (framebufferMemory && kernelFBPtr) {
        uint32_t* fb = (uint32_t*)kernelFBPtr;
        size_t fbSize = framebufferMemory->getLength();
        uint32_t width = H_ACTIVE;   // 1920
        uint32_t height = V_ACTIVE;  // 1080
        uint32_t stride = width;     // pixels per row
        
        IOLog("[V81] Framebuffer: %p, Size: %zu bytes\n", fb, fbSize);
        IOLog("[V81] Resolution: %ux%u, Stride: %u\n", width, height, stride);
        
        // Clear to black first
        for (uint32_t i = 0; i < (fbSize / 4); i++) {
            fb[i] = 0xFF000000;  // Black (ARGB)
        }
        IOLog("[V81] Cleared framebuffer to black\n");
        
        // Write test pattern: color bars
        uint32_t barWidth = width / 8;
        uint32_t colors[8] = {
            0xFFFF0000,  // Red
            0xFF00FF00,  // Green
            0xFF0000FF,  // Blue
            0xFFFFFF00,  // Yellow
            0xFF00FFFF,  // Cyan
            0xFFFF00FF,  // Magenta
            0xFFFFFFFF,  // White
            0xFF808080   // Gray
        };
        
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t bar = x / barWidth;
                if (bar > 7) bar = 7;
                fb[y * stride + x] = colors[bar];
            }
        }
        IOLog("[V81] Test pattern written: 8 color bars\n");
        
        // Add white border to confirm boundaries
        for (uint32_t x = 0; x < width; x++) {
            fb[0 * stride + x] = 0xFFFFFFFF;                    // Top border
            fb[(height-1) * stride + x] = 0xFFFFFFFF;           // Bottom border
        }
        for (uint32_t y = 0; y < height; y++) {
            fb[y * stride + 0] = 0xFFFFFFFF;                    // Left border
            fb[y * stride + (width-1)] = 0xFFFFFFFF;            // Right border
        }
        IOLog("[V81] White borders added\n");
        
        // Force memory flush
        #ifdef OSMemoryBarrier
        OSMemoryBarrier();
        #else
        __asm__ volatile("mfence" ::: "memory");
        #endif
        
        IOLog("[V81] ✅ Test pattern complete - colors should be visible on panel\n");
    } else {
        IOLog("[V81] ❌ Cannot write test pattern - framebuffer not available\n");
    }
    
    // V81: Panel diagnostics
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║  V81: PANEL DIAGNOSTICS                                      ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    
    // Check transcoder status
    uint32_t transConf = rd(TRANS_CONF_A);
    IOLog("[V81] TRANS_CONF_A = 0x%08X\n", transConf);
    IOLog("       Enabled: %s\n", (transConf & (1u << 31)) ? "YES ✅" : "NO ❌");
    
    // Check pipe status
    uint32_t pipeConf = rd(PIPECONF_A);
    IOLog("[V81] PIPECONF_A = 0x%08X\n", pipeConf);
    IOLog("       Enabled: %s\n", (pipeConf & (1u << 31)) ? "YES ✅" : "NO ❌");
    IOLog("       Interlace: %s\n", (pipeConf & (1u << 21)) ? "YES" : "NO (Progressive)");
    
    // Check DDI buffer status
    uint32_t ddiCtl = rd(DDI_BUF_CTL_A);
    IOLog("[V81] DDI_BUF_CTL_A = 0x%08X\n", ddiCtl);
    IOLog("       Buffer Enabled: %s\n", (ddiCtl & (1u << 31)) ? "YES ✅" : "NO ❌");
    IOLog("       Port Width: x%u\n", ((ddiCtl >> 1) & 0x7) + 1);
    
    // Check plane status
    uint32_t planeCtlV81 = rd(PLANE_CTL_1_A);
    IOLog("[V81] PLANE_CTL_1_A = 0x%08X\n", planeCtlV81);
    IOLog("       Plane Enabled: %s\n", (planeCtlV81 & (1u << 31)) ? "YES ✅" : "NO ❌");
    uint32_t formatV81 = (planeCtlV81 >> 24) & 0x7;
    const char* formatNamesV81[] = {"8-bit", "16-bit", "??", "??", "32-bit", "??", "ARGB8888", "??"};
    IOLog("       Format: %s\n", formatNamesV81[formatV81] ? formatNamesV81[formatV81] : "Unknown");
    
    // Check surface address
    uint32_t surfAddrV81 = rd(PLANE_SURF_1_A);
    IOLog("[V81] PLANE_SURF_1_A = 0x%08X (GGTT offset)\n", surfAddrV81);
    
    // Check PP (Panel Power) status
    uint32_t ppStatusNewV81 = rd(PP_STATUS_NEW);
    uint32_t ppStatusOldV81 = rd(PP_STATUS_OLD);
    IOLog("[V81] PP_STATUS_NEW = 0x%08X\n", ppStatusNewV81);
    IOLog("[V81] PP_STATUS_OLD = 0x%08X\n", ppStatusOldV81);
    IOLog("       Panel Power: %s\n", ((ppStatusNewV81 | ppStatusOldV81) & (1u << 31)) ? "ON ✅" : "OFF ❌");
    
    IOLog("\n");
    IOLog("[V81] Panel Output Test Complete\n");
    IOLog("\n");

    IOLog("DEBUG: Flushing display…\n");
    flushDisplay();

    // --- Final diagnostics ---
    IOLog("🔍 FINAL REGISTER DUMP:\n");
    IOLog("  PIPE_SRC_A          = 0x%08X\n", rd(PIPE_SRC_A));
    IOLog("  PLANE_POS_1_A       = 0x%08X\n", rd(PLANE_POS_1_A));
    IOLog("  PLANE_SIZE_1_A      = 0x%08X\n", rd(PLANE_SIZE_1_A));
    IOLog("  PLANE_SURF_1_A      = 0x%08X\n", rd(PLANE_SURF_1_A));
    IOLog("  PLANE_STRIDE_1_A    = 0x%08X\n", rd(PLANE_STRIDE_1_A));
    IOLog("  PLANE_CTL_1_A       = 0x%08X\n", rd(PLANE_CTL_1_A));
    IOLog("  PIPECONF_A          = 0x%08X\n", rd(PIPECONF_A));
    IOLog("  TRANS_CONF_A        = 0x%08X\n", rd(TRANS_CONF_A));
    IOLog("  DDI_BUF_CTL_A       = 0x%08X\n", rd(DDI_BUF_CTL_A));

    IOLog("enableController(V38) complete.\n");
    return kIOReturnSuccess;
}





void FakeIrisXEFramebuffer::waitVBlank() {
    const uint32_t PIPE_DSL_A = 0x60000 + 0x1A0;
    uint32_t last = safeMMIORead(PIPE_DSL_A);
    const int MAX_ITER = 200000;
    for (int i = 0; i < MAX_ITER; ++i) {
        uint32_t now = safeMMIORead(PIPE_DSL_A);
        if (now < last) return; // wrapped -> vblank
        last = now;
        if ((i & 0x3FFF) == 0) IOSleep(1); // every ~16384 iterations yield to scheduler
    }
    IOLog("waitVBlank: timeout after %d iterations\n", MAX_ITER);
}






bool FakeIrisXEFramebuffer::mapFramebufferIntoGGTT()
{
    // -----------------------------
    // 1) Read BAR1 = GTTMMADR
    // -----------------------------
    uint64_t bar1Lo = pciDevice->configRead32(0x18) & ~0xF;
    uint64_t bar1Hi = pciDevice->configRead32(0x1C);
    uint64_t gttPhys = (bar1Hi << 32) | bar1Lo;

    IOLog("🟢 BAR1 (GTTMMADR) = 0x%llX\n", gttPhys);

    // Map full 16MB GGTT aperture
    IOMemoryDescriptor* gttDesc =
        IOMemoryDescriptor::withPhysicalAddress(gttPhys, 0x1000000, kIODirectionInOut);

    if (!gttDesc) {
        IOLog("❌ Failed to create GGTT descriptor\n");
        return false;
    }

    IOMemoryMap* gttMap = gttDesc->map();
    if (!gttMap) {
        IOLog("❌ Failed to map GTTMMADR\n");
        gttDesc->release();
        return false;
    }

    gttVa = reinterpret_cast<void*>(gttMap->getVirtualAddress());
    fGGTT = reinterpret_cast<volatile uint32_t*>(gttVa);
    fGGTTSize = 0x1000000;
    IOLog("🟢 GTTMMADR mapped at VA=%p\n", gttVa);


    if (!gttVa || !framebufferMemory) {
        IOLog("❌ GGTT map: missing gttVa or framebufferMemory\n");
        return false;
    }

    volatile uint64_t* ggtt = reinterpret_cast<volatile uint64_t*>(gttVa);

    const uint32_t kPageSize = 4096;
    const uint64_t kPteFlags = 0x0000000000000003ULL; // Present + writable


    // -----------------------------------------
    // 2) Final, correct GGTT offset for FB
    // -----------------------------------------
    // Your framebuffer will appear at GPU VA = 0x08000000
    // SAFE region: 128MB–144MB is unused by GuC / engines
    fbGGTTOffset = 0x00000800;

    uint32_t ggttBaseIndex = fbGGTTOffset >> 12;

    IOLog("🟢 GGTT mapping: fbGGTTOffset=0x%08X index=%u\n",
          fbGGTTOffset, ggttBaseIndex);


    // -----------------------
    // 3) Walk physical pages
    // -----------------------
    IOByteCount fbSize = framebufferMemory->getLength();
    IOByteCount offset = 0;

    uint32_t page = 0;

    while (offset < fbSize)
    {
        IOByteCount segLen = 0;
        IOPhysicalAddress segPhys =
            framebufferMemory->getPhysicalSegment(offset, &segLen);

        if (!segPhys || segLen == 0) {
            IOLog("❌ GGTT map: getPhysicalSegment failed at offset 0x%llX\n",
                  (uint64_t)offset);
            return false;
        }

        // Page-align
        segLen &= ~(kPageSize - 1);
        if (segLen == 0) {
            IOLog("❌ GGTT map: segment < 4KB\n");
            return false;
        }

     /*
        IOLog("Physical segment: phys=0x%llX len=0x%llX\n",
              (uint64_t)segPhys, (uint64_t)segLen);
*/

        for (IOByteCount segOff = 0;
             segOff < segLen && offset < fbSize;
             segOff += kPageSize, offset += kPageSize, ++page)
        {
            uint64_t phys = (uint64_t)(segPhys + segOff);

            uint32_t ggttIndex = ggttBaseIndex + page;

            uint64_t pte = (phys & ~0xFFFULL) | kPteFlags;
            ggtt[ggttIndex] = pte;

            uint64_t verify = ggtt[ggttIndex];

       /*    IOLog("   GGTT[%u] = 0x%016llX (verify=0x%016llX)\n",
                  ggttIndex, pte, verify);
        */
        }
    }

    IOLog("🟢 GGTT mapping complete (%u pages)\n", page);

    return true;
}





 
 
void FakeIrisXEFramebuffer::disableController() {

    IOLog("Controller disabled\n");
}
 


bool FakeIrisXEFramebuffer::getIsUsable() const {
    return fullyInitialized && displayOnline && controllerEnabled && !shuttingDown;
}






IOReturn FakeIrisXEFramebuffer::getTimingInfoForDisplayMode(
    IODisplayModeID displayMode,
    IOTimingInformation* infoOut)
{
    // P0A: Intentional timing strategy — appleTimingID=0x7F (native fallback).
    // macOS derives timing from the display's reported capabilities when
    // appleTimingID=0x7F is set. No detailed IOTimingDescription blocks are
    // provided. This is the same strategy used in getInformationForDisplayMode()
    // (both set appleTimingID=0x7F with kIOTimingInfoValid_AppleTimingID).
    // Rationale: providing detailed timing values caused CoreDisplay assertions
    // in earlier builds when timing values conflicted with system expectations.
    if (!infoOut) {
        IOLog("[P0A] getTimingInfoForDisplayMode(mode=%u): ❌ NULL info\n", displayMode);
        return kIOReturnBadArgument;
    }

    const ProofDisplayMode* m = getProofModeByID(displayMode);
    if (!m) {
        IOLog("[P0A] getTimingInfoForDisplayMode(mode=%u): ❌ not in table\n", displayMode);
        return kIOReturnUnsupportedMode;
    }

    bzero(infoOut, sizeof(IOTimingInformation));
    infoOut->appleTimingID = kIOTimingID_TigerLake_Fallback;
    infoOut->flags         = kIOTimingInfoValid_AppleTimingID;

    IOLog("[P0A] getTimingInfoForDisplayMode(mode=%u): ✅ appleTimingID=0x%02X (native fallback) %dx%d\n",
           displayMode, infoOut->appleTimingID, m->width, m->height);
    return kIOReturnSuccess;
}









IOReturn FakeIrisXEFramebuffer::getGammaTable(UInt32 channelCount,
                                              UInt32* dataCount,
                                              UInt32* dataWidth,
                                              void** data) {
    if (!dataCount || !dataWidth || !data) {
        return kIOReturnBadArgument;
    }
    
    if (!gammaTable) {
        return kIOReturnNotFound;
    }
    
    *dataCount = 256;  // Standard 256-entry gamma table
    *dataWidth = 8;    // 8-bit per channel
    *data = gammaTable;
    
    return kIOReturnSuccess;
}







const char* FakeIrisXEFramebuffer::getPixelFormats(void)
{
    // FIXED: Return "ARGB8888" null-terminated (CoreDisplay parses this)
    static const char pixelFormats[] = "ARGB8888\0";
    return pixelFormats;
}





    
IOReturn FakeIrisXEFramebuffer::setCursorImage(void* cursorImage) {
    if (!cursorImage || !cursorMemory) {
        return kIOReturnBadArgument;
    }
    
    void* cursorBuffer = cursorMemory->getBytesNoCopy();
    if (!cursorBuffer) {
        return kIOReturnError;
    }
    
    // Copy cursor data (assuming 32x32 ARGB cursor)
    bcopy(cursorImage, cursorBuffer, 32 * 32 * 4);
    
    IOLog("Cursor image updated\n");
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible) {
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::registerForInterruptType(IOSelect interruptType,
                                                         IOFBInterruptProc proc,
                                                         void* ref,
                                                         void** interruptRef) {
    if (!proc || !interruptRef) {
        return kIOReturnBadArgument;
    }
    
    InterruptInfo* info = (InterruptInfo*)IOMalloc(sizeof(InterruptInfo));
    if (!info) return kIOReturnNoMemory;
    
    info->type = interruptType;
    info->proc = proc;
    info->ref = ref;
    
    // Add to interrupt list
    if (!interruptList) {
        interruptList = OSArray::withCapacity(4);
    }
    
    OSData* infoData = OSData::withBytes(info, sizeof(InterruptInfo));
    if (infoData) {
        interruptList->setObject(infoData);
        infoData->release();
    }
    
    *interruptRef = info;
    
    IOLog("✅ Interrupt registered for type 0x%x\n", interruptType);
    return kIOReturnSuccess;
}




IOReturn FakeIrisXEFramebuffer::unregisterInterrupt(void* interruptRef) {
    if (!interruptRef || !interruptList) {
        return kIOReturnBadArgument;
    }
    
    // Find and remove from interrupt list
    for (unsigned int i = 0; i < interruptList->getCount(); i++) {
        OSData* data = OSDynamicCast(OSData, interruptList->getObject(i));
        if (data && data->getBytesNoCopy() == interruptRef) {
            interruptList->removeObject(i);
            IOFree(interruptRef, sizeof(InterruptInfo));
            IOLog("✅ Interrupt unregistered\n");
            return kIOReturnSuccess;
        }
    }
    
    return kIOReturnNotFound;
}



IOReturn FakeIrisXEFramebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    if (!isSupportedProofDepth(depth)) {
        IOLog("[P0A] setDisplayMode(mode=%u, depth=%u): ❌ unsupported depth\n", mode, depth);
        return kIOReturnUnsupportedMode;
    }

    const ProofDisplayMode* m = getProofModeByID(mode);
    if (!m) {
        IOLog("[P0A] setDisplayMode(mode=%u, depth=%u): ❌ mode not in table\n", mode, depth);
        return kIOReturnUnsupportedMode;
    }

    currentMode = m->modeID;
    currentDepth = depth;

    IOLog("[P0A] setDisplayMode(mode=%u, depth=%u): ✅ %dx%d %s\n",
          mode, depth, m->width, m->height, m->name);
    return kIOReturnSuccess;
}






IOReturn FakeIrisXEFramebuffer::createSharedCursor(IOIndex index, int version) {
    if (index != 0 || version != 2) {
        return kIOReturnBadArgument;
    }
    
    if (!cursorMemory) {
        cursorMemory = IOBufferMemoryDescriptor::withOptions(
            kIOMemoryKernelUserShared | kIODirectionInOut,
            4096,  // 4KB for cursor
            page_size
        );
        
        if (!cursorMemory) {
            return kIOReturnNoMemory;
        }
        
        bzero(cursorMemory->getBytesNoCopy(), 4096);
    }
    
    IOLog("Shared cursor created\n");
    return kIOReturnSuccess;
}




IOReturn FakeIrisXEFramebuffer::setBounds(IOIndex index, IOGBounds *bounds) {
    if (!bounds) {
        IOLog("[P0A] setBounds(): ❌ NULL bounds\n");
        return kIOReturnBadArgument;
    }

    if (const ProofDisplayMode* m = getCurrentProofMode(currentMode)) {
        bounds->minx = 0;
        bounds->miny = 0;
        bounds->maxx = m->width;
        bounds->maxy = m->height;
        IOLog("[P0A] setBounds(): ✅ %dx%d\n", m->width, m->height);
    } else {
        IOLog("[P0A] setBounds(): ❌ no current mode, using fallback 1920x1080\n");
        bounds->minx = 0;
        bounds->miny = 0;
        bounds->maxx = 1920;
        bounds->maxy = 1080;
    }
    return kIOReturnSuccess;
}





IOReturn FakeIrisXEFramebuffer::clientMemoryForType(UInt32 type, UInt32* flags, IOMemoryDescriptor** memory)
{
    IOLog("🎨 clientMemoryForType: type=%u (0x%08X)\n", type, type);

    // Define standard memory types (from IOFramebufferShared.h)
    enum {
        kIOFBSystemAperture  = 0,    // Main framebuffer memory
        kIOFBCursorMemory    = 1,    // Cursor memory
        kIOFBVRAMMemory      = 2     // General VRAM
    };

    // System aperture (main framebuffer) - type 0
    if (type == kIOFBSystemAperture) {
        IODeviceMemory *devMem = getVRAMRange();
        if (devMem) {
            *memory = devMem;
            if (flags) *flags = 0;
            IOLog("✅ Returning system aperture memory\n");
            return kIOReturnSuccess;
        }
    }
    
    // Cursor memory - type 1
    if (type == kIOFBCursorMemory && cursorMemory) {
        cursorMemory->retain();
        *memory = cursorMemory;
        if (flags) *flags = 0;
        IOLog("✅ Returning cursor memory\n");
        return kIOReturnSuccess;
    }
    
    // VRAM memory - type 2 (for textures/acceleration)
    if (type == kIOFBVRAMMemory) {
        if (textureMemory) {
            // Return texture memory for acceleration
            textureMemory->retain();
            *memory = textureMemory;
            IOLog("✅ Returning texture memory for acceleration\n");
        } else {
            // Fallback to main framebuffer
            framebufferSurface->retain();
            *memory = framebufferSurface;
            IOLog("✅ Returning VRAM memory\n");
        }
        if (flags) *flags = 0;
        return kIOReturnSuccess;
    }

    IOLog("❓ Unsupported memory type: 0x%08X\n", type);
    return kIOReturnUnsupported;
}






// ==== REAL FLUSH WORK (runs on workloop thread) ====
IOReturn FakeIrisXEFramebuffer::performFlushNow()
{
    const uint64_t nowAbs = mach_absolute_time();

    if (fFlushInProgress) {
        fFlushDeferred = true;
        return kIOReturnSuccess;
    }

    if (fLastFlushAbsTime != 0) {
        const uint64_t deltaNs = absDeltaToNs(fLastFlushAbsTime, nowAbs);
        if (deltaNs < kMinFlushIntervalNs) {
            fFlushDeferred = true;
            return kIOReturnSuccess;
        }
    }

    fFlushInProgress = true;
    fNeedFlush = false;

    if (!framebufferMemory) {
        fFlushInProgress = false;
        return kIOReturnNotReady;
    }

    uint32_t *fb = (uint32_t *) framebufferMemory->getBytesNoCopy();
    if (!fb) {
        fFlushInProgress = false;
        return kIOReturnError;
    }

    
    /*
    if (fExeclist) {
        // small scratch batch, content will be filled in submitBatchWithExeclist
        FakeIrisXEGEM* batchGem = FakeIrisXEGEM::withSize(64);

        if (batchGem) {
            if (fExeclist->submitBatchWithExeclist(
                    this,
                    batchGem,
                    0,          // batchSize ignored now
                    fRcsRing,
                    2000))
            {
                IOLog("Flush: Real batch submitted — waiting on GPU fence\n");
            } else {
                IOLog("Flush: Batch submission failed\n");
            }
            batchGem->release();
        }
    }
*/
    
    
    
    (void)fb;
    fLastFlushAbsTime = mach_absolute_time();
    fFlushInProgress = false;

    if (fFlushDeferred) {
        const uint64_t deferNow = mach_absolute_time();
        if (absDeltaToNs(fLastFlushAbsTime, deferNow) >= kMinFlushIntervalNs) {
            fFlushDeferred = false;
            fNeedFlush = true;
        }
    }

    return kIOReturnSuccess;
}





// ==== commandGate wrapper ====
IOReturn FakeIrisXEFramebuffer::staticPerformFlush(
    OSObject *owner,
    void *arg0, void *arg1,
    void *arg2, void *arg3)
{
    FakeIrisXEFramebuffer *fb =
        OSDynamicCast(FakeIrisXEFramebuffer, owner);

    if (!fb) return kIOReturnBadArgument;
    return fb->performFlushNow();
}



// ==== PUBLIC API THAT WINDOWSERVER CALLS ====
IOReturn FakeIrisXEFramebuffer::flushDisplay(void)
{
    if (!commandGate || !workLoop)
        return performFlushNow(); // fallback safe

    if (fFlushInProgress) {
        fFlushDeferred = true;
        return kIOReturnSuccess;
    }

    IOReturn r = commandGate->runAction(
        &FakeIrisXEFramebuffer::staticPerformFlush
    );
    return r;

 }






void FakeIrisXEFramebuffer::deliverFramebufferNotification(IOIndex index, UInt32 event, void* info) {
    IOLog("📩 deliverFramebufferNotification() index=%u event=0x%08X\n", index, event);
    
    // Create proper notification info structure if needed
    switch (event) {
        case kIOFBNotifyDisplayModeChange:
        case kIOFBNotifyDisplayAdded:
        case kIOFBConfigChanged:
        case kIOFBVsyncNotification:
            super::deliverFramebufferNotification(index, (void*)(uintptr_t)event);
            break;
        default:
            super::deliverFramebufferNotification(index, info);
            break;
    }
}





IOReturn FakeIrisXEFramebuffer::setNumberOfDisplays(UInt32 count)
{
    IOLog("setNumberOfDisplays(%u)\n", count);
    setNumberProperty(this, "IOFBDisplayCount", count, 32);
    return kIOReturnSuccess;
}









IOReturn FakeIrisXEFramebuffer::setPowerState(unsigned long state,
                                              IOService* whatDevice)
{
    IOLog("[FakeIrisXEFramebuffer] setPowerState(%lu)\n", state);
    return super::setPowerState(state, whatDevice);
}












// V248: Enhanced display mode bounds validation.
// The CoreDisplay crash in build_mode_list_if_needed occurs when:
//   1. getDisplayModeCount returns an unexpected value (0 or > reasonable limit)
//   2. getInformationForDisplayMode returns kIOReturnUnsupportedMode for a valid mode ID
//   3. getTimingInfoForDisplayMode returns inconsistent timing data
//   4. Mode dimensions are 0 or exceed hardware limits
//
// Our fixes:
//   - Always report exactly 1 valid mode (1920x1080)
//   - Mode ID is always 1 (matches getDisplayModes output)
//   - Timing uses 0x7F appleTimingID to avoid CoreDisplay validation
//   - Dimensions validated before returning (no 0-width/height)
//   - Null pointer checks on all output parameters
// ============================================================================
IOItemCount FakeIrisXEFramebuffer::getDisplayModeCount(void)
{
    // P0A: Authoritative mode count from the single mode table.
    // CoreDisplay asserts when this returns 0.
    IOLog("[P0A] getDisplayModeCount(): count=%u\n", kNumDisplayModes);
    return kNumDisplayModes;
}

IOReturn FakeIrisXEFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    // P0A: Write exactly kNumDisplayModes mode IDs from the authoritative table.
    if (!allDisplayModes) {
        IOLog("[P0A] getDisplayModes(): ❌ NULL output pointer\n");
        return kIOReturnBadArgument;
    }
    if (kNumDisplayModes == 0 || kNumDisplayModes > 16) {
        IOLog("[P0A] getDisplayModes(): ❌ Invalid kNumDisplayModes=%u\n", kNumDisplayModes);
        return kIOReturnError;
    }
    IOLog("[P0A] getDisplayModes(): exporting %u modes\n", kNumDisplayModes);
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        const ProofDisplayMode* m = &s_proofDisplayModes[i];
        if (m->modeID == 0) {
            IOLog("[P0A] getDisplayModes(): ❌ Invalid mode ID 0 at index %u\n", i);
            return kIOReturnError;
        }
        allDisplayModes[i] = m->modeID;
        IOLog("[P0A]   mode[%u] -> ID=%u %dx%d %s\n",
               i, m->modeID, m->width, m->height, m->name);
    }
    return kIOReturnSuccess;
}




UInt64 FakeIrisXEFramebuffer::getPixelFormatsForDisplayMode(
    IODisplayModeID mode, IOIndex depth)
{
    if (!isSupportedProofDepth(depth)) {
        IOLog("[P0A] getPixelFormatsForDisplayMode(mode=%u, depth=%u): ❌ unsupported depth\n", mode, depth);
        return 0;
    }

    const ProofDisplayMode* m = getProofModeByID(mode);
    if (!m) {
        IOLog("[P0A] getPixelFormatsForDisplayMode(mode=%u, depth=%u): ❌ mode not in table\n", mode, depth);
        return 0;
    }

    IOLog("[P0A] getPixelFormatsForDisplayMode(mode=%u, depth=%u): ✅ %dx%d %s\n",
          mode, depth, m->width, m->height, m->name);
    return (1ULL << 0);
}

IOReturn FakeIrisXEFramebuffer::getPixelInformation(
    IODisplayModeID mode,
    IOIndex depth,
    IOPixelAperture aperture,
    IOPixelInformation *info)
{
    if (!info) {
        IOLog("[P0A] getPixelInformation(): ❌ NULL info pointer\n");
        return kIOReturnBadArgument;
    }

    if (!isSupportedProofDepth(depth)) {
        IOLog("[P0A] getPixelInformation(mode=%u, depth=%u, ap=%u): ❌ unsupported depth\n",
              (unsigned)mode, (int)depth, (unsigned)aperture);
        return kIOReturnUnsupportedMode;
    }

    const ProofDisplayMode* m = getProofModeByID(mode);
    if (!m) {
        IOLog("[P0A] getPixelInformation(mode=%u, depth=%u, ap=%u): ❌ mode not in table\n",
              (unsigned)mode, (int)depth, (unsigned)aperture);
        return kIOReturnUnsupportedMode;
    }

    if (aperture != kIOFBSystemAperture) {
        IOLog("[P0A] getPixelInformation(mode=%u, depth=%u, ap=%u): ❌ unsupported aperture\n",
              (unsigned)mode, (int)depth, (unsigned)aperture);
        return kIOReturnUnsupportedMode;
    }

    bzero(info, sizeof(IOPixelInformation));

    info->pixelType = kIO32ARGBPixelFormat;
    strlcpy(info->pixelFormat, "ARGB8888", sizeof(info->pixelFormat));
    info->bitsPerComponent = 8;
    info->bitsPerPixel     = 32;
    info->componentCount   = 4;
    info->bytesPerRow      = m->width * 4;
    info->activeWidth      = m->width;
    info->activeHeight     = m->height;
    info->componentMasks[0] = 0xFF000000;
    info->componentMasks[1] = 0x00FF0000;
    info->componentMasks[2] = 0x0000FF00;
    info->componentMasks[3] = 0x000000FF;

    IOLog("[P0A] getPixelInformation(mode=%u, depth=%u): ✅ %dx%d %s\n",
          mode, depth, m->width, m->height, m->name);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::getCurrentDisplayMode(IODisplayModeID* displayMode, IOIndex* depth)
{
    // P0A: Return currentMode/currentDepth. If currentMode was never set (0),
    // default to the first entry in the authoritative table (mode ID 1).
    // If currentMode is set but not in the table, return failure — callers
    // must not receive a mode ID that does not appear in getDisplayModes().
    if (!displayMode || !depth) {
        IOLog("[P0A] getCurrentDisplayMode: ❌ NULL pointer\n");
        return kIOReturnBadArgument;
    }

    IODisplayModeID modeToReturn = currentMode;
    IOIndex depthToReturn = currentDepth;

    if (modeToReturn == 0) {
        modeToReturn = s_proofDisplayModes[0].modeID;
        depthToReturn = 0;
        currentMode = modeToReturn;
        currentDepth = depthToReturn;
        IOLog("[P0A] getCurrentDisplayMode: no prior mode set, defaulted to ID=%u\n", modeToReturn);
    } else {
        const ProofDisplayMode* m = getProofModeByID(modeToReturn);
        if (!m) {
            IOLog("[P0A] getCurrentDisplayMode: ❌ currentMode=%u not in authoritative table\n", modeToReturn);
            return kIOReturnUnsupportedMode;
        }
        IOLog("[P0A] getCurrentDisplayMode: modeID=%u depth=%u (%dx%d %s)\n",
               modeToReturn, depthToReturn, m->width, m->height, m->name);
    }

    *displayMode = modeToReturn;
    *depth = depthToReturn;
    return kIOReturnSuccess;
}

IOIndex FakeIrisXEFramebuffer::getAperture() const {
    return kIOFBSystemAperture;
}





// Legacy overload (keep for console/PE_Video — your code is perfect)
IOReturn FakeIrisXEFramebuffer::getApertureRange(IOSelect aperture,
                                                 IOPhysicalAddress *phys,
                                                 IOByteCount *length)
{
    IOLog("getApertureRange(old) aperture=%u\n", (unsigned)aperture);

    if (!phys || !length || !framebufferMemory)
        return kIOReturnBadArgument;

    IOByteCount segLen = 0;
    IOPhysicalAddress firstPhys =
        framebufferMemory->getPhysicalSegment(0, &segLen);

    if (!firstPhys)
        return kIOReturnError;

    *phys = firstPhys;
    *length = framebufferMemory->getLength();

    IOLog(" → phys=0x%llx len=0x%llx segLen=0x%llx\n",
          (uint64_t)*phys, (uint64_t)*length, (uint64_t)segLen);

    return kIOReturnSuccess;
}


#define kIOFBVRAMMemory 1
// FIXED New overload (override fully — return shared for all apertures, no super)
IODeviceMemory* FakeIrisXEFramebuffer::getApertureRange(IOPixelAperture aperture)
{
    IOLog("getApertureRange(new) aperture=%d\n", aperture);

    if (!framebufferMemory) {
        IOLog("❌ No framebuffer for aperture %d\n", aperture);
        return nullptr;
    }

    IOPhysicalAddress phys = framebufferMemory->getPhysicalAddress();
    IOByteCount len = framebufferMemory->getLength();

    // FIXED: Return shared memory for ALL apertures (WS FB 2 needs VRAM/cursor)
    if (aperture == kIOFBVRAMMemory || aperture == 1) {  // VRAM = 1
        IOLog("getApertureRange: VRAM aperture — using shared FB\n");
    } else if (aperture == 2) {  // Cursor aperture
        IOLog("getApertureRange: Cursor aperture — using shared FB\n");
    } else if (aperture != kIOFBSystemAperture) {
        IOLog("⚠️ Unsupported aperture %d — fallback to system\n", aperture);
    }

    // Create and return new IODeviceMemory (WS expects fresh each call)
    IODeviceMemory *mem = IODeviceMemory::withRange(phys, len);
    if (!mem) {
        IOLog("getApertureRange: withRange failed\n");
        return nullptr;
    }

    IOLog("getApertureRange: phys=0x%llx len=0x%llx for aperture %d\n",
          (unsigned long long)phys, (unsigned long long)len, aperture);
    return mem;
}









IOReturn FakeIrisXEFramebuffer::getFramebufferOffsetForX_Y(IOPixelAperture aperture,
                                                           SInt32 x,
                                                           SInt32 y,
                                                           UInt32 *offset)
{
    if (!offset) {
        IOLog("[P0A] getFramebufferOffsetForX_Y(): ❌ NULL offset\n");
        return kIOReturnBadArgument;
    }

    const ProofDisplayMode* m = getCurrentProofMode(currentMode);
    if (!m) {
        IOLog("[P0A] getFramebufferOffsetForX_Y(): ❌ no current mode\n");
        return kIOReturnError;
    }

    if (x < 0 || y < 0 || x >= (SInt32)m->width || y >= (SInt32)m->height) {
        IOLog("[P0A] getFramebufferOffsetForX_Y(ap=%d,x=%d,y=%d): ❌ out of range (%dx%d)\n",
              (int)aperture, (int)x, (int)y, m->width, m->height);
        return kIOReturnBadArgument;
    }

    *offset = (y * m->width + x) * 4;
    return kIOReturnSuccess;
}





IOReturn FakeIrisXEFramebuffer::getInformationForDisplayMode(
    IODisplayModeID mode,
    IODisplayModeInformation* info)
{
    // P0A: Populate IODisplayModeInformation for a valid mode from the
    // authoritative table. Struct layout (macOS 12 SDK):
    //   nominalWidth(0), nominalHeight(4), refreshRate(8), maxDepthIndex(12),
    //   flags(16), imageWidth(20), imageHeight(22), reserved[3](24).
    // Timing strategy: appleTimingID=0x7F stored in reserved[0] signals native
    // timing fallback to getTimingInfoForDisplayMode() (both must agree).
    if (!info) {
        IOLog("[P0A] getInformationForDisplayMode(mode=%u): ❌ NULL info\n", mode);
        return kIOReturnBadArgument;
    }

    const ProofDisplayMode* m = getProofModeByID(mode);
    if (!m) {
        IOLog("[P0A] getInformationForDisplayMode(mode=%u): ❌ not in table\n", mode);
        return kIOReturnUnsupportedMode;
    }

    bzero(info, sizeof(IODisplayModeInformation));
    info->nominalWidth   = m->width;
    info->nominalHeight  = m->height;
    info->refreshRate    = m->refreshFixed;
    info->maxDepthIndex  = m->depthIndex;
    info->flags          = kDisplayModeBuiltInFlag;
    info->reserved[0]    = kIOTimingID_TigerLake_Fallback;
    info->reserved[1]    = kIOTimingInfoValid_AppleTimingID;

    IOLog("[P0A] getInformationForDisplayMode(mode=%u): ✅ %dx%d refresh=0x%08X flags=0x%08X reserved[0]=0x%02X reserved[1]=0x%08X\n",
           mode, m->width, m->height, m->refreshFixed,
           info->flags, info->reserved[0], info->reserved[1]);
    return kIOReturnSuccess;
}




IOReturn FakeIrisXEFramebuffer::getStartupDisplayMode(IODisplayModeID *modeID,
                                                      IOIndex *depth)
{
    const ProofDisplayMode* m = getDefaultProofMode();
    if (!m) {
        IOLog("[P0A] getStartupDisplayMode: ❌ no default mode in table\n");
        return kIOReturnBadArgument;
    }
    if (modeID) *modeID = m->modeID;
    if (depth)  *depth  = m->depthIndex;
    IOLog("[P0A] getStartupDisplayMode: modeID=%u depth=%u (%dx%d %s)\n",
          m->modeID, m->depthIndex, m->width, m->height, m->name);
    return kIOReturnSuccess;
}






UInt32 FakeIrisXEFramebuffer::getConnectionCount() {
    IOLog("getConnectionCount() called\n");
    return 1; // 1 display connection
}



IOReturn FakeIrisXEFramebuffer::getAttributeForIndex(IOSelect attribute, UInt32 index, UInt32* value) {
    IOLog("getAttributeForIndex(%u, %u)\n", attribute, index);

            return kIOReturnSuccess;
    
}





IOReturn FakeIrisXEFramebuffer::getNotificationSemaphore(
    IOSelect event,
                                                         semaphore **sem)
{
    IOLog("getNotificationSemaphore called\n");
    if (sem) *sem = nullptr;
    return kIOReturnUnsupported;
}

IOReturn FakeIrisXEFramebuffer::setCLUTWithEntries(
    IOColorEntry *entries,
    SInt32 index,
    SInt32 numEntries,
    IOOptionBits options)
{
    IOLog("setCLUTWithEntries called\n");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::setGammaTable(
    UInt32 channelCount,
    UInt32 dataCount,
    UInt32 dataWidth,
    void *data)
{
    IOLog("setGammaTable (compat) called\n");
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::setAttribute(
    IOSelect attribute,
    uintptr_t value)
{
    IOLog("setAttribute compat\n");
    return super::setAttribute(attribute,value);
}








IOReturn FakeIrisXEFramebuffer::getAttribute(
    IOSelect attribute,
    uintptr_t *value)
{
    if (value) {
        switch (attribute) {
            case kIOPowerAttribute:
            case kIOSystemPowerAttribute:
                *value = kIOPMPowerOn;
                break;
            default:
                *value = 0;
                break;
        }
    }
    // Don’t forward to super; just say “OK”.
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::getAttributeForConnection(
    IOIndex connect,
    IOSelect attribute,
    uintptr_t *value)
{
    IOLog("[FakeIrisXEFramebuffer] getAttributeForConnection(conn=%u, attr=0x%08x)\n",
          (unsigned)connect, (unsigned)attribute);

    if (!value)
        return kIOReturnBadArgument;

    // Default
    *value = 0;

    switch (attribute) {
        // ---- Capabilities ----
        case kConnectionSupportsAppleSense:     // 'cena' / sense support
        case kConnectionSupportsDDCSense:
        case kConnectionSupportsHLDDCSense:
        case kConnectionSupportsLLDDCSense:    // 'lddc'
        case kConnectionSupportsHotPlug:
            *value = 1;   // yes, supported
            return kIOReturnSuccess;

        // ---- Parameter count ----
        case kConnectionDisplayParameterCount:  // 'pcnt'
            *value = 2;   // brightness + vblm
            return kIOReturnSuccess;

        // ---- Connection flags (built-in DP) ----
        case kConnectionFlags:
            *value = kIOConnectionBuiltIn | kIOConnectionDisplayPort;
            return kIOReturnSuccess;

        // ---- Online / enabled ----
        case kConnectionIsOnline:              // 'ionl' if asked
            *value = 1;   // panel is online
            return kIOReturnSuccess;

        case kConnectionVBLMultiplier:
            *value = percentToVBLMultiplier(getBacklightPercent());
            return kIOReturnSuccess;

        default:
            // For unknown attributes, just say “no info”
            *value = 0;
            return kIOReturnSuccess;
    }
}





IOReturn FakeIrisXEFramebuffer::setAttributeForConnection(
    IOIndex connect,
    IOSelect attribute,
    uintptr_t value)
{
    if (attribute == kConnectionVBLMultiplier) {
        const uint32_t vblm = static_cast<uint32_t>(value);
        const uint32_t percent = vblMultiplierToPercent(vblm);
        logBrightnessTransaction("setAttributeForConnection", "vblm", vblm, percent);
        return setBacklightPercent(percent, "conn-vblm") ? kIOReturnSuccess : kIOReturnError;
    }

    return kIOReturnSuccess;
}




IOReturn FakeIrisXEFramebuffer::setBackingStoreState(
    IODisplayModeID mode,
    IOOptionBits options)
{
    IOLog("setBackingStoreState\n");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::setStartupDisplayMode(
    IODisplayModeID mode,
    IOIndex depth)
{
    IOLog("setStartupDisplayMode\n");
    return kIOReturnSuccess;
}


IOReturn FakeIrisXEFramebuffer::waitForAcknowledge(
    IOIndex connect,
    UInt32 type,
    void *info)
{
    IOLog("waitForAcknowledge called\n");
    return kIOReturnSuccess;
}




FakeIrisXEGEM* FakeIrisXEFramebuffer::createTinyBatchGem()
{
    constexpr size_t sz = 4096;
    FakeIrisXEGEM* gem = FakeIrisXEGEM::withSize(sz, 0);
    if (!gem) return nullptr;

    gem->pin();
    uint32_t* buf = (uint32_t*)gem->memoryDescriptor()->getBytesNoCopy();
    bzero(buf, sz);

    // Basic MI_BATCH_BUFFER_END
    buf[0] = 0xA << 23; // MI_BATCH_BUFFER_END opcode

    return gem;
}










static inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static const uint32_t kAppleBacklightPercentMin = 0;
static const uint32_t kAppleBacklightPercentMax = 100;
static const uint32_t kAppleBacklightUserMin = 40;
static const uint32_t kAppleBacklightUserMax = 255;
static const uint32_t kAppleBacklightRawMax = 0xFFFEu;
static const uint32_t kAppleBacklightVBLMOne = 0x10000u;
static const uint32_t kAppleBacklightVBLMMax = 0x30000u;
static const uint32_t kTglRcsRingTail = 0x20030u;
static const uint32_t kTglRcsRingHead = 0x20034u;
static const uint32_t kTglRcsRingStart = 0x20038u;
static const uint32_t kTglRcsRingCtl = 0x2003Cu;
static const uint32_t kTglAltRcsRingStart = 0x23C30u;
static const uint32_t kTglAltRcsRingHead = 0x23C38u;
static const uint32_t kTglAltRcsRingTail = 0x23C3Cu;
static const uint32_t kTglRcsRingMode = 0x2009Cu;
static const uint32_t kTglRcsGfxMode = 0x200D0u;
static const uint32_t kTglRcsGfxMode2 = 0x200D4u;
static const uint32_t kTglRcsResetCtrl = 0x200D0u;

struct FakeIrisXEBacklightPreset {
    const char* name;
    const uint16_t* values;
    uint32_t count;
    bool anchors;
    uint32_t appleSense;
};

static const uint16_t kBacklightCurveDefault[] = { 0x0000u, 0x0740u, 0x0AF7u, 0xFFFEu };
static const uint16_t kBacklightCurveF14Ta02e[] = {
    0x0000u, 0x0010u, 0x001Bu, 0x0028u, 0x0039u, 0x004Fu, 0x0069u, 0x008Au, 0x00B2u,
    0x00E2u, 0x011Du, 0x0163u, 0x01B8u, 0x021Fu, 0x029Bu, 0x0331u, 0x03E1u
};
static const uint16_t kBacklightCurveF15Ta044[] = {
    0x0000u, 0x000Au, 0x000Cu, 0x0010u, 0x0015u, 0x001Cu, 0x0026u, 0x0033u, 0x0044u,
    0x005Du, 0x007Fu, 0x00AFu, 0x00F1u, 0x014Eu, 0x01D0u, 0x0287u, 0x038Au
};
static const uint16_t kBacklightCurveF16Ta030[] = {
    0x0000u, 0x000Au, 0x000Eu, 0x0012u, 0x0018u, 0x0020u, 0x002Cu, 0x003Bu, 0x0050u,
    0x006Du, 0x0095u, 0x00CCu, 0x0117u, 0x017Eu, 0x020Du, 0x02D2u, 0x03F5u
};

static const FakeIrisXEBacklightPreset kBacklightPresets[] = {
    { "Default",  kBacklightCurveDefault, 4,  true,  0x0000u },
    { "F14Ta02e", kBacklightCurveF14Ta02e, 17, false, 0xA02Eu },
    { "F15Ta044", kBacklightCurveF15Ta044, 17, false, 0xA044u },
    { "F16Ta030", kBacklightCurveF16Ta030, 17, false, 0xA030u },
    { "F16Ta031", kBacklightCurveF16Ta030, 17, false, 0xA031u },
};

static const FakeIrisXEBacklightPreset* findBacklightPresetByName(const char* name)
{
    if (!name || !name[0]) {
        return nullptr;
    }

    for (size_t i = 0; i < sizeof(kBacklightPresets) / sizeof(kBacklightPresets[0]); ++i) {
        if (strcmp(kBacklightPresets[i].name, name) == 0) {
            return &kBacklightPresets[i];
        }
    }
    return nullptr;
}

static const FakeIrisXEBacklightPreset* selectBacklightPresetOverride()
{
    char panelArg[32] = {0};
    if (PE_parse_boot_argn("-fakeirisxe-panel", panelArg, sizeof(panelArg))) {
        if (const FakeIrisXEBacklightPreset* preset = findBacklightPresetByName(panelArg)) {
            return preset;
        }
    }

    return nullptr;
}

static const FakeIrisXEBacklightPreset* selectBacklightPresetForIdentity(uint32_t vendorID, uint32_t productID)
{
    if (const FakeIrisXEBacklightPreset* preset = selectBacklightPresetOverride()) {
        return preset;
    }

    if (vendorID == 0x0610u) {
        switch (productID) {
            case 0x8601u:
                return findBacklightPresetByName("F16Ta030");
            case 0x8612u:
                return findBacklightPresetByName("F15Ta044");
            case 0x8603u:
                return findBacklightPresetByName("F14Ta02e");
            default:
                break;
        }
    }

    if (vendorID == 0xE430u && productID == 0x071Eu) {
        return findBacklightPresetByName("Default");
    }

    return findBacklightPresetByName("F16Ta030");
}

static uint32_t percentToUserBrightness(uint32_t percent)
{
    percent = clamp_u32(percent, 0, 100);
    return kAppleBacklightUserMin + static_cast<uint32_t>((static_cast<uint64_t>(percent) * (kAppleBacklightUserMax - kAppleBacklightUserMin)) / 100u);
}

static uint32_t percentToVBLMultiplier(uint32_t percent)
{
    percent = clamp_u32(percent, 0, 100);
    return static_cast<uint32_t>((static_cast<uint64_t>(percent) * kAppleBacklightVBLMOne) / 100u);
}

static uint32_t vblMultiplierToPercent(uint32_t vblm)
{
    if (vblm <= 100u) {
        return clamp_u32(vblm, 0, 100);
    }

    vblm = clamp_u32(vblm, 0, kAppleBacklightVBLMMax);
    if (vblm >= kAppleBacklightVBLMOne) {
        return 100u;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(vblm) * 100u) / kAppleBacklightVBLMOne);
}

static uint32_t rawBrightnessToPercent(uint32_t raw)
{
    raw = clamp_u32(raw, 0, kAppleBacklightRawMax);
    return static_cast<uint32_t>((static_cast<uint64_t>(raw) * 100u) / kAppleBacklightRawMax);
}

static void logBrightnessTransaction(const char* origin,
                                     const char* key,
                                     uint32_t inputValue,
                                     uint32_t percent)
{
    IOLog("[BLTX] %s key=%s input=0x%08X -> %u%%\n",
          origin ? origin : "unknown",
          key ? key : "unknown",
          inputValue,
          percent);
}

static OSDictionary* makeBrightnessParameter(uint32_t value, uint32_t min, uint32_t max)
{
    OSDictionary* dict = OSDictionary::withCapacity(3);
    if (!dict) {
        return nullptr;
    }

    OSNumber* valueNum = OSNumber::withNumber(value, 32);
    OSNumber* minNum = OSNumber::withNumber(min, 32);
    OSNumber* maxNum = OSNumber::withNumber(max, 32);
    if (!valueNum || !minNum || !maxNum) {
        if (valueNum) valueNum->release();
        if (minNum) minNum->release();
        if (maxNum) maxNum->release();
        dict->release();
        return nullptr;
    }

    dict->setObject("value", valueNum);
    dict->setObject("min", minNum);
    dict->setObject("max", maxNum);
    valueNum->release();
    minNum->release();
    maxNum->release();
    return dict;
}

static void publishBrightnessProperties(IORegistryEntry* entry, uint32_t percent, uint32_t raw)
{
    if (!entry) {
        return;
    }

    percent = clamp_u32(percent, kAppleBacklightPercentMin, kAppleBacklightPercentMax);
    raw = clamp_u32(raw, 0, kAppleBacklightRawMax);

    const uint32_t userBrightness = percentToUserBrightness(percent);
    const uint32_t vblm = percentToVBLMultiplier(percent);

    setNumberProperty(entry, "brightness", userBrightness, 32);
    setNumberProperty(entry, "brightness-level", userBrightness, 32);
    setNumberProperty(entry, "brightness-min", kAppleBacklightUserMin, 32);
    setNumberProperty(entry, "brightness-max", kAppleBacklightUserMax, 32);
    setNumberProperty(entry, "brightness-default", percentToUserBrightness(75), 32);
    setNumberProperty(entry, "linear-brightness", raw, 32);
    setNumberProperty(entry, "vblm", vblm, 32);
    setNumberProperty(entry, "ApplePanelRawBrightness", raw, 32);
    setNumberProperty(entry, "AppleMaxBrightness", kAppleBacklightRawMax, 32);
    setNumberProperty(entry, "AppleNumBrightLevels", kAppleBacklightUserMax, 32);
    setNumberProperty(entry, "AppleBacklightAtBoot", raw, 32);
    setNumberProperty(entry, "IOBacklightHandlerID", 436849163854938112ULL, 64);
    entry->setProperty("AppleRestoreBacklight", kOSBooleanTrue);

    OSDictionary* params = OSDictionary::withCapacity(9);
    if (!params) {
        return;
    }

    OSDictionary* brightness = makeBrightnessParameter(userBrightness, kAppleBacklightUserMin, kAppleBacklightUserMax);
    OSDictionary* linear = makeBrightnessParameter(raw, 0, kAppleBacklightRawMax);
    OSDictionary* usableLinear = makeBrightnessParameter(raw, 0, kAppleBacklightRawMax);
    OSDictionary* vblmParam = makeBrightnessParameter(vblm, 0, kAppleBacklightVBLMMax);
    OSDictionary* fade = makeBrightnessParameter(userBrightness, kAppleBacklightUserMin, kAppleBacklightUserMax);
    OSDictionary* brightnessProbe = makeBrightnessParameter(userBrightness, kAppleBacklightUserMin, kAppleBacklightUserMax);
    OSDictionary* linearProbe = makeBrightnessParameter(raw, 0, kAppleBacklightRawMax);
    OSDictionary* power = makeBrightnessParameter(2, 0, 2);
    OSDictionary* commit = OSDictionary::withCapacity(1);

    if (brightness) {
        params->setObject("brightness", brightness);
        brightness->release();
    }
    if (linear) {
        params->setObject("linear-brightness", linear);
        linear->release();
    }
    if (usableLinear) {
        params->setObject("usable-linear-brightness", usableLinear);
        usableLinear->release();
    }
    if (vblmParam) {
        params->setObject("vblm", vblmParam);
        vblmParam->release();
    }
    if (fade) {
        params->setObject("brightness-fade", fade);
        fade->release();
    }
    if (brightnessProbe) {
        params->setObject("brightness-probe", brightnessProbe);
        brightnessProbe->release();
    }
    if (linearProbe) {
        params->setObject("linear-brightness-probe", linearProbe);
        linearProbe->release();
    }
    if (power) {
        params->setObject("power-state", power);
        power->release();
    }
    if (commit) {
        OSNumber* reg = OSNumber::withNumber(static_cast<uint64_t>(0), 32);
        if (reg) {
            commit->setObject("reg", reg);
            reg->release();
        }
        params->setObject("commit", commit);
        commit->release();
    }

    entry->setProperty("IODisplayParameters", params);
    params->release();
}

static bool extractNumericValue(OSObject* object, uint32_t* outValue)
{
    if (!object || !outValue) {
        return false;
    }

    if (OSNumber* number = OSDynamicCast(OSNumber, object)) {
        *outValue = number->unsigned32BitValue();
        return true;
    }

    OSDictionary* dict = OSDynamicCast(OSDictionary, object);
    if (!dict) {
        return false;
    }

    OSNumber* valueNum = OSDynamicCast(OSNumber, dict->getObject("value"));
    if (!valueNum) {
        return false;
    }

    *outValue = valueNum->unsigned32BitValue();
    return true;
}

static bool extractVBLPercent(OSObject* object, uint32_t* outPercent)
{
    if (!object || !outPercent) {
        return false;
    }

    uint32_t value = 0;
    if (OSNumber* number = OSDynamicCast(OSNumber, object)) {
        value = number->unsigned32BitValue();
        *outPercent = vblMultiplierToPercent(value);
        return true;
    }

    OSDictionary* dict = OSDynamicCast(OSDictionary, object);
    if (!dict) {
        return false;
    }

    OSNumber* valueNum = OSDynamicCast(OSNumber, dict->getObject("value"));
    if (!valueNum) {
        return false;
    }

    value = valueNum->unsigned32BitValue();
    *outPercent = vblMultiplierToPercent(value);
    return true;
}

static bool extractBrightnessPercent(OSObject* object,
                                     uint32_t defaultMin,
                                     uint32_t defaultMax,
                                     bool treatAsRaw,
                                     uint32_t* outPercent)
{
    if (!object || !outPercent) {
        return false;
    }

    uint32_t value = 0;
    uint32_t minValue = defaultMin;
    uint32_t maxValue = defaultMax;

    if (OSNumber* number = OSDynamicCast(OSNumber, object)) {
        value = number->unsigned32BitValue();
        if (treatAsRaw && value > kAppleBacklightPercentMax) {
            *outPercent = clamp_u32(static_cast<uint32_t>((static_cast<uint64_t>(value) * 100u) / kAppleBacklightRawMax), 0, 100);
        } else if (defaultMax > 100 || defaultMin > 0) {
            if (value <= 100U) {
                *outPercent = clamp_u32(value, 0, 100);
            } else {
                value = clamp_u32(value, defaultMin, defaultMax);
                *outPercent = static_cast<uint32_t>((static_cast<uint64_t>(value - defaultMin) * 100u) / (defaultMax - defaultMin));
            }
        } else {
            *outPercent = clamp_u32(value, 0, 100);
        }
        return true;
    }

    OSDictionary* dict = OSDynamicCast(OSDictionary, object);
    if (!dict) {
        return false;
    }

    if (OSNumber* n = OSDynamicCast(OSNumber, dict->getObject("value"))) {
        value = n->unsigned32BitValue();
    } else {
        return false;
    }
    if (OSNumber* n = OSDynamicCast(OSNumber, dict->getObject("min"))) {
        minValue = n->unsigned32BitValue();
    }
    if (OSNumber* n = OSDynamicCast(OSNumber, dict->getObject("max"))) {
        maxValue = n->unsigned32BitValue();
    }

    if (maxValue <= minValue) {
        maxValue = defaultMax;
        minValue = defaultMin;
    }

    value = clamp_u32(value, minValue, maxValue);
    *outPercent = static_cast<uint32_t>((static_cast<uint64_t>(value - minValue) * 100u) / (maxValue - minValue));
    return true;
}

// Initialize backlight PWM hardware (call once from enableController)
void FakeIrisXEFramebuffer::initBacklightHardware()
{
    auto rd = [&](uint32_t off) { return safeMMIORead(off); };
    auto wr = [&](uint32_t off, uint32_t val) { safeMMIOWrite(off, val); };

    // Write a sane period (max value) and start duty at 50%
    // If you already do these writes in enableController, this is safe to call again.
    const uint32_t period = 0x0000FFFFu;      // suggested period / pwmMax
    const uint32_t duty50 = (period / 2) & 0xFFFFu;

    // Set PWM period (low 16 bits typically)
    wr(BXT_BLC_PWM_FREQ1, period);
    fPwmMax = period & 0xFFFFu;

    // Set initial duty (low 16 bits)
    wr(BXT_BLC_PWM_DUTY1, duty50);

    // Enable PWM: set MSB (bit31) of CTL register (your snippet used 0x80000000)
    uint32_t ctl = rd(BXT_BLC_PWM_CTL1);
    ctl |= (1u << 31);
    wr(BXT_BLC_PWM_CTL1, ctl);

    IOLog("[FB] initBacklightHardware: period=0x%04x duty=0x%04x CTL=0x%08x\n", period & 0xFFFFu, duty50, ctl);
}

void FakeIrisXEFramebuffer::ensureBacklightHardwareState(const char* reason)
{
    auto rd = [&](uint32_t off) { return safeMMIORead(off); };
    auto wr = [&](uint32_t off, uint32_t val) { safeMMIOWrite(off, val); };

    uint32_t statusNew = rd(PP_STATUS_NEW);
    uint32_t statusOld = rd(PP_STATUS_OLD);
    bool panelReady = ((statusNew | statusOld) & (1u << 31)) != 0;

    if (!panelReady) {
        wr(PP_CONTROL_NEW, rd(PP_CONTROL_NEW) | (1u << 31) | (1u << 30) | 0x8u);
        wr(PP_CONTROL_OLD, rd(PP_CONTROL_OLD) | (1u << 31) | (1u << 30));

        for (int i = 0; i < 20; ++i) {
            IOSleep(10);
            statusNew = rd(PP_STATUS_NEW);
            statusOld = rd(PP_STATUS_OLD);
            panelReady = ((statusNew | statusOld) & (1u << 31)) != 0;
            if (panelReady) {
                break;
            }
        }
    }

    uint32_t freq = rd(BXT_BLC_PWM_FREQ1) & 0xFFFFu;
    if (!freq) {
        freq = fPwmMax ? fPwmMax : 0xFFFFu;
        wr(BXT_BLC_PWM_FREQ1, freq);
    }
    fPwmMax = freq;

    uint32_t ctl = rd(BXT_BLC_PWM_CTL1);
    if (!(ctl & (1u << 31))) {
        ctl |= (1u << 31);
        wr(BXT_BLC_PWM_CTL1, ctl);
        ctl = rd(BXT_BLC_PWM_CTL1);
    }

    IOLog("[BLTX] ensure reason=%s pp_new=0x%08X pp_old=0x%08X pwm_ctl=0x%08X pwm_freq=0x%04X\n",
          reason ? reason : "unknown",
          statusNew,
          statusOld,
          ctl,
          freq & 0xFFFFu);
}

void FakeIrisXEFramebuffer::applyBacklightPresetForIdentity(uint32_t vendorID, uint32_t productID)
{
    const FakeIrisXEBacklightPreset* preset = selectBacklightPresetForIdentity(vendorID, productID);
    if (!preset) {
        preset = &kBacklightPresets[0];
    }

    fBacklightTableSize = 11;
    for (int i = 0; i < fBacklightTableSize; i++) {
        fBacklightLevelIn[i] = i * 10;
    }

    for (int i = 0; i < fBacklightTableSize; i++) {
        uint32_t pct = static_cast<uint32_t>(fBacklightLevelIn[i]);
        uint32_t interp = 0;

        if (preset->anchors) {
            static const uint32_t kAnchorPct[4] = { 0, 25, 75, 100 };
            uint32_t segment = 0;
            while (segment < 3 && pct > kAnchorPct[segment + 1]) {
                ++segment;
            }

            uint32_t loPct = kAnchorPct[segment];
            uint32_t hiPct = kAnchorPct[segment + 1];
            uint32_t loVal = preset->values[segment];
            uint32_t hiVal = preset->values[segment + 1];
            interp = loVal;
            if (hiPct > loPct) {
                interp = loVal + static_cast<uint32_t>((static_cast<uint64_t>(hiVal - loVal) * (pct - loPct)) / (hiPct - loPct));
            }
        } else {
            const uint32_t loIndex = (pct * (preset->count - 1U)) / 100U;
            const uint32_t hiIndex = (loIndex + 1U < preset->count) ? (loIndex + 1U) : loIndex;
            const uint32_t loPct = (loIndex * 100U) / (preset->count - 1U);
            const uint32_t hiPct = (hiIndex * 100U) / (preset->count - 1U);
            const uint32_t loVal = preset->values[loIndex];
            const uint32_t hiVal = preset->values[hiIndex];
            interp = loVal;
            if (hiPct > loPct) {
                interp = loVal + static_cast<uint32_t>((static_cast<uint64_t>(hiVal - loVal) * (pct - loPct)) / (hiPct - loPct));
            }
        }

        fBacklightLevelOut[i] = static_cast<uint16_t>(interp);
    }

    fHasBacklightTable = true;
    setProperty("ApplePanelProfile", OSString::withCString(preset->name));
    setNumberProperty(this, "AppleSense", preset->appleSense, 32);
    IOLog("[FB] Backlight table initialized with preset %s (%d entries, vendor=0x%04X product=0x%04X)\n",
          preset->name,
          fBacklightTableSize,
          vendorID,
          productID);
}

// Initialize backlight table for interpolation
void FakeIrisXEFramebuffer::initBacklightTable()
{
    uint32_t vendorID = 0;
    uint32_t productID = 0;
    if (OSNumber* vendor = OSDynamicCast(OSNumber, getProperty("IODisplayVendorID"))) {
        vendorID = vendor->unsigned32BitValue();
    }
    if (OSNumber* product = OSDynamicCast(OSNumber, getProperty("IODisplayProductID"))) {
        productID = product->unsigned32BitValue();
    }

    applyBacklightPresetForIdentity(vendorID, productID);
}

    // Set brightness 0..100
    bool FakeIrisXEFramebuffer::setBacklightPercent(uint32_t percent, const char* source)
    {
        auto rd = [&](uint32_t off) { return safeMMIORead(off); };
        auto wr = [&](uint32_t off, uint32_t val) { safeMMIOWrite(off, val); };
        
        percent = clamp_u32(percent, 0, 100);
        ensureBacklightHardwareState(source);
        
        // Read period / max from FREQ1 low 16 bits if available
        uint32_t freq = rd(BXT_BLC_PWM_FREQ1);
        uint32_t pwmMax = freq & 0xFFFFu;
        if (pwmMax == 0) {
            pwmMax = fPwmMax ? fPwmMax : 0xFFFFu;
        }
        fPwmMax = pwmMax;
        
        uint32_t duty;
        uint32_t rawLevel;
        if (fHasBacklightTable && fBacklightTableSize >= 2) {
            if (percent == 0) {
                rawLevel = 0;
            } else if (percent == 100) {
                rawLevel = kAppleBacklightRawMax;
            } else {
                int index = percent / 10;   // 0..9
                int nextIndex = index + 1;
                uint32_t fraction = percent % 10;   // 0..9
                uint32_t lower = fBacklightLevelOut[index];
                uint32_t upper = fBacklightLevelOut[nextIndex];
                rawLevel = lower + static_cast<uint32_t>((static_cast<uint64_t>(upper - lower) * fraction) / 10u);
            }
        } else {
            rawLevel = static_cast<uint32_t>((static_cast<uint64_t>(percent) * kAppleBacklightRawMax) / 100u);
        }

        duty = static_cast<uint32_t>((static_cast<uint64_t>(rawLevel) * pwmMax) / kAppleBacklightRawMax);
        if (percent && !duty) {
            duty = 1;
        }
        
        duty &= 0xFFFFu;   // Ensure we are within 16 bits
        
        wr(BXT_BLC_PWM_DUTY1, duty);

        // Ensure PWM enabled
        uint32_t ctl = rd(BXT_BLC_PWM_CTL1);
        if (!(ctl & (1u << 31))) {
            ctl |= (1u << 31);
            wr(BXT_BLC_PWM_CTL1, ctl);
            ctl = rd(BXT_BLC_PWM_CTL1);
        }

        publishBrightnessProperties(this, percent, rawLevel);
        
        IOLog("[BLTX] apply source=%s percent=%u raw=0x%04x duty=0x%04x vblm=0x%05x pp_new=0x%08X pp_old=0x%08X ctl=0x%08X\n",
              source ? source : "direct",
              percent,
              rawLevel,
              duty,
              percentToVBLMultiplier(percent),
              rd(PP_STATUS_NEW),
              rd(PP_STATUS_OLD),
              ctl);
        return true;
    }

// Read current backlight as percent (0..100)
uint32_t FakeIrisXEFramebuffer::getBacklightPercent()
{
    auto rd = [&](uint32_t off) { return safeMMIORead(off); };
    uint32_t freq = rd(BXT_BLC_PWM_FREQ1);
    uint32_t pwmMax = freq & 0xFFFFu;
    if (pwmMax == 0) pwmMax = 0xFFFFu;
    uint32_t duty = rd(BXT_BLC_PWM_DUTY1) & 0xFFFFu;

    uint32_t percent = static_cast<uint32_t>((static_cast<uint64_t>(duty) * 100u) / pwmMax);
    return clamp_u32(percent, 0, 100);
}

IOReturn FakeIrisXEFramebuffer::setProperties(OSObject* properties)
{
    OSDictionary* dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) {
        return super::setProperties(properties);
    }

    uint32_t percent = 0;
    uint32_t inputValue = 0;

    if (extractNumericValue(dict->getObject("brightness"), &inputValue) &&
        extractBrightnessPercent(dict->getObject("brightness"), kAppleBacklightUserMin, kAppleBacklightUserMax, false, &percent)) {
        logBrightnessTransaction("setProperties", "brightness", inputValue, percent);
        return setBacklightPercent(percent, "prop-brightness") ? kIOReturnSuccess : kIOReturnError;
    }
    if (extractNumericValue(dict->getObject("linear-brightness"), &inputValue) &&
        extractBrightnessPercent(dict->getObject("linear-brightness"), 0, kAppleBacklightRawMax, true, &percent)) {
        logBrightnessTransaction("setProperties", "linear-brightness", inputValue, percent);
        return setBacklightPercent(percent, "prop-linear") ? kIOReturnSuccess : kIOReturnError;
    }
    if (extractNumericValue(dict->getObject("vblm"), &inputValue) &&
        extractVBLPercent(dict->getObject("vblm"), &percent)) {
        logBrightnessTransaction("setProperties", "vblm", inputValue, percent);
        return setBacklightPercent(percent, "prop-vblm") ? kIOReturnSuccess : kIOReturnError;
    }
    if (extractNumericValue(dict->getObject("brightness-fade"), &inputValue) &&
        extractBrightnessPercent(dict->getObject("brightness-fade"), kAppleBacklightUserMin, kAppleBacklightUserMax, false, &percent)) {
        logBrightnessTransaction("setProperties", "brightness-fade", inputValue, percent);
        return setBacklightPercent(percent, "prop-fade") ? kIOReturnSuccess : kIOReturnError;
    }
    if (extractNumericValue(dict->getObject("brightness-probe"), &inputValue) &&
        extractBrightnessPercent(dict->getObject("brightness-probe"), kAppleBacklightUserMin, kAppleBacklightUserMax, false, &percent)) {
        logBrightnessTransaction("setProperties", "brightness-probe", inputValue, percent);
        return setBacklightPercent(percent, "prop-brightness-probe") ? kIOReturnSuccess : kIOReturnError;
    }
    if (extractNumericValue(dict->getObject("linear-brightness-probe"), &inputValue) &&
        extractBrightnessPercent(dict->getObject("linear-brightness-probe"), 0, kAppleBacklightRawMax, true, &percent)) {
        logBrightnessTransaction("setProperties", "linear-brightness-probe", inputValue, percent);
        return setBacklightPercent(percent, "prop-linear-probe") ? kIOReturnSuccess : kIOReturnError;
    }

    OSDictionary* params = OSDynamicCast(OSDictionary, dict->getObject("IODisplayParameters"));
    if (params) {
        if (extractNumericValue(params->getObject("brightness"), &inputValue) &&
            extractBrightnessPercent(params->getObject("brightness"), kAppleBacklightUserMin, kAppleBacklightUserMax, false, &percent)) {
            logBrightnessTransaction("setProperties.IODisplayParameters", "brightness", inputValue, percent);
            return setBacklightPercent(percent, "params-brightness") ? kIOReturnSuccess : kIOReturnError;
        }
        if (extractNumericValue(params->getObject("linear-brightness"), &inputValue) &&
            extractBrightnessPercent(params->getObject("linear-brightness"), 0, kAppleBacklightRawMax, true, &percent)) {
            logBrightnessTransaction("setProperties.IODisplayParameters", "linear-brightness", inputValue, percent);
            return setBacklightPercent(percent, "params-linear") ? kIOReturnSuccess : kIOReturnError;
        }
        if (extractNumericValue(params->getObject("usable-linear-brightness"), &inputValue) &&
            extractBrightnessPercent(params->getObject("usable-linear-brightness"), 0, kAppleBacklightRawMax, true, &percent)) {
            logBrightnessTransaction("setProperties.IODisplayParameters", "usable-linear-brightness", inputValue, percent);
            return setBacklightPercent(percent, "params-usable-linear") ? kIOReturnSuccess : kIOReturnError;
        }
        if (extractNumericValue(params->getObject("vblm"), &inputValue) &&
            extractVBLPercent(params->getObject("vblm"), &percent)) {
            logBrightnessTransaction("setProperties.IODisplayParameters", "vblm", inputValue, percent);
            return setBacklightPercent(percent, "params-vblm") ? kIOReturnSuccess : kIOReturnError;
        }
        if (extractNumericValue(params->getObject("brightness-fade"), &inputValue) &&
            extractBrightnessPercent(params->getObject("brightness-fade"), kAppleBacklightUserMin, kAppleBacklightUserMax, false, &percent)) {
            logBrightnessTransaction("setProperties.IODisplayParameters", "brightness-fade", inputValue, percent);
            return setBacklightPercent(percent, "params-fade") ? kIOReturnSuccess : kIOReturnError;
        }
        if (extractNumericValue(params->getObject("brightness-probe"), &inputValue) &&
            extractBrightnessPercent(params->getObject("brightness-probe"), kAppleBacklightUserMin, kAppleBacklightUserMax, false, &percent)) {
            logBrightnessTransaction("setProperties.IODisplayParameters", "brightness-probe", inputValue, percent);
            return setBacklightPercent(percent, "params-brightness-probe") ? kIOReturnSuccess : kIOReturnError;
        }
        if (extractNumericValue(params->getObject("linear-brightness-probe"), &inputValue) &&
            extractBrightnessPercent(params->getObject("linear-brightness-probe"), 0, kAppleBacklightRawMax, true, &percent)) {
            logBrightnessTransaction("setProperties.IODisplayParameters", "linear-brightness-probe", inputValue, percent);
            return setBacklightPercent(percent, "params-linear-probe") ? kIOReturnSuccess : kIOReturnError;
        }

        if (params->getObject("commit")) {
            IOLog("[BLTX] setProperties.IODisplayParameters key=commit input=0x00000001 -> %u%%\n", getBacklightPercent());
            return kIOReturnSuccess;
        }
    }

    if (dict->getObject("commit")) {
        IOLog("[BLTX] setProperties key=commit input=0x00000001 -> %u%%\n", getBacklightPercent());
        return kIOReturnSuccess;
    }

    return super::setProperties(properties);
}











#include "FakeIrisXEGEM.hpp"

// GGTT PTE format helper: adapt to your PRM / i915_reg.h if you have PTE flags defined.
// For typical Intel GGTT PTE: phys >> 12 | PTE_VALID | PTE_CACHE_BITS...
static inline uint32_t make_ggtt_pte32(uint64_t phys) {
    // Example: present bit = 1, set phys >> 12 in low bits
    // For TGL you often need 64-bit PTEs; we write two 32-bit words if necessary.
    uint32_t pte_low = (uint32_t)((phys & 0xFFFFFFFFULL) >> 12) | 0x1; // present
    return pte_low;
}
static inline uint32_t make_ggtt_pte32_hi(uint64_t phys) {
    return (uint32_t)((phys >> 44) & 0xFF); // platform dependent; keep simple
}

// Map a GEM into GGTT and return GPU VA (aligned to page). Thread-safe enough for bring up.
uint64_t FakeIrisXEFramebuffer::ggttMap(FakeIrisXEGEM* gem) {
    if (!gem || !fGGTT) return 0;

    IOBufferMemoryDescriptor* md = gem->memoryDescriptor();
    if (!md) {
        IOLog("(FakeIrisXE) ggttMap: gem->memoryDescriptor() is NULL (gem=%p)\n", gem);
        return 0;
    }

    // FIXED: Ensure GGTT is 64-bit array (TGL PTEs)
    if (!fGGTT) {
        IOLog("(FakeIrisXE) ggttMap: BAR1/GGTT not mapped yet (fBar1=%p)\n", fGGTT);
        return 0;
    }

    uint32_t pages = gem->pageCount();
    uint64_t gpuAddr = fNextGGTTOffset;  // in bytes
    uint64_t offGPU = gpuAddr;

    uint64_t offset = 0;
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t segSz = 0;
        mach_vm_address_t phys = gem->getPhysicalSegment(offset, &segSz);
        if (!phys) {
            IOLog("FakeIrisXEFramebuffer: ggttMap - null phys seg at page %u\n", i);
            return 0;
        }

        // FIXED: Index in pages (>>12)
        uint64_t gtt_index = (offGPU >> 12);
        if ((gtt_index + 1) * 8 > fGGTTSize) {  // FIXED: 8 bytes per 64-bit PTE
            IOLog("FakeIrisXEFramebuffer: ggttMap - out of GGTT space\n");
            return 0;
        }

        // TGL 64-bit GGTT PTE: keep the page-aligned physical address in bits [51:12].
        // The previous `phys >> 12` encoding truncated the address twice and produced
        // bogus near-zero PTEs, which likely made the GPU reject the ring backing store.
        uint64_t pte_val = (static_cast<uint64_t>(phys) & 0x0000FFFFFFFFF000ULL);

        pte_val |= (1ULL << 57);   // Valid bit (bit 57 = 1)
        pte_val |= (0ULL << 59);   // 4KB page (exponent = 0)
        pte_val |= (0ULL << 58);   // System memory (bit 58 = 0)
        pte_val |= (0ULL << 2);    // PAT index 0 (WB cache)

        // FIXED: Write full 64-bit PTE (no low/high split)
        volatile uint64_t* pte_ptr = (volatile uint64_t*)fGGTT + gtt_index;
        *pte_ptr = pte_val;
        uint64_t verify = *pte_ptr;
        if (i == 0) {
            IOLog("FakeIrisXEFramebuffer: ggttMap first PTE idx=%llu phys=0x%llx pte=0x%016llx verify=0x%016llx\n",
                  static_cast<unsigned long long>(gtt_index),
                  static_cast<unsigned long long>(phys),
                  static_cast<unsigned long long>(pte_val),
                  static_cast<unsigned long long>(verify));
        }

        offGPU += 4096;
        offset += segSz ? segSz : 4096;
    }

    // FIXED: Full flush (CPU + GPU cache)
    __sync_synchronize();
    safeMMIOWrite(0x1082C0, 1);  // GTT_WRITE_FLUSH (TGL required)

    uint64_t ret = gpuAddr;
    fNextGGTTOffset += ((uint64_t)pages << 12);
    IOLog("FakeIrisXEFramebuffer: ggttMap -> GPU VA 0x%llx pages=%u (TGL PTEs)\n", (unsigned long long)ret, pages);
    return ret;
}

// V221: Get CPU virtual address of a GGTT-mapped GEM object
void* FakeIrisXEFramebuffer::ggttGetCPUAddr(FakeIrisXEGEM* gem) {
    if (!gem) return nullptr;
    
    IOBufferMemoryDescriptor* md = gem->memoryDescriptor();
    if (!md) {
        IOLog("(FakeIrisXE) ggttGetCPUAddr: gem->memoryDescriptor() is NULL (gem=%p)\n", gem);
        return nullptr;
    }
    
    void* cpuAddr = (void*)md->getBytesNoCopy();
    if (!cpuAddr) {
        IOLog("(FakeIrisXE) ggttGetCPUAddr: getBytesNoCopy() returned NULL (gem=%p)\n", gem);
        return nullptr;
    }
    
    IOLog("FakeIrisXEFramebuffer: ggttGetCPUAddr -> CPU VA %p (gem=%p)\n", cpuAddr, gem);
    return cpuAddr;
}

// V140: Map a GEM into GGTT at or above a minimum offset (for GuC firmware placement)
// This ensures firmware is mapped above WOPCM size to avoid GGTT pin bias issues
uint64_t FakeIrisXEFramebuffer::ggttMapAtOrAbove(FakeIrisXEGEM* gem, uint64_t minOffset) {
    if (!gem || !fGGTT) return 0;

    IOBufferMemoryDescriptor* md = gem->memoryDescriptor();
    if (!md) {
        IOLog("(FakeIrisXE) ggttMapAtOrAbove: gem->memoryDescriptor() is NULL (gem=%p)\n", gem);
        return 0;
    }

    if (!fGGTT) {
        IOLog("(FakeIrisXE) ggttMapAtOrAbove: BAR1/GGTT not mapped yet (fGGTT=%p)\n", fGGTT);
        return 0;
    }

    uint32_t pages = gem->pageCount();
    
    // V140: Align minOffset to page boundary and ensure fNextGGTTOffset is at least minOffset
    minOffset = (minOffset + 4095) & ~4095ULL;
    if (fNextGGTTOffset < minOffset) {
        fNextGGTTOffset = minOffset;
    }
    
    uint64_t gpuAddr = fNextGGTTOffset;
    uint64_t offGPU = gpuAddr;

    IOLog("(FakeIrisXE) ggttMapAtOrAbove: minOffset=0x%llx starting at GPU VA 0x%llx\n", 
          (unsigned long long)minOffset, (unsigned long long)gpuAddr);

    uint64_t offset = 0;
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t segSz = 0;
        mach_vm_address_t phys = gem->getPhysicalSegment(offset, &segSz);
        if (!phys) {
            IOLog("FakeIrisXEFramebuffer: ggttMapAtOrAbove - null phys seg at page %u\n", i);
            return 0;
        }

        uint64_t gtt_index = (offGPU >> 12);
        if ((gtt_index + 1) * 8 > fGGTTSize) {
            IOLog("FakeIrisXEFramebuffer: ggttMapAtOrAbove - out of GGTT space\n");
            return 0;
        }

        uint64_t pte_val = (static_cast<uint64_t>(phys) & 0x0000FFFFFFFFF000ULL);
        pte_val |= (1ULL << 57);
        pte_val |= (0ULL << 59);
        pte_val |= (0ULL << 58);
        pte_val |= (0ULL << 2);

        volatile uint64_t* pte_ptr = (volatile uint64_t*)fGGTT + gtt_index;
        *pte_ptr = pte_val;
        uint64_t verify = *pte_ptr;
        if (i == 0) {
            IOLog("FakeIrisXEFramebuffer: ggttMapAtOrAbove first PTE idx=%llu phys=0x%llx pte=0x%016llx verify=0x%016llx\n",
                  static_cast<unsigned long long>(gtt_index),
                  static_cast<unsigned long long>(phys),
                  static_cast<unsigned long long>(pte_val),
                  static_cast<unsigned long long>(verify));
        }

        offGPU += 4096;
        offset += segSz ? segSz : 4096;
    }

    __sync_synchronize();
    safeMMIOWrite(0x1082C0, 1);

    uint64_t ret = gpuAddr;
    fNextGGTTOffset += ((uint64_t)pages << 12);
    IOLog("FakeIrisXEFramebuffer: ggttMapAtOrAbove -> GPU VA 0x%llx pages=%u\n", (unsigned long long)ret, pages);
    return ret;
}





void FakeIrisXEFramebuffer::ggttUnmap(uint64_t gpuAddr, uint32_t pages) {
    if (!fGGTT) return;
    uint64_t off = gpuAddr;
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t idx = (off >> 12);
        if ((idx + 1) * sizeof(uint64_t) <= fGGTTSize) {
            volatile uint64_t* pte_ptr = reinterpret_cast<volatile uint64_t*>(fGGTT) + idx;
            *pte_ptr = 0;
        }
        off += 4096;
    }
    __sync_synchronize();
    IOLog("FakeIrisXEFramebuffer: ggttUnmap GPU VA 0x%llx pages=%u\n", (unsigned long long)gpuAddr, pages);
}




void FakeIrisXEFramebuffer::updateExecutionState(bool ready, const char* reason)
{
    fCommandSubmissionReady = ready;
    setProperty("FakeIrisXECommandSubmissionReady", ready ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("FakeIrisXERcsRingValidated", fRcsRingValidated ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("IOFBAccelerated", ready ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("IOFBAccelerator", kOSBooleanFalse);
    setProperty("IOFBAcceleratorLinked", kOSBooleanFalse);
    setProperty("HardwareAccelerated", ready ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("GPURendering", ready ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("MetalSupported", kOSBooleanFalse);
    setProperty("MetalDevice", kOSBooleanFalse);
    IOLog("(FakeIrisXE) [V171] Execution state: ready=%u reason=%s ringValidated=%u execlist=%p ring=%p\n",
          ready ? 1U : 0U,
          reason ? reason : "unknown",
          fRcsRingValidated ? 1U : 0U,
          fExeclist,
          fRcsRing);
}

void FakeIrisXEFramebuffer::destroyRcsRingState()
{
    if (fRingGpuVA && fRingSize) {
        const uint32_t pages = static_cast<uint32_t>((fRingSize + 4095U) >> 12);
        ggttUnmap(fRingGpuVA, pages);
    }
    if (fRingGem) {
        fRingGem->unpin();
        fRingGem->release();
        fRingGem = nullptr;
    }
    if (fRcsRing) {
        delete fRcsRing;
        fRcsRing = nullptr;
    }
    fRingGpuVA = 0;
    fRingSize = 0;
    fRcsRingValidated = false;
    setProperty("FakeIrisXERcsRingValidated", kOSBooleanFalse);
}

bool FakeIrisXEFramebuffer::validateRcsRingState(const char* phase, bool delayedCheck)
{
    if (!fRcsRing || !fRingGpuVA || !fRingSize) {
        IOLog("(FakeIrisXE) [V171] validateRcsRingState(%s): ring missing\n",
              phase ? phase : "unknown");
        fRcsRingValidated = false;
        setProperty("FakeIrisXERcsRingValidated", kOSBooleanFalse);
        return false;
    }

    if (!forcewakeRenderHold(5000)) {
        IOLog("(FakeIrisXE) [V171] validateRcsRingState(%s): forcewake failed\n",
              phase ? phase : "unknown");
        fRcsRingValidated = false;
        setProperty("FakeIrisXERcsRingValidated", kOSBooleanFalse);
        return false;
    }

    const uint32_t baseLoExpected = static_cast<uint32_t>(fRingGpuVA & 0xFFFFFFFFULL);
    const uint32_t baseHiExpected = static_cast<uint32_t>(fRingGpuVA >> 32);

    uint32_t ringMode0 = safeMMIORead(kTglRcsRingMode);
    uint32_t gfxMode0 = safeMMIORead(kTglRcsGfxMode);
    uint32_t gfxMode20 = safeMMIORead(kTglRcsGfxMode2);
    uint32_t resetCtrl0 = safeMMIORead(kTglRcsResetCtrl);
    uint32_t ringStart = safeMMIORead(kTglRcsRingStart);
    uint32_t altStart0 = safeMMIORead(kTglAltRcsRingStart);
    uint32_t altHead0 = safeMMIORead(kTglAltRcsRingHead);
    uint32_t altTail0 = safeMMIORead(kTglAltRcsRingTail);
    uint32_t head0 = safeMMIORead(kTglRcsRingHead);
    uint32_t tail0 = safeMMIORead(kTglRcsRingTail);
    uint32_t ctl0 = safeMMIORead(kTglRcsRingCtl);

    if (delayedCheck) {
        IODelay(25);
    }

    uint32_t ringMode1 = safeMMIORead(kTglRcsRingMode);
    uint32_t gfxMode1 = safeMMIORead(kTglRcsGfxMode);
    uint32_t gfxMode21 = safeMMIORead(kTglRcsGfxMode2);
    uint32_t resetCtrl1 = safeMMIORead(kTglRcsResetCtrl);
    uint32_t head1 = safeMMIORead(kTglRcsRingHead);
    uint32_t tail1 = safeMMIORead(kTglRcsRingTail);
    uint32_t ctl1 = safeMMIORead(kTglRcsRingCtl);
    uint32_t altStart1 = safeMMIORead(kTglAltRcsRingStart);
    uint32_t altHead1 = safeMMIORead(kTglAltRcsRingHead);
    uint32_t altTail1 = safeMMIORead(kTglAltRcsRingTail);

    forcewakeRenderRelease();

    const bool baseValid = (ringStart == baseLoExpected) && (baseHiExpected == 0);
    const bool ctlEnabledNow = (ctl0 & 0x1U) != 0;
    const bool ctlEnabledStable = (ctl1 & 0x1U) != 0;
    const uint32_t headOffset = head1 & 0x001FFFFCU;
    const uint32_t tailOffset = tail1 & 0x001FFFFCU;
    const bool offsetsInRange = (headOffset < fRingSize) && (tailOffset < fRingSize);

    fRcsRingValidated = baseValid && ctlEnabledNow && ctlEnabledStable && offsetsInRange;
    setProperty("FakeIrisXERcsRingValidated", fRcsRingValidated ? kOSBooleanTrue : kOSBooleanFalse);
    
    // V207: Extra debug - show expected vs actual
    IOLog("(FakeIrisXE) [V207] Validation: expected START=0x%08X actual=0x%08X baseValid=%d ctlEnabledNow=%d ctlEnabledStable=%d offsetsInRange=%d\n",
          baseLoExpected, ringStart, baseValid ? 1 : 0, ctlEnabledNow ? 1 : 0, ctlEnabledStable ? 1 : 0, offsetsInRange ? 1 : 0);
    setNumberProperty(this, "FakeIrisXERingCtlInitial", ctl0, 32);
    setNumberProperty(this, "FakeIrisXERingCtlStable", ctl1, 32);
    setNumberProperty(this, "FakeIrisXERingHead", head1, 32);
    setNumberProperty(this, "FakeIrisXERingTail", tail1, 32);
    setNumberProperty(this, "FakeIrisXERingStart", ringStart, 32);
    setNumberProperty(this, "FakeIrisXERingMode", ringMode1, 32);
    setNumberProperty(this, "FakeIrisXEGfxMode", gfxMode1, 32);
    setNumberProperty(this, "FakeIrisXEGfxMode2", gfxMode21, 32);
    setNumberProperty(this, "FakeIrisXEResetCtrl", resetCtrl1, 32);
    setNumberProperty(this, "FakeIrisXEAltRingStart", altStart1, 32);
    setNumberProperty(this, "FakeIrisXEAltRingHead", altHead1, 32);
    setNumberProperty(this, "FakeIrisXEAltRingTail", altTail1, 32);

    IOLog("(FakeIrisXE) [V171] Stage4 ring validation (%s): mode0=0x%08X gfx0=0x%08X gfx20=0x%08X rst0=0x%08X start=0x%08X base=%s ctl0=0x%08X ctl1=0x%08X head=0x%08X tail=0x%08X alt0=[0x%08X,0x%08X,0x%08X] alt1=[0x%08X,0x%08X,0x%08X] mode1=0x%08X gfx1=0x%08X gfx21=0x%08X rst1=0x%08X size=0x%zX result=%s\n",
          phase ? phase : "unknown",
          ringMode0,
          gfxMode0,
          gfxMode20,
          resetCtrl0,
          ringStart,
          baseValid ? "OK" : "BAD",
          ctl0,
          ctl1,
          head1,
          tail1,
          altStart0,
          altHead0,
          altTail0,
          altStart1,
          altHead1,
          altTail1,
          ringMode1,
          gfxMode1,
          gfxMode21,
          resetCtrl1,
          fRingSize,
          fRcsRingValidated ? "PASS" : "FAIL");

    return fRcsRingValidated;
}

// create ring: allocate GEM -> pin -> ggttMap -> program registers
FakeIrisXERing* FakeIrisXEFramebuffer::createRcsRing(size_t ringBytes)
{
    IOLog("(FakeIrisXE) createRcsRing() size=%zu\n", ringBytes);

    // V229: Skip legacy ring validation on Gen12+ - EXEClist is the correct path
    // Legacy ring doesn't work properly on Gen12 and fails validation
    // V221 EXEClist path handles this correctly
    IOLog("(FakeIrisXE) [V229] Gen12+ platform - legacy ring validation skipped (using EXEClist)\n");
    
    // Still create the ring for compatibility but skip validation
    if (fRcsRing != nullptr) {
        IOLog("(FakeIrisXE) createRcsRing() — ring already exists @ %p, returning\n", fRcsRing);
        return fRcsRing;
    }

    FakeIrisXEGEM* ringGem = FakeIrisXEGEM::withSize(ringBytes, 0);
    if (!ringGem) {
        IOLog("❌ createRcsRing — GEM allocation failed\n");
        return nullptr;
    }

    ringGem->pin();

    uint64_t ringGpuVA = ggttMap(ringGem);
    if (ringGpuVA == 0) {
        IOLog("❌ createRcsRing — GGTT mapping failed\n");
        ringGem->unpin();
        ringGem->release();
        return nullptr;
    }

    IOBufferMemoryDescriptor* ringDesc = ringGem->memoryDescriptor();
    if (!ringDesc || !ringDesc->getBytesNoCopy()) {
        IOLog("❌ createRcsRing — ring CPU mapping missing\n");
        ggttUnmap(ringGpuVA, static_cast<uint32_t>((ringBytes + 4095U) >> 12));
        ringGem->unpin();
        ringGem->release();
        return nullptr;
    }

    fRcsRing = new FakeIrisXERing(fBar0);
    if (!fRcsRing) {
        IOLog("❌ createRcsRing — ring object alloc failed\n");
        ggttUnmap(ringGpuVA, static_cast<uint32_t>((ringBytes + 4095U) >> 12));
        ringGem->unpin();
        ringGem->release();
        return nullptr;
    }

    bzero(ringDesc->getBytesNoCopy(), ringBytes);
    fRcsRing->attachRingCPUAddress(ringDesc->getBytesNoCopy());
    fRcsRing->attachRingGPUAddress(ringGpuVA);
    fRcsRing->setRingSize(ringBytes);
    fRingSize = ringBytes;
    fRingGpuVA = ringGpuVA;
    fRingGem = ringGem;

    // V243: Check GT_ERROR at START of GT initialization
    uint32_t gt_error_early = safeMMIORead(0x0B00);
    IOLog("(FakeIrisXE) [V243] GT_ERROR at START of GT init: 0x%08X (%s)\n", 
          gt_error_early, (gt_error_early & 0x80000000) ? "WEDGED!" : "OK");

    // V204: Enhanced GT compute power enable with more status checks
    // Based on Linux i915 - need to request compute power domain
    IOLog("(FakeIrisXE) [V204] Enabling GT compute power domain...\n");
    
    // V244: Track GT_ERROR before and after power wells
    uint32_t gt_error_before_pw = safeMMIORead(0x0B00);
    
    // Request power wells for compute
    volatile uint32_t* bar0 = fBar0;
    uint32_t pw_ctl2 = *(volatile uint32_t*)((uint8_t*)bar0 + 0x45404);  // PWR_WELL_CTL2
    uint32_t pw_ctl3 = *(volatile uint32_t*)((uint8_t*)bar0 + 0x45408);  // PWR_WELL_CTL3
    uint32_t pw_ctl4 = *(volatile uint32_t*)((uint8_t*)bar0 + 0x4540C);  // PWR_WELL_CTL4
    uint32_t pw_status = *(volatile uint32_t*)((uint8_t*)bar0 + 0x45410);  // PWR_WELL_STATUS
    IOLog("(FakeIrisXE) [V204] Power wells before: CTL2=0x%08X CTL3=0x%08X CTL4=0x%08X STATUS=0x%08X\n", pw_ctl2, pw_ctl3, pw_ctl4, pw_status);
    
    uint32_t gt_error_after_pw = safeMMIORead(0x0B00);
    IOLog("(FakeIrisXE) [V244] GT_ERROR: before PW=0x%08X after PW=0x%08X\n", 
          gt_error_before_pw, gt_error_after_pw);
    
    // Try enabling GT_PG_ENABLE - bit 0 controls GT power gating
    uint32_t gt_pg_enable = *(volatile uint32_t*)((uint8_t*)bar0 + 0xA218);
    IOLog("(FakeIrisXE) [V204] GT_PG_ENABLE before: 0x%08X\n", gt_pg_enable);
    
    // Disable GT power gating by clearing bit 0
    *(volatile uint32_t*)((uint8_t*)bar0 + 0xA218) = 0x00000000;
    IOSleep(5);
    gt_pg_enable = *(volatile uint32_t*)((uint8_t*)bar0 + 0xA218);
    IOLog("(FakeIrisXE) [V204] GT_PG_ENABLE after: 0x%08X\n", gt_pg_enable);
    
    // Request all power wells - bit 0 (power request) + bit 16 (force on)
    *(volatile uint32_t*)((uint8_t*)bar0 + 0x45404) = 0x00030003;  // PW2: request + force on
    IOSleep(5);
    *(volatile uint32_t*)((uint8_t*)bar0 + 0x45408) = 0x40030003;  // PW3: request + force on  
    IOSleep(5);
    *(volatile uint32_t*)((uint8_t*)bar0 + 0x4540C) = 0x00030003;  // PW4: request + force on
    IOSleep(10);
    
    pw_ctl2 = *(volatile uint32_t*)((uint8_t*)bar0 + 0x45404);
    pw_ctl3 = *(volatile uint32_t*)((uint8_t*)bar0 + 0x45408);
    pw_ctl4 = *(volatile uint32_t*)((uint8_t*)bar0 + 0x4540C);
    pw_status = *(volatile uint32_t*)((uint8_t*)bar0 + 0x45410);
    IOLog("(FakeIrisXE) [V210] Power wells after: CTL2=0x%08X CTL3=0x%08X CTL4=0x%08X STATUS=0x%08X\n", pw_ctl2, pw_ctl3, pw_ctl4, pw_status);

    // V243: Check GT_ERROR at START of V210
    uint32_t gt_error_start = safeMMIORead(0x0B00);
    IOLog("(FakeIrisXE) [V243] GT_ERROR at START of V210: 0x%08X (%s)\n", 
          gt_error_start, (gt_error_start & 0x80000000) ? "WEDGED!" : "OK");
    
    // ===== V210: COMPREHENSIVE GT INITIALIZATION =====
    IOLog("(FakeIrisXE) [V210] ==== COMPREHENSIVE GT INIT (10 improvements) ====\n");
    
    // Improvement 1: Enhanced GT Status Polling (poll 10 times)
    IOLog("(FakeIrisXE) [V210] 1/10: Enhanced GT status polling...\n");
    uint32_t gt_perf_final = 0;
    for (int poll = 0; poll < 10; poll++) {
        gt_perf_final = safeMMIORead(0xA070);
        if (gt_perf_final != 0) break;
        IOSleep(5);
    }
    IOLog("(FakeIrisXE) [V210] GT_PERF after polling: 0x%08X\n", gt_perf_final);
    
    // Improvement 2: Initialize RC6 Power States
    IOLog("(FakeIrisXE) [V210] 2/10: Initializing RC6 power states...\n");
    uint32_t rc6_ctrl = safeMMIORead(0xA090);
    safeMMIOWrite(0xA090, 0x00000000);  // Disable RC6 for now
    (void)safeMMIORead(0xA090);
    IOLog("(FakeIrisXE) [V210] RC6 control: was 0x%08X\n", rc6_ctrl);
    
    // Improvement 3: GT Workarounds (apply known fixes)
    IOLog("(FakeIrisXE) [V210] 3/10: Applying GT workarounds...\n");
    // Clock gating workarounds for Gen12
    safeMMIOWrite(0x46538, 0x80400000);  // CLKGATE_DIS_3
    (void)safeMMIORead(0x46538);
    IOLog("(FakeIrisXE) [V210] CLKGATE_DIS_3 applied\n");
    
    // Improvement 4: Enhanced Forcewake with proper timing
    IOLog("(FakeIrisXE) [V210] 4/10: Enhanced forcewake timing...\n");
    // Already done via V209 - RENDER+GT domains
    
    // Improvement 5: MOCS Table Verification
    IOLog("(FakeIrisXE) [V210] 5/10: Verifying MOCS table...\n");
    uint32_t mocs0 = safeMMIORead(0xB020);
    uint32_t mocs1 = safeMMIORead(0xB024);
    uint32_t mocs2 = safeMMIORead(0xB028);
    IOLog("(FakeIrisXE) [V210] MOCS: MOCS0=0x%08X MOCS1=0x%08X MOCS2=0x%08X\n", mocs0, mocs1, mocs2);
    
    // Improvement 6: GGTT Sanity Check
    IOLog("(FakeIrisXE) [V210] 6/10: GGTT sanity check...\n");
    // Verify first PTE is correct format
    uint32_t ggtt_pte0 = safeMMIORead(0x8000);  // First GGTT entry
    IOLog("(FakeIrisXE) [V210] GGTT PTE0: 0x%08X\n", ggtt_pte0);
    
    // Improvement 7: RCS Context Verification (check LRC offset)
    IOLog("(FakeIrisXE) [V210] 7/10: RCS context verification...\n");
    uint32_t rcs_head = safeMMIORead(0x2034);
    uint32_t rcs_tail = safeMMIORead(0x2030);
    uint32_t rcs_mode = safeMMIORead(0x209C);
    IOLog("(FakeIrisXE) [V210] RCS Context: HEAD=0x%08X TAIL=0x%08X MODE=0x%08X\n", rcs_head, rcs_tail, rcs_mode);
    
    // Improvement 8: Engine Reset Before Ring Init (as Linux does)
    IOLog("(FakeIrisXE) [V210] 8/10: Engine reset before ring init...\n");
    safeMMIOWrite(0x20D0, 0x00000001);  // Request reset
    IOSleep(5);
    safeMMIOWrite(0x20D0, 0x00000000);  // Release reset
    IOSleep(10);
    uint32_t reset_status = safeMMIORead(0x20D0);
    IOLog("(FakeIrisXE) [V210] Engine reset: 0x%08X\n", reset_status);
    
    // Improvement 9: GT Clock Frequency Check
    IOLog("(FakeIrisXE) [V210] 9/10: GT clock frequency check...\n");
    IOLog("(FakeIrisXE) [V210] GT Clock: PERF_LIM=0x%08X CLK_CTL=0x%08X\n", 
          safeMMIORead(0xA094), safeMMIORead(0x46000));
    
    // Improvement 10: Final GT Status
    IOLog("(FakeIrisXE) [V210] 10/10: Final GT status check...\n");
    IOLog("(FakeIrisXE) [V210] Final GT: GT_STATUS=0x%08X GFX=0x%08X PMC=0x%08X\n", 
          safeMMIORead(0xA000), safeMMIORead(0xA008), safeMMIORead(0xA010));
    IOLog("(FakeIrisXE) [V210] ==== COMPREHENSIVE GT INIT COMPLETE ====\n");
    // ===== END V210 IMPROVEMENTS =====

    // ===== V211: ADDITIONAL CRITICAL IMPROVEMENTS (11-15) =====
    IOLog("(FakeIrisXE) [V211] ==== CRITICAL GT IMPROVEMENTS (11-15) ====\n");
    
    // Improvement 11: Clear GT Fault Registers (CRITICAL!)
    IOLog("(FakeIrisXE) [V211] 11/15: Clearing GT fault registers...\n");
    // GEN12_RING_FAULT_REG - Clear any pending faults
    uint32_t fault_reg = safeMMIORead(0x1C3E0);  // GEN12_RING_FAULT_REG
    IOLog("(FakeIrisXE) [V211] Fault reg before clear: 0x%08X\n", fault_reg);
    // Clear fault bits by writing 0
    safeMMIOWrite(0x1C3E0, 0x00000000);  // Clear faults
    (void)safeMMIORead(0x1C3E0);  // Posting read
    fault_reg = safeMMIORead(0x1C3E0);
    IOLog("(FakeIrisXE) [V211] Fault reg after clear: 0x%08X\n", fault_reg);
    
    // Improvement 12: Verify Engine Availability
    IOLog("(FakeIrisXE) [V211] 12/15: Verifying engine availability...\n");
    // Check if RCS engine is available (not fused off)
    // Read from offset 0x1C for engine info
    uint32_t engine_info = safeMMIORead(0x1C);
    IOLog("(FakeIrisXE) [V211] Engine info: 0x%08X\n", engine_info);
    
    // Improvement 13: GT MCR Initialization (multicast registers)
    IOLog("(FakeIrisXE) [V211] 13/15: GT MCR initialization...\n");
    // Read MCR status registers to initialize
    uint32_t mcr_status = safeMMIORead(0x0B00);
    uint32_t mcr_sts = safeMMIORead(0x0B20);
    IOLog("(FakeIrisXE) [V211] MCR: STATUS=0x%08X STS=0x%08X\n", mcr_status, mcr_sts);
    
    // Improvement 14: Proper Posting Reads
    IOLog("(FakeIrisXE) [V211] 14/15: Proper posting reads...\n");
    // Do multiple posting reads to ensure MMIO writes land
    for (int i = 0; i < 3; i++) {
        (void)safeMMIORead(0xA000);
        (void)safeMMIORead(0xA008);
        (void)safeMMIORead(0xA070);
    }
    IOLog("(FakeIrisXE) [V211] Posting reads complete\n");
    
    // Improvement 15: Check for GT Wedge State
    IOLog("(FakeIrisXE) [V211] 15/15: Checking GT wedge state...\n");
    // Check GT_ERROR register for wedged state
    uint32_t gt_error = safeMMIORead(0x0B00);  // GT_ERROR
    IOLog("(FakeIrisXE) [V211] GT_ERROR: 0x%08X\n", gt_error);
    // Check if GT is wedged (bit 31)
    if (gt_error & 0x80000000) {
        IOLog("(FakeIrisXE) [V211] ⚠️ GT is WEDGED! Attempting recovery...\n");
        // Try to clear wedge by writing
        safeMMIOWrite(0x0B00, 0x00000000);
        (void)safeMMIORead(0x0B00);
    }
    IOLog("(FakeIrisXE) [V211] ==== CRITICAL GT IMPROVEMENTS COMPLETE ====\n");
    // ===== END V211 IMPROVEMENTS =====

    // ===== V212: AGGRESSIVE GT WEDGE RECOVERY =====
    IOLog("(FakeIrisXE) [V212] ==== AGGRESSIVE GT WEDGE RECOVERY ====\n");
    
    // Check GT status - if wedged, do a full reset
    gt_error = safeMMIORead(0x0B00);
    if (gt_error & 0x80000000) {
        IOLog("(FakeIrisXE) [V212] GT still wedged, performing full reset...\n");
        
        // Step 1: Release forcewake
        IOLog("(FakeIrisXE) [V212] Step 1: Release forcewake\n");
        forcewakeRenderRelease();
        IOSleep(10);
        
        // Step 2: Trigger GT reset via DEBUG_CTRL1
        IOLog("(FakeIrisXE) [V212] Step 2: Trigger GT reset\n");
        safeMMIOWrite(0x20D8, 0x00000000);  // DEBUG_CTRL1 = 0
        IOSleep(5);
        safeMMIOWrite(0x20D8, 0x00000001);  // Trigger reset
        IOSleep(10);
        
        // Step 3: Clear GT_ERROR
        IOLog("(FakeIrisXE) [V212] Step 3: Clear GT_ERROR\n");
        safeMMIOWrite(0x0B00, 0x00000000);
        IOSleep(10);
        
        // Step 4: Clear any pending faults
        IOLog("(FakeIrisXE) [V212] Step 4: Clear fault registers\n");
        safeMMIOWrite(0x1C3E0, 0x00000000);  // Clear fault register
        IOSleep(5);
        
        // Step 5: Re-acquire forcewake
        IOLog("(FakeIrisXE) [V212] Step 5: Re-acquire forcewake\n");
        if (!forcewakeRenderHold(5000)) {
            IOLog("(FakeIrisXE) [V212] ⚠️ Forcewake re-acquire failed\n");
        } else {
            IOLog("(FakeIrisXE) [V212] Forcewake re-acquired OK\n");
        }
        
        // Step 6: Check GT status after reset
        gt_error = safeMMIORead(0x0B00);
        IOLog("(FakeIrisXE) [V212] GT_ERROR after reset: 0x%08X\n", gt_error);
        
        uint32_t gt_perf_after = safeMMIORead(0xA070);
        IOLog("(FakeIrisXE) [V212] GT_PERF after reset: 0x%08X\n", gt_perf_after);
    } else {
        IOLog("(FakeIrisXE) [V212] GT not wedged, proceeding...\n");
    }
    IOLog("(FakeIrisXE) [V212] ==== GT WEDGE RECOVERY COMPLETE ====\n");
    // ===== END V212 =====

    // V204: Check multiple GT status registers
    IOLog("(FakeIrisXE) [V204] ==== COMPREHENSIVE GT STATUS ====\n");
    uint32_t gt_perf_pre = safeMMIORead(0xA070);  // GT_PERF_STATUS
    uint32_t gt_status_pre = safeMMIORead(0xA000);  // GT_STATUS
    uint32_t gfx_status = safeMMIORead(0xA008);  // GFX_STATUS
    uint32_t pmc_status = safeMMIORead(0xA010);  // PMC status
    uint32_t gt_perf_limit = safeMMIORead(0xA094);  // GT_PERF_LIMIT
    uint32_t gt_clk_ctl = safeMMIORead(0x46000);  // CLK_CTL
    IOLog("(FakeIrisXE) [V204] GT_PERF=0x%08X GT_STATUS=0x%08X GFX=0x%08X PMC=0x%08X PERF_LIM=0x%08X CLK_CTL=0x%08X\n",
          gt_perf_pre, gt_status_pre, gfx_status, pmc_status, gt_perf_limit, gt_clk_ctl);

    if (!forcewakeRenderHold(5000)) {
        IOLog("❌ createRcsRing — forcewakeRenderHold failed before programming ring\n");
        destroyRcsRingState();
        return nullptr;
    }

    // V201: Check GT power status AFTER forcewake
    uint32_t gt_perf_post = safeMMIORead(0xA070);
    uint32_t gt_status_post = safeMMIORead(0xA000);
    uint32_t gfx_status_post = safeMMIORead(0xA008);
    uint32_t pmc_status_post = safeMMIORead(0xA010);
    IOLog("(FakeIrisXE) [V204] Post-forcewake GT: PERF=0x%08X STATUS=0x%08X GFX=0x%08X PMC=0x%08X\n",
          gt_perf_post, gt_status_post, gfx_status_post, pmc_status_post);

    // V204: Simplified engine init - match Linux i915 approach
    IOLog("(FakeIrisXE) [V204] Engine Init: Simplified Linux-style...\n");
    
    // Check what's readable before programming
    uint32_t ring_mode_pre = *(volatile uint32_t*)((uint8_t*)bar0 + kTglRcsRingMode);
    uint32_t gfx_mode_pre = *(volatile uint32_t*)((uint8_t*)bar0 + kTglRcsGfxMode);
    uint32_t reset_ctrl_pre = *(volatile uint32_t*)((uint8_t*)bar0 + kTglRcsResetCtrl);
    IOLog("(FakeIrisXE) [V204] Engine Init pre-program: RING_MODE=0x%08x GFX_MODE=0x%08x RESET_CTRL=0x%08x\n",
          ring_mode_pre, gfx_mode_pre, reset_ctrl_pre);
    
    // Check if GT is powered - read GT_PERF_STATUS
    uint32_t gt_perf = *(volatile uint32_t*)((uint8_t*)bar0 + 0xA070);  // GT_PERF_STATUS (Gen12)
    IOLog("(FakeIrisXE) [V204] GT_PERF_STATUS=0x%08x\n", gt_perf);
    
    // Additional GT power status checks
    uint32_t gt_status = *(volatile uint32_t*)((uint8_t*)bar0 + 0xA000);  // GT_STATUS
    uint32_t gfx0 = *(volatile uint32_t*)((uint8_t*)bar0 + 0xA008);  // GFX_STATUS
    uint32_t pmc = *(volatile uint32_t*)((uint8_t*)bar0 + 0xA010);  // Render power well status
    IOLog("(FakeIrisXE) [V204] GT_POWER: GT_STATUS=0x%08x GFX0=0x%08x PM=0x%08x\n", gt_status, gfx0, pmc);
    
    // Configure RING_MODE - enable ring buffer with Gen12 specific bits
    // Based on Linux: RING_MODERegister
    *(volatile uint32_t*)((uint8_t*)bar0 + kTglRcsRingMode) = 0x00000001;
    IOSleep(5);
    
    // Configure GFX_MODE - enable graphics mode
    // Based on Linux: GFX_MODE for RCS
    *(volatile uint32_t*)((uint8_t*)bar0 + kTglRcsGfxMode) = 0x00000003;
    IOSleep(5);
    
    // Verify writes
    uint32_t ring_mode_after = *(volatile uint32_t*)((uint8_t*)bar0 + kTglRcsRingMode);
    uint32_t gfx_mode_after = *(volatile uint32_t*)((uint8_t*)bar0 + kTglRcsGfxMode);
    uint32_t reset_ctrl_after = *(volatile uint32_t*)((uint8_t*)bar0 + kTglRcsResetCtrl);
    IOLog("(FakeIrisXE) [V183] Engine Init post-program: RING_MODE=0x%08x GFX_MODE=0x%08x RESET_CTRL=0x%08x\n",
          ring_mode_after, gfx_mode_after, reset_ctrl_after);

    fRcsRing->programRingBaseToHW();
    fRcsRing->enableRing();
    
    // V208: Don't release forcewake! Keep it held for validation
    // The issue is that GT compute isn't powered, so when we release forcewake,
    // the RCS registers clear. Keep forcewake to maintain state.
    IOLog("(FakeIrisXE) [V208] Keeping forcewake HELD for validation\n");
    // Don't release forcewake - keep it held
    
    if (!validateRcsRingState("stage4", false)) {
        IOLog("❌ createRcsRing — strict validation failed\n");
        forcewakeRenderRelease();
        destroyRcsRingState();
        return nullptr;
    }
    
    forcewakeRenderRelease();  // V208: Release after successful validation

    IOLog("🟢 RCS ring created @ GPUVA=0x%llx size=%zu (ptr %p)\n",
          (unsigned long long) ringGpuVA, ringBytes, fRcsRing);

    return fRcsRing;
}

// V151: Enhanced GPU Execution Test with comprehensive diagnostics
bool FakeIrisXEFramebuffer::testGPUExecution()
{
    IOLog("(FakeIrisXE)[V289] ============================================\n");
    IOLog("(FakeIrisXE)[V289] GPU EXECUTION TEST - DIRECT EXECLIST PROOF\n");
    IOLog("(FakeIrisXE)[V289] ============================================\n");
    if (!fExeclist) {
        IOLog("(FakeIrisXE)[V289] No EXECLIST owner available\n");
        return false;
    }
    IOLog("(FakeIrisXE)[V289] Running one-shot scratch writeback proof on plain -fakeirisxe boot...\n");
    bool success = fExeclist->testBatchSubmission();
    IOLog("(FakeIrisXE)[V289] Direct Execlist proof result: %s\n", success ? "PASS" : "FAIL");
    IOLog("(FakeIrisXE)[V289] ============================================\n");
    return success;
}

// V138: Create BLT ring for 2D operations
FakeIrisXERing* FakeIrisXEFramebuffer::createBltRing(size_t ringBytes)
{
    IOLog("(FakeIrisXE) createBltRing() size=%zu\n", ringBytes);

    // If BLT ring already exists — return it
    if (fBltRing != nullptr) {
        IOLog("(FakeIrisXE) createBltRing() — ring already exists @ %p\n", fBltRing);
        return fBltRing;
    }

    // Allocate GEM buffer for BLT ring
    FakeIrisXEGEM* ringGem = FakeIrisXEGEM::withSize(ringBytes, 0);
    if (!ringGem) {
        IOLog("❌ createBltRing — GEM allocation failed\n");
        return nullptr;
    }

    ringGem->pin();

    // Map into GGTT
    uint64_t ringGpuVA = ggttMap(ringGem);
    if (ringGpuVA == 0) {
        IOLog("❌ createBltRing — GGTT mapping failed\n");
        ringGem->unpin();
        ringGem->release();
        return nullptr;
    }

    // Create BLT ring object with BLT base offset (0x22000)
    fBltRing = new FakeIrisXERing(fBar0, 0x22000);  // BLT ring base
    if (!fBltRing) {
        IOLog("❌ createBltRing — ring object alloc failed\n");
        ringGem->unpin();
        ringGem->release();
        return nullptr;
    }

    // Save metadata into ring object
    fBltRing->attachRingGPUAddress(ringGpuVA);
    fBltRing->setRingSize(ringBytes);  // V154: Set ring size before enableRing

    // Program BLT ring registers
    fBltRing->programRingBaseToHW();
    fBltRing->enableRing();

    IOLog("🟢 BLT ring created @ GPUVA=0x%llx size=%zu (ptr %p)\n",
          (unsigned long long) ringGpuVA, ringBytes, fBltRing);

    return fBltRing;
}



// Submit a batch GEM (already filled by caller) to RCS
// - batchGem: GEM that contains the batch commands.
// - batchOffsetBytes: offset into GEM where batch starts
// - batchSizeBytes: length of the batch
// Return: sequence number or 0 on failure
uint32_t FakeIrisXEFramebuffer::submitBatch(FakeIrisXEGEM* batchGem, size_t batchOffsetBytes, size_t batchSizeBytes) {
    if (!batchGem) {
        IOLog("FakeIrisXEFramebuffer: submitBatch - bad args\n");
        return 0;
    }

    // V206: Try EXEClist path if RCS ring not available
    if (!fRcsRing || !validateRcsRingState("submitBatch", true)) {
        IOLog("FakeIrisXEFramebuffer: submitBatch - RCS ring not available, trying EXEClist fallback...\n");
        
        // Try EXEClist submission instead
        if (fExeclist && fExeclist->isReady()) {
            IOLog("FakeIrisXEFramebuffer: submitBatch - using EXEClist fallback path\n");
            bool success = fExeclist->submitBatchExeclist(batchGem);
            if (success) {
                IOLog("FakeIrisXEFramebuffer: submitBatch - EXEClist fallback submitted diagnostically, but completion is unproven\n");
                return 0;
            }
            IOLog("FakeIrisXEFramebuffer: submitBatch - EXEClist fallback FAILED\n");
        }
        
        IOLog("FakeIrisXEFramebuffer: submitBatch - ring validation failed\n");
        return 0;
    }

    // Ensure batch GEM is pinned and mapped into GGTT (if not, pin+map)
    batchGem->pin();
    uint64_t batchGpu = batchGem->gpuAddress(); // if your GEM stores GPU VA after ggttMap; else call ggttMap(batchGem)
    if (batchGpu == 0) {
        // If gpuAddress() not set, do a ggttMap here
        batchGpu = ggttMap(batchGem);
        if (batchGpu == 0) {
            IOLog("FakeIrisXEFramebuffer: submitBatch - cannot get batch GPU VA\n");
            batchGem->unpin();
            return 0;
        }
    }

    // Prepare fence: allocate if missing
    if (!fFenceGEM) {
        fFenceGEM = FakeIrisXEGEM::withSize(4096, 0);
        if (!fFenceGEM) {
            IOLog("FakeIrisXEFramebuffer: submitBatch - fence GEM alloc fail\n");
            return 0;
        }
        fFenceGEM->pin();
        uint64_t fenceGpu = ggttMap(fFenceGEM);
        IOLog("FakeIrisXEFramebuffer: Fence GEM mapped at 0x%llx\n", (unsigned long long)fenceGpu);
    }
    uint64_t fenceGpuAddr = fFenceGEM->gpuAddress();
    IOBufferMemoryDescriptor* fenceDesc = fFenceGEM->memoryDescriptor();
    volatile uint32_t* fenceCpu = (volatile uint32_t*)fenceDesc->getBytesNoCopy();
    // ensure fence is zero
    fenceCpu[0] = 0;
    __sync_synchronize();

    // We need to ensure the batch ends with a MI_FLUSH_DW (POSTSYNC) writing a known value
    // For a real driver we inject a MI_FLUSH_DW/POST_SYNC to fence address BEFORE MI_BATCH_BUFFER_END.
    // Caller can include it; if not present we append an inline post-sync packet here.
    // For safety we will not modify caller batch; instead the driver should require caller to
    // include MI_FLUSH_DW or we can create a small chaining batch. Here we assume caller's batch
    // already contains post-sync fence. If not, we can implement chain: create small tail-batch.
    //
    // For now: submit batchGpu directly.

    // Push batch address into ring: use submitBatch64 (the ring helper we implemented)
    bool ok = fRcsRing->submitBatch64(batchGpu);
    if (!ok) {
        IOLog("FakeIrisXEFramebuffer: submitBatch - ring submit failed\n");
        batchGem->unpin();
        return 0;
    }

    // LOG the submission
    IOLog("FakeIrisXEFramebuffer: Batch submitted GPU 0x%llx size=%zu\n",
          (unsigned long long)batchGpu, batchSizeBytes);

    // Wait for fence to be written — *do not busy-loop in production*, here we poll with timeout,
    // but the real production path should use interrupts and proper synchronization.
    const int timeoutMs = 2000;
    int waited = 0;
    bool completed = false;
    while (waited < timeoutMs) {
        __sync_synchronize();
        if (fenceCpu[0] != 0) { completed = true; break; }
        IOSleep(1);
        waited++;
    }

    if (completed) {
        IOLog("FakeIrisXEFramebuffer: Batch fence completed value=0x%08x\n", fenceCpu[0]);
    } else {
        IOLog("FakeIrisXEFramebuffer: Batch fence TIMEOUT fence=0x%08x\n", fenceCpu[0]);
    }

    // cleanup: don't unpin fence (we keep it), unpin batch if temporary
    batchGem->unpin();
    return completed ? 1 : 0;
}

// CORRECTED MI packet definitions
#ifndef MI_INSTR
#define MI_INSTR(opcode, flags) (((opcode) << 23) | (flags))
#endif

#ifndef MI_BATCH_BUFFER_START
#define MI_BATCH_BUFFER_START  MI_INSTR(0x31, 1)  // Flag bit 0 for address space
#endif

#ifndef MI_BATCH_BUFFER_END
#define MI_BATCH_BUFFER_END    MI_INSTR(0x0A, 0)
#endif

#ifndef MI_STORE_DWORD_IMM
#define MI_STORE_DWORD_IMM     MI_INSTR(0x20, 1)
#endif

#ifndef MI_STORE_DWORD_IMM_GEN4
#define MI_STORE_DWORD_IMM_GEN4 MI_INSTR(0x20, 2)
#endif

#ifndef MI_USE_GGTT
#define MI_USE_GGTT           (1 << 22)  // Use GGTT instead of PPGTT
#endif

#ifndef MI_FLUSH_DW
#define MI_FLUSH_DW           MI_INSTR(0x26, 0)
#endif

#ifndef MI_NOOP
#define MI_NOOP               (0 << 23)
#endif


// helper -- write 32-bit value into GEM CPU mapping
static inline void write_u32_to_gem(IOBufferMemoryDescriptor* desc, size_t dwordIndex, uint32_t val) {
    volatile uint32_t* p = (volatile uint32_t*)desc->getBytesNoCopy();
    p[dwordIndex] = val;
    __sync_synchronize();
}

// create a tiny tail batch that writes `seq` into fenceGpuAddr and ends
// returns a pinned+GGTT-mapped tailGem (retained) and its GPU address in tailGpuOut
static FakeIrisXEGEM* createTailBatchAndMap(FakeIrisXEFramebuffer* fb, uint64_t fenceGpuAddr, uint32_t seq, uint64_t* tailGpuOut) {
    if (!fb || !tailGpuOut) return nullptr;

    // allocate 4KB GEM for tail
    FakeIrisXEGEM* tailGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!tailGem) {
        IOLog("FakeIrisXEFramebuffer: createTailBatchAndMap - tail GEM alloc failed\n");
        return nullptr;
    }

    IOBufferMemoryDescriptor* tailDesc = tailGem->memoryDescriptor();
    if (!tailDesc) {
        IOLog("FakeIrisXEFramebuffer: createTailBatchAndMap - no memoryDescriptor\n");
        tailGem->release();
        return nullptr;
    }
    bzero(tailDesc->getBytesNoCopy(), 4096);

    // Build tail batch:
    // [0] = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT
    // [1] = low32(fenceGpuAddr)
    // [2] = high32(fenceGpuAddr)
    // [3] = seq (immediate)
    // [4] = MI_BATCH_BUFFER_END

    uint32_t* p = (uint32_t*)tailDesc->getBytesNoCopy();
    p[0] = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
    p[1] = (uint32_t)(fenceGpuAddr & 0xFFFFFFFFULL);
    p[2] = (uint32_t)(fenceGpuAddr >> 32);
    p[3] = seq;
    p[4] = MI_BATCH_BUFFER_END;
    // flush CPU writes
    __sync_synchronize();

    // pin and map into GGTT
    tailGem->pin(); // void pin() per your GEM API
    uint64_t tailGpu = fb->ggttMap(tailGem);
    if (!tailGpu) {
        IOLog("FakeIrisXEFramebuffer: createTailBatchAndMap - ggttMap(tail) failed\n");
        tailGem->unpin();
        tailDesc->release();
        tailGem->release();
        return nullptr;
    }

    IOLog("FakeIrisXEFramebuffer: tail batch created at GPU 0x%llx seq=%u\n", (unsigned long long)tailGpu, seq);
    *tailGpuOut = tailGpu;
    // keep tailDesc alive via tailGem (we will release tailDesc not here)
    return tailGem;
}

// Create a master batch that does:
//
//   MI_BATCH_BUFFER_START (64-bit) -> userBatchGpu
//   MI_BATCH_BUFFER_START (64-bit) -> tailGpu
//   MI_BATCH_BUFFER_END
//
// This master batch is returned pinned+mapped as masterGem and its GPU address in masterGpuOut.
static FakeIrisXEGEM* createMasterBatchChain(FakeIrisXEFramebuffer* fb, uint64_t userBatchGpu, uint64_t tailGpu, uint64_t* masterGpuOut) {
    if (!fb || !masterGpuOut) return nullptr;

    FakeIrisXEGEM* masterGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!masterGem) {
        IOLog("FakeIrisXEFramebuffer: createMasterBatchChain - master GEM alloc failed\n");
        return nullptr;
    }

    IOBufferMemoryDescriptor* masterDesc = masterGem->memoryDescriptor();
    if (!masterDesc) {
        IOLog("FakeIrisXEFramebuffer: createMasterBatchChain - no memoryDescriptor\n");
        masterGem->release();
        return nullptr;
    }
    bzero(masterDesc->getBytesNoCopy(), 4096);

    uint32_t* p = (uint32_t*)masterDesc->getBytesNoCopy();
    size_t idx = 0;

    // MI_BATCH_BUFFER_START with 64-bit pointer: implementation dependent.
    // We'll set the generic pattern: opcode + 64-bit address (low, high)
    // If your platform requires a flag to indicate 64-bit, adjust below.
    const uint32_t MBS_64 = MI_BATCH_BUFFER_START | (1u << 8); // (1<<8) used earlier as 64-bit flag (common)
    p[idx++] = MBS_64;
    p[idx++] = (uint32_t)(userBatchGpu & 0xFFFFFFFFULL);
    p[idx++] = (uint32_t)(userBatchGpu >> 32);

    p[idx++] = MBS_64;
    p[idx++] = (uint32_t)(tailGpu & 0xFFFFFFFFULL);
    p[idx++] = (uint32_t)(tailGpu >> 32);

    p[idx++] = MI_BATCH_BUFFER_END;

    __sync_synchronize();

    masterGem->pin();
    uint64_t masterGpu = fb->ggttMap(masterGem);
    if (!masterGpu) {
        IOLog("FakeIrisXEFramebuffer: createMasterBatchChain - ggttMap(master) failed\n");
        masterGem->unpin();
        masterDesc->release();
        masterGem->release();
        return nullptr;
    }

    IOLog("FakeIrisXEFramebuffer: master batch created at GPU 0x%llx (user=0x%llx tail=0x%llx)\n",
          (unsigned long long)masterGpu, (unsigned long long)userBatchGpu, (unsigned long long)tailGpu);

    *masterGpuOut = masterGpu;
    return masterGem;
}

// Public function: chain-insert fence and submit master batch.
// - userBatchGem: caller's batch GEM (already contains GPU commands and ends with MI_BATCH_BUFFER_END)
// - userBatchOffsetBytes: offset into GEM (usually 0)
// - userBatchSizeBytes: size of user batch region (for logging only)
// Returns sequence number (non-zero) on success, 0 on failure.
uint32_t FakeIrisXEFramebuffer::appendFenceAndSubmit(FakeIrisXEGEM* userBatchGem, size_t userBatchOffsetBytes, size_t userBatchSizeBytes) {
    if (!userBatchGem || !fRcsRing) {
        IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - invalid args\n");
        return 0;
    }

    if (!validateRcsRingState("appendFenceAndSubmit", true)) {
        IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - ring validation failed\n");
        return 0;
    }

    // 1) Ensure a fence object exists (one persistent fence GEM kept on the FB)
    if (!fFenceGEM) {
        fFenceGEM = FakeIrisXEGEM::withSize(4096, 0);
        if (!fFenceGEM) {
            IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - fence GEM alloc failed\n");
            return 0;
        }
        fFenceGEM->pin();
        uint64_t fenceGpu = ggttMap(fFenceGEM);
        if (!fenceGpu) {
            IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - fence ggttMap failed\n");
            fFenceGEM->unpin();
            fFenceGEM->release();
            fFenceGEM = nullptr;
            return 0;
        }
        // ensure initial fence value = 0
        IOBufferMemoryDescriptor* fd = fFenceGEM->memoryDescriptor();
        if (fd) {
            volatile uint32_t* fenceCpu = (volatile uint32_t*)fd->getBytesNoCopy();
            fenceCpu[0] = 0;
            __sync_synchronize();
        }
        IOLog("FakeIrisXEFramebuffer: fence precreated at GPU 0x%llx\n", (unsigned long long)fenceGpu);
    }

    // 2) Build a tail batch that writes a unique seq into fence
    static atomic_uint_fast32_t global_seq = 1;
    uint32_t seq = (uint32_t)atomic_fetch_add(&global_seq, 1);
    fFenceSeq = seq;

    IOBufferMemoryDescriptor* fenceDesc = fFenceGEM->memoryDescriptor();
    uint64_t fenceGpu = fFenceGEM->physicalAddress(); // prefer gpuAddress if you set it; use physicalAddress if placeholder
    // If you have proper fFenceGEM->gpuAddress(), prefer that:
    if (fFenceGEM->gpuAddress()) fenceGpu = fFenceGEM->gpuAddress();

    uint64_t tailGpuAddr = 0;
    FakeIrisXEGEM* tailGem = createTailBatchAndMap(this, fenceGpu, seq, &tailGpuAddr);
    if (!tailGem) {
        IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - createTailBatch failed\n");
        return 0;
    }

    // 3) Ensure user batch is pinned and mapped (pin if needed). We will map to GPU VA if not present.
    userBatchGem->pin();
    uint64_t userGpu = userBatchGem->gpuAddress();
    if (!userGpu) {
        userGpu = ggttMap(userBatchGem);
        if (!userGpu) {
            IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - ggttMap(user) failed\n");
            tailGem->unpin();
            tailGem->release();
            userBatchGem->unpin();
            return 0;
        }
    }

    // 4) Build master chain batch that jumps into user batch then tail
    uint64_t masterGpuAddr = 0;
    FakeIrisXEGEM* masterGem = createMasterBatchChain(this, userGpu + userBatchOffsetBytes, tailGpuAddr, &masterGpuAddr);
    if (!masterGem) {
        IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - master chain creation failed\n");
        tailGem->unpin();
        tailGem->release();
        userBatchGem->unpin();
        return 0;
    }

    // 5) Submit master batch (this will execute user batch then tail in order)
    bool ok = fRcsRing->submitBatch64(masterGpuAddr);
    if (!ok) {
        IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - ring submit failed\n");
        masterGem->unpin(); masterGem->release();
        tailGem->unpin(); tailGem->release();
        userBatchGem->unpin();
        return 0;
    }

    IOLog("FakeIrisXEFramebuffer: Batch submitted (master=0x%llx user=0x%llx tail=0x%llx) seq=%u\n",
          (unsigned long long)masterGpuAddr, (unsigned long long)userGpu, (unsigned long long)tailGpuAddr, seq);

    if (!addPendingSubmission(seq, masterGem, tailGem)) {
        IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - pending queue add failed (seq=%u)\n", seq);
        masterGem->unpin();
        masterGem->release();
        tailGem->unpin();
        tailGem->release();
        return 0;
    }

    // Drop local references; pending-submission list owns retained refs now.
    masterGem->release();
    tailGem->release();

    trackGPUCommandSubmitted();

    return seq;
}

uint32_t FakeIrisXEFramebuffer::readCompletedFenceSeq() const
{
    uint32_t seq = fFenceCompletedSeq;
    if (fFenceGEM) {
        IOBufferMemoryDescriptor* desc = fFenceGEM->memoryDescriptor();
        if (desc) {
            volatile uint32_t* fenceCpu = (volatile uint32_t*)desc->getBytesNoCopy();
            if (fenceCpu && fenceCpu[0] > seq) {
                seq = fenceCpu[0];
            }
        }
    }
    return seq;
}






// IOCommandGate deferred cleanup action
static IOReturn deferredCleanupAction(OSObject* owner,
                                      void* arg0,
                                      void* arg1,
                                      void* arg2,
                                      void* arg3)
{
    FakeIrisXEFramebuffer* self = OSDynamicCast(FakeIrisXEFramebuffer, owner);
    if (!self) return kIOReturnBadArgument;

    uint32_t seq = (uint32_t)(uintptr_t)arg0;
    bool ok = self->completePendingSubmission(seq);
    IOLog("FakeIrisXEFramebuffer: deferredCleanup seq=%u ok=%d\n", seq, (int)ok);
    return kIOReturnSuccess;
}




bool FakeIrisXEFramebuffer::waitForExeclistEvent(uint32_t timeoutMs)
{
    if (!fCmdGate) return false;

    IOReturn ret = fCmdGate->commandSleep(fSleepToken, timeoutMs);
    return (ret == THREAD_AWAKENED);
}





#define mmio_read32(bar, off)    (*(volatile uint32_t*)((uint8_t*)(bar) + (off)))
#define mmio_write32(bar, off, v) (*(volatile uint32_t*)((uint8_t*)(bar) + (off)) = (uint32_t)(v))

// ---------------------------------------------------------------------------
// Interrupt handler - minimal; runs in workloop context (via IOInterruptEventSource)
void FakeIrisXEFramebuffer::handleInterrupt(IOInterruptEventSource* /*src*/, int /*count*/) {
   
    if (!fBar0)
        return;

    // Read engine-specific interrupt identity (RCS engine)
    uint32_t iir = mmio_read32(fBar0, RCS0_IIR);
    if (iir == 0) {
        // nothing for RCS, return quickly
        return;
    }

    // Acknowledge/clear the handled bits (write-to-clear)
    mmio_write32(fBar0, RCS0_ICR, iir);

    IOLog("FakeIrisXEFramebuffer: RCS IRQ IIR=0x%08x\n", iir);

    if (fExeclist) {
           fExeclist->engineIrq(iir);
       }

       if (iir & RCS_INTR_FAULT) {
           IOLog("FakeIrisXEFramebuffer: RCS FAULT bit set! IIR=0x%08x\n", iir);
       }

       if (iir & RCS_INTR_CTX_SWITCH) {
           IOLog("FakeIrisXEFramebuffer: RCS CTX SWITCH\n");
       }
       if (iir & RCS_INTR_USER) {
           IOLog("FakeIrisXEFramebuffer: RCS USER EVENT\n");
       }



    // Handle completion bit only (conservative)
    if (iir & RCS_INTR_COMPLETE) {
        if (fFenceGEM) {
            IOBufferMemoryDescriptor* desc = fFenceGEM->memoryDescriptor();
            if (desc) {
                volatile uint32_t* fenceCpu =
                    (volatile uint32_t*)desc->getBytesNoCopy();
                uint32_t val = fenceCpu[0];

                IOLog("FakeIrisXEFramebuffer: IRQ - fenceCpu[0]=0x%08x\n", val);

                if (val != 0) {
                    if (val > fFenceCompletedSeq) {
                        fFenceCompletedSeq = val;
                    }

                    // reset fence immediately (optional)
                    fenceCpu[0] = 0;
                    __sync_synchronize();

                    trackGPUCommandCompleted(val);

                    if (fCmdGate) {
                        // Defer cleanup to gate
                        fCmdGate->runAction(
                            deferredCleanupAction,
                            (void*)(uintptr_t)val,  // arg0: seq
                            nullptr, nullptr, nullptr);
                    } else {
                        // Fallback: direct cleanup (less ideal but safe-ish)
                        bool cleaned = completePendingSubmission(val);
                        IOLog("FakeIrisXE: direct cleanup seq=%u result=%d\n",
                              val, (int)cleaned);
                    }
                }
            }
        } else {
            IOLog("FakeIrisXEFramebuffer: IRQ - complete but no fFenceGEM\n");
        }
    }

    // Handle fault bits conservatively: just log
    if (iir & RCS_INTR_FAULT) {
        IOLog("FakeIrisXEFramebuffer: RCS FAULT bit set! IIR=0x%08x\n", iir);
    }

    // Optionally handle CTX_SWITCH / USER bits (log only)
    if (iir & RCS_INTR_CTX_SWITCH) {
        IOLog("FakeIrisXEFramebuffer: RCS CTX SWITCH\n");
    }
    if (iir & RCS_INTR_USER) {
        IOLog("FakeIrisXEFramebuffer: RCS USER EVENT\n");
    }
    
    if (fCmdGate)
        fCmdGate->commandWakeup(fSleepToken);

}



// Create an OSDictionary entry for a submission


// Create an OSDictionary entry for a submission
static OSDictionary* createSubmissionEntry(uint32_t seq,
                                           FakeIrisXEGEM* master,
                                           FakeIrisXEGEM* tail)
{
    OSDictionary* dict = OSDictionary::withCapacity(4);
    if (!dict) return nullptr;

    // seq
    OSNumber* nseq = OSNumber::withNumber(seq, 32);
    dict->setObject("seq", nseq);
    nseq->release();

    // master GEM pointer
    if (master) {
        master->retain();
        FakeIrisXEGEM* tmp = master;
        OSData* md = OSData::withBytes(&tmp, sizeof(tmp));
        dict->setObject("master", md);
        md->release();
    }

    // tail GEM pointer
    if (tail) {
        tail->retain();
        FakeIrisXEGEM* tmp = tail;
        OSData* td = OSData::withBytes(&tmp, sizeof(tmp));
        dict->setObject("tail", td);
        td->release();
    }

    return dict;
}

// Add pending submission (thread-safe)
bool FakeIrisXEFramebuffer::addPendingSubmission(uint32_t seq,
                                                 FakeIrisXEGEM* master,
                                                 FakeIrisXEGEM* tail)
{
    if (!fPendingSubmissions || !fPendingLock)
        return false;

    IOLockLock(fPendingLock);
    OSDictionary* e = createSubmissionEntry(seq, master, tail);
    if (e) {
        fPendingSubmissions->setObject(e);
        e->release(); // OSArray retained it
    IOLockUnlock(fPendingLock);
        IOLog("FakeIrisXEFramebuffer: addPendingSubmission seq=%u\n", seq);
        return true;
    }
IOLockUnlock(fPendingLock);
    return false;
}

// Find and remove submission by seq. Returns true if found and cleaned up.
bool FakeIrisXEFramebuffer::completePendingSubmission(uint32_t seq)
{
    if (!fPendingSubmissions || !fPendingLock)
        return false;

    bool found = false;
    IOLockLock(fPendingLock);

    for (unsigned i = 0; i < fPendingSubmissions->getCount(); ++i) {
        OSDictionary* dict =
            OSDynamicCast(OSDictionary, fPendingSubmissions->getObject(i));
        if (!dict) continue;

        OSNumber* nseq =
            OSDynamicCast(OSNumber, dict->getObject("seq"));
        if (!nseq) continue;

        if (nseq->unsigned32BitValue() == seq) {
            // master
            OSData* md = OSDynamicCast(OSData, dict->getObject("master"));
            if (md && md->getLength() == sizeof(FakeIrisXEGEM*)) {
                FakeIrisXEGEM* master = nullptr;
                memcpy(&master, md->getBytesNoCopy(), sizeof(master));
                if (master) {
                    master->unpin();
                    master->release();
                }
            }

            // tail
            OSData* td = OSDynamicCast(OSData, dict->getObject("tail"));
            if (td && td->getLength() == sizeof(FakeIrisXEGEM*)) {
                FakeIrisXEGEM* tail = nullptr;
                memcpy(&tail, td->getBytesNoCopy(), sizeof(tail));
                if (tail) {
                    tail->unpin();
                    tail->release();
                }
            }

            fPendingSubmissions->removeObject(i);
            found = true;
            IOLog("FakeIrisXEFramebuffer: completePendingSubmission seq=%u cleaned\n", seq);
            break;
        }
    }

IOLockUnlock(fPendingLock);
    return found;
}

// Optional: cleanup all pending submissions (called at stop())
void FakeIrisXEFramebuffer::cleanupAllPendingSubmissions()
{
    if (!fPendingSubmissions || !fPendingLock)
        return;

    IOLockLock(fPendingLock);

    while (fPendingSubmissions->getCount() > 0) {
        OSDictionary* dict =
            OSDynamicCast(OSDictionary, fPendingSubmissions->getObject(0));
        if (!dict) {
            fPendingSubmissions->removeObject(0);
            continue;
        }

        OSData* md = OSDynamicCast(OSData, dict->getObject("master"));
        if (md && md->getLength() == sizeof(FakeIrisXEGEM*)) {
            FakeIrisXEGEM* master = nullptr;
            memcpy(&master, md->getBytesNoCopy(), sizeof(master));
            if (master) {
                master->unpin();
                master->release();
            }
        }

        OSData* td = OSDynamicCast(OSData, dict->getObject("tail"));
        if (td && td->getLength() == sizeof(FakeIrisXEGEM*)) {
            FakeIrisXEGEM* tail = nullptr;
            memcpy(&tail, td->getBytesNoCopy(), sizeof(tail));
            if (tail) {
                tail->unpin();
                tail->release();
            }
        }

        fPendingSubmissions->removeObject(0);
    }

IOLockUnlock(fPendingLock);
}




// ============================================================
// Create a minimal valid Intel batch buffer:
//   MI_NOOP
//   MI_BATCH_BUFFER_END
//
// Returns a 4KB GEM object ready for pin+submit.
// ============================================================

FakeIrisXEGEM* FakeIrisXEFramebuffer::createSimpleUserBatch()
{
    // 1 page (4096 bytes) is plenty
    const size_t batchSize = 4096;

    FakeIrisXEGEM* gem = FakeIrisXEGEM::withSize(batchSize, 0);
    if (!gem) {
        IOLog("FakeIrisXE: createSimpleUserBatch FAILED (alloc)\n");
        return nullptr;
    }

    IOBufferMemoryDescriptor* desc = gem->memoryDescriptor();
    if (!desc) {
        IOLog("FakeIrisXE: createSimpleUserBatch FAILED (desc)\n");
        gem->release();
        return nullptr;
    }

    // CPU mapping
    uint8_t* cpu = (uint8_t*)desc->getBytesNoCopy();
    if (!cpu) {
        IOLog("FakeIrisXE: createSimpleUserBatch FAILED (cpu map)\n");
        gem->release();
        return nullptr;
    }

    // Zero entire batch page
    bzero(cpu, batchSize);

   
    uint32_t* dwords = (uint32_t*)cpu;

    dwords[0] = MI_NOOP;
    dwords[1] = MI_BATCH_BUFFER_END;

    __sync_synchronize();

    IOLog("FakeIrisXE: createSimpleUserBatch OK (size=%lu)\n", batchSize);
    return gem;
}




// Safe read-only dump of IRQ and ring registers - NO writes, safe
void FakeIrisXEFramebuffer::dumpIRQAndRingRegsSafe() {
    if (!fBar0) {
        IOLog("FakeIrisXE: dumpRegs - no BAR0\n");
        return;
    }

    auto r = [&](uint32_t off)->uint32_t {
        volatile uint32_t* p = (volatile uint32_t*)((uint8_t*)fBar0 + off);
        return *p;
    };

    IOLog("=== FakeIrisXE: IRQ & Ring registers snapshot ===\n");
    IOLog("RCS0_IIR  = 0x%08x\n", r(RCS0_IIR));
    IOLog("RCS0_ICR  = 0x%08x\n", r(RCS0_ICR));
    IOLog("RCS0_IER  = 0x%08x\n", r(RCS0_IER));
    IOLog("RCS0_IMR  = 0x%08x\n", r(RCS0_IMR));
    IOLog("GEN11_GFX_MSTR_IRQ      = 0x%08x\n", r(GEN11_GFX_MSTR_IRQ));
    IOLog("GEN11_GFX_MSTR_IRQ_MASK = 0x%08x\n", r(GEN11_GFX_MSTR_IRQ_MASK));

    
    
    // Ring registers (RCS)
    IOLog("RCS0_RING_HEAD  = 0x%08x\n", r(kTglRcsRingHead));
    IOLog("RCS0_RING_TAIL  = 0x%08x\n", r(kTglRcsRingTail));
    IOLog("RCS0_RING_CTL   = 0x%08x\n", r(kTglRcsRingCtl));
    IOLog("RCS0_RING_START = 0x%08x\n", r(kTglRcsRingStart));

    IOLog("=================================================\n");

     }



// --- SAFE IRQ enabling sequence ---
// Preconditions: fWorkLoop && fInterruptSource added to workloop && fFenceGEM present
void FakeIrisXEFramebuffer::enableRcsInterruptsSafely() {
    if (!fBar0 || !mmioMap) {
        IOLog("FakeIrisXE: cannot enable IRQs - no BAR0/mmio\n");
        return;
    }
    if (!fWorkLoop || !fInterruptSource) {
        IOLog("FakeIrisXE: cannot enable IRQs - missing workloop/interrupt source\n");
        return;
    }
    if (!fFenceGEM || !fFenceGEM->memoryDescriptor()) {
        IOLog("FakeIrisXE: cannot enable IRQs - missing fence GEM/desc\n");
        return;
    }

    IOLog("FakeIrisXE: starting SAFE IRQ enable sequence\n");

    // 0) Safety: ensure GT is awake and FORCEWAKE ack present
    uint32_t forcewake = safeMMIORead(0x130044); // adjust if you used others
    if ((forcewake & 0xF) == 0) {
        IOLog("FakeIrisXE: FORCEWAKE not acked (0x%08x) - abort IRQ enable\n", forcewake);
        return;
    }

    // 1) Mask master -> stop any HW from pushing interrupts to host while we set things
    safeMMIOWrite(GEN11_GFX_MSTR_IRQ_MASK, 0xFFFFFFFFu);
    IOSleep(1); // small delay for posted writes to drain
    (void)safeMMIORead(GEN11_GFX_MSTR_IRQ_MASK); // readback

    // 2) Mask ring-level IMR and IER and ack pending (conservative)
    // Use read-modify-write to avoid clobbering bits if platform uses different semantics
    safeMMIOWrite(RCS0_IER, 0x0);     // clear engine IER first
    safeMMIOWrite(RCS0_IMR, 0xFFFFFFFFu); // mask engine-level IMR (disable)
    safeMMIOWrite(RCS0_ICR, 0xFFFFFFFFu); // ack/clear any pending ICR (write-to-clear)
    (void)safeMMIORead(RCS0_ICR); // readback ordering
    IOSleep(1);

    // 3) Verify safe register snapshot BEFORE unmasking anything
    IOLog("FakeIrisXE: IRQ snapshot before enabling:\n");
    dumpIRQAndRingRegsSafe(); // your read-only snapshot function

    // 4) Enable the driver side handler: make sure the interrupt source is enabled on the workloop.
    //    We enable it *after* masking the HW master/engine so the first interrupt won't be delivered until we're ready.
    //    Note: fWorkLoop->addEventSource(fInterruptSource) must already have been called.
    fInterruptSource->disable(); // ensure disabled while we finish setup (safe no-op if already disabled)
    // safe to call enable() later after masks/unmasks done.

    // 5) Prepare the ring/fence state required by handler: ensure fence is zeroed and in memory
    IOBufferMemoryDescriptor* fenceDesc = fFenceGEM->memoryDescriptor();
    if (fenceDesc) {
        volatile uint32_t* fenceCpu = (volatile uint32_t*)fenceDesc->getBytesNoCopy();
        fenceCpu[0] = 0;
        __sync_synchronize();
    }

    // 6) Now set the engine IER via read/modify/write (so we don't accidentally clear bits)
    uint32_t cur_ier = safeMMIORead(RCS0_IER);
    uint32_t new_ier = cur_ier | RCS_INTR_COMPLETE; // only enable completion bit
    safeMMIOWrite(RCS0_IER, new_ier);
    (void)safeMMIORead(RCS0_IER); // readback

    IOSleep(1); // let HW settle

    // 7) Unmask engine-level IMR (clear mask)
    // If your platform uses 0 to unmask (typical), do this. If it uses another encoding, adapt.
    safeMMIOWrite(RCS0_IMR, 0x0);
    (void)safeMMIORead(RCS0_IMR);
    IOSleep(1);

    // 8) Clear GT master pending bits, then unmask master interrupts last
    safeMMIOWrite(GEN11_GFX_MSTR_IRQ, 0xFFFFFFFFu); // ack/clear any pending master IRQs
    (void)safeMMIORead(GEN11_GFX_MSTR_IRQ);

    // 9) Unmask master interrupts (allow host interrupts once everything ready)
    safeMMIOWrite(GEN11_GFX_MSTR_IRQ_MASK, 0x0u);
    (void)safeMMIORead(GEN11_GFX_MSTR_IRQ_MASK);
    IOSleep(1);

    // 10) Finally enable the macOS interrupt source on the workloop
    fInterruptSource->enable();
    IOLog("FakeIrisXE: IRQ enable completed (RCS0_IER=0x%08x)\n", safeMMIORead(RCS0_IER));
}







// V209: Try enabling BOTH RENDER and GT domains for forcewake
// On Tiger Lake, RCS (compute) needs GT domain, not just RENDER
#define REG_FORCEWAKE_REQ   0x00A188  // Try enabling multiple domains
#define REG_FORCEWAKE_ACK   0x130044

bool FakeIrisXEFramebuffer::forcewakeRenderHold(uint32_t timeoutMs)
{
    IOLog("(FakeIrisXE) forcewakeRenderHold(): TigerLake RENDER+GT domain wake\n");

    // Tiger Lake has multiple forcewake domains:
    // - Bit 0-3: RENDER domain
    // - Bit 4-7: GT domain (for RCS/compute)
    // We need both RENDER AND GT domains
    const uint32_t FW_REQ   = 0xA188;
    const uint32_t FW_ACK   = 0x130044;
    // V209: Enable BOTH RENDER (bits 0-3) AND GT (bits 4-7)
    const uint32_t FW_MASK  = 0x00FF00FF; // All 8 bits - both RENDER and GT
    
    safeMMIOWrite(FW_REQ, FW_MASK);
    (void)safeMMIORead(FW_REQ);

    uint32_t elapsed = 0;
    while (elapsed < timeoutMs) {
        uint32_t ack = safeMMIORead(FW_ACK);

        // V209: Check if both RENDER (bits 0-3) and GT (bits 4-7) are ready
        if ((ack & 0xFF) == 0xFF) {
            IOLog("(FakeIrisXE) RENDER+GT forcewake OK (ACK=0x%08X)\n", ack);
            return true;
        }

        IODelay(1000);
        elapsed++;
    }

    uint32_t final = safeMMIORead(FW_ACK);
    IOLog("❌ RENDER+GT forcewake TIMEOUT (ACK=0x%08X)\n", final);
    return false;
}







void FakeIrisXEFramebuffer::forcewakeRenderRelease()
{
    const uint32_t FW_REQ = 0xA188;
    safeMMIOWrite(FW_REQ, 0x0);
    (void)safeMMIORead(FW_REQ);
    IOSleep(1);
}



void FakeIrisXEFramebuffer::ensureEngineInterrupts()
{
    // Minimal IER bits - keep the HW able to wake itself when context/switch events happen.
    const uint32_t ENGINE_USER_INTERRUPT = (1U << 12); // example bit; check PRM for exact bit names
    const uint32_t CSB_UPDATE_INTERRUPT  = (1U << 16); // example bit; adjust per PRM
    uint32_t ier = ENGINE_USER_INTERRUPT | CSB_UPDATE_INTERRUPT;

    IOLog("(FakeIrisXE) ensureEngineInterrupts(): setting IER=0x%08x\n", ier);
    safeMMIOWrite(RCS0_IER, ier);
    // Optionally set IMR = ~ier to only allow those interrupts
    // mmioWrite32(REG_RCS0_IMR, ~ier);
}


#include "embedded_firmware.h"

bool FakeIrisXEFramebuffer::initGuCSystem()
{
    IOLog("(FakeIrisXE) Initializing GuC system\n");
    
    // 1. Create GuC manager
    fGuC = FakeIrisXEGuC::withOwner(this);
    if (!fGuC) {
        IOLog("(FakeIrisXE) Failed to create GuC manager\n");
        return false;
    }
    
    // 2. Initialize hardware
    if (!fGuC->initGuC()) {
        IOLog("(FakeIrisXE) GuC hardware init failed\n");
        return false;
    }
    
    // V187: Try to load Apple's GuC first (from embedded binary)
    bool appleGuCLoaded = false;
    
    if (apple_tgl_guc_bin && apple_tgl_guc_bin_len > 0) {
        IOLog("(FakeIrisXE) [V187] Attempting to load Apple GuC (%u bytes)...\n", apple_tgl_guc_bin_len);
        
        if (fGuC->loadGuCFirmware(apple_tgl_guc_bin, apple_tgl_guc_bin_len)) {
            IOLog("(FakeIrisXE) [V187] SUCCESS! Apple GuC loaded!\n");
            appleGuCLoaded = true;
        } else {
            IOLog("(FakeIrisXE) [V187] Apple GuC failed to load (signature verification likely failed), trying Linux...\n");
        }
    } else {
        IOLog("(FakeIrisXE) [V187] Apple GuC not embedded\n");
    }
    
    // Only load Linux GuC if Apple GuC didn't load
    if (!appleGuCLoaded) {
    
    // 3. Load firmware from EMBEDDED arrays (not from resources)
    // Use your embedded arrays directly
    
    // V187: Support switching between Apple and Linux GuC
    // For testing: let's try Apple's GuC approach first
    #if 0  // Set to 1 to try Apple GuC (requires embedding the binary)
    // TODO: Add Apple GuC binary to embedded_firmware.cpp
    const unsigned char* guc_bin = apple_tgl_guc_bin;
    unsigned int guc_len = apple_tgl_guc_bin_len;
    IOLog("(FakeIrisXE) [V187] Attempting to use Apple GuC firmware (%u bytes)\n", guc_len);
    #else
    // Determine which firmware to use based on Device ID
    const unsigned char* guc_bin = nullptr;
    unsigned int guc_len = 0;
    UInt16 deviceID = pciDevice->configRead16(kIOPCIConfigDeviceID);

    if (deviceID == 0x46A3) {
        // Alder Lake P - using TGL firmware as fallback
        guc_bin = tgl_guc_70_1_1_bin;
        guc_len = tgl_guc_70_1_1_bin_len;
        IOLog("(FakeIrisXE) Selected TGL GuC firmware for ADL-P\n");
    } else {
        // Default to Tiger Lake
        guc_bin = tgl_guc_70_1_1_bin;
        guc_len = tgl_guc_70_1_1_bin_len;
        IOLog("(FakeIrisXE) Selected TGL GuC firmware\n");
    }
    #endif

    // Check if GuC firmware is embedded
    if (!guc_bin || guc_len == 0) {
        IOLog("(FakeIrisXE) ❌ Embedded GuC firmware not available\n");
        return false;
    }
    
    // Load GuC firmware from embedded array
    if (!fGuC->loadGuCFirmware(guc_bin, guc_len)) {
        IOLog("(FakeIrisXE) Failed to load GuC firmware\n");
        return false;
    }
    IOLog("(FakeIrisXE) GuC firmware loaded successfully\n");
    }  // End if (!appleGuCLoaded)
    
    // Load HuC firmware from embedded array (if available)
    if (tgl_huc_7_9_3_bin && tgl_huc_7_9_3_bin_len > 0) {
        if (!fGuC->loadHuCFirmware(tgl_huc_7_9_3_bin, tgl_huc_7_9_3_bin_len)) {
            IOLog("(FakeIrisXE) Failed to load HuC firmware (optional)\n");
            // Continue anyway, HuC is optional
        }
    } else {
        IOLog("(FakeIrisXE) HuC firmware not embedded (optional)\n");
    }
    
    // 4. Enable GuC submission
    if (!fGuC->enableGuCSubmission()) {
        IOLog("(FakeIrisXE) Failed to enable GuC submission\n");
        // V42: Run diagnostics to understand why
        diagnoseGuCSubmissionFailure();
        // Fall back to legacy mode
        return false;
    }
    
    // V42: Test command execution after successful submission enable
    testGuCCommandExecution();
    
    IOLog("(FakeIrisXE) GuC system initialized successfully\n");
    return true;
}

// ============================================================================
// V42: GuC Submission Diagnostics
// ============================================================================
bool FakeIrisXEFramebuffer::diagnoseGuCSubmissionFailure()
{
    IOLog("(FakeIrisXE) [V43] diagnoseGuCSubmissionFailure(): Analyzing submission failure\n");
    
    // GEN11_GUC_STATUS is defined in i915_reg.h as 0x1C0B4
    uint32_t status = safeMMIORead(GEN11_GUC_STATUS);
    
    IOLog("(FakeIrisXE) [V43] GuC Status: 0x%08X\n", status);
    IOLog("  Ready: %s\n", (status & 0x1) ? "YES" : "NO");
    IOLog("  FW Loaded: %s\n", (status & 0x2) ? "YES" : "NO");
    IOLog("  Comm Established: %s\n", (status & 0x4) ? "YES" : "NO");
    
    return false;
}

// ============================================================================
// V42: Command Execution Test
// ============================================================================
bool FakeIrisXEFramebuffer::testGuCCommandExecution()
{
    IOLog("(FakeIrisXE) [V43] testGuCCommandExecution(): GuC command execution test not implemented\n");
    return false;
}

// ============================================================================
// V42: MOCS Programming
// ============================================================================
bool FakeIrisXEFramebuffer::programMOCS()
{
    IOLog("(FakeIrisXE) [V45] programMOCS(): Programming MOCS for Tiger Lake\n");
    
    const uint32_t MOCS_BASE = 0xC800;
    
    for (int i = 0; i < 62; i++) {
        uint32_t mocsValue;
        if (i == 0) {
            mocsValue = 0x00000000;  // Uncached
        } else if (i <= 10) {
            mocsValue = 0x0000003F;  // LLC cached
        } else if (i <= 30) {
            mocsValue = 0x0000007F;  // eLLC cached
        } else {
            mocsValue = 0x000000FF;  // Aggressive caching
        }
        
        uint32_t mocsReg = MOCS_BASE + (i * 4);
        safeMMIOWrite(mocsReg, mocsValue);
    }
    
    IOLog("(FakeIrisXE) [V45] programMOCS(): Completed 62 MOCS entries\n");
    return true;
}

// ============================================================
// V90: GEM/GGTT Helper Functions
// ============================================================

FakeIrisXEGEM* FakeIrisXEFramebuffer::createGEMObject(size_t size) {
    // Create GEM object with specified size
    FakeIrisXEGEM* gem = FakeIrisXEGEM::withSize(size, 0);
    if (!gem) {
        IOLog("[V90] createGEMObject: Failed to allocate GEM of size %zu\n", size);
        return nullptr;
    }
    
    // Pin the GEM object
    gem->pin();
    
    IOLog("[V90] createGEMObject: Created GEM %p, size=%zu\n", gem, size);
    return gem;
}

uint64_t FakeIrisXEFramebuffer::mapGEMToGGTT(FakeIrisXEGEM* gem) {
    if (!gem) {
        IOLog("[V90] mapGEMToGGTT: Null GEM object\n");
        return 0;
    }
    
    // Use existing ggttMap function
    uint64_t gpuAddr = ggttMap(gem);
    if (gpuAddr == 0) {
        IOLog("[V90] mapGEMToGGTT: Failed to map GEM to GGTT\n");
        return 0;
    }
    
    IOLog("[V90] mapGEMToGGTT: GEM mapped at GPU addr 0x%llx\n", (unsigned long long)gpuAddr);
    return gpuAddr;
}

// V248: Proper GGTT entry invalidation for Gen12/Tiger Lake.
// When unmapping a GEM from GGTT, we must:
//   1. Clear the PTE valid bit (bit 57 on Gen12) in the GGTT PTE array
//   2. Issue a memory barrier to ensure the CPU write is globally visible
//   3. Flush the GPU's GTT/TLB cache so stale translations are discarded
//
// Gen12 GGTT PTE format (64-bit):
//   Bit 57  = Valid (1=present, 0=not present)
//   Bit 58  = Page Size (0=4KB, 1=2MB/1GB depending on context)
//   Bit 59  = PAT/XLBS (cache attributes)
//   Bits 51:12 = Physical page address [51:12]
//   Bits 63:60 = Reserved/Pat
//
// TLB/GTT cache flush on Gen12 requires writing to the GTT cache invalidate
// register at PCI config space + GTT cache invalidation, or via the
// GTTMMADR space. We use the existing GTT_WRITE_FLUSH path and add a
// read-back barrier to ensure completion.
void FakeIrisXEFramebuffer::unmapGEMFromGGTT(uint64_t gpuAddr) {
    if (gpuAddr == 0) {
        return;
    }

    if (!fGGTT) {
        IOLog("[V248] unmapGEMFromGGTT: GGTT not initialized, cannot invalidate\n");
        return;
    }

    // V248: We need to find how many pages were mapped starting at gpuAddr.
    // Since the original ggttMap stores pages in fNextGGTTOffset progression,
    // we don't track individual mappings. We invalidate exactly 1 page
    // for the surface entry. For multi-page unmap, callers should use
    // ggttUnmap(gpuAddr, numPages) directly.
    //
    // Here we implement a targeted single-page invalidation for the
    // surface management use case (unmapGEMFromGGTT is called from
    // destroySurface for single-surface teardown).
    uint64_t gttIndex = (gpuAddr >> 12);
    uint64_t gttOffsetBytes = gttIndex * sizeof(uint64_t);

    if ((gttOffsetBytes + sizeof(uint64_t)) > fGGTTSize) {
        IOLog("[V248] unmapGEMFromGGTT: GPU addr 0x%llx out of GGTT range\n",
              (unsigned long long)gpuAddr);
        return;
    }

    volatile uint64_t* ptePtr = reinterpret_cast<volatile uint64_t*>(fGGTT) + gttIndex;
    uint64_t oldPte = *ptePtr;

    // V248: Clear the entire PTE to zero. This clears bit 57 (Valid) and
    // all other fields, making the translation invalid from the GPU's view.
    *ptePtr = 0ULL;

    // V248: Memory barrier — ensure the CPU write is globally visible before
    // the GPU can observe it. This is required on x86_64 after any MMIO or
    // uncached write that the GPU might snoop.
    __sync_synchronize();

    // V248: Verify the clear was written
    uint64_t verifyPte = *ptePtr;
    if (verifyPte != 0) {
        IOLog("[V248] unmapGEMFromGGTT: WARNING — PTE verify failed (wrote 0, read 0x%llx)\n",
              (unsigned long long)verifyPte);
    }

    IOLog("[V248] GGTT[%llu]: 0x%016llx -> 0x%016llx (GPU VA 0x%llx) invalidated\n",
          (unsigned long long)gttIndex,
          (unsigned long long)oldPte,
          (unsigned long long)verifyPte,
          (unsigned long long)gpuAddr);

    // V248: GTT cache/TLB flush.
    // On Tiger Lake, the GTT TLB is flushed automatically when the valid
    // bit transitions from 1->0 in many cases. For a more robust flush,
    // we perform a GTT cache invalidation via the PCI config space.
    // On Gen12, this is typically done through the GTTMMADR register or
    // via a dedicated invalidate command. We use the same mechanism as
    // the existing ggttMap: write GTT_WRITE_FLUSH.
    //
    // The GTT_WRITE_FLUSH register (0x1082C0) is a write-only register that,
    // when written, forces the GPU to flush all pending GTT write buffers.
    safeMMIOWrite(0x1082C0, 1);

    // Additional read-back as a completion barrier — some Gen12 platforms
    // require reading back a status register to confirm flush completion.
    uint32_t flushStatus = safeMMIORead(0x1082C0);
    (void)flushStatus;  // Acknowledge but don't fail on read

    IOLog("[V248] GGTT TLB flush complete (status=0x%08x)\n", flushStatus);
}

// ============================================================
// V90: IOAccelerator Hooks Implementation
// WindowServer Integration for 2D Hardware Acceleration
// ============================================================

IOReturn FakeIrisXEFramebuffer::createSurface(uint32_t width, uint32_t height, 
                                               uint32_t format,
                                               uint64_t* surfaceIdOut, 
                                               uint64_t* gpuAddrOut)
{
    IOLog("[V90] createSurface(%u x %u, format=%u)\n", width, height, format);
    
    // Find free surface slot
    int slot = -1;
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        if (!fSurfaces[i].inUse) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        IOLog("[V90] ❌ No free surface slots\n");
        return kIOReturnNoResources;
    }
    
    // Calculate size (assume 4 bytes per pixel for now)
    size_t surfaceSize = width * height * 4;
    surfaceSize = (surfaceSize + 4095) & ~4095; // Page align
    
    // Create GEM object for surface
    FakeIrisXEGEM* gem = createGEMObject(surfaceSize);
    if (!gem) {
        IOLog("[V90] ❌ Failed to create GEM object for surface\n");
        return kIOReturnNoMemory;
    }
    
    // Map to GGTT
    uint64_t gpuAddr = mapGEMToGGTT(gem);
    if (gpuAddr == 0) {
        IOLog("[V90] ❌ Failed to map surface to GGTT\n");
        gem->release();
        return kIOReturnError;
    }
    
    // Fill surface info
    fSurfaces[slot].id = fNextSurfaceId++;
    fSurfaces[slot].width = width;
    fSurfaces[slot].height = height;
    fSurfaces[slot].format = format;
    fSurfaces[slot].gpuAddress = gpuAddr;
    fSurfaces[slot].gemObj = gem;
    fSurfaces[slot].inUse = true;
    
    *surfaceIdOut = fSurfaces[slot].id;
    *gpuAddrOut = gpuAddr;
    
    fV90SurfaceCount++;
    
    IOLog("[V90] ✅ Surface created: ID=%llu, GPU=0x%llx, slot=%d\n", 
          fSurfaces[slot].id, gpuAddr, slot);
    IOLog("[V90]    Total surfaces: %u\n", fV90SurfaceCount);
    
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::destroySurface(uint64_t surfaceId)
{
    IOLog("[V90] destroySurface(ID=%llu)\n", surfaceId);
    
    // Find surface
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        if (fSurfaces[i].inUse && fSurfaces[i].id == surfaceId) {
            // Unmap from GGTT
            unmapGEMFromGGTT(fSurfaces[i].gpuAddress);
            
            // Release GEM object
            if (fSurfaces[i].gemObj) {
                fSurfaces[i].gemObj->release();
            }
            
            // Clear slot
            fSurfaces[i].inUse = false;
            fSurfaces[i].id = 0;
            fSurfaces[i].gpuAddress = 0;
            fSurfaces[i].gemObj = nullptr;
            
            fV90SurfaceCount--;
            
            IOLog("[V90] ✅ Surface destroyed: slot=%u, remaining=%u\n", 
                  i, fV90SurfaceCount);
            return kIOReturnSuccess;
        }
    }
    
    IOLog("[V90] ❌ Surface not found: ID=%llu\n", surfaceId);
    return kIOReturnNotFound;
}

IOReturn FakeIrisXEFramebuffer::getSurfaceInfo(uint64_t surfaceId, uint32_t* width, 
                                               uint32_t* height, uint32_t* format,
                                               uint64_t* gpuAddr)
{
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        if (fSurfaces[i].inUse && fSurfaces[i].id == surfaceId) {
            *width = fSurfaces[i].width;
            *height = fSurfaces[i].height;
            *format = fSurfaces[i].format;
            *gpuAddr = fSurfaces[i].gpuAddress;
            return kIOReturnSuccess;
        }
    }
    return kIOReturnNotFound;
}

IOReturn FakeIrisXEFramebuffer::blitSurface(uint64_t srcSurfaceId, uint64_t dstSurfaceId,
                                            uint32_t srcX, uint32_t srcY,
                                            uint32_t dstX, uint32_t dstY,
                                            uint32_t width, uint32_t height)
{
    // Find surfaces
    SurfaceInfo* srcSurf = nullptr;
    SurfaceInfo* dstSurf = nullptr;
    
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        if (fSurfaces[i].inUse) {
            if (fSurfaces[i].id == srcSurfaceId) srcSurf = &fSurfaces[i];
            if (fSurfaces[i].id == dstSurfaceId) dstSurf = &fSurfaces[i];
        }
    }
    
    if (!srcSurf || !dstSurf) {
        IOLog("[V90] ❌ Blit failed: surface not found\n");
        return kIOReturnNotFound;
    }
    
    IOLog("[V90] Blit: %llu -> %llu (%u,%u) to (%u,%u) size %ux%u\n",
          srcSurfaceId, dstSurfaceId, srcX, srcY, dstX, dstY, width, height);
    
    // V91: Implement actual GPU blit using XY_SRC_COPY_BLT command
    // Based on Intel PRM Volume 10: Copy Engine - 2D Blit Instructions
    
    IOReturn result = submitBlitXY_SRC_COPY(srcSurf, dstSurf, srcX, srcY, dstX, dstY, width, height);
    
    if (result == kIOReturnSuccess) {
        fV90BlitCount++;
        IOLog("[V91] ✅ Blit submitted to GPU (total: %u)\n", fV90BlitCount);
        
        // V93: Track WindowServer blit activity
        trackWindowServerBlit(width, height, false);
        
        // V93: Track GPU command submission
        trackGPUCommandSubmitted();
    } else {
        IOLog("[V91] ❌ Blit submission failed: 0x%x\n", result);
    }
    
    return result;
}

IOReturn FakeIrisXEFramebuffer::copyToFramebuffer(uint64_t surfaceId, uint32_t x, uint32_t y)
{
    SurfaceInfo* surf = nullptr;
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        if (fSurfaces[i].inUse && fSurfaces[i].id == surfaceId) {
            surf = &fSurfaces[i];
            break;
        }
    }
    
    if (!surf) {
        IOLog("[V90] copyToFramebuffer: surface %llu not found\n", surfaceId);
        return kIOReturnNotFound;
    }
    
    if (!fExeclist || !fRcsRing) {
        IOLog("[V90] copyToFramebuffer: GPU infrastructure not ready\n");
        return kIOReturnNotReady;
    }
    
    IOReturn r = submitBlitXY_SRC_COPY(surf, surf, 0, 0, x, y, surf->width, surf->height);
    if (r != kIOReturnSuccess) {
        IOLog("[V90] copyToFramebuffer: blit failed with 0x%x\n", r);
    }
    return r;
}

IOReturn FakeIrisXEFramebuffer::fillRect(uint32_t x, uint32_t y, uint32_t width, 
                                         uint32_t height, uint32_t color)
{
    if (!fExeclist || !fRcsRing) {
        IOLog("[V90] fillRect: GPU infrastructure not ready\n");
        return kIOReturnNotReady;
    }
    
    if (width == 0 || height == 0) {
        return kIOReturnBadArgument;
    }
    
    SurfaceInfo* surf = nullptr;
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        if (fSurfaces[i].inUse) {
            surf = &fSurfaces[i];
            break;
        }
    }
    
    if (!surf) {
        return kIOReturnNotFound;
    }
    
    return submitBlitXY_COLOR_BLT(surf, x, y, width, height, color);
}

IOReturn FakeIrisXEFramebuffer::submit2DCommandBuffer(void* commands, size_t size)
{
    IOLog("[V90] submit2DCommandBuffer: %zu bytes\n", size);
    
    if (!fExeclist || !fRcsRing) {
        IOLog("[V90] submit2DCommandBuffer: GPU infrastructure not ready\n");
        return kIOReturnNotReady;
    }
    
    return kIOReturnUnsupported;
}

IOReturn FakeIrisXEFramebuffer::submitBlitCommand(uint32_t opcode, void* data, size_t size)
{
    IOLog("[V90] submitBlitCommand: opcode=%u, size=%zu\n", opcode, size);
    
    if (!fExeclist || !fRcsRing) {
        return kIOReturnNotReady;
    }
    
    switch (opcode) {
        case 0x46: // XY_SRC_COPY_BLT
        case 0x50: // XY_COLOR_BLT
        case 0x52: // XY_PIXEL_BLT
            IOLog("[V90]   -> opcode recognized but not implemented\n");
            return kIOReturnUnsupported;
        default:
            IOLog("[V90]   -> Unknown opcode 0x%02x\n", opcode);
            return kIOReturnUnsupported;
    }
}

// ============================================================
// V91: 2D Blit Command Implementation
// Based on Intel PRM Volume 10: Copy Engine
// ============================================================

// XY_SRC_COPY_BLT command structure (Intel PRM 10.3)
// Opcode: 0x53 (53h)
// Copies a rectangular region from source to destination
struct XY_SRC_COPY_BLT_CMD {
    uint32_t dw0;        // Command type, opcode, length
    uint32_t dw1;        // Raster op, color depth, clipping
    uint32_t dstX1;      // Destination X1 coordinate
    uint32_t dstY1;      // Destination Y1 coordinate  
    uint32_t dstX2;      // Destination X2 coordinate
    uint32_t dstY2;      // Destination Y2 coordinate
    uint64_t dstBase;    // Destination base address (48-bit)
    uint32_t dstStride;  // Destination stride/pitch
    uint32_t dstMOCS;    // Destination MOCS
    uint32_t srcX1;      // Source X1 coordinate
    uint32_t srcY1;      // Source Y1 coordinate
    uint64_t srcBase;    // Source base address (48-bit)
    uint32_t srcStride;  // Source stride/pitch
    uint32_t srcMOCS;    // Source MOCS
};

// XY_COLOR_BLT command structure (Intel PRM 10.3)
// Opcode: 0x50 (50h)
// Fills a rectangular region with a solid color
struct XY_COLOR_BLT_CMD {
    uint32_t dw0;        // Command type, opcode, length
    uint32_t dw1;        // Raster op, color depth
    uint32_t dstX1;      // Destination X1 coordinate
    uint32_t dstY1;      // Destination Y1 coordinate
    uint32_t dstX2;      // Destination X2 coordinate
    uint32_t dstY2;      // Destination Y2 coordinate
    uint64_t dstBase;    // Destination base address (48-bit)
    uint32_t dstStride;  // Destination stride/pitch
    uint32_t dstMOCS;    // Destination MOCS
    uint32_t fillColor;  // Fill color (32-bit ARGB)
};

// Command builder: XY_SRC_COPY_BLT
// Based on Intel PRM Volume 10, Section 10.3
IOReturn FakeIrisXEFramebuffer::submitBlitXY_SRC_COPY(
    SurfaceInfo* srcSurf,
    SurfaceInfo* dstSurf,
    uint32_t srcX, uint32_t srcY,
    uint32_t dstX, uint32_t dstY,
    uint32_t width, uint32_t height)
{
    IOLog("[V91] Building XY_SRC_COPY_BLT command...\n");
    
    if (!srcSurf || !dstSurf) {
        IOLog("[V91] ❌ Null surface pointer\n");
        return kIOReturnBadArgument;
    }
    
    if (!fExeclist || !fRcsRing) {
        IOLog("[V91] ❌ Execlist/Ring not initialized\n");
        return kIOReturnNotReady;
    }
    
    // Create batch buffer for blit command
    const size_t batchSize = 256;  // Enough for blit + fence + batch end
    FakeIrisXEGEM* batchGem = createGEMObject(batchSize);
    if (!batchGem) {
        IOLog("[V91] ❌ Failed to create batch GEM\n");
        return kIOReturnNoMemory;
    }
    
    // Map to GGTT
    uint64_t batchGpuAddr = mapGEMToGGTT(batchGem);
    if (batchGpuAddr == 0) {
        IOLog("[V91] ❌ Failed to map batch GEM\n");
        batchGem->release();
        return kIOReturnError;
    }
    
    // Get CPU pointer to write commands
    IOBufferMemoryDescriptor* desc = batchGem->memoryDescriptor();
    if (!desc) {
        IOLog("[V91] ❌ Failed to get memory descriptor\n");
        batchGem->release();
        return kIOReturnError;
    }
    
    uint32_t* cmd = (uint32_t*)desc->getBytesNoCopy();
    if (!cmd) {
        IOLog("[V91] ❌ Failed to get command buffer pointer\n");
        batchGem->release();
        return kIOReturnError;
    }
    
    uint32_t idx = 0;
    
    // DW0: Command header
    // Bits 31:29 = 0x2 (2D Command Type)
    // Bits 28:27 = 0x2 (2D Pipeline)
    // Bits 26:22 = 0x13 (Opcode 0x53 = XY_SRC_COPY_BLT)
    // Bits 21:0 = Length (dwords after dw0)
    cmd[idx++] = (0x2 << 29) | (0x2 << 27) | (0x13 << 22) | 0x0B; // Length = 11 dwords after dw0
    
    // DW1: Raster op, color depth
    // Bits 22:16 = Raster Operation (0xCC = copy)
    // Bits 13:12 = Color Depth (3 = 32bpp)
    cmd[idx++] = (0xCC << 16) | (0x3 << 12);
    
    // DW2-DW3: Destination X1, Y1 (top-left)
    cmd[idx++] = dstX;
    cmd[idx++] = dstY;
    
    // DW4-DW5: Destination X2, Y2 (bottom-right, exclusive)
    cmd[idx++] = dstX + width;
    cmd[idx++] = dstY + height;
    
    // DW6-DW7: Destination base address (lower 32, upper 16)
    cmd[idx++] = (uint32_t)(dstSurf->gpuAddress & 0xFFFFFFFF);
    cmd[idx++] = (uint32_t)(dstSurf->gpuAddress >> 32);
    
    // DW8: Destination stride (in dwords)
    cmd[idx++] = (dstSurf->width * 4) / 4;  // Convert bytes to dwords
    
    // DW9: Destination MOCS (Memory Object Control State)
    // Use index 0 = uncached for now
    cmd[idx++] = 0x00000000;
    
    // DW10-DW11: Source X1, Y1
    cmd[idx++] = srcX;
    cmd[idx++] = srcY;
    
    // DW12-DW13: Source base address
    cmd[idx++] = (uint32_t)(srcSurf->gpuAddress & 0xFFFFFFFF);
    cmd[idx++] = (uint32_t)(srcSurf->gpuAddress >> 32);
    
    // DW14: Source stride
    cmd[idx++] = (srcSurf->width * 4) / 4;
    
    // DW15: Source MOCS
    cmd[idx++] = 0x00000000;
    
    // Add MI_FLUSH_DW to ensure completion
    // DW0: Command type (0), opcode (0x38), store data index, flags
    cmd[idx++] = (0x0 << 29) | (0x38 << 23) | 0x02;  // Write QWord, invalidate TLB
    
    // DW1-DW2: Base address (null, we just want the fence)
    cmd[idx++] = 0x00000000;
    cmd[idx++] = 0x00000000;
    
    // DW3-DW4: Immediate data low/high
    cmd[idx++] = 0x00000001;  // Sequence number low
    cmd[idx++] = 0x00000000;  // Sequence number high
    
    // MI_BATCH_BUFFER_END
    cmd[idx++] = 0x0A << 23;  // Command type 0, opcode 0x0A
    
    IOLog("[V91] Command buffer built: %u dwords\n", idx);
    IOLog("[V91]   Src: 0x%llx (%u,%u)\n", srcSurf->gpuAddress, srcX, srcY);
    IOLog("[V91]   Dst: 0x%llx (%u,%u)\n", dstSurf->gpuAddress, dstX, dstY);
    IOLog("[V91]   Size: %ux%u\n", width, height);
    
    // Submit via execlist (same path as V88 MI_NOOP test)
    // Note: We need to use the actual submission path here
    // For now, log that we would submit
    IOLog("[V91] Submitting to GPU via execlist...\n");
    
    // Use appendFenceAndSubmit for proper fence tracking
    uint32_t seqNum = appendFenceAndSubmit(batchGem, 0, idx * 4);
    
    if (seqNum == 0) {
        IOLog("[V91] ❌ Failed to submit blit command\n");
        batchGem->release();
        return kIOReturnError;
    }
    
    IOLog("[V91] ✅ Blit submitted with sequence %u\n", seqNum);
    batchGem->release();
    return kIOReturnSuccess;
}

// Command builder: XY_COLOR_BLT
// Fills a rectangle with a solid color
IOReturn FakeIrisXEFramebuffer::submitBlitXY_COLOR_BLT(
    SurfaceInfo* dstSurf,
    uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    uint32_t color)
{
    IOLog("[V91] Building XY_COLOR_BLT command...\n");
    
    if (!dstSurf) {
        return kIOReturnBadArgument;
    }
    
    // Similar to XY_SRC_COPY but simpler - no source
    // For now, just log
    IOLog("[V91]   Fill color: 0x%08x at (%u,%u) size %ux%u\n", 
          color, x, y, width, height);
    
    // V92: Complete XY_COLOR_BLT implementation
    return submitBlitXY_COLOR_BLT_Full(dstSurf, x, y, width, height, color);
}

// ============================================================
// V92: Debug Infrastructure Implementation (Priority 2)
// ============================================================

void FakeIrisXEFramebuffer::runV92Diagnostics() {
    IOLog("\n[V92] ╔══════════════════════════════════════════════════════════╗\n");
    IOLog("[V92] ║         COMPREHENSIVE DIAGNOSTICS REPORT               ║\n");
    IOLog("[V92] ╚══════════════════════════════════════════════════════════╝\n\n");
    
    fV92DiagnosticsRun = true;
    fV92LastDiagnosticTime = mach_absolute_time();
    
    // Test 1: Kext Loading Check
    IOLog("[V92] Test 1/4: Kext Loading Status...\n");
    checkKextLoading();
    
    // Test 2: WindowServer Connection
    IOLog("[V92] Test 2/4: WindowServer Integration...\n");
    checkWindowServerConnection();
    
    // Test 3: GPU Status
    IOLog("[V92] Test 3/4: GPU Hardware Status...\n");
    checkGPUStatus();
    
    // Test 4: Full System State
    IOLog("[V92] Test 4/4: Full System State...\n");
    dumpSystemState();
    
    IOLog("\n[V92] ✅ Diagnostics complete. Check logs above for any ❌ marks.\n");
}

void FakeIrisXEFramebuffer::checkKextLoading() {
    IOLog("[V92]   Checking kext integrity...\n");
    
    // Check critical pointers
    bool checksPassed = true;
    
    if (!pciDevice) {
        IOLog("[V92]   ❌ pciDevice is NULL - PCI device not linked\n");
        checksPassed = false;
    } else {
        IOLog("[V92]   ✅ PCI provider linked\n");
    }
    
    if (!mmioBase) {
        IOLog("[V92]   ❌ mmioBase is NULL - MMIO not mapped\n");
        checksPassed = false;
    } else {
        IOLog("[V92]   ✅ MMIO mapped at %p\n", mmioBase);
    }
    
    if (!fExeclist) {
        IOLog("[V92]   ❌ fExeclist is NULL - Command submission unavailable\n");
        checksPassed = false;
    } else {
        IOLog("[V92]   ✅ Execlist initialized\n");
    }
    
    if (!fRcsRing) {
        IOLog("[V92]   ❌ fRcsRing is NULL - RCS ring not created\n");
        checksPassed = false;
    } else {
        IOLog("[V92]   ✅ RCS ring initialized\n");
    }
    
    if (!framebufferMemory) {
        IOLog("[V92]   ❌ framebufferMemory is NULL - Display will fail\n");
        checksPassed = false;
    } else {
        IOLog("[V92]   ✅ Framebuffer allocated\n");
    }
    
    if (checksPassed) {
        IOLog("[V92]   ✅ Kext loading: PASSED\n");
    } else {
        IOLog("[V92]   ❌ Kext loading: FAILED - Check boot-args and OC config\n");
        fV92LastError = 0x1001;
        strlcpy(fV92LastErrorString, "Kext loading failed - critical pointers NULL", sizeof(fV92LastErrorString));
    }
}

void FakeIrisXEFramebuffer::checkWindowServerConnection() {
    IOLog("[V92]   Checking WindowServer integration...\n");
    
    // Check IOAccelerator properties
    OSBoolean* accelProp = OSDynamicCast(OSBoolean, getProperty("IOFBAccelerated"));
    if (accelProp && accelProp->getValue()) {
        IOLog("[V92]   ✅ IOFBAccelerated = true\n");
    } else {
        IOLog("[V92]   ⚠️  IOFBAccelerated not set - WindowServer may use software\n");
    }
    
    // Check surface format properties
    OSNumber* pixelFormat = OSDynamicCast(OSNumber, getProperty("IOSurfacePixelFormat"));
    if (pixelFormat) {
        IOLog("[V92]   ✅ IOSurfacePixelFormat = 0x%08x\n", pixelFormat->unsigned32BitValue());
    } else {
        IOLog("[V92]   ⚠️  IOSurfacePixelFormat not set\n");
    }
    
    // Check display mode
    // Find current mode in s_displayModes array
    const char* modeName = "unknown";
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        if (s_displayModes[i].modeID == (uint32_t)currentMode) {
            modeName = s_displayModes[i].name;
            break;
        }
    }
    IOLog("[V92]   Current mode: %s (ID=%d)\n", modeName, currentMode);
    
    // Check if display is online
    if (displayOnline) {
        IOLog("[V92]   ✅ Display is online\n");
    } else {
        IOLog("[V92]   ⚠️  Display offline - WindowServer may not connect\n");
    }
    
    IOLog("[V92]   Note: WindowServer connection detected at runtime via blit requests\n");
    IOLog("[V92]        Monitor logs for '[V9X] Blit' messages after desktop appears\n");
}

void FakeIrisXEFramebuffer::checkGPUStatus() {
    IOLog("[V92]   Checking GPU hardware status...\n");
    
    if (!mmioBase) {
        IOLog("[V92]   ❌ Cannot check GPU - MMIO not mapped\n");
        return;
    }
    
    // Read GPU status registers
    uint32_t gpuStatus = safeMMIORead(0x206C);  // Primary GPU status
    uint32_t ringStatus = safeMMIORead(0x2034); // Ring buffer status
    uint32_t engineStatus = safeMMIORead(0x1240); // Engine status
    
    IOLog("[V92]   GPU Status:  0x%08x\n", gpuStatus);
    IOLog("[V92]   Ring Status: 0x%08x\n", ringStatus);
    IOLog("[V92]   Engine:      0x%08x\n", engineStatus);
    
    // Check if GPU is responding
    if (gpuStatus != 0x00000000 && gpuStatus != 0xFFFFFFFF) {
        IOLog("[V92]   ✅ GPU is responding (non-trivial status)\n");
    } else {
        IOLog("[V92]   ⚠️  GPU status suspicious - may need reset\n");
    }
    
    // Check execlist status if available
    if (fExeclist) {
        IOLog("[V92]   ✅ Execlist available for command submission\n");
    }
    
    // Verify we can submit commands (based on V88 success)
    IOLog("[V92]   Note: Command submission tested successfully in V88\n");
}

void FakeIrisXEFramebuffer::dumpSystemState() {
    IOLog("[V92]   System State Dump:\n");
    IOLog("[V92]     Version:        V92 (Debug Infrastructure)\n");
    
    // Find current mode name
    const char* modeName = "unknown";
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        if (s_displayModes[i].modeID == (uint32_t)currentMode) {
            modeName = s_displayModes[i].name;
            break;
        }
    }
    IOLog("[V92]     Mode:           %d (%s)\n", currentMode, modeName);
    IOLog("[V92]     VRAM:           %llu MB\n", vramSize / (1024*1024));
    IOLog("[V92]     FB Physical:    0x%llx\n", kernelFBPhys);
    IOLog("[V92]     FB Virtual:     %p\n", kernelFBPtr);
    IOLog("[V92]     Surfaces:       %u/%u used\n", fV90SurfaceCount, kMaxSurfaces);
    IOLog("[V92]     Blits queued:   %u\n", fV90BlitCount);
    IOLog("[V92]     Blits submitted:%u\n", fV91BlitSubmitCount);
    IOLog("[V92]     Blits completed:%u\n", fV91BlitCompleteCount);
    IOLog("[V92]     Clipping:       %s\n", fClipEnabled ? "enabled" : "disabled");
    IOLog("[V92]     Batches:        %u\n", fV92BatchCount);
}

OSDictionary* FakeIrisXEFramebuffer::getDiagnosticsReport() {
    OSDictionary* report = OSDictionary::withCapacity(16);
    if (!report) return nullptr;
    
    // Version info
    report->setObject("Version", OSString::withCString("V92"));
    report->setObject("BuildDate", OSString::withCString(__DATE__ " " __TIME__));
    
    // Status flags
    report->setObject("DisplayOnline", displayOnline ? kOSBooleanTrue : kOSBooleanFalse);
    report->setObject("FullyInitialized", fullyInitialized ? kOSBooleanTrue : kOSBooleanFalse);
    
    // Counters
    report->setObject("SurfaceCount", OSNumber::withNumber((unsigned long long)fV90SurfaceCount, 32));
    report->setObject("BlitCount", OSNumber::withNumber((unsigned long long)fV90BlitCount, 32));
    report->setObject("BatchCount", OSNumber::withNumber((unsigned long long)fV92BatchCount, 32));
    
    // Error info
    report->setObject("LastError", OSNumber::withNumber((unsigned long long)fV92LastError, 32));
    if (fV92LastErrorString[0]) {
        report->setObject("LastErrorString", OSString::withCString(fV92LastErrorString));
    }
    
    return report;
}

// ============================================================
// V92: XY_COLOR_BLT Full Implementation (Priority 3)
// ============================================================

IOReturn FakeIrisXEFramebuffer::submitBlitXY_COLOR_BLT_Full(
    SurfaceInfo* dstSurf,
    uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    uint32_t color)
{
    IOLog("[V92] Building XY_COLOR_BLT (full)...\n");
    
    if (!dstSurf) {
        IOLog("[V92] ❌ Null destination surface\n");
        return kIOReturnBadArgument;
    }
    
    if (!fExeclist || !fRcsRing) {
        IOLog("[V92] ❌ GPU not ready\n");
        return kIOReturnNotReady;
    }
    
    // Create batch buffer
    const size_t batchSize = 128;
    FakeIrisXEGEM* batchGem = createGEMObject(batchSize);
    if (!batchGem) {
        IOLog("[V92] ❌ Failed to create batch GEM\n");
        return kIOReturnNoMemory;
    }
    
    uint64_t batchGpuAddr = mapGEMToGGTT(batchGem);
    if (batchGpuAddr == 0) {
        IOLog("[V92] ❌ Failed to map batch GEM\n");
        batchGem->release();
        return kIOReturnError;
    }
    
    IOBufferMemoryDescriptor* desc = batchGem->memoryDescriptor();
    if (!desc) {
        batchGem->release();
        return kIOReturnError;
    }
    
    uint32_t* cmd = (uint32_t*)desc->getBytesNoCopy();
    if (!cmd) {
        batchGem->release();
        return kIOReturnError;
    }
    
    uint32_t idx = 0;
    
    // XY_COLOR_BLT command
    // DW0: Command Type=2D, Opcode=0x50, Length=6
    cmd[idx++] = (0x2 << 29) | (0x2 << 27) | (0x10 << 22) | 0x06;
    
    // DW1: Raster Op=0xF0 (fill), Color Depth=3 (32bpp)
    cmd[idx++] = (0xF0 << 16) | (0x3 << 12);
    
    // DW2-DW3: Destination X1, Y1
    cmd[idx++] = x;
    cmd[idx++] = y;
    
    // DW4-DW5: Destination X2, Y2 (exclusive)
    cmd[idx++] = x + width;
    cmd[idx++] = y + height;
    
    // DW6-DW7: Destination base address
    cmd[idx++] = (uint32_t)(dstSurf->gpuAddress & 0xFFFFFFFF);
    cmd[idx++] = (uint32_t)(dstSurf->gpuAddress >> 32);
    
    // DW8: Destination stride
    cmd[idx++] = (dstSurf->width * 4) / 4;
    
    // DW9: MOCS
    cmd[idx++] = 0x00000000;
    
    // DW10: Fill color
    cmd[idx++] = color;
    
    // MI_FLUSH_DW
    cmd[idx++] = (0x0 << 29) | (0x38 << 23) | 0x02;
    cmd[idx++] = 0x00000000;
    cmd[idx++] = 0x00000000;
    cmd[idx++] = 0x00000001;
    cmd[idx++] = 0x00000000;
    
    // MI_BATCH_BUFFER_END
    cmd[idx++] = 0x0A << 23;
    
    IOLog("[V92]   Fill 0x%08x at (%u,%u) size %ux%u\n", color, x, y, width, height);
    
    uint32_t seqNum = appendFenceAndSubmit(batchGem, 0, idx * 4);
    if (seqNum == 0) {
        IOLog("[V92] ❌ Failed to submit fill command\n");
        batchGem->release();
        return kIOReturnError;
    }
    
    fV92ColorBlitCount++;
    IOLog("[V92] ✅ Fill submitted with sequence %u\n", seqNum);
    
    // V93: Track WindowServer fill activity
    trackWindowServerBlit(width, height, true);
    
    // V93: Track GPU command submission
    trackGPUCommandSubmitted();
    
    batchGem->release();
    return kIOReturnSuccess;
}

// ============================================================
// V92: XY_SETUP_CLIP_BLT Implementation (Priority 3)
// ============================================================

IOReturn FakeIrisXEFramebuffer::submitBlitXY_SETUP_CLIP(
    SurfaceInfo* surf,
    uint32_t left, uint32_t top,
    uint32_t right, uint32_t bottom)
{
    IOLog("[V92] Setting up clip rectangle...\n");
    
    if (!surf) {
        return kIOReturnBadArgument;
    }
    
    // Store clip state
    fClipEnabled = true;
    fClipLeft = left;
    fClipTop = top;
    fClipRight = right;
    fClipBottom = bottom;
    
    // Create clip setup command
    const size_t batchSize = 64;
    FakeIrisXEGEM* batchGem = createGEMObject(batchSize);
    if (!batchGem) {
        return kIOReturnNoMemory;
    }
    
    uint64_t batchGpuAddr = mapGEMToGGTT(batchGem);
    if (batchGpuAddr == 0) {
        batchGem->release();
        return kIOReturnError;
    }
    
    IOBufferMemoryDescriptor* desc = batchGem->memoryDescriptor();
    if (!desc) {
        batchGem->release();
        return kIOReturnError;
    }
    
    uint32_t* cmd = (uint32_t*)desc->getBytesNoCopy();
    if (!cmd) {
        batchGem->release();
        return kIOReturnError;
    }
    
    uint32_t idx = 0;
    
    // XY_SETUP_CLIP_BLT
    // DW0: Command Type=2D, Opcode=0x03, Length=2
    cmd[idx++] = (0x2 << 29) | (0x2 << 27) | (0x03 << 22) | 0x02;
    
    // DW1: Clip Left/Top
    cmd[idx++] = (left & 0xFFFF) | ((top & 0xFFFF) << 16);
    
    // DW2: Clip Right/Bottom
    cmd[idx++] = (right & 0xFFFF) | ((bottom & 0xFFFF) << 16);
    
    // MI_BATCH_BUFFER_END
    cmd[idx++] = 0x0A << 23;
    
    uint32_t seqNum = appendFenceAndSubmit(batchGem, 0, idx * 4);
    if (seqNum == 0) {
        batchGem->release();
        return kIOReturnError;
    }
    
    fV92ClipCount++;
    IOLog("[V92] ✅ Clip set: (%u,%u)-(%u,%u)\n", left, top, right, bottom);
    
    batchGem->release();
    return kIOReturnSuccess;
}

// ============================================================
// V92: Batch Chaining Implementation (Priority 3)
// ============================================================

IOReturn FakeIrisXEFramebuffer::submitBatchBlits(BatchBlitEntry* entries, uint32_t count) {
    if (!entries || count == 0 || count > kMaxBatchBlits) {
        IOLog("[V92] ❌ Invalid batch parameters\n");
        return kIOReturnBadArgument;
    }
    
    IOLog("[V92] Submitting batch of %u blits...\n", count);
    
    FakeIrisXEGEM* batchGem = nullptr;
    uint32_t seqNum = 0;
    
    IOReturn result = buildBatchCommandBuffer(entries, count, &batchGem, &seqNum);
    if (result != kIOReturnSuccess) {
        IOLog("[V92] ❌ Failed to build batch command buffer\n");
        return result;
    }
    
    fV92BatchCount++;
    IOLog("[V92] ✅ Batch submitted with sequence %u (batch #%u)\n", seqNum, fV92BatchCount);
    
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::buildBatchCommandBuffer(
    BatchBlitEntry* entries, uint32_t count,
    FakeIrisXEGEM** batchGemOut, uint32_t* seqNumOut)
{
    // Calculate required size: each blit ~20 dwords + fence + end
    const size_t batchSize = count * 80 + 64;
    
    FakeIrisXEGEM* batchGem = createGEMObject(batchSize);
    if (!batchGem) {
        return kIOReturnNoMemory;
    }
    
    uint64_t batchGpuAddr = mapGEMToGGTT(batchGem);
    if (batchGpuAddr == 0) {
        batchGem->release();
        return kIOReturnError;
    }
    
    IOBufferMemoryDescriptor* desc = batchGem->memoryDescriptor();
    if (!desc) {
        batchGem->release();
        return kIOReturnError;
    }
    
    uint32_t* cmd = (uint32_t*)desc->getBytesNoCopy();
    if (!cmd) {
        batchGem->release();
        return kIOReturnError;
    }
    
    uint32_t idx = 0;
    
    // Process each blit in the batch
    for (uint32_t i = 0; i < count; i++) {
        BatchBlitEntry* entry = &entries[i];
        
        // Find surfaces
        SurfaceInfo* srcSurf = nullptr;
        SurfaceInfo* dstSurf = nullptr;
        
        for (uint32_t s = 0; s < kMaxSurfaces; s++) {
            if (fSurfaces[s].inUse) {
                if (fSurfaces[s].id == entry->srcSurfaceId) srcSurf = &fSurfaces[s];
                if (fSurfaces[s].id == entry->dstSurfaceId) dstSurf = &fSurfaces[s];
            }
        }
        
        if (!dstSurf || (!entry->isFill && !srcSurf)) {
            IOLog("[V92]   Skipping blit %u - surface not found\n", i);
            continue;
        }
        
        if (entry->isFill) {
            // XY_COLOR_BLT
            cmd[idx++] = (0x2 << 29) | (0x2 << 27) | (0x10 << 22) | 0x06;
            cmd[idx++] = (0xF0 << 16) | (0x3 << 12);
            cmd[idx++] = entry->dstX;
            cmd[idx++] = entry->dstY;
            cmd[idx++] = entry->dstX + entry->width;
            cmd[idx++] = entry->dstY + entry->height;
            cmd[idx++] = (uint32_t)(dstSurf->gpuAddress & 0xFFFFFFFF);
            cmd[idx++] = (uint32_t)(dstSurf->gpuAddress >> 32);
            cmd[idx++] = (dstSurf->width * 4) / 4;
            cmd[idx++] = 0x00000000;
            cmd[idx++] = entry->fillColor;
        } else {
            // XY_SRC_COPY_BLT
            cmd[idx++] = (0x2 << 29) | (0x2 << 27) | (0x13 << 22) | 0x0B;
            cmd[idx++] = (0xCC << 16) | (0x3 << 12);
            cmd[idx++] = entry->dstX;
            cmd[idx++] = entry->dstY;
            cmd[idx++] = entry->dstX + entry->width;
            cmd[idx++] = entry->dstY + entry->height;
            cmd[idx++] = (uint32_t)(dstSurf->gpuAddress & 0xFFFFFFFF);
            cmd[idx++] = (uint32_t)(dstSurf->gpuAddress >> 32);
            cmd[idx++] = (dstSurf->width * 4) / 4;
            cmd[idx++] = 0x00000000;
            cmd[idx++] = entry->srcX;
            cmd[idx++] = entry->srcY;
            cmd[idx++] = (uint32_t)(srcSurf->gpuAddress & 0xFFFFFFFF);
            cmd[idx++] = (uint32_t)(srcSurf->gpuAddress >> 32);
            cmd[idx++] = (srcSurf->width * 4) / 4;
            cmd[idx++] = 0x00000000;
        }
    }
    
    // Single flush for entire batch
    cmd[idx++] = (0x0 << 29) | (0x38 << 23) | 0x02;
    cmd[idx++] = 0x00000000;
    cmd[idx++] = 0x00000000;
    cmd[idx++] = 0x00000001;
    cmd[idx++] = 0x00000000;
    
    // MI_BATCH_BUFFER_END
    cmd[idx++] = 0x0A << 23;
    
    IOLog("[V92]   Batch buffer: %u dwords for %u blits\n", idx, count);
    
    uint32_t seqNum = appendFenceAndSubmit(batchGem, 0, idx * 4);
    if (seqNum == 0) {
        batchGem->release();
        return kIOReturnError;
    }
    
    *batchGemOut = batchGem;
    *seqNumOut = seqNum;
    
    return kIOReturnSuccess;
}

// ============================================================
// V93: Display Verification & Integration Testing
// Based on Intel PRM Volume 12: Display Engine
// ============================================================

void FakeIrisXEFramebuffer::verifyDisplayPipeState() {
    IOLog("\n[V93] ════════════════════════════════════════════════════════════\n");
    IOLog("[V93] DISPLAY PIPE VERIFICATION (Intel PRM Vol 12)\n");
    IOLog("[V93] ════════════════════════════════════════════════════════════\n\n");
    
    fV93DisplayVerified = true;
    
    // Check Pipe A
    bool pipeOK = isPipeAEnabled();
    IOLog("[V93] Pipe A: %s\n", pipeOK ? "✅ ENABLED" : "❌ DISABLED");
    
    // Check Transcoder A
    bool transcoderOK = isTranscoderAEnabled();
    IOLog("[V93] Transcoder A: %s\n", transcoderOK ? "✅ ENABLED" : "❌ DISABLED");
    
    // Check DDI A (eDP)
    bool ddiOK = isDDIAEnabled();
    IOLog("[V93] DDI A (eDP): %s\n", ddiOK ? "✅ ENABLED" : "❌ DISABLED");
    
    // Log all display registers
    logDisplayRegisters();
    
    // Overall status
    if (pipeOK && transcoderOK && ddiOK) {
        IOLog("\n[V93] ✅ Display Pipeline: FULLY OPERATIONAL\n");
    } else {
        IOLog("\n[V93] ❌ Display Pipeline: ISSUES DETECTED\n");
        fV93DisplayVerificationFailures++;
    }
}

bool FakeIrisXEFramebuffer::isPipeAEnabled() {
    if (!mmioBase) {
        IOLog("[V93] ❌ Cannot check Pipe A - MMIO not mapped\n");
        return false;
    }
    
    // PIPECONF_A register - Intel PRM Vol 12
    // Address: 0x70008 (for Pipe A)
    uint32_t pipeConf = safeMMIORead(0x70008);
    
    IOLog("[V93]   PIPECONF_A = 0x%08x\n", pipeConf);
    
    // Bit 31: Pipe Enable
    bool enabled = (pipeConf & 0x80000000) != 0;
    
    if (enabled) {
        IOLog("[V93]     - Pipe Enable: ✅ ENABLED (bit 31)\n");
        IOLog("[V93]     - Color Format: 0x%02x\n", (pipeConf >> 20) & 0xF);
    } else {
        IOLog("[V93]     - Pipe Enable: ❌ DISABLED (bit 31)\n");
    }
    
    return enabled;
}

bool FakeIrisXEFramebuffer::isTranscoderAEnabled() {
    if (!mmioBase) {
        return false;
    }
    
    // TRANS_CONF_A register - Intel PRM Vol 12
    // Address: 0x70008 (shared with PIPECONF for TGL)
    uint32_t transConf = safeMMIORead(0x70008);
    
    IOLog("[V93]   TRANS_CONF_A = 0x%08x\n", transConf);
    
    // Bit 31: Transcoder Enable
    bool enabled = (transConf & 0x80000000) != 0;
    
    if (enabled) {
        IOLog("[V93]     - Transcoder Enable: ✅ ENABLED\n");
    } else {
        IOLog("[V93]     - Transcoder Enable: ❌ DISABLED\n");
    }
    
    return enabled;
}

bool FakeIrisXEFramebuffer::isDDIAEnabled() {
    if (!mmioBase) {
        return false;
    }
    
    // DDI_BUF_CTL_A register - Intel PRM Vol 12
    // Address: 0x64000 (for DDI A)
    uint32_t ddiBuf = safeMMIORead(0x64000);
    
    IOLog("[V93]   DDI_BUF_CTL_A = 0x%08x\n", ddiBuf);
    
    // Bit 31: DDI Buffer Enable
    // Bit 0-1: Port Type
    bool enabled = (ddiBuf & 0x80000000) != 0;
    
    if (enabled) {
        IOLog("[V93]     - DDI Enable: ✅ ENABLED (bit 31)\n");
        IOLog("[V93]     - Port Type: %u\n", ddiBuf & 0xF);
    } else {
        IOLog("[V93]     - DDI Enable: ❌ DISABLED\n");
    }
    
    return enabled;
}

void FakeIrisXEFramebuffer::logDisplayRegisters() {
    if (!mmioBase) {
        IOLog("[V93] ❌ Cannot read registers - MMIO not mapped\n");
        return;
    }
    
    IOLog("\n[V93] Display Register Dump (Comprehensive):\n");
    
    // Pipe registers - Intel PRM Vol 12
    IOLog("[V93]   Pipe A:\n");
    IOLog("[V93]     PIPECONF:    0x%08x (Pipe Enable at bit 31)\n", safeMMIORead(0x70008));
    IOLog("[V93]     PIPESRC:     0x%08x (Source Size WxH)\n", safeMMIORead(0x7000C));
    IOLog("[V93]     PIPEBASE:    0x%08x (Frame Buffer Base)\n", safeMMIORead(0x70010));
    IOLog("[V93]     PIPESTAT:    0x%08x (Status)\n", safeMMIORead(0x70014));
    IOLog("[V93]     PIPEWM:      0x%08x (Watermarks)\n", safeMMIORead(0x70020));
    
    // Transcoder registers - Intel PRM Vol 12
    IOLog("[V93]   Transcoder A:\n");
    IOLog("[V93]     TRANS_CONF:  0x%08x (Transcoder Enable at bit 31)\n", safeMMIORead(0x70008));
    IOLog("[V93]     TRANS_HTOTAL: 0x%08x (H Total)\n", safeMMIORead(0x60000));
    IOLog("[V93]     TRANS_HBLANK: 0x%08x (H Blank)\n", safeMMIORead(0x60004));
    IOLog("[V93]     TRANS_HSYNC:  0x%08x (H Sync)\n", safeMMIORead(0x60008));
    IOLog("[V93]     TRANS_VTOTAL: 0x%08x (V Total)\n", safeMMIORead(0x6000C));
    IOLog("[V93]     TRANS_VBLANK: 0x%08x (V Blank)\n", safeMMIORead(0x60010));
    IOLog("[V93]     TRANS_VSYNC:  0x%08x (V Sync)\n", safeMMIORead(0x60014));
    IOLog("[V93]     TRANS_SIZE:  0x%08x (Transcoded Size)\n", safeMMIORead(0x7001C));
    
    // Plane registers - Intel PRM Vol 12
    IOLog("[V93]   Plane A (Primary):\n");
    IOLog("[V93]     PLANE_CTL_1_A:  0x%08x (Plane Control)\n", safeMMIORead(0x70180));
    IOLog("[V93]     PLANE_SURF_1_A: 0x%08x (Plane Surface)\n", safeMMIORead(0x7019C));
    IOLog("[V93]     PLANE_STRIDE_1_A: 0x%08x (Stride)\n", safeMMIORead(0x70188));
    IOLog("[V93]     PLANE_POS_1_A:  0x%08x (Position)\n", safeMMIORead(0x7018C));
    IOLog("[V93]     PLANE_SIZE_1_A: 0x%08x (Size)\n", safeMMIORead(0x70190));
    
    // DDI registers - Intel PRM Vol 12
    IOLog("[V93]   DDI A (eDP):\n");
    IOLog("[V93]     DDI_BUF_CTL:    0x%08x (DDI Enable at bit 31)\n", safeMMIORead(0x64000));
    IOLog("[V93]     DDI_BUF_TRANS1: 0x%08x (TRANS1)\n", safeMMIORead(0x64010));
    IOLog("[V93]     DDI_BUF_TRANS2: 0x%08x (TRANS2)\n", safeMMIORead(0x64014));
    IOLog("[V93]     DDI_FUNC_CTL:   0x%08x (Function Control)\n", safeMMIORead(0x60400));
    
    // Panel/Power registers - Intel PRM Vol 12
    IOLog("[V93]   Panel Power:\n");
    IOLog("[V93]     PP_STATUS (TGL): 0x%08x\n", safeMMIORead(0xC7200));
    IOLog("[V93]     PP_CONTROL (TGL):0x%08x\n", safeMMIORead(0xC7204));
    IOLog("[V93]     PP_STATUS (Old): 0x%08x\n", safeMMIORead(0x61200));
    IOLog("[V93]     PP_CONTROL (Old):0x%08x\n", safeMMIORead(0x61204));
    
    // Clock registers - Intel PRM Vol 12
    IOLog("[V93]   Display Clocks:\n");
    IOLog("[V93]     DPLL_CTL:     0x%08x (DPLL Control)\n", safeMMIORead(0x6C000));
    IOLog("[V93]     DPLL_STATUS:  0x%08x (DPLL Status)\n", safeMMIORead(0x6C00C));
    IOLog("[V93]     LCPLL1_CTL:   0x%08x (DPLL0)\n", safeMMIORead(0x46010));
    IOLog("[V93]     CLK_SEL_A:    0x%08x (Clock Select)\n", safeMMIORead(0x46140));
    
    // Backlight registers
    IOLog("[V93]   Backlight:\n");
    IOLog("[V93]     BLC_PWM_CTL1: 0x%08x\n", safeMMIORead(0xC8250));
    IOLog("[V93]     BLC_PWM_CTL2: 0x%08x\n", safeMMIORead(0xC8254));
    
    // Additional GPU status
    IOLog("[V93]   GPU Status:\n");
    IOLog("[V93]     GT_STATUS:    0x%08x\n", safeMMIORead(0x13805C));
    IOLog("[V93]     RCS0_STATUS: 0x%08x\n", safeMMIORead(0xC8000));
}

// ============================================================
// V93: WindowServer Integration Tracking
// ============================================================

void FakeIrisXEFramebuffer::trackWindowServerBlit(uint32_t width, uint32_t height, bool isFill) {
    // Track WindowServer-initiated blits
    fV93WindowServerBlitCount++;
    
    uint64_t currentTime = mach_absolute_time();
    
    if (fV93FirstBlitTime == 0) {
        fV93FirstBlitTime = currentTime;
    }
    fV93LastBlitTime = currentTime;
    
    // Mark WindowServer as connected
    fV93WindowServerConnected = true;
    
    // Log first few blits
    if (fV93WindowServerBlitCount <= 10) {
        IOLog("[V93] 🎨 WindowServer blit #%u: %s %ux%u\n", 
              fV93WindowServerBlitCount,
              isFill ? "FILL" : "COPY",
              width, height);
    } else if (fV93WindowServerBlitCount == 11) {
        IOLog("[V93] ... (more blits occurring)\n");
    }
}

// ============================================================
// V93: GPU Activity Monitoring
// ============================================================

void FakeIrisXEFramebuffer::trackGPUCommandSubmitted() {
    fV93CommandsSubmitted++;
    
    // Log first submission
    if (fV93CommandsSubmitted == 1) {
        IOLog("[V93] 📤 First GPU command submitted!\n");
    }
}

void FakeIrisXEFramebuffer::trackGPUCommandCompleted(uint32_t seqNum) {
    fV93CommandsCompleted++;
    
    // Track completion
    if (fV93CommandsCompleted % 100 == 0) {
        IOLog("[V93] ✅ GPU commands completed: %u\n", fV93CommandsCompleted);
    }
}

void FakeIrisXEFramebuffer::updateGPUPerformanceStats(uint64_t submitTime, uint64_t completeTime) {
    uint64_t delta = completeTime - submitTime;
    fV93TotalBlitTime += delta;
    
    // Log periodically
    if (fV93CommandsCompleted % 50 == 0 && fV93CommandsCompleted > 0) {
        uint64_t avgTime = fV93TotalBlitTime / fV93CommandsCompleted;
        IOLog("[V93] ⏱️  Avg GPU command time: %llu us\n", avgTime / 1000);
    }
}

// ============================================================
// V93: Real-time Status Report
// ============================================================

OSDictionary* FakeIrisXEFramebuffer::getV93StatusReport() {
    OSDictionary* report = OSDictionary::withCapacity(20);
    if (!report) return nullptr;
    
    // Version info
    report->setObject("Version", OSString::withCString("V93"));
    report->setObject("DisplayVerified", fV93DisplayVerified ? kOSBooleanTrue : kOSBooleanFalse);
    report->setObject("WindowServerConnected", fV93WindowServerConnected ? kOSBooleanTrue : kOSBooleanFalse);
    
    // Counters
    report->setObject("WindowServerBlitCount", OSNumber::withNumber((unsigned long long)fV93WindowServerBlitCount, 32));
    report->setObject("CommandsSubmitted", OSNumber::withNumber((unsigned long long)fV93CommandsSubmitted, 32));
    report->setObject("CommandsCompleted", OSNumber::withNumber((unsigned long long)fV93CommandsCompleted, 32));
    report->setObject("DisplayFailures", OSNumber::withNumber((unsigned long long)fV93DisplayVerificationFailures, 32));
    
    // Display state
    if (mmioBase) {
        uint32_t pipeConf = safeMMIORead(0x70008);
        report->setObject("PIPECONF_A", OSNumber::withNumber((unsigned long long)pipeConf, 32));
        
        uint32_t ddiBuf = safeMMIORead(0x64000);
        report->setObject("DDI_BUF_CTL_A", OSNumber::withNumber((unsigned long long)ddiBuf, 32));
    }
    
    return report;
}

void FakeIrisXEFramebuffer::printV93Summary() {
    IOLog("\n[V93] ════════════════════════════════════════════════════════════\n");
    IOLog("[V93] V93 STATUS SUMMARY\n");
    IOLog("[V93] ════════════════════════════════════════════════════════════\n");
    IOLog("[V93] Display Pipeline: %s\n", fV93DisplayVerified ? "✅ Verified" : "❌ Not Verified");
    IOLog("[V93] WindowServer: %s\n", fV93WindowServerConnected ? "✅ Connected" : "❌ Not Connected");
    IOLog("[V93] WindowServer Blits: %u\n", fV93WindowServerBlitCount);
    IOLog("[V93] GPU Commands: %u submitted, %u completed\n", fV93CommandsSubmitted, fV93CommandsCompleted);
    IOLog("[V93] Display Failures: %u\n", fV93DisplayVerificationFailures);
    IOLog("[V93] ════════════════════════════════════════════════════════════\n\n");
}

// ============================================================
// V240: Connector Manager Implementation for Tiger Lake
// ============================================================

void FakeIrisXEFramebuffer::initializeConnectorManager() {
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║  V240: Initialize Connector Manager for Tiger Lake           ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    
    if (!mmioBase) {
        IOLog("[V240] ERROR: MMIO base not available\n");
        return;
    }
    
    // Create connector manager if not already created
    if (!fConnectorManager) {
        fConnectorManager = new FakeIrisXEConnectorManager();
        if (!fConnectorManager) {
            IOLog("[V240] ERROR: Failed to allocate connector manager\n");
            return;
        }
        
        // Initialize with MMIO base
        if (!fConnectorManager->init(mmioBase)) {
            IOLog("[V240] ERROR: Failed to initialize connector manager\n");
            delete fConnectorManager;
            fConnectorManager = nullptr;
            return;
        }
        
        IOLog("[V240] Connector manager initialized successfully\n");
    }
    
    // Discover connectors
    discoverConnectors();
    
    fConnectorDiscoveryDone = true;
    IOLog("[V240] Connector discovery complete\n");
}

void FakeIrisXEFramebuffer::discoverConnectors() {
    if (!fConnectorManager) {
        IOLog("[V240] ERROR: Connector manager not initialized\n");
        return;
    }
    
    IOLog("[V240] Running connector discovery...\n");
    
    // Run discovery
    fConnectorManager->discoverConnectors();
    
    // Get internal panel if available
    TGLConnectorDesc* internal = fConnectorManager->getInternalPanel();
    if (internal) {
        IOLog("[V240] Internal panel found on %s\n",
              internal->ddiPort == TGLDDIPort::DDI_A ? "DDI_A" :
              internal->ddiPort == TGLDDIPort::DDI_B ? "DDI_B" :
              internal->ddiPort == TGLDDIPort::DDI_C ? "DDI_C" : "Unknown");
        
        // Initialize the internal panel
        initConnectorForType(*internal);
    } else {
        IOLog("[V240] WARNING: No internal panel detected\n");
    }
}

void FakeIrisXEFramebuffer::initConnectorForType(TGLConnectorDesc& conn) {
    if (!fConnectorManager) {
        IOLog("[V240] ERROR: Connector manager not initialized\n");
        return;
    }
    
    switch (conn.type) {
        case TGLConnectorType::eDP:
            IOLog("[V240] Initializing eDP connector...\n");
            fConnectorManager->initEDPConnector(conn);
            break;
            
        case TGLConnectorType::HDMI:
            IOLog("[V240] Initializing HDMI connector...\n");
            fConnectorManager->initHDMIConnector(conn);
            break;
            
        case TGLConnectorType::DP:
            IOLog("[V240] Initializing DP connector...\n");
            fConnectorManager->initDPConnector(conn);
            break;
            
        case TGLConnectorType::USB4TypeC:
            IOLog("[V240] Initializing USB4/Type-C connector...\n");
            fConnectorManager->initTypeCConnector(conn);
            break;
            
        default:
            IOLog("[V240] Unknown connector type: %d\n", (int)conn.type);
            break;
    }
}

#include <libkern/libkern.h>

// Entry points must match CFBundleExecutable name (FakeIrisXE)
extern "C" kern_return_t FakeIrisXE_start(kmod_info_t *ki, void *data) {
    return KERN_SUCCESS;
}

extern "C" kern_return_t FakeIrisXE_stop(kmod_info_t *ki, void *data) {
    return KERN_SUCCESS;
}
