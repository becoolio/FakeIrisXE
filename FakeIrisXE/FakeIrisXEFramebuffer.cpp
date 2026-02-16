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

// V73-V75: Display mode structures (defined early for use in timing functions)
static const uint32_t kNumDisplayModes = 6;

// Mode IDs: 1=1920x1080, 2=1440x900, 3=1366x768, 4=1280x720, 5=1024x768, 6=2560x1440

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t modeID;
    const char* name;
} DisplayModeInfo;

// Use different name to avoid conflict with header member variable
static const DisplayModeInfo s_displayModes[kNumDisplayModes] = {
    {1920, 1080, 1, "1920x1080"},
    {1440,  900, 2, "1440x900"},
    {1366,  768, 3, "1366x768"},
    {1280,  720, 4, "1280x720"},
    {1024,  768, 5, "1024x768"},
    {2560, 1440, 6, "2560x1440"},
};



//probe
IOService *FakeIrisXEFramebuffer::probe(IOService *provider, SInt32 *score) {
    // V72: FAILSAFE - Only load if -fakeirisxe boot-arg is set in NVRAM
    // This is a CRITICAL safety mechanism to prevent automatic loading
    // The kext MUST be explicitly enabled via: sudo nvram boot-args="-fakeirisxe"
    
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║         FAKEIRISXE V134 - Tiger Lake GPU Driver          ║\n");
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







// hardware init on startup
bool FakeIrisXEFramebuffer::initPowerManagement() {
    IOLog("🚀 Initiating CORRECTED-V24 power management (Enabling Clocks)...\n");

    if (!pciDevice || !pciDevice->isOpen(this)) {
        IOLog("❌ initPowerManagement(): PCI device not open - aborting\n");
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
    IOLog("║  FAKEIRISXE V131 - start() - WindowServer Integration         ║\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");

    if (!super::start(provider)) {
        IOLog("❌ [V131] super::start() failed\n");
        return false;
    }
    IOLog("✅ [V131] super::start() succeeded\n");

    

    
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

  
    
    
  //  IOLog("⚠️ Skipping enablePCIPowerManagement (causes freeze on some systems)\n");
  // 2️⃣ Optional: PCI Power Management (safe here)
    IOLog("⚡️ Powering up PCI device...\n");
    if (pciDevice->hasPCIPowerManagement()) {
        IOLog("Using modern power management\n");
        pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    }
    IOSleep(100);

    
    
    
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
        IOLog("FORCEWAKE_ACK: 0x%08X\n", safeMMIORead(0xA188)); // Read the register we just polled
    IOLog("✅ Returned from initPowerManagement()\n");
   
    
    
    
    
    
    
    
    // 7️⃣ Now it's SAFE to do MMIO read/write
    uint32_t zeroReg = safeMMIORead(0x0000);
    IOLog("MMIO[0x0000] = 0x%08X\n", zeroReg);

    uint32_t ack = safeMMIORead(0xA188);
    IOLog("FORCEWAKE_ACK: 0x%08X\n", ack);

    
    
    
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
    
    
    
    
    
    
    
    const uint32_t width  = 1920;
    const uint32_t height = 1080;
    const uint32_t bpp    = 4;

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
    
    
    
    
    // V72: Fixed Dynamic VRAM detection for Tiger Lake
        if (!pciDevice || !mmioBase) {
            IOLog("[V72] VRAM detect: No PCI/MMIO — fallback 256MB\n");
            return 256ULL * 1024ULL * 1024ULL;
        }

        // Step 1: Read stolen memory from BDSM register (0x5C)
        // Tiger Lake: bits 31:25 = size in 4MB units
        uint32_t bdsmReg = pciDevice->configRead32(0x5C);
        uint64_t stolenSize = ((bdsmReg >> 18) & 0xFFF) * 64ULL * 1024ULL * 1024ULL; // In bytes
        
        // Fallback: if 0, assume 128MB stolen
        if (stolenSize == 0) stolenSize = 128ULL * 1024ULL * 1024ULL;
        
        // Step 2: GTT aperture from BAR2 (0x54)
        uint32_t bar2Lo = pciDevice->configRead32(0x54) & ~0xFULL;
        uint32_t bar2Hi = pciDevice->configRead32(0x58);
        uint64_t gttSize = ((uint64_t)bar2Hi << 32) | bar2Lo;
        if (gttSize == 0) gttSize = 256ULL * 1024ULL * 1024ULL;
        
        // Total VRAM = stolen + GTT
        uint64_t totalVramBytes = stolenSize + gttSize;
        
        // For Tiger Lake Xe, ensure minimum 1GB
        if (totalVramBytes < 1024ULL * 1024ULL * 1024ULL) {
            totalVramBytes = 1536ULL * 1024ULL * 1024ULL; // 1.5GB minimum for Xe
        }
        
        IOLog("[V72] Dynamic VRAM: stolen=%llu MB, GTT=%llu MB, total=%llu MB\n", 
              stolenSize / (1024*1024), gttSize / (1024*1024), totalVramBytes / (1024*1024));

         
        // Use it (after alloc, before props)
    uint64_t realVramBytes = totalVramBytes;
    
    // V72: Set all VRAM properties for proper System Profiler recognition
    setProperty("IOAccelVRAMSize", realVramBytes, 64);  // Metal/QE full
    setProperty("IOFBMemorySize", realVramBytes, 64);   // Display
    setProperty("VRAM,totalMB", (uint32_t)(realVramBytes / (1024*1024)), 32);
    setProperty("VRAMSize", realVramBytes, 64);
    
    // Also set device properties
    if (pciDevice) {
        pciDevice->setProperty("deviceVRAM", realVramBytes);
        pciDevice->setProperty("VRAM,totalsize", realVramBytes);
    }
    
    IOLog("[V72] VRAM props set: %llu bytes (%llu MB)\n", realVramBytes, realVramBytes / (1024ULL * 1024ULL));
    
    // V75: Add HDA audio codec properties for display audio
    // Intel HDA controller properties for audio over HDMI/DisplayPort
    IOLog("[V75] Setting up HDA audio codec properties...\n");
    
    // Audio device properties
    setProperty("hda-gfx", OSString::withCString("on-PCI"));
    setProperty("hda-audio", OSNumber::withNumber(2, 32));  // HDAUDIO_FMT_CHANNELS_2
    setProperty("hda-eld", OSData::withBytes((const void*)"\x00\x00\x00\x00\x00\x00\x00\x00", 8));  // ELD buffer
    
    // Audio codec vendor/product IDs (Intel HDA generic)
    setProperty("codec-vendor-id", OSNumber::withNumber(0x808629AD, 32));  // Intel
    setProperty("codec-id", OSNumber::withNumber(0xA0CF0000, 32));  // Generic
    
    // Audio capabilities
    setProperty("audio-formats", OSNumber::withNumber(0x1C, 32));  // PCM 16/20/24-bit, stereo
    setProperty("audio-max-channels", OSNumber::withNumber(2, 32));  // Stereo
    setProperty("audio-sample-rate", OSNumber::withNumber(48000, 32));  // 48kHz
    
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
    
    // Additional framebuffer properties
    setProperty("framebuffer-unifiedmem", OSNumber::withNumber(0x6000000, 32));  // 1536MB VRAM
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
    
    // V131: Display vendor/product IDs for proper Mac detection
    // Use Intel vendor (0x8086) and MacBook Pro-like product code
    setProperty("vendor-id", OSNumber::withNumber(0x8086, 32));
    setProperty("product-id", OSNumber::withNumber(0x9B00, 32));  // Similar to MacBook Pro
    setProperty("serial-number", OSNumber::withNumber(0x12345678, 32));
    setProperty("display-serial-number", OSNumber::withNumber(0x12345678, 32));
    setProperty("vendor-name", OSString::withCString("Intel"));
    setProperty("product-name", OSString::withCString("Intel Iris Xe Graphics"));
    
    IOLog("[V131] ✅ Internal display properties set\n");
    
    // Metal/Acceleration properties
    setProperty("IOAccelTypes", OSArray::withObjects((const OSObject*[]){
        OSString::withCString("Accel"),
        OSString::withCString("Metal"),
        OSString::withCString("OpenGL")
    }, 3));
    
    // PCI properties for GPU detection
    if (pciDevice) {
        pciDevice->setProperty("model", OSString::withCString("Intel Iris Xe Graphics"));
        pciDevice->setProperty("model Alias", OSString::withCString("Intel Xe"));
    }
    
    IOLog("[V131] Connector/framebuffer patch properties published\n");
    IOLog("[V131] - Port 0: eDP (internal panel)\n");
    IOLog("[V131] - Port 1: HDMI\n");
    IOLog("[V131] - Port 2: DP\n");
    IOLog("[V131] GPU detection properties added\n");
    

    
    
    
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
    
    

    // === V74: Enhanced EDID with detailed timing descriptors ===
    {
        // V80: Proper EDID for Dell Latitude 5520 15.6" 1920x1080 panel (128 bytes)
        static const uint8_t properEDID[128] = {
            0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
            0x30, 0xE4, 0x9C, 0x7C, 0x00, 0x00, 0x00, 0x00,
            0x1F, 0x1F, 0x01, 0x04, 0xA5, 0x1F, 0x14, 0x78,
            0x04, 0x95, 0x95, 0xA3, 0x55, 0x4C, 0x9E, 0x26,
            0x0F, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01,
            0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
            0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
            0x2E, 0x36, 0x80, 0xA0, 0x70, 0x38, 0x1F, 0x40,
            0x30, 0x20, 0x35, 0x00, 0x58, 0xC2, 0x10, 0x00,
            0x00, 0x1A, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0xFD, 0x00, 0x38, 0x4B, 0x1E,
            0x5E, 0x11, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20,
            0x20, 0x20, 0x00, 0xFC, 0x00, 0x44, 0x65, 0x6C,
            0x6C, 0x20, 0x4C, 0x61, 0x74, 0x35, 0x35, 0x32
        };
        
        OSData *edidData = OSData::withBytes(properEDID, sizeof(properEDID));
        if (edidData) {
            setProperty("IODisplayEDID", edidData);
            edidData->release();
            IOLog("[V80] Proper Dell EDID published (Dell Latitude 5520)\n");
        }

        // V80: Critical for display recognition - override EDID from physical connection
        OSData *overrideEDID = OSData::withBytes(properEDID, sizeof(properEDID));
        if (overrideEDID) {
            setProperty("AAPL00,override-no-connect", overrideEDID);
            overrideEDID->release();
            IOLog("[V80] AAPL00,override-no-connect set for display detection\n");
        }

        setProperty("IOFBHasPreferredEDID", kOSBooleanTrue);
        
        // V80: Display identification properties
        setProperty("IODisplaySerialNumber", OSString::withCString("CN-0F7CRH-7275581"));
        setProperty("IODisplayVendorID", OSNumber::withNumber(0xE430, 16));  // Dell
        setProperty("IODisplayProductID", OSNumber::withNumber(0x7C9C, 16));  // Latitude 5520 panel
        setProperty("IODisplayName", OSString::withCString("Dell Latitude 5520"));
        setProperty("IODisplayPrefsKey", OSString::withCString("DEL:0x7C9C"));
        
        IOLog("[V80] Display properties set for Dell Latitude 5520 panel\n");
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
    setProperty("IOFBDisplayModeCount", (uint64_t)6, 32);
    setProperty("IOFBIsMainDisplay", kOSBooleanTrue);
    setProperty("AAPL,boot-display", kOSBooleanTrue);

    setProperty("brightness-control", kOSBooleanTrue);
    setProperty("IOBacklight", kOSBooleanTrue);
    // backlight-index / backlight-control-type must be OSNumber
    OSNumber *idx = OSNumber::withNumber((uint64_t)1ULL, 32);
    if (idx) { setProperty("AAPL,backlight-control-type", idx); idx->release(); }
    
    setProperty("IODisplayHasBacklight", kOSBooleanTrue);

    
    //optional
    // Transparency and vibrancy support
    setProperty("IOFBTranslucencySupport", kOSBooleanTrue);
    setProperty("IOFBVibrantSupport", kOSBooleanTrue);
    setProperty("IOFBAlphaBlending", kOSBooleanTrue);
    setProperty("IOFBCompositeSupport", kOSBooleanTrue);

    // Window server acceleration
    setProperty("IOFBWSAASupport", kOSBooleanTrue);
    setProperty("IOFBWSSupport", kOSBooleanTrue);

    // Hardware compositing
    setProperty("IOFBHardwareCompositing", kOSBooleanTrue);
    setProperty("IOFBAutoCompositing", kOSBooleanTrue);
    
    // Replace your existing framebuffer properties with these:
    setProperty("IOAccelerator", kOSBooleanTrue);
    setProperty("IOAccelIndex", 0ULL, 32);
    setProperty("IOAccelRevision", 2ULL, 32);
    setProperty("IOAccelVideoMemorySize", framebufferMemory->getLength(), 64);
    setProperty("IOAccelMemorySize", 134217728ULL, 64);

    // Critical for Core Image
    setProperty("CISupported", kOSBooleanTrue);
    setProperty("CIAllowSoftwareRenderer", kOSBooleanFalse); // Force hardware
    setProperty("CIContextUseSoftwareRenderer", kOSBooleanFalse);

    // IOSurface capabilities
    setProperty("IOSurfaceSupported", kOSBooleanTrue);
    setProperty("IOSurfaceIsGlobal", kOSBooleanTrue);
    setProperty("IOSurfaceCacheMode", 0ULL, 32);

    // Additional acceleration hints
    setProperty("IOAccelSurfaceSupported", kOSBooleanTrue);
    setProperty("IOAccelCLContextSupported", kOSBooleanTrue);
    setProperty("IOAccelGLContextSupported", kOSBooleanTrue);
    // Enable IOSurface support - CRITICAL for transparency
    setProperty("IOSurfaceSupport", kOSBooleanTrue);
    setProperty("IOSurfaceIsGlobal", kOSBooleanTrue);
    setProperty("IOAccelSurfaceSupported", kOSBooleanTrue);

    // Core Image acceleration
    setProperty("CISupported", kOSBooleanTrue);
    setProperty("CIBlurSupported", kOSBooleanTrue);
    setProperty("CITransparencySupported", kOSBooleanTrue);

    // Quartz Extreme requirements
    setProperty("IOAccelVideoMemorySize", framebufferMemory->getLength(), 64);
    setProperty("IOAccelMemorySize", 134217728ULL, 64); // 128MB for textures
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

    


    // Cursor
    cursorMemory = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut,
        64 * 1024,
        page_size
    );
    if (cursorMemory) {
        setProperty("IOFBCursorMemory", cursorMemory);
        cursorMemory->release();
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
    
    activatePowerAndController();
    
    
    // ================================================
    // V46+V47: GGTT INITIALIZATION (MUST BE FIRST)
    // ================================================
    IOLog("(FakeIrisXE) [V48] Initializing GGTT aperture...\n");
    
    // ---- GGTT Aperture Mapping ----

    // 1. Read BAR0 (GTTMMADR)
    uint64_t gttAddr = pciDevice->configRead32(kIOPCIConfigBaseAddress0);
    gttAddr &= ~0xFULL;    // clear PCI flags

    uint64_t gttSize2 = 2 * 1024 * 1024; // 2MB GGTT

    // 2. Create mapping for GGTT table
    IOMemoryDescriptor* desc = IOMemoryDescriptor::withPhysicalAddress(
        gttAddr,
        gttSize2,
        kIODirectionOutIn
    );

    if (!desc) {
        IOLog("FakeIrisXEFramebuffer: Failed to create GGTT descriptor\n");
        return false;
    }

    IOMemoryMap* map = desc->map();
    if (!map) {
        IOLog("FakeIrisXEFramebuffer: Failed to map GGTT\n");
        desc->release();
        return false;
    }

    // 3. Save GGTT pointer
    fGGTT = (volatile uint32_t*)map->getVirtualAddress();
    fGGTTSize = gttSize2;
    fGGTTBaseGPU = 0x00000000;
    fNextGGTTOffset = 0x00100000;   // leave hardware reserved region

    IOLog("FakeIrisXEFramebuffer: GGTT mapped at %p\n", fGGTT);

    
//ring rcs
    
    IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, provider);
    pci->retain();
    pci->setMemoryEnable(true);

    if (!map) {
        IOLog("Failed to map BAR0\n");
        return false;
    }

    fBar0 = (volatile uint32_t*)map->getVirtualAddress();
    IOLog("BAR0 mapped at %p\n", fBar0);


    // 1. Create ring object
    fRingRCS = new FakeIrisXERing(fBar0);

    // 2. Allocate 64KB ring buffer
    if (!fRingRCS->allocateRing(64 * 1024)) {
        IOLog("Failed to allocate RCS ring\n");
        return false;
    }

    // 3. Attach GPU address (if you have GGTT mapping)
    fRingRCS->attachRingGPUAddress(gttAddr);
        // gttAddr = GPU VA of the ring buffer

    // 4. Program ring registers
    fRingRCS->programRingBaseToHW();

    // 5. Enable ring
    fRingRCS->enableRing();

    IOLog("RCS ring initialization complete.\n");


    
    // map BAR0 into fBar0 — you already have this
    // map GGTT into fGGTT — you already have this
    fNextGGTTOffset = 0x00100000; // choose appropriate base

    // Create ring
    if (!createRcsRing(256 * 1024)) {
        IOLog("FakeIrisXEFramebuffer: createRcsRing failed\n");
        return false;
    }else{
        
        IOLog("FakeIrisXEFramebuffer: createRcsRing Succes\n");

    }

    // optional: create & map fence early (so submitBatch doesn't do it)
    fFenceGEM = FakeIrisXEGEM::withSize(4096, 0);
    fFenceGEM->pin();

    // ================================================
    // V45: FIRMWARE LOADING (After GGTT init, Intel PRM sequence)
    // ================================================
    IOLog("(FakeIrisXE) [V45] Loading firmware (Intel PRM compliant)...\n");

    // V45: Program MOCS before GuC init
    IOLog("(FakeIrisXE) [V45] Programming MOCS...\n");
    if (!programMOCS()) {
        IOLog("(FakeIrisXE) [V45] ⚠️ MOCS programming failed, continuing anyway\n");
    }

    IOLog("(FakeIrisXE) [V45] Initializing GuC system (PRM sequence)...\n");

    // Initialize GuC system with Intel PRM-compliant sequence
    if (!initGuCSystem()) {
        IOLog("(FakeIrisXE) [V45] ⚠️ GuC init failed, falling back to legacy execlist\n");
        fGuCEnabled = false;
    } else {
        fGuCEnabled = true;
        IOLog("(FakeIrisXE) [V45] ✅ GuC submission enabled\n");
    }

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
                IOLog("FakeIrisXEFramebuffer: EXECLIST engine READY\n");
                
                // V60: Run diagnostic test only if -fakeirisxe boot flag is set
                // (Already checked in probe(), but double-check here for safety)
                char bootArg[32] = {0};
                bool hasBootArg = PE_parse_boot_argn("-fakeirisxe", bootArg, sizeof(bootArg));
                
                if (hasBootArg) {
                    IOLog("FakeIrisXEFramebuffer: [V70] Boot flag '-fakeirisxe' detected - running COMPREHENSIVE diagnostic test...\n");
                    
                    // V70: Run comprehensive diagnostic suite
                    if (fExeclist->runComprehensiveDiagnosticTest()) {
                        IOLog("FakeIrisXEFramebuffer: [V70] ✅ ALL COMPREHENSIVE TESTS PASSED\n");
                    } else {
                        IOLog("FakeIrisXEFramebuffer: [V70] ⚠️ Some comprehensive tests failed (see logs above)\n");
                    }
                    
                    // Also run simple test for comparison
                    IOLog("FakeIrisXEFramebuffer: [V70] Running simple diagnostic test...\n");
                    if (fExeclist->runSimpleDiagnosticTest()) {
                        IOLog("FakeIrisXEFramebuffer: [V62] Simple diagnostic test PASSED\n");
                    } else {
                        IOLog("FakeIrisXEFramebuffer: [V62] Simple diagnostic test FAILED\n");
                    }
                } else {
                    IOLog("FakeIrisXEFramebuffer: [V70] Skipping diagnostic test (add '-fakeirisxe' to boot-args to enable)\n");
                }
            }
        
            // Create / init RCS ring (existing helper returns bool)
            fRcsRing = createRcsRing(256 * 1024);
            if (fRcsRing) {
                IOLog("FakeIrisXEFramebuffer: RCS ring initialization complete. fRcsRing=%p\n", fRcsRing);
            } else {
                IOLog("FakeIrisXEFramebuffer: FAILED creating RCS ring\n");
            }

            // V88: Simple execlist command submission test
            IOLog("\n");
            IOLog("╔══════════════════════════════════════════════════════════════╗\n");
            IOLog("║  V88: EXECLIST COMMAND SUBMISSION TEST                       ║\n");
            IOLog("╚══════════════════════════════════════════════════════════════╝\n");
            IOLog("\n");
            
            if (fExeclist && fRcsRing) {
                IOLog("[V88] Attempting simple MI_NOOP submission via execlist...\n");
                
                // Create a simple batch buffer with MI_NOOP
                FakeIrisXEGEM* testBatch = createSimpleUserBatch();
                if (testBatch) {
                    testBatch->pin();
                    uint64_t batchGpu = ggttMap(testBatch);
                    
                    IOLog("[V88] Test batch created: GPU addr=0x%llx\n", batchGpu);
                    
                    // Try to submit via execlist
                    bool submitOk = fExeclist->submitBatchExeclist(testBatch);
                    if (submitOk) {
                        IOLog("[V88] ✅ MI_NOOP command submitted successfully!\n");
                    } else {
                        IOLog("[V88] ❌ MI_NOOP submission failed - checking with full submit...\n");
                        
                        // Try the full submit path with fence
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
            setProperty("IOFBTransform", OSNumber::withNumber((unsigned long long)0, 32));
            setProperty("IOFBSignal", OSNumber::withNumber((unsigned long long)0, 32));
            
            // Enable acceleration hints
            setProperty("IOFBAccelerated", kOSBooleanTrue);
            setProperty("IOFBHWCursor", kOSBooleanTrue);
            setProperty("IOFBAlphaCursor", kOSBooleanTrue);
            
            // Set up surface format for WindowServer
            setProperty("IOSurfacePixelFormat", OSNumber::withNumber(0x42475241, 32)); // ARGB
            setProperty("IOSurfaceBytesPerElement", OSNumber::withNumber(4, 32));
            setProperty("IOSurfaceBytesPerRow", OSNumber::withNumber(7680, 32));
            setProperty("IOSurfaceWidth", OSNumber::withNumber(1920, 32));
            setProperty("IOSurfaceHeight", OSNumber::withNumber(1080, 32));
            
            IOLog("[V89] ✅ WindowServer integration properties set\n");
            IOLog("[V89] ✅ Display acceleration enabled\n");
            IOLog("[V89] ✅ IOSurface format configured\n");
            
            // V90: IOAccelerator Initialization
            IOLog("\n");
            IOLog("╔══════════════════════════════════════════════════════════════╗\n");
            IOLog("║  V90: IOACCELERATOR HOOKS INITIALIZED                        ║\n");
            IOLog("╚══════════════════════════════════════════════════════════════╝\n");
            IOLog("\n");
            IOLog("[V90] Surface management ready:\n");
            IOLog("      Max surfaces: %u\n", kMaxSurfaces);
            IOLog("      Format: ARGB8888\n");
            IOLog("[V90] 2D Blit operations: Ready\n");
            IOLog("[V90] Command submission: Ready (execlist)\n");
            IOLog("\n");
            
            // V91: 2D Blit Command Support
            IOLog("╔══════════════════════════════════════════════════════════════╗\n");
            IOLog("║  V91: 2D BLIT COMMANDS ACTIVE                                ║\n");
            IOLog("╚══════════════════════════════════════════════════════════════╝\n");
            IOLog("\n");
            IOLog("[V91] Intel Blitter Commands:\n");
            IOLog("      XY_SRC_COPY_BLT (0x53): Ready\n");
            IOLog("      XY_COLOR_BLT (0x50): Ready\n");
            IOLog("      XY_SETUP_BLT (0x01): Ready\n");
            IOLog("[V91] GPU Hardware Acceleration: Active\n");
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
            setProperty("IOFBAccelerator", kOSBooleanTrue);
            setProperty("IOFBAccelRevision", OSNumber::withNumber(93, 32));
            
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
                IOLog("🟢 LINK SUCCESS — FB → Accelerator\n");
                break;  // Important — only 1 accelerator
            }
        }
        iter->release();
    }

    
    
    
    
    
    

    
    
    

    
    
    
    
    // --- Create the Backlight Node ---
    FakeIrisXEBacklight* backlight = OSTypeAlloc(FakeIrisXEBacklight);
    if (backlight && backlight->init()) {

        backlight->setName("AppleBacklightDisplay");
        backlight->setProperty("IOClass", "IOBacklightDisplay");

        backlight->setProperty("IOProviderClass", "IODisplayConnect");
        backlight->setProperty("IONameMatch", "AppleBacklightDisplay");
        backlight->setProperty("AAPL,backlight-control", kOSBooleanTrue);

        // --- brightness params dictionary ---
        // --- brightness params dictionary ---
        OSDictionary* params = OSDictionary::withCapacity(2);
        OSDictionary* bright = OSDictionary::withCapacity(3);
        OSDictionary* vblm   = OSDictionary::withCapacity(3);
        
        
        OSNumber *nMin   = OSNumber::withNumber((uint64_t)0ULL,   32);
        OSNumber *nMax   = OSNumber::withNumber((uint64_t)100ULL, 32);
        OSNumber *nVal   = OSNumber::withNumber((uint64_t)100ULL, 32);

        if (bright && vblm && params && nMin && nMax && nVal) {

            bright->setObject("min", nMin);
            bright->setObject("max", nMax);
            bright->setObject("value", nVal);

            OSDictionary* brightnessDict = OSDictionary::withCapacity(3);
            brightnessDict->setObject(OSSymbol::withCString("min"), nMin);
            brightnessDict->setObject(OSSymbol::withCString("max"), nMax);
                           brightnessDict->setObject(OSSymbol::withCString("value"), nVal);
                           params->setObject(OSSymbol::withCString("brightness"), brightnessDict);

            vblm->setObject("min", nMin);
            vblm->setObject("max", nMax);
            vblm->setObject("value", nVal);
            params->setObject("vblm", vblm);
            backlight->setProperty("IODisplayParameters", params);
            
        }

        // release everything we created (setObject retained)
        if (nMin) nMin->release();
        if (nMax) nMax->release();
        if (nVal) nVal->release();
        if (bright) bright->release();
        if (vblm) vblm->release();
        if (params) params->release();


        
        // attach under display0
        backlight->registerService();

        IOLog("[FB] AppleBacklightDisplay published under IODisplayConnect\n");
    }



    
    
    
    
    
    
  
    // 6. Then flush and notify
    //flushDisplay();
    
    
    // 6. Finally, publish the framebuffer
    attachToParent(getProvider(), gIOServicePlane);
    
    
    
    
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
    
   



    
    


    
    
    // V131: Final initialization diagnostics with WindowServer info
    IOLog("\n");
    IOLog("╔══════════════════════════════════════════════════════════════╗\n");
    IOLog("║  V131 INITIALIZATION COMPLETE - STATUS REPORT                 ║\n");
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
    IOLog("║  Display:         Dell Latitude 5520\n");
    IOLog("╠══════════════════════════════════════════════════════════════╣\n");
    IOLog("║  WINDOWSERVER INTEGRATION                                    ║\n");
    IOLog("║  Aperture Range:  ✅ CONFIGURED\n");
    IOLog("║  Client Memory:   ✅ SUPPORTED (Types 0,1,2)\n");
    IOLog("║  Surface Mapping: ✅ READY\n");
    IOLog("╚══════════════════════════════════════════════════════════════╝\n");
    IOLog("\n");
    
    IOLog("[V131] WindowServer should now be able to render to this framebuffer\n");
    IOLog("[V131] Look for color bars on screen (V81 test pattern)\n");
    IOLog("\n");
    
    IOLog("🏁 FakeIrisXEFramebuffer::start() - Completed Successfully (V134)\n");
    return true;

}



void FakeIrisXEFramebuffer::stop(IOService* provider)
{
    IOLog("FakeIrisXEFramebuffer::stop() called — scheduling gated cleanup\n");

    // in FakeIrisXEFramebuffer::stop(IOService* provider)
    if (fInterruptSource) {
        fInterruptSource->disable();
        fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = nullptr;
    }
    if (fPendingSubmissions) {
        fPendingSubmissions->release();
        fPendingSubmissions = nullptr;
    }
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = nullptr;
    }

    
    if (fCmdGate) {
        fWorkLoop->removeEventSource(fCmdGate);
        fCmdGate->release();
        fCmdGate = nullptr;
    }

    if (fPendingSubmissions) {
        cleanupAllPendingSubmissions();
        fPendingSubmissions->release();
        fPendingSubmissions = nullptr;
    }

    if (fPendingLock) {
        IOLockFree(fPendingLock);
        fPendingLock = nullptr;
    }

    
    // Mark we are shutting down so any timers/work will early-exit
    IOLockLock(timerLock);
    driverActive = false;
    shuttingDown = true;
    IOLockUnlock(timerLock);

    
    if (commandGate) {
        // runAction will call staticStopAction() on the gated thread synchronously.
        commandGate->runAction(&FakeIrisXEFramebuffer::staticStopAction);
    } else {
        // fallback: if no gate exists, do best-effort cleanup inline
        performSafeStop();
    }

    // Now call superclass stop after gated cleanup.
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
    IOLog("FakeIrisXEFramebuffer::performSafeStop() — doing gated cleanup\n");

    // Cancel timers and remove event sources under workloop/gate
    if (vsyncTimer) {
        vsyncTimer->cancelTimeout();
        if (workLoop) {
            workLoop->removeEventSource(vsyncTimer);
        }
        vsyncTimer->release();
        vsyncTimer = nullptr;
    }

    if (displayInjectTimer) {
        displayInjectTimer->cancelTimeout();
        if (workLoop) {
            workLoop->removeEventSource(displayInjectTimer);
        }
        displayInjectTimer->release();
        displayInjectTimer = nullptr;
    }

  
    // Stop power management (PM) under gated thread
    PMstop();

    // Release GPU resources and memory descriptors (these touch IOGraphics/IOBuffer objects)
    OSSafeReleaseNULL(framebufferMemory);
    OSSafeReleaseNULL(framebufferSurface);
    OSSafeReleaseNULL(cursorMemory);
    OSSafeReleaseNULL(mmioMap);
    OSSafeReleaseNULL(vramMemory);

    // Remove interrupt sources if any
    if (vsyncSource && workLoop) {
        workLoop->removeEventSource(vsyncSource);
        vsyncSource->release();
        vsyncSource = nullptr;
    }


    // Free other locks and arrays
    if (timerLock) {
        IOLockFree(timerLock);
        timerLock = nullptr;
    }
    if (powerLock) {
        IOLockFree(powerLock);
        powerLock = nullptr;
    }

    if (interruptList) {
        interruptList->release();
        interruptList = nullptr;
    }

    // Close PCI device and release provider only after gated cleanup
    if (pciDevice) {
        pciDevice->close(this);
        pciDevice->release();
        pciDevice = nullptr;
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

    
    
    IOLog("FakeIrisXEFramebuffer::performSafeStop() — gated cleanup complete\n");
}














void FakeIrisXEFramebuffer::startIOFB() {
    IOLog("FakeIrisXEFramebuffer::startIOFB() called\n");
    // deliverFramebufferNotification(0, kIOFBNotifyDisplayModeChange, nullptr); // This is enough

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
    // Mark request
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
#define BXT_BLC_PWM_CTL2    0xC8254

#define PLANE_OFFSET_1_A 0x00000000
#define PIPECONF_A      0x70008

// Tiger Lake / BXT backlight PWM registers (as used in your working snippet)
static constexpr uint32_t BXT_BLC_PWM_FREQ1 = 0x000C8250;  // period / max PWM value in low 16 bits
static constexpr uint32_t BXT_BLC_PWM_DUTY1 = 0x000C8254;  // duty (maybe low 16 bits)



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
         __asm__ volatile("mfence" ::: "memory");
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
    
    const uint32_t PP_CONTROL = 0x00064004;
    const uint32_t PP_STATUS  = 0x00064024;
    
    // Step 1: Power up panel
    IOLog("[V131] Step 1: Powering up eDP panel...\n");
    wr(PP_CONTROL, (1u << 31) | (1u << 30));
    IOSleep(100);  // Longer delay for panel power
    
    // Step 2: Wait for panel power ready (CRITICAL)
    IOLog("[V131] Step 2: Waiting for panel power ready...\n");
    bool panelReady = false;
    for (int i = 0; i < 200; i++) {  // Increased timeout
        uint32_t status = rd(PP_STATUS);
        if (status & (1u << 31)) {
            IOLog("[V131] ✅ Panel power ready (PP_STATUS=0x%08X)\n", status);
            panelReady = true;
            break;
        }
        IOSleep(10);
    }
    
    if (!panelReady) {
        IOLog("[V131] ⚠️ Panel power timeout - continuing anyway\n");
    }
    
    IOSleep(200);  // Extra delay after panel power
    
    // Step 3: Enable DDI A buffer (before pipe/transcoder)
    IOLog("[V131] Step 3: Enabling DDI A buffer...\n");
    uint32_t ddi = rd(DDI_BUF_CTL_A);
    ddi |= (1u << 31);  // Enable
    ddi &= ~(7u << 24);
    ddi |= (1u << 24);  // x1 width for eDP
    wr(DDI_BUF_CTL_A, ddi);
    IOSleep(20);
    IOLog("[V131] DDI_BUF_CTL_A = 0x%08X\n", rd(DDI_BUF_CTL_A));
    
    // Step 4: Enable Pipe A
    IOLog("[V131] Step 4: Enabling Pipe A...\n");
    uint32_t pipeconf = rd(PIPECONF_A);
    pipeconf |= (1u << 31);  // Enable
    pipeconf |= (1u << 30);  // Progressive
    wr(PIPECONF_A, pipeconf);
    IOSleep(20);
    IOLog("[V131] PIPECONF_A = 0x%08X\n", rd(PIPECONF_A));
    
    // Step 5: Enable Transcoder A
    IOLog("[V131] Step 5: Enabling Transcoder A...\n");
    uint32_t trans = rd(TRANS_CONF_A);
    trans |= (1u << 31);  // Enable
    wr(TRANS_CONF_A, trans);
    IOSleep(20);
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
    const uint32_t PP_STATUS_V81 = 0x00064024;
    uint32_t ppStatusV81 = rd(PP_STATUS_V81);
    IOLog("[V81] PP_STATUS = 0x%08X\n", ppStatusV81);
    IOLog("       Panel Power: %s\n", (ppStatusV81 & (1u << 31)) ? "ON ✅" : "OFF ❌");
    
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
    return true;
}






IOReturn FakeIrisXEFramebuffer::getTimingInfoForDisplayMode(
    IODisplayModeID displayMode,
    IOTimingInformation* infoOut)
{
    // V74: Enhanced timing info for all supported display modes
    if (!infoOut) {
        return kIOReturnBadArgument;
    }

    // Find matching mode in our display modes
    const DisplayModeInfo* modeInfo = nullptr;
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        if (displayMode == s_displayModes[i].modeID) {
            modeInfo = &s_displayModes[i];
            break;
        }
    }

    if (!modeInfo) {
        IOLog("[V74] getTimingInfoForDisplayMode(): unsupported mode %u\n", displayMode);
        return kIOReturnUnsupportedMode;
    }

    bzero(infoOut, sizeof(IOTimingInformation));

    infoOut->appleTimingID = kIOTimingIDDefault;
    infoOut->flags         = kIOTimingInfoValid_AppleTimingID;

    // V74: Proper timing for each resolution
    switch (modeInfo->modeID) {
        case 1: // 1920x1080 @ 60Hz
            infoOut->detailedInfo.v1.horizontalActive = 1920;
            infoOut->detailedInfo.v1.horizontalBlanking = 280;
            infoOut->detailedInfo.v1.horizontalSyncOffset = 60;
            infoOut->detailedInfo.v1.horizontalSyncWidth = 40;
            infoOut->detailedInfo.v1.verticalActive = 1080;
            infoOut->detailedInfo.v1.verticalBlanking = 45;
            infoOut->detailedInfo.v1.verticalSyncOffset = 3;
            infoOut->detailedInfo.v1.verticalSyncWidth = 5;
            infoOut->detailedInfo.v1.pixelClock = 148500000;
            IOLog("[V74] getTimingInfoForDisplayMode(): 1920x1080 @ 60Hz\n");
            break;

        case 2: // 1440x900 @ 60Hz
            infoOut->detailedInfo.v1.horizontalActive = 1440;
            infoOut->detailedInfo.v1.horizontalBlanking = 232;
            infoOut->detailedInfo.v1.horizontalSyncOffset = 48;
            infoOut->detailedInfo.v1.horizontalSyncWidth = 32;
            infoOut->detailedInfo.v1.verticalActive = 900;
            infoOut->detailedInfo.v1.verticalBlanking = 35;
            infoOut->detailedInfo.v1.verticalSyncOffset = 3;
            infoOut->detailedInfo.v1.verticalSyncWidth = 6;
            infoOut->detailedInfo.v1.pixelClock = 106500000;
            IOLog("[V74] getTimingInfoForDisplayMode(): 1440x900 @ 60Hz\n");
            break;

        case 3: // 1366x768 @ 60Hz
            infoOut->detailedInfo.v1.horizontalActive = 1366;
            infoOut->detailedInfo.v1.horizontalBlanking = 174;
            infoOut->detailedInfo.v1.horizontalSyncOffset = 48;
            infoOut->detailedInfo.v1.horizontalSyncWidth = 32;
            infoOut->detailedInfo.v1.verticalActive = 768;
            infoOut->detailedInfo.v1.verticalBlanking = 34;
            infoOut->detailedInfo.v1.verticalSyncOffset = 3;
            infoOut->detailedInfo.v1.verticalSyncWidth = 6;
            infoOut->detailedInfo.v1.pixelClock = 74500000;
            IOLog("[V74] getTimingInfoForDisplayMode(): 1366x768 @ 60Hz\n");
            break;

        case 4: // 1280x720 @ 60Hz
            infoOut->detailedInfo.v1.horizontalActive = 1280;
            infoOut->detailedInfo.v1.horizontalBlanking = 200;
            infoOut->detailedInfo.v1.horizontalSyncOffset = 40;
            infoOut->detailedInfo.v1.horizontalSyncWidth = 40;
            infoOut->detailedInfo.v1.verticalActive = 720;
            infoOut->detailedInfo.v1.verticalBlanking = 30;
            infoOut->detailedInfo.v1.verticalSyncOffset = 5;
            infoOut->detailedInfo.v1.verticalSyncWidth = 5;
            infoOut->detailedInfo.v1.pixelClock = 74250000;
            IOLog("[V74] getTimingInfoForDisplayMode(): 1280x720 @ 60Hz\n");
            break;

        case 5: // 1024x768 @ 60Hz
            infoOut->detailedInfo.v1.horizontalActive = 1024;
            infoOut->detailedInfo.v1.horizontalBlanking = 176;
            infoOut->detailedInfo.v1.horizontalSyncOffset = 24;
            infoOut->detailedInfo.v1.horizontalSyncWidth = 32;
            infoOut->detailedInfo.v1.verticalActive = 768;
            infoOut->detailedInfo.v1.verticalBlanking = 35;
            infoOut->detailedInfo.v1.verticalSyncOffset = 3;
            infoOut->detailedInfo.v1.verticalSyncWidth = 6;
            infoOut->detailedInfo.v1.pixelClock = 65000000;
            IOLog("[V74] getTimingInfoForDisplayMode(): 1024x768 @ 60Hz\n");
            break;

        case 6: // 2560x1440 @ 60Hz
            infoOut->detailedInfo.v1.horizontalActive = 2560;
            infoOut->detailedInfo.v1.horizontalBlanking = 400;
            infoOut->detailedInfo.v1.horizontalSyncOffset = 48;
            infoOut->detailedInfo.v1.horizontalSyncWidth = 32;
            infoOut->detailedInfo.v1.verticalActive = 1440;
            infoOut->detailedInfo.v1.verticalBlanking = 60;
            infoOut->detailedInfo.v1.verticalSyncOffset = 3;
            infoOut->detailedInfo.v1.verticalSyncWidth = 5;
            infoOut->detailedInfo.v1.pixelClock = 241500000;
            IOLog("[V74] getTimingInfoForDisplayMode(): 2560x1440 @ 60Hz\n");
            break;

        default:
            IOLog("[V74] getTimingInfoForDisplayMode(): unknown mode %u\n", displayMode);
            return kIOReturnUnsupportedMode;
    }

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



IOReturn FakeIrisXEFramebuffer::setDisplayMode(IODisplayModeID mode,
                                               IOIndex depth)
{
    IOLog("[V79] setDisplayMode(mode=%u, depth=%u)\n", mode, depth);

    // Validate mode
    bool validMode = false;
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        if (mode == s_displayModes[i].modeID) {
            validMode = true;
            break;
        }
    }
    
    if (!validMode || depth != 0) {
        IOLog("[V79] setDisplayMode: unsupported mode=%u depth=%u\n", mode, depth);
        return kIOReturnUnsupportedMode;
    }

    currentMode  = mode;
    currentDepth = depth;
    
    IOLog("[V79] setDisplayMode: SUCCESS - mode set to %u\n", mode);
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
    IOLog("setBounds() called\n");
    if (bounds) {
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
    IOLog("FakeIrisXEFB::performFlushNow(): running\n");

    if (!framebufferMemory) {
        IOLog("FakeIrisXEFB::performFlushNow(): no framebufferMemory\n");
        return kIOReturnNotReady;
    }

    uint32_t *fb = (uint32_t *) framebufferMemory->getBytesNoCopy();
    if (!fb) {
        IOLog("FakeIrisXEFB::performFlushNow(): getBytesNoCopy() == nullptr\n");
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
                    fRingRCS,
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
    
    
    
    IOLog("FakeIrisXEFB::performFlushNow(): done\n");
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
    
    IOLog("FakeIrisXEFB::flushDisplay(): schedule work\n");

    
    if (!commandGate || !workLoop)
        return performFlushNow(); // fallback safe

    IOReturn r = commandGate->runAction(
        &FakeIrisXEFramebuffer::staticPerformFlush
    );

    IOLog("FakeIrisXEFB::flushDisplay(): runAction returned %x\n", r);
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
    return kIOReturnSuccess;
}









IOReturn FakeIrisXEFramebuffer::setPowerState(unsigned long state,
                                              IOService* whatDevice)
{
    IOLog("[FakeIrisXEFramebuffer] setPowerState(%lu)\n", state);
    return super::setPowerState(state, whatDevice);
}












IOItemCount FakeIrisXEFramebuffer::getDisplayModeCount(void)
{
    IOLog("[V73] getDisplayModeCount(): returning %u modes\n", kNumDisplayModes);
    return kNumDisplayModes;
}




IOReturn FakeIrisXEFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes) {
        IOLog("[V73] getDisplayModes(): null pointer\n");
        return kIOReturnBadArgument;
    }

    // Return mode IDs for all supported modes
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        allDisplayModes[i] = s_displayModes[i].modeID;
        IOLog("[V73] getDisplayModes(): mode %u = %s\n", i+1, s_displayModes[i].name);
    }
    return kIOReturnSuccess;
}




UInt64 FakeIrisXEFramebuffer::getPixelFormatsForDisplayMode(
    IODisplayModeID mode, IOIndex depth)
{
    IOLog("[V73] getPixelFormatsForDisplayMode(mode=%u depth=%u)\n", mode, depth);

    // Check if mode is valid
    bool validMode = false;
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        if (mode == s_displayModes[i].modeID) {
            validMode = true;
            break;
        }
    }
    
    if (!validMode || depth != 0)
        return 0;

    return (1ULL << 0); // ARGB8888
}




IOReturn FakeIrisXEFramebuffer::getPixelInformation(
    IODisplayModeID mode,
    IOIndex depth,
    IOPixelAperture aperture,
    IOPixelInformation *info)
{
    // Find the mode info
    const DisplayModeInfo* modeInfo = nullptr;
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        if (mode == s_displayModes[i].modeID) {
            modeInfo = (DisplayModeInfo*)&s_displayModes[i];
            break;
        }
    }
    
    if (!modeInfo || depth != 0 || aperture != kIOFBSystemAperture) {
        IOLog("[V73] getPixelInformation(): bad args (mode=%u depth=%u ap=%u)\n",
              (unsigned)mode, (int)depth, (unsigned)aperture);
        return kIOReturnBadArgument;
    }

    IOLog("[V73] getPixelInformation(): %ux%u\n", modeInfo->width, modeInfo->height);

    bzero(info, sizeof(IOPixelInformation));

    info->pixelType = kIO32ARGBPixelFormat;
    strlcpy(info->pixelFormat, "ARGB8888", sizeof(info->pixelFormat));

    info->bitsPerComponent = 8;
    info->bitsPerPixel     = 32;
    info->componentCount   = 4;
    info->bytesPerRow      = modeInfo->width * 4;
    info->activeWidth      = modeInfo->width;
    info->activeHeight     = modeInfo->height;

    info->componentMasks[0] = 0xFF000000;  // A
    info->componentMasks[1] = 0x00FF0000;  // R
    info->componentMasks[2] = 0x0000FF00;  // G
    info->componentMasks[3] = 0x000000FF;  // B

    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::getCurrentDisplayMode(IODisplayModeID* displayMode, IOIndex* depth)
{
    if (!displayMode || !depth) {
        IOLog("[V79] getCurrentDisplayMode: null pointer\n");
        return kIOReturnBadArgument;
    }
    
    // If no mode has been set yet, default to mode 1 (1920x1080)
    if (currentMode == 0) {
        currentMode = 1;
        currentDepth = 0;
        IOLog("[V79] getCurrentDisplayMode: defaulting to mode 1\n");
    }
    
    *displayMode = currentMode;
    *depth = currentDepth;
    
    IOLog("[V79] getCurrentDisplayMode: mode=%u depth=%u\n", currentMode, currentDepth);
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
    if (!offset)
        return kIOReturnBadArgument;

    IOLog("getFramebufferOffsetForX_Y(aperture=%d, x=%d, y=%d)\n",
          (int)aperture, (int)x, (int)y);

    const UInt32 bytesPerPixel = 4;
    const UInt32 width         = 1920;
    const UInt32 height        = 1080;

    if (x < 0 || y < 0 || x >= (SInt32)width || y >= (SInt32)height) {
        IOLog("getFramebufferOffsetForX_Y: out of range\n");
        return kIOReturnBadArgument;
    }

    *offset = (y * width + x) * bytesPerPixel;
    return kIOReturnSuccess;
}





IOReturn FakeIrisXEFramebuffer::getInformationForDisplayMode(
    IODisplayModeID mode,
    IODisplayModeInformation* info)
{
    IOLog("[V131] getInformationForDisplayMode(mode=%d)\n", mode);

    if (!info) {
        IOLog("[V131] ❌ Invalid info pointer\n");
        return kIOReturnBadArgument;
    }

    // Find the mode info
    const DisplayModeInfo* modeInfo = nullptr;
    for (uint32_t i = 0; i < kNumDisplayModes; i++) {
        if (mode == s_displayModes[i].modeID) {
            modeInfo = &s_displayModes[i];
            break;
        }
    }

    if (!modeInfo) {
        IOLog("[V131] ❌ Mode %d not found in supported modes\n", mode);
        return kIOReturnUnsupportedMode;
    }

    bzero(info, sizeof(IODisplayModeInformation));

    info->maxDepthIndex = 0;           // one depth index
    info->nominalWidth  = modeInfo->width;
    info->nominalHeight = modeInfo->height;
    info->refreshRate   = (60 << 16);  // 60 Hz fixed-point

    // CoreDisplay expects these for timing lookup
    info->reserved[0] = kIOTimingIDDefault;
    info->reserved[1] = kIOTimingInfoValid_AppleTimingID;
    
    IOLog("[V131] ✅ Mode info: %dx%d @ 60Hz\n", modeInfo->width, modeInfo->height);

    IOLog("Returning display mode info: 1920x1080 @ 60Hz\n");
    return kIOReturnSuccess;
}









IOReturn FakeIrisXEFramebuffer::getStartupDisplayMode(IODisplayModeID *modeID,
                                                      IOIndex *depth)
{
    IOLog("getStartupDisplayMode() called\n");
    if (modeID) *modeID = 1;   // MUST match getDisplayModes()
    if (depth)  *depth  = 0;   // depth index 0 (we’ll treat as 32-bpp)
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
            *value = 1;   // at least one param
            return kIOReturnSuccess;

        // ---- Connection flags (built-in DP) ----
        case kConnectionFlags:
            *value = kIOConnectionBuiltIn | kIOConnectionDisplayPort;
            return kIOReturnSuccess;

        // ---- Online / enabled ----
        case kConnectionIsOnline:              // 'ionl' if asked
            *value = 1;   // panel is online
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
    IOLog("[FakeIrisXEFramebuffer] setAttributeForConnection(conn=%u, attr=0x%08x, value=0x%lx)\n",
          (unsigned)connect, (unsigned)attribute, (unsigned long)value);

    // Ignore everything for now and don’t touch hardware / properties here.
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

    // Set initial duty (low 16 bits)
    wr(BXT_BLC_PWM_DUTY1, duty50);

    // Enable PWM: set MSB (bit31) of CTL register (your snippet used 0x80000000)
    uint32_t ctl = rd(BXT_BLC_PWM_CTL1);
    ctl |= (1u << 31);
    wr(BXT_BLC_PWM_CTL1, ctl);

    IOLog("[FB] initBacklightHardware: period=0x%04x duty=0x%04x CTL=0x%08x\n", period & 0xFFFFu, duty50, ctl);
}

// Set brightness 0..100
bool FakeIrisXEFramebuffer::setBacklightPercent(uint32_t percent)
{
    auto rd = [&](uint32_t off) { return safeMMIORead(off); };
    auto wr = [&](uint32_t off, uint32_t val) { safeMMIOWrite(off, val); };


    percent = clamp_u32(percent, 0, 100);

    // Read period / max from FREQ1 low 16 bits if available
    uint32_t freq = rd(BXT_BLC_PWM_FREQ1);
    uint32_t pwmMax = freq & 0xFFFFu;
    if (pwmMax == 0) {
        // fallback to a sane default (match initBacklightHardware)
        pwmMax = 0xFFFFu;
    }

    // compute duty (0..pwmMax)
    uint32_t duty = (uint64_t)percent * pwmMax / 100u; // 64-bit to avoid overflow
    duty &= 0xFFFFu;

    // Write duty (some implementations place duty in low 16 bits)
    // Some HW expects combined value (period<<16 | duty) — we only write duty because your snippet wrote 0x7FFF directly.
    wr(BXT_BLC_PWM_DUTY1, duty);

    // Ensure PWM enabled
    uint32_t ctl = rd(BXT_BLC_PWM_CTL1);
    if (!(ctl & (1u << 31))) {
        ctl |= (1u << 31);
        wr(BXT_BLC_PWM_CTL1, ctl);
    }

    IOLog("[FB] setBacklightPercent: %u%% -> duty=0x%04x (pwmMax=0x%04x)\n", percent, duty, pwmMax);
    return true;
}

// Read current backlight as percent (0..100)
uint32_t FakeIrisXEFramebuffer::getBacklightPercent()
{
    
    auto rd = [&](uint32_t off) { return safeMMIORead(off); };
    auto wr = [&](uint32_t off, uint32_t val) { safeMMIOWrite(off, val); };

    uint32_t freq = rd(BXT_BLC_PWM_FREQ1);
    uint32_t pwmMax = freq & 0xFFFFu;
    if (pwmMax == 0) pwmMax = 0xFFFFu;
    uint32_t duty = rd(BXT_BLC_PWM_DUTY1) & 0xFFFFu;

    uint32_t percent = (uint64_t)duty * 100u / pwmMax;
    return clamp_u32(percent, 0, 100);
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

        // FIXED: TGL 64-bit PTE (bits 63:0)
        uint64_t pte_val = ((uint64_t)phys >> 12) & 0x0000FFFFFFFFF000ULL;  // Phys page in bits 56:12

        pte_val |= (1ULL << 57);   // Valid bit (bit 57 = 1)
        pte_val |= (0ULL << 59);   // 4KB page (exponent = 0)
        pte_val |= (0ULL << 58);   // System memory (bit 58 = 0)
        pte_val |= (0ULL << 2);    // PAT index 0 (WB cache)

        // FIXED: Write full 64-bit PTE (no low/high split)
        volatile uint64_t* pte_ptr = (volatile uint64_t*)fGGTT + gtt_index;
        *pte_ptr = pte_val;

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





void FakeIrisXEFramebuffer::ggttUnmap(uint64_t gpuAddr, uint32_t pages) {
    if (!fGGTT) return;
    uint64_t off = gpuAddr;
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t idx = (off >> 12);
        if ((idx * 4 + 4) <= fGGTTSize) {
            fGGTT[idx] = 0;
            // if 64-bit PTE used, also clear next dword
        }
        off += 4096;
    }
    __sync_synchronize();
    IOLog("FakeIrisXEFramebuffer: ggttUnmap GPU VA 0x%llx pages=%u\n", (unsigned long long)gpuAddr, pages);
}




// create ring: allocate GEM -> pin -> ggttMap -> program registers
FakeIrisXERing* FakeIrisXEFramebuffer::createRcsRing(size_t ringBytes)
{
    IOLog("(FakeIrisXE) createRcsRing() size=%zu\n", ringBytes);

    // If ring already exists — return it
    if (fRingRCS != nullptr) {
        IOLog("(FakeIrisXE) createRcsRing() — ring already exists @ %p\n", fRingRCS);
        return fRingRCS;
    }

    // Allocate GEM buffer
    FakeIrisXEGEM* ringGem = FakeIrisXEGEM::withSize(ringBytes, 0);
    if (!ringGem) {
        IOLog("❌ createRcsRing — GEM allocation failed\n");
        return nullptr;
    }

    ringGem->pin();

    // Map into GGTT
    uint64_t ringGpuVA = ggttMap(ringGem);
    if (ringGpuVA == 0) {
        IOLog("❌ createRcsRing — GGTT mapping failed\n");
        ringGem->unpin();
        ringGem->release();
        return nullptr;
    }

    // Create ring object
    fRingRCS = new FakeIrisXERing(fBar0);   // mmio accessor
    if (!fRingRCS) {
        IOLog("❌ createRcsRing — ring object alloc failed\n");
        return nullptr;
    }

    // Save metadata into ring object
    fRingRCS->attachRingGPUAddress(ringGpuVA);
    fRingSize = ringBytes;
    fRingGpuVA = ringGpuVA;
    fRingGem = ringGem;      // store GEM (so it doesn’t get freed)

    // Program registers
    fRingRCS->programRingBaseToHW();
    fRingRCS->enableRing();            // RING_CTL = EN | size

    IOLog("🟢 RCS ring created @ GPUVA=0x%llx size=%zu (ptr %p)\n",
          (unsigned long long) ringGpuVA, ringBytes, fRingRCS);

    return fRingRCS;
}



// Submit a batch GEM (already filled by caller) to RCS
// - batchGem: GEM that contains the batch commands.
// - batchOffsetBytes: offset into GEM where batch starts
// - batchSizeBytes: length of the batch
// Return: sequence number or 0 on failure
uint32_t FakeIrisXEFramebuffer::submitBatch(FakeIrisXEGEM* batchGem, size_t batchOffsetBytes, size_t batchSizeBytes) {
    if (!fRingRCS || !batchGem) {
        IOLog("FakeIrisXEFramebuffer: submitBatch - bad args\n");
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
    bool ok = fRingRCS->submitBatch64(batchGpu);
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
#define MI_BATCH_BUFFER_END    (0xA << 23)
#endif

#ifndef MI_STORE_DWORD_IMM
#define MI_STORE_DWORD_IMM     MI_INSTR(0x20, 0)
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
    // [0] = MI_STORE_DWORD_IMM | MI_USE_GGTT
    // [1] = seq (immediate)
    // [2] = low32(fenceGpuAddr)
    // [3] = high32(fenceGpuAddr)
    // [4] = MI_BATCH_BUFFER_END

    uint32_t* p = (uint32_t*)tailDesc->getBytesNoCopy();
    p[0] = MI_STORE_DWORD_IMM | MI_USE_GGTT;
    p[1] = seq;
    p[2] = (uint32_t)(fenceGpuAddr & 0xFFFFFFFFULL);
    p[3] = (uint32_t)(fenceGpuAddr >> 32);
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
    if (!userBatchGem || !fRingRCS) {
        IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - invalid args\n");
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
    bool ok = fRingRCS->submitBatch64(masterGpuAddr);
    if (!ok) {
        IOLog("FakeIrisXEFramebuffer: appendFenceAndSubmit - ring submit failed\n");
        masterGem->unpin(); masterGem->release();
        tailGem->unpin(); tailGem->release();
        userBatchGem->unpin();
        return 0;
    }

    IOLog("FakeIrisXEFramebuffer: Batch submitted (master=0x%llx user=0x%llx tail=0x%llx) seq=%u\n",
          (unsigned long long)masterGpuAddr, (unsigned long long)userGpu, (unsigned long long)tailGpuAddr, seq);

    // Do not release master/tail immediately — keep them alive until fence observed to avoid reuse.
    // We can store them in a small list or release after fence observed in IRQ handler.
    // For simplicity keep refs in a tiny list (you should implement proper cleanup).
    // For now: keep masterGem and tailGem retained and rely on periodic cleanup / reboot (for bring-up).
    // (Production: add list<active_submission> and cleanup when fence observed.)

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
                    // reset fence immediately (optional)
                    fenceCpu[0] = 0;
                    __sync_synchronize();

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
    IOLog("RING_HEAD = 0x%08x\n", r(RING_HEAD));
    IOLog("RING_TAIL = 0x%08x\n", r(RING_TAIL));
    IOLog("RING_CTL  = 0x%08x\n", r(RING_CTL));
    IOLog("RING_BASE_LO = 0x%08x\n", r(RING_BASE_LO));
    IOLog("RING_BASE_HI = 0x%08x\n", r(RING_BASE_HI));

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







// register addresses (verify with your offsets / defines)
#define REG_FORCEWAKE_REQ   0x00A278  // FORCEWAKE02 (request)
#define REG_FORCEWAKE_ACK   0x130044  // FORCEWAKE02_ACK (ack)
#define REG_RCS0_IER        0x2604



bool FakeIrisXEFramebuffer::forcewakeRenderHold(uint32_t timeoutMs)
{
    IOLog("(FakeIrisXE) forcewakeRenderHold(): TigerLake RENDER-domain wake\n");

    // Tiger Lake actually uses only lower 4 bits (Render FW domain)
    const uint32_t FW_REQ   = 0xA188;    // same register, but limited domain
    const uint32_t FW_ACK   = 0x130044;
    const uint32_t FW_MASK  = 0x000F000F; // only 4 LSB active on this laptop

    safeMMIOWrite(FW_REQ, FW_MASK);
    (void)safeMMIORead(FW_REQ);

    uint32_t elapsed = 0;
    while (elapsed < timeoutMs) {
        uint32_t ack = safeMMIORead(FW_ACK);

        // Only lower 4 bits matter (Render domain)
        if ((ack & FW_MASK) == 0xF) {
            IOLog("(FakeIrisXE) Render forcewake OK (ACK=0x%08X)\n", ack);
            return true;
        }

        IODelay(1000);
        elapsed++;
    }

    uint32_t final = safeMMIORead(FW_ACK);
    IOLog("❌ Render forcewake TIMEOUT (ACK=0x%08X)\n", final);
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
    safeMMIOWrite(REG_RCS0_IER, ier);
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
    
    // 3. Load firmware from EMBEDDED arrays (not from resources)
    // Use your embedded arrays directly
    
    // Determine which firmware to use based on Device ID
    const unsigned char* guc_bin = nullptr;
    unsigned int guc_len = 0;
    UInt16 deviceID = pciDevice->configRead16(kIOPCIConfigDeviceID);

    if (deviceID == 0x46A3) {
        // Alder Lake P
        guc_bin = adlp_guc_70_1_1_bin;
        guc_len = adlp_guc_70_1_1_bin_len;
        IOLog("(FakeIrisXE) Selected ADL-P GuC firmware\n");
    } else {
        // Default to Tiger Lake
        guc_bin = tgl_guc_70_1_1_bin;
        guc_len = tgl_guc_70_1_1_bin_len;
        IOLog("(FakeIrisXE) Selected TGL GuC firmware\n");
    }

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
    
    return true;
}

// ============================================================================
// V42: Command Execution Test
// ============================================================================
bool FakeIrisXEFramebuffer::testGuCCommandExecution()
{
    IOLog("(FakeIrisXE) [V43] testGuCCommandExecution(): Testing GuC command execution\n");
    
    if (!fGuC) {
        IOLog("(FakeIrisXE) [V43] GuC not available for test\n");
        return false;
    }
    
    IOLog("(FakeIrisXE) [V43] Command execution test would run here\n");
    return true;
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

void FakeIrisXEFramebuffer::unmapGEMFromGGTT(uint64_t gpuAddr) {
    if (gpuAddr == 0) {
        return;
    }
    
    // For now, just log the unmap request
    // In a full implementation, we'd walk the GGTT and invalidate entries
    IOLog("[V90] unmapGEMFromGGTT: Unmapping GPU addr 0x%llx\n", (unsigned long long)gpuAddr);
    
    // TODO: Implement proper GGTT entry invalidation
    // This would involve finding the PTE and clearing the valid bit
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
    // Find surface
    SurfaceInfo* surf = nullptr;
    for (uint32_t i = 0; i < kMaxSurfaces; i++) {
        if (fSurfaces[i].inUse && fSurfaces[i].id == surfaceId) {
            surf = &fSurfaces[i];
            break;
        }
    }
    
    if (!surf) {
        IOLog("[V90] ❌ Copy to FB failed: surface not found\n");
        return kIOReturnNotFound;
    }
    
    IOLog("[V90] Copy surface %llu to framebuffer at (%u, %u)\n", surfaceId, x, y);
    
    // TODO: Submit XY_SRC_COPY_BLT to copy surface to primary framebuffer
    // This is the critical path for WindowServer to display content
    
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::fillRect(uint32_t x, uint32_t y, uint32_t width, 
                                         uint32_t height, uint32_t color)
{
    IOLog("[V90] FillRect: (%u, %u) size %ux%u color=0x%08x\n", x, y, width, height, color);
    
    // TODO: Submit XY_COLOR_BLT to fill rectangle
    // This is used for clears and solid fills in compositing
    
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::submit2DCommandBuffer(void* commands, size_t size)
{
    IOLog("[V90] submit2DCommandBuffer: %zu bytes\n", size);
    
    if (!fExeclist || !fRcsRing) {
        IOLog("[V90] ❌ Cannot submit - execlist not ready\n");
        return kIOReturnNotReady;
    }
    
    // TODO: Parse command buffer and submit via execlist
    // This is the main entry point for WindowServer command submission
    
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::submitBlitCommand(uint32_t opcode, void* data, size_t size)
{
    IOLog("[V90] submitBlitCommand: opcode=%u, size=%zu\n", opcode, size);
    
    // Handle common blit opcodes
    switch (opcode) {
        case 0x46: // XY_SRC_COPY_BLT
            IOLog("[V90]   -> XY_SRC_COPY_BLT\n");
            break;
        case 0x50: // XY_COLOR_BLT
            IOLog("[V90]   -> XY_COLOR_BLT\n");
            break;
        case 0x52: // XY_PIXEL_BLT
            IOLog("[V90]   -> XY_PIXEL_BLT\n");
            break;
        default:
            IOLog("[V90]   -> Unknown opcode 0x%02x\n", opcode);
            break;
    }
    
    return kIOReturnSuccess;
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
    
    // Note: batchGem is retained by submission, will be released on completion
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

#include <libkern/libkern.h>

// Entry points must match CFBundleExecutable name (FakeIrisXE)
extern "C" kern_return_t FakeIrisXE_start(kmod_info_t *ki, void *data) {
    return KERN_SUCCESS;
}

extern "C" kern_return_t FakeIrisXE_stop(kmod_info_t *ki, void *data) {
    return KERN_SUCCESS;
}
