//
//  FakeIrisXEBacklight.cpp
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 01/12/25.
//

#include "FakeIrisXEBacklight.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <string.h>


#define super IOService

#include "FakeIrisXEFramebuffer.hpp"

OSDefineMetaClassAndStructors(FakeIrisXEBacklight, IOService);

bool FakeIrisXEBacklight::init(OSDictionary* dict) {
    if (!super::init(dict)) return false;
    fOwnerFB = nullptr;
    fBrightness = 100;
    fMaxBrightness = 100;
    fPanelType = 0;
    fPwmFrequency = 200;
    fBacklightEnabled = true;
    fRampingEnabled = true;
    fRampDelayMs = 50;
    fMinBrightness = 0;
    fCurrentPwm = 0;
    fBacklightLevel = 100;
    fGammaEntries = 256;
    fGammaTable = nullptr;
    fNotifier = nullptr;
    fRampTarget = nullptr;
    fTargetBrightness = 100;
    fLastBrightness = 100;
    fPanelSerial = 0;
    fBacklightNits = 300;
    fDisplayType = 2;
    fSmoothBrightness = true;
    fSavedBrightness = 100;
    fRampTimer = nullptr;
    memset(fPanelVendor, 0, sizeof(fPanelVendor));
    memset(fPanelID, 0, sizeof(fPanelID));
    return true;
}

bool FakeIrisXEBacklight::start(IOService* provider) {
    if (!super::start(provider)) return false;

    // Find display0 under the framebuffer FIRST - needed for initializeBacklightHW
    OSIterator* it = provider->getChildIterator(gIOServicePlane);
    if (it) {
        IOService* child = nullptr;
        while ((child = OSDynamicCast(IOService, it->getNextObject()))) {
            if (!strcmp(child->getName(), "display0")) {
                fOwnerFB = child->getProvider();
                break;
            }
        }
        it->release();
    }

    if (!fOwnerFB) fOwnerFB = provider;

    // Initialize backlight hardware AFTER fOwnerFB is set
    initializeBacklightHW();
    
    // Publish standard properties
    setProperty("AppleBacklightDisplay", kOSBooleanTrue);
    setProperty("IOProviderClass", "IODisplayConnect");
    setProperty("IONameMatch", "AppleBacklightDisplay");
    setProperty("AAPL,backlight-control", kOSBooleanTrue);

    // Set panel defaults
    fPanelType = 1;
    fPwmFrequency = 200;
    strncpy(fPanelVendor, "Intel", sizeof(fPanelVendor) - 1);
    strncpy(fPanelID, "IrisXe", sizeof(fPanelID) - 1);
    fPanelSerial = 1234567890ULL;

    // Publish numeric properties
    publishBacklightProperties();
    
    // Log initial state
    logBacklightState();

    registerService();
    IOLog("[FakeIrisXEBacklight] started (brightness=%u max=%u pwm=%uHz)\n", 
          fBrightness, fMaxBrightness, fPwmFrequency);
    return true;
}

void FakeIrisXEBacklight::stop(IOService* provider) {
    shutdownBacklightHW();
    stopRampTimer();
    IOLog("[FakeIrisXEBacklight] stop\n");
    super::stop(provider);
}

void FakeIrisXEBacklight::free() {
    if (fGammaTable) {
        IODelete(fGammaTable, uint16_t, fGammaEntries * 3);
        fGammaTable = nullptr;
    }
    super::free();
}

void FakeIrisXEBacklight::publishBacklightProperties() {
    OSNumber* nMax = OSNumber::withNumber((uint64_t)fMaxBrightness, 32);
    if (nMax) { setProperty("max-brightness", nMax); nMax->release(); }

    OSNumber* nCur = OSNumber::withNumber((uint64_t)fBrightness, 32);
    if (nCur) { setProperty("brightness", nCur); nCur->release(); }

    OSNumber* nNits = OSNumber::withNumber((uint64_t)300, 32);
    if (nNits) { setProperty("IOBacklightNits", nNits); nNits->release(); }

    OSNumber* nBlk = OSNumber::withNumber((uint64_t)1, 32);
    if (nBlk) { setProperty("AAPL,backlight-index", nBlk); nBlk->release(); }

    OSNumber* nPwm = OSNumber::withNumber((uint64_t)fPwmFrequency, 32);
    if (nPwm) { setProperty("pwm-frequency", nPwm); nPwm->release(); }

    OSNumber* nMin = OSNumber::withNumber((uint64_t)fMinBrightness, 32);
    if (nMin) { setProperty("min-brightness", nMin); nMin->release(); }

    OSNumber* nPanel = OSNumber::withNumber(fPanelSerial, 32);
    if (nPanel) { setProperty("panel-serial", nPanel); nPanel->release(); }

    OSString* strVendor = OSString::withCString(fPanelVendor);
    if (strVendor) { setProperty("panel-vendor", strVendor); strVendor->release(); }

    OSString* strID = OSString::withCString(fPanelID);
    if (strID) { setProperty("panel-id", strID); strID->release(); }

    OSBoolean* bEnabled = fBacklightEnabled ? kOSBooleanTrue : kOSBooleanFalse;
    setProperty("backlight-enabled", bEnabled);

    OSBoolean* bRamp = fRampingEnabled ? kOSBooleanTrue : kOSBooleanFalse;
    setProperty("smooth-brightness", bRamp);

    // V290: Additional properties from IntelBacklight
    OSNumber* nVer = OSNumber::withNumber((uint64_t)290, 32);
    if (nVer) { setProperty("software-version", nVer); nVer->release(); }

    OSNumber* nHWVer = OSNumber::withNumber((uint64_t)1, 32);
    if (nHWVer) { setProperty("hardware-version", nHWVer); nHWVer->release(); }

    OSNumber* nDispType = OSNumber::withNumber((uint64_t)2, 32);
    if (nDispType) { setProperty("display-type", nDispType); nDispType->release(); }

    setProperty("driver-type", "FakeIrisXE");
    setProperty("backlight-engine", "PWM");

    OSNumber* nGammaSize = OSNumber::withNumber((uint64_t)fGammaEntries, 32);
    if (nGammaSize) { setProperty("gamma-size", nGammaSize); nGammaSize->release(); }

    OSNumber* nRampSteps = OSNumber::withNumber((uint64_t)10, 32);
    if (nRampSteps) { setProperty("ramp-steps", nRampSteps); nRampSteps->release(); }
}

void FakeIrisXEBacklight::logBacklightState() {
    IOLog("[FakeIrisXEBacklight] State: brightness=%u max=%u pwm=%uHz min=%u enabled=%u ramping=%u rampDelay=%ums\n",
          fBrightness, fMaxBrightness, fPwmFrequency, fMinBrightness,
          fBacklightEnabled, fRampingEnabled, fRampDelayMs);
    IOLog("[FakeIrisXEBacklight] Panel: vendor=%s id=%s serial=%llu type=%u\n",
          fPanelVendor, fPanelID, fPanelSerial, fPanelType);
}

IOReturn FakeIrisXEBacklight::setBacklightEnabled(bool enabled) {
    fBacklightEnabled = enabled;
    if (enabled) {
        enableBacklightPwm();
    } else {
        disableBacklightPwm();
    }
    OSBoolean* bEnabled = enabled ? kOSBooleanTrue : kOSBooleanFalse;
    setProperty("backlight-enabled", bEnabled);
    IOLog("[FakeIrisXEBacklight] setBacklightEnabled(%s)\n", enabled ? "true" : "false");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setPwmFrequency(uint32_t freq) {
    if (freq < 100 || freq > 10000) {
        IOLog("[FakeIrisXEBacklight] Invalid PWM frequency %u\n", freq);
        return kIOReturnBadArgument;
    }
    fPwmFrequency = freq;
    OSNumber* nPwm = OSNumber::withNumber((uint64_t)freq, 32);
    if (nPwm) { setProperty("pwm-frequency", nPwm); nPwm->release(); }
    IOLog("[FakeIrisXEBacklight] setPwmFrequency(%u Hz)\n", freq);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setMinBrightness(uint32_t min) {
    if (min > fMaxBrightness) {
        IOLog("[FakeIrisXEBacklight] Invalid min brightness %u\n", min);
        return kIOReturnBadArgument;
    }
    fMinBrightness = min;
    OSNumber* nMin = OSNumber::withNumber((uint64_t)min, 32);
    if (nMin) { setProperty("min-brightness", nMin); nMin->release(); }
    IOLog("[FakeIrisXEBacklight] setMinBrightness(%u)\n", min);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setRampingEnabled(bool enabled) {
    fRampingEnabled = enabled;
    OSBoolean* bRamp = enabled ? kOSBooleanTrue : kOSBooleanFalse;
    setProperty("smooth-brightering", bRamp);
    IOLog("[FakeIrisXEBacklight] setRampingEnabled(%s)\n", enabled ? "true" : "false");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setRampDelay(uint32_t delayMs) {
    fRampDelayMs = delayMs;
    IOLog("[FakeIrisXEBacklight] setRampDelay(%u ms)\n", delayMs);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setGammaTable(uint16_t* table, uint32_t entries) {
    if (!table || entries == 0 || entries > 1024) {
        IOLog("[FakeIrisXEBacklight] Invalid gamma table\n");
        return kIOReturnBadArgument;
    }
    if (fGammaTable) {
        IOFree(fGammaTable, fGammaEntries * 3 * sizeof(uint16_t));
        fGammaTable = nullptr;
    }
    fGammaTable = (uint16_t*)IOMalloc(entries * 3 * sizeof(uint16_t));
    if (!fGammaTable) {
        return kIOReturnNoMemory;
    }
    memcpy(fGammaTable, table, entries * 3 * sizeof(uint16_t));
    fGammaEntries = entries;
    IOLog("[FakeIrisXEBacklight] setGammaTable(%u entries)\n", entries);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setPanelInfo(uint32_t serial, const char* vendor, const char* id) {
    fPanelSerial = serial;
    if (vendor) strncpy(fPanelVendor, vendor, sizeof(fPanelVendor) - 1);
    if (id) strncpy(fPanelID, id, sizeof(fPanelID) - 1);
    
    OSNumber* nPanel = OSNumber::withNumber(serial, 32);
    if (nPanel) { setProperty("panel-serial", nPanel); nPanel->release(); }
    
    OSString* strVendor = OSString::withCString(fPanelVendor);
    if (strVendor) { setProperty("panel-vendor", strVendor); strVendor->release(); }
    
    OSString* strID = OSString::withCString(fPanelID);
    if (strID) { setProperty("panel-id", strID); strID->release(); }
    
    IOLog("[FakeIrisXEBacklight] setPanelInfo(serial=%u vendor=%s id=%s)\n", serial, vendor, id);
    return kIOReturnSuccess;
}

uint32_t FakeIrisXEBacklight::calculatePwmFromBrightness(uint32_t brightness) {
    if (brightness > fMaxBrightness) brightness = fMaxBrightness;
    if (brightness < fMinBrightness) brightness = fMinBrightness;
    
    uint32_t range = fMaxBrightness - fMinBrightness;
    if (range == 0) return 0;
    
    uint32_t adjusted = brightness - fMinBrightness;
    uint32_t pwm = (adjusted * 0xFFFF) / range;
    
    fCurrentPwm = pwm;
    return pwm;
}

uint32_t FakeIrisXEBacklight::calculateBrightnessFromPwm(uint32_t pwm) {
    if (pwm > 0xFFFF) pwm = 0xFFFF;
    
    uint32_t range = fMaxBrightness - fMinBrightness;
    if (range == 0) return fMinBrightness;
    
    uint32_t brightness = ((pwm * range) / 0xFFFF) + fMinBrightness;
    return brightness;
}

IOReturn FakeIrisXEBacklight::readBacklightRegisters() {
    IOLog("[FakeIrisXEBacklight] readBacklightRegisters() - placeholder\n");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::writeBacklightRegisters() {
    IOLog("[FakeIrisXEBacklight] writeBacklightRegisters() pwm=0x%08X\n", fCurrentPwm);
    if (fOwnerFB) {
        FakeIrisXEFramebuffer* fb = OSDynamicCast(FakeIrisXEFramebuffer, fOwnerFB);
        if (fb) {
            fb->setBacklightPercent(fBrightness);
        }
    }
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::verifyBacklightWrite() {
    IOLog("[FakeIrisXEBacklight] verifyBacklightWrite()\n");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::enableBacklightPwm() {
    IOLog("[FakeIrisXEBacklight] enableBacklightPwm() - enabling PWM hardware\n");
    
    // V342: Actually enable PWM by calling into framebuffer
    if (fOwnerFB) {
        FakeIrisXEFramebuffer* fb = OSDynamicCast(FakeIrisXEFramebuffer, fOwnerFB);
        if (fb) {
            fb->setBacklightPercent(fBrightness, "enablePwm");
            IOLog("[FakeIrisXEBacklight] enableBacklightPwm() - called framebuffer setBacklightPercent(%u)\n", fBrightness);
            setProperty("FakeIrisXEBacklightPwmEnabled", kOSBooleanTrue);
            return kIOReturnSuccess;
        }
    }
    
    IOLog("[FakeIrisXEBacklight] enableBacklightPwm() - no framebuffer, marking enabled anyway\n");
    setProperty("FakeIrisXEBacklightPwmEnabled", kOSBooleanTrue);
    fBacklightEnabled = true;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::disableBacklightPwm() {
    IOLog("[FakeIrisXEBacklight] disableBacklightPwm()\n");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setBacklightLevelDirect(uint32_t level) {
    fBacklightLevel = level;
    calculatePwmFromBrightness(level);
    writeBacklightRegisters();
    IOLog("[FakeIrisXEBacklight] setBacklightLevelDirect(%u)\n", level);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::getBacklightLevelDirect(uint32_t* level) {
    if (!level) return kIOReturnBadArgument;
    *level = fBacklightLevel;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::initializeBacklightHW() {
    IOLog("[FakeIrisXEBacklight] initializeBacklightHW()\n");
    fBacklightEnabled = true;
    fCurrentPwm = calculatePwmFromBrightness(fBrightness);
    
    // V342: Actually enable PWM hardware
    return enableBacklightPwm();
}

IOReturn FakeIrisXEBacklight::shutdownBacklightHW() {
    IOLog("[FakeIrisXEBacklight] shutdownBacklightHW()\n");
    fBacklightEnabled = false;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setSmoothBrightness(uint32_t target) {
    if (target > fMaxBrightness) target = fMaxBrightness;
    
    fTargetBrightness = target;
    
    if (fRampingEnabled && fTargetBrightness != fLastBrightness) {
        return applyBrightnessWithRamp(target);
    } else {
        return applyBrightnessImmediate(target);
    }
}

IOReturn FakeIrisXEBacklight::applyBrightnessWithRamp(uint32_t target) {
    IOLog("[FakeIrisXEBacklight] applyBrightnessWithRamp(%u -> %u)\n", fBrightness, target);
    fTargetBrightness = target;
    fBrightness = target;
    fLastBrightness = target;
    calculatePwmFromBrightness(target);
    writeBacklightRegisters();
    return setBrightnessInternal(target);
}

IOReturn FakeIrisXEBacklight::applyBrightnessImmediate(uint32_t level) {
    IOLog("[FakeIrisXEBacklight] applyBrightnessImmediate(%u)\n", level);
    fBrightness = level;
    fLastBrightness = level;
    calculatePwmFromBrightness(level);
    writeBacklightRegisters();
    return setBrightnessInternal(level);
}

// Called by system (or our user-client). Update registry, call into framebuffer.
IOReturn FakeIrisXEBacklight::setBrightnessInternal(uint32_t level) {
    if (level > fMaxBrightness) level = fMaxBrightness;
    fBrightness = level;

    // update registry property (so UI sees current value)
    OSNumber* nCur = OSNumber::withNumber((uint64_t)fBrightness, 32);
    if (nCur) {
        setProperty("brightness", nCur);
        nCur->release();
    }


    // Now call into the framebuffer to actually set PWM / registers.
    if (fOwnerFB) {
        FakeIrisXEFramebuffer* fb = OSDynamicCast(FakeIrisXEFramebuffer, fOwnerFB);
        if (fb) {
            // you must implement this method in FakeIrisXEFramebuffer:
            fb->setBacklightPercent(level);
            IOLog("[FakeIrisXEBacklight] forwarded brightness=%u to framebuffer\n", fBrightness);
            return kIOReturnSuccess;
        } else {
            IOLog("[FakeIrisXEBacklight] provider is not FakeIrisXEFramebuffer\n");
            return kIOReturnUnsupported;
        }
    }

    return kIOReturnNotAttached;
}


// Add near other methods in FakeIrisXEBacklight.cpp
IOReturn FakeIrisXEBacklight::setProperties(OSObject *properties) {
    IOLog("[FakeIrisXEBacklight] setProperties() called\n");

    if (!properties) return super::setProperties(properties);

    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) {
        IOLog("[FakeIrisXEBacklight] setProperties(): not a dict\n");
        return super::setProperties(properties);
    }

    // 1) Direct "brightness" = OSNumber
    OSObject *obj = dict->getObject("brightness");
    if (obj) {
        OSNumber *num = OSDynamicCast(OSNumber, obj);
        if (num) {
            uint32_t v = num->unsigned32BitValue();
            IOLog("[FakeIrisXEBacklight] setProperties(): brightness (direct) = %u\n", v);
            return setBrightnessInternal(v);
        }
    }

    // 1b) Direct "vblm" = OSNumber (AppleDisplay parameter key)
    obj = dict->getObject("vblm");
    if (obj) {
        OSNumber *num = OSDynamicCast(OSNumber, obj);
        if (num) {
            uint32_t v = num->unsigned32BitValue();
            IOLog("[FakeIrisXEBacklight] setProperties(): vblm (direct) = %u\n", v);
            return setBrightnessInternal(v);
        }
    }

    // 2) Some clients send "IODisplayParameters" => { "brightness": { "value": <num> } }
    obj = dict->getObject("IODisplayParameters");
    if (obj) {
        OSDictionary *params = OSDynamicCast(OSDictionary, obj);
        if (params) {
            OSObject *b = params->getObject("brightness");
            OSDictionary *bDict = OSDynamicCast(OSDictionary, b);
            if (bDict) {
                OSObject *valObj = bDict->getObject("value");
                OSNumber *valNum = OSDynamicCast(OSNumber, valObj);
                if (valNum) {
                    uint32_t v = valNum->unsigned32BitValue();
                    IOLog("[FakeIrisXEBacklight] setProperties(): brightness (IODisplayParameters.value) = %u\n", v);
                    return setBrightnessInternal(v);
                }
            }
            // fallback: sometimes "brightness" is OSNumber directly under IODisplayParameters
            OSNumber *num = OSDynamicCast(OSNumber, params->getObject("brightness"));
            if (num) {
                uint32_t v = num->unsigned32BitValue();
                IOLog("[FakeIrisXEBacklight] setProperties(): brightness (IODisplayParameters, direct) = %u\n", v);
                return setBrightnessInternal(v);
            }

            // AppleDisplay may use "vblm"
            OSObject *vObj = params->getObject("vblm");
            OSDictionary *vDict = OSDynamicCast(OSDictionary, vObj);
            if (vDict) {
                OSObject *valObj = vDict->getObject("value");
                OSNumber *valNum = OSDynamicCast(OSNumber, valObj);
                if (valNum) {
                    uint32_t v = valNum->unsigned32BitValue();
                    IOLog("[FakeIrisXEBacklight] setProperties(): vblm (IODisplayParameters.value) = %u\n", v);
                    return setBrightnessInternal(v);
                }
            }
            OSNumber *vNum = OSDynamicCast(OSNumber, params->getObject("vblm"));
            if (vNum) {
                uint32_t v = vNum->unsigned32BitValue();
                IOLog("[FakeIrisXEBacklight] setProperties(): vblm (IODisplayParameters, direct) = %u\n", v);
                return setBrightnessInternal(v);
            }
        }
    }

    // Not handled — pass to superclass
    IOLog("[FakeIrisXEBacklight] setProperties(): no brightness key found\n");
    return super::setProperties(properties);
}

// V290: Enhanced backlight control from IntelBacklight
IOReturn FakeIrisXEBacklight::setBacklightNits(uint32_t nits) {
    if (nits > 1000) nits = 1000;
    fBacklightNits = nits;
    OSNumber* nNits = OSNumber::withNumber((uint64_t)nits, 32);
    if (nNits) { setProperty("IOBacklightNits", nNits); nNits->release(); }
    IOLog("[FakeIrisXEBacklight] setBacklightNits: %u nits\n", nits);
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEBacklight::setDisplayType(uint32_t type) {
    fDisplayType = type;
    OSNumber* nType = OSNumber::withNumber((uint64_t)type, 32);
    if (nType) { setProperty("display-type", nType); nType->release(); }
    IOLog("[FakeIrisXEBacklight] setDisplayType: %u\n", type);
    return kIOReturnSuccess;
}

bool FakeIrisXEBacklight::saveBrightnessState() {
    fSavedBrightness = fBrightness;
    IOLog("[FakeIrisXEBacklight] saveBrightnessState: %u\n", fSavedBrightness);
    return true;
}

bool FakeIrisXEBacklight::restoreBrightnessState() {
    if (fSavedBrightness != fBrightness) {
        IOLog("[FakeIrisXEBacklight] restoreBrightnessState: %u -> %u\n", fBrightness, fSavedBrightness);
        return setBrightnessInternal(fSavedBrightness) == kIOReturnSuccess;
    }
    return true;
}

IOReturn FakeIrisXEBacklight::notifyBrightnessChange(uint32_t oldLevel, uint32_t newLevel) {
    IOLog("[FakeIrisXEBacklight] notifyBrightnessChange: %u -> %u\n", oldLevel, newLevel);
    return kIOReturnSuccess;
}

void FakeIrisXEBacklight::startRampTimer() {
    if (!fRampTimer) {
        IOLog("[FakeIrisXEBacklight] startRampTimer: no timer source\n");
        return;
    }
    fTargetBrightness = fBrightness;
    IOLog("[FakeIrisXEBacklight] startRampTimer: target=%u\n", fTargetBrightness);
    fRampTimer->setTimeoutMS(fRampDelayMs);
}

void FakeIrisXEBacklight::stopRampTimer() {
    if (fRampTimer) {
        fRampTimer->cancelTimeout();
        IOLog("[FakeIrisXEBacklight] stopRampTimer: cancelled\n");
    }
}

void FakeIrisXEBacklight::rampTimerCallback(OSObject* owner, IOTimerEventSource* timer) {
    FakeIrisXEBacklight* self = OSDynamicCast(FakeIrisXEBacklight, owner);
    if (!self) return;
    
    if (self->fTargetBrightness != self->fBrightness) {
        uint32_t step = (self->fTargetBrightness > self->fBrightness) ? 1 : -1;
        self->fBrightness += step;
        self->applyBrightnessImmediate(self->fBrightness);
        self->fRampTimer->setTimeoutMS(self->fRampDelayMs);
    }
}
