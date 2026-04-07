//
//  FakeIrisXEBacklight.hpp
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 01/12/25.
//
#ifndef FAKE_IRIS_XE_BACKLIGHT_HPP
#define FAKE_IRIS_XE_BACKLIGHT_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

class FakeIrisXEFramebuffer;

class FakeIrisXEBacklight : public IOService {
    OSDeclareDefaultStructors(FakeIrisXEBacklight);

private:
    IOService* fOwnerFB;
    uint32_t  fBrightness;
    uint32_t  fMaxBrightness;
    uint32_t  fPanelType;
    uint32_t  fPwmFrequency;
    bool      fBacklightEnabled;
    bool      fRampingEnabled;
    uint32_t  fRampDelayMs;
    uint32_t  fMinBrightness;
    uint32_t  fCurrentPwm;
    uint32_t  fBacklightLevel;
    uint32_t  fGammaEntries;
    uint16_t* fGammaTable;
    OSObject* fNotifier;
    OSObject* fRampTarget;
    uint32_t  fTargetBrightness;
    uint32_t  fLastBrightness;
    uint64_t  fPanelSerial;
    char      fPanelVendor[16];
    char      fPanelID[16];

public:
    virtual bool init(OSDictionary* = nullptr) override;
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;
    virtual void free() override;

    IOReturn setBrightnessInternal(uint32_t level);
    uint32_t getBrightnessInternal() const { return fBrightness; }
    uint32_t getMaxBrightness() const { return fMaxBrightness; }
    IOReturn setProperties(OSObject* properties)override;

    IOReturn setBacklightEnabled(bool enabled);
    bool getBacklightEnabled() const { return fBacklightEnabled; }
    
    IOReturn setPwmFrequency(uint32_t freq);
    uint32_t getPwmFrequency() const { return fPwmFrequency; }
    
    IOReturn setMinBrightness(uint32_t min);
    uint32_t getMinBrightness() const { return fMinBrightness; }
    
    IOReturn setGammaTable(uint16_t* table, uint32_t entries);
    uint16_t* getGammaTable() const { return fGammaTable; }
    
    IOReturn setPanelInfo(uint32_t serial, const char* vendor, const char* id);
    
    IOReturn setRampingEnabled(bool enabled);
    bool getRampingEnabled() const { return fRampingEnabled; }
    
    IOReturn setRampDelay(uint32_t delayMs);
    uint32_t getRampDelay() const { return fRampDelayMs; }
    
    uint32_t calculatePwmFromBrightness(uint32_t brightness);
    uint32_t calculateBrightnessFromPwm(uint32_t pwm);
    
    IOReturn readBacklightRegisters();
    IOReturn writeBacklightRegisters();
    IOReturn verifyBacklightWrite();
    
    IOReturn enableBacklightPwm();
    IOReturn disableBacklightPwm();
    
    IOReturn setBacklightLevelDirect(uint32_t level);
    IOReturn getBacklightLevelDirect(uint32_t* level);
    
    IOReturn initializeBacklightHW();
    IOReturn shutdownBacklightHW();
    
    const char* getPanelVendor() const { return fPanelVendor; }
    const char* getPanelID() const { return fPanelID; }
    uint64_t getPanelSerial() const { return fPanelSerial; }
    
    IOReturn setSmoothBrightness(uint32_t target);
    void startRampTimer();
    void stopRampTimer();
    
    static void rampTimerCallback(OSObject* owner, IOTimerEventSource* timer);

    // V290: Enhanced backlight control from IntelBacklight
    IOReturn setBacklightNits(uint32_t nits);
    uint32_t getBacklightNits() const { return fBacklightNits; }
    IOReturn setDisplayType(uint32_t type);
    uint32_t getDisplayType() const { return fDisplayType; }
    bool saveBrightnessState();
    bool restoreBrightnessState();
    IOReturn notifyBrightnessChange(uint32_t oldLevel, uint32_t newLevel);

protected:
    IOReturn applyBrightnessWithRamp(uint32_t target);
    IOReturn applyBrightnessImmediate(uint32_t level);
    void publishBacklightProperties();
    void logBacklightState();
    uint32_t fBacklightNits;
    uint32_t fDisplayType;
    bool fSmoothBrightness;
    uint32_t fSavedBrightness;
    IOTimerEventSource* fRampTimer;
};

#endif // FAKE_IRIS_XE_BACKLIGHT_HPP
