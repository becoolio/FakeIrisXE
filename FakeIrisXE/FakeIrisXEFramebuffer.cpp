#include "FakeIrisXEFramebuffer.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
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
#include "AppleSafeRegisterAccess.hpp"



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

// V73-V75: Display mode structures (defined early for use in timing functions)
static const uint32_t kNumDisplayModes = 1;

// Mode ID 1: stable built-in timing (1920x1080)

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t modeID;
    const char* name;
} DisplayModeInfo;

// Use different name to avoid conflict with header member variable
static const DisplayModeInfo s_displayModes[kNumDisplayModes] = {
    {1920, 1080, 1, "1920x1080"},
};

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

    static constexpr uint64_t kMergedDisplayProductID = 40178ULL;
    static constexpr uint64_t kMergedDisplayVendorID = 1552ULL;
    static constexpr uint64_t kMergedDisplayGUID = 436849163854938112ULL;
    static constexpr uint64_t kOriginalDisplayProductID = 1815ULL;
    static constexpr uint64_t kOriginalDisplayVendorID = 1970170734ULL;

    setNumberProperty(service, "DisplayProductID", kMergedDisplayProductID, 32);
    setNumberProperty(service, "DisplayVendorID", kMergedDisplayVendorID, 32);
    setNumberProperty(service, "IODisplayGUID", kMergedDisplayGUID, 64);
    setNumberProperty(service, "DisplayProductIDOld", kOriginalDisplayProductID, 32);
    setNumberProperty(service, "DisplayVendorIDOld", kOriginalDisplayVendorID, 32);
    service->setProperty("AppleBacklightDisplay", kOSBooleanTrue);

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
    IOLog("║         FAKEIRISXE V168 - PCI Device IDs + Enhanced AGPM    ║\n");
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
  //  mmioBase = nullptr;
   // mmioWrite32 = nullptr;
    currentMode = 1;  // V131: Start with mode 1 (1920x1080) instead of 0
    currentDepth = 0;
    vramSize = 1920 * 1080 * 4;
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



uint32_t FakeIrisXEFramebuffer::safeMMIORead(uint32_t offset) {
    if (!mmioMap || !mmioBase) {
        IOLog("(FakeIrisXE) invalid MMIO read: mapping missing offset=0x%08X\n", offset);
        return 0xFFFFFFFFU;
    }
    return AppleSafeRegisterAccess::read32(mmioBase,
                                           static_cast<uint32_t>(mmioMap->getLength()),
                                           offset,
                                           "Framebuffer::safeMMIORead");
}

void FakeIrisXEFramebuffer::safeMMIOWrite(uint32_t offset, uint32_t value) {
    if (!mmioMap || !mmioBase) {
        IOLog("(FakeIrisXE) invalid MMIO write: mapping missing offset=0x%08X\n", offset);
        return;
    }
    (void)AppleSafeRegisterAccess::write32(mmioBase,
                                           static_cast<uint32_t>(mmioMap->getLength()),
                                           offset,
                                           value,
                                           "Framebuffer::safeMMIOWrite",
                                           nullptr);
}

uint32_t FakeIrisXEFramebuffer::safeReadRegister32(uint32_t offset) {
    return safeMMIORead(offset);
}

void FakeIrisXEFramebuffer::safeWriteRegister32(uint32_t offset, uint32_t value) {
    safeMMIOWrite(offset, value);
}

void FakeIrisXEFramebuffer::initializeForceWakeSystem(void) {
    AppleSafeRegisterAccess::init();
}

bool FakeIrisXEFramebuffer::gpuPowerOn() {
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







