//
//  FakeIrisXEConnectorManager.hpp
//  FakeIrisXEFramebuffer
//
//  Tiger Lake Connector Manager - Real connector discovery and enablement
//

#ifndef FakeIrisXEConnectorManager_hpp
#define FakeIrisXEConnectorManager_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <stddef.h>
#include <stdint.h>

static const size_t kFakeIrisXEMaxEdidBytes = 512;
static const size_t kFakeIrisXEMaxDpcdBytes = 16;
static const size_t kFakeIrisXEMaxVbtBytes = 16384;

// Tiger Lake has 4 DDI ports: A, B, C, D (some shared with USB-C/TC)
enum class TGLConnectorType {
    Unknown = 0,
    eDP = 4,      // 0x04 - Embedded DisplayPort
    HDMI = 8,     // 0x08 - HDMI
    DP = 16,     // 0x10 - DisplayPort
    USB4TypeC = 32 // 0x20 - USB4/Thunderbolt Type-C
};

// DDI Port mapping for Tiger Lake
enum class TGLDDIPort {
    DDI_A = 0,
    DDI_B = 1,
    DDI_C = 2,
    DDI_D = 3,
    DDI_TC1 = 4,  // Type-C port 1
    DDI_TC2 = 5,  // Type-C port 2
    DDI_TC3 = 6,  // Type-C port 3
    DDI_TC4 = 7   // Type-C port 4
};

// AUX channel mapping for Tiger Lake
enum class TGLAUXChannel {
    AUX_A = 0,
    AUX_B = 1,
    AUX_C = 2,
    AUX_D = 3,
    AUX_TC1 = 4,
    AUX_TC2 = 5,
    AUX_TC3 = 6,
    AUX_TC4 = 7
};

// HPD pin mapping
enum class TGLHPDPin {
    HPD_NONE = 0,
    HPD_PIN_0 = 1,
    HPD_PIN_1 = 2,
    HPD_PIN_2 = 3,
    HPD_PIN_3 = 4,
    HPD_PIN_4 = 5,
    HPD_PIN_5 = 6,
    HPD_PIN_6 = 7,
    HPD_PIN_7 = 8
};

// Connector descriptor - describes a physical connector on the system
struct TGLConnectorDesc {
    uint8_t index;              // 0-3, corresponds to framebuffer-conX
    TGLConnectorType type;       // eDP, HDMI, DP, USB4TypeC
    TGLDDIPort ddiPort;         // Which DDI is this connector on?
    TGLAUXChannel auxChannel;   // Which AUX channel for DP/eDP?
    TGLHPDPin hpdPin;           // Hotplug detect pin
    uint8_t maxLanes;           // Maximum lane count (1, 2, 4)
    uint16_t maxBitRate;        // Maximum bit rate in Mbps (8100, 10100, etc.)
    bool isInternal;            // True for eDP/internal panels
    bool supportsAudio;        // HDMI/DP audio capable
    uint32_t hdpBit;            // HDP status bit mask for this connector
    bool present;               // Link/panel presence detected
    bool discoveredFromVbt;     // Connector came from real VBT parsing
    bool hasDpcd;               // DPCD base bytes cached
    bool hasEdid;               // EDID bytes cached
    uint8_t panelType;          // VBT panel type when known
    uint16_t edidLength;        // Cached EDID byte count
    uint8_t dpcd[kFakeIrisXEMaxDpcdBytes];
    uint8_t edid[kFakeIrisXEMaxEdidBytes];
};

// Transcoder to pipe mapping
struct TGLTranscoderPipe {
    uint8_t transcoder;  // 0=Transcoder A, 1=Transcoder B, etc.
    uint8_t pipe;        // 0=Pipe A, 1=Pipe B, etc.
    TGLDDIPort ddiPort;  // Associated DDI
};

// Class declaration
class FakeIrisXEConnectorManager
{
public:
    FakeIrisXEConnectorManager();
    ~FakeIrisXEConnectorManager();
    
    // Initialize with MMIO base for register access
    bool init(volatile uint8_t* mmioBase, IOPCIDevice* provider = nullptr);
    
    // Discover connectors from VBT or fallback
    void discoverConnectors();
    
    // Probe for connector presence (HPD, AUX)
    void probeConnectors();
    
    // Initialize a specific connector type
    bool initEDPConnector(TGLConnectorDesc& conn);
    bool initHDMIConnector(TGLConnectorDesc& conn);
    bool initDPConnector(TGLConnectorDesc& conn);
    bool initTypeCConnector(TGLConnectorDesc& conn);
    
    // Enable pipe and transcoder for active connector
    bool enablePipeAndTranscoder(uint8_t pipe, uint8_t transcoder, TGLDDIPort ddi);
    
    // Get connector descriptor by index
    TGLConnectorDesc* getConnector(uint8_t index);
    
    // Get number of discovered connectors
    uint8_t getConnectorCount() { return m_connectorCount; }
    
    // Get internal panel connector (eDP)
    TGLConnectorDesc* getInternalPanel();

    const uint8_t* getConnectorEDID(uint8_t index, uint16_t* outLength) const;
    const uint8_t* getPrimaryDisplayEDID(uint16_t* outLength, TGLConnectorDesc** outConnector) const;
    const char* getOpRegionSource() const { return m_opregionSource; }
    uint64_t getOpRegionPhys() const { return m_opregionPhys; }
    uint32_t getOpRegionHeaderOffset() const { return m_opregionHeaderOffset; }
    bool isOpRegionSignatureValid() const { return m_opregionSignatureValid; }
    uint64_t getOpRegionRvda() const { return m_opregionRvda; }
    uint32_t getOpRegionRvds() const { return m_opregionRvds; }
    uint32_t getVBTHeaderOffset() const { return m_vbtHeaderOffset; }
    uint64_t getVBTPhys() const { return m_vbtPhys; }
    bool isRealVBTLoaded() const { return m_vbtLoaded; }
    uint16_t getVBTVersion() const { return m_vbtVersion; }
    uint16_t getBDBVersion() const { return m_bdbVersion; }
    uint32_t getPanelPowerOnDelay() const { return m_panelPowerOnDelay; }
    uint32_t getPanelPowerOffDelay() const { return m_panelPowerOffDelay; }
    uint8_t getDpcdBacklightCaps() const { return m_dpcdBacklightCaps; }
    bool isDisplayTreeReady() const { return m_displayTreeReady; }
    void setDisplayTreeReady(bool ready) { m_displayTreeReady = ready; }
    
    // Publish connector properties to IORegistry (for compatibility)
    void publishConnectorProperties();
    
    // Logging
    void logConnectorInfo();
    void logDDIRegisters();
    void logHPDStatus();
    
private:
    volatile uint8_t* m_mmioBase;
    IOPCIDevice* m_provider;
    TGLConnectorDesc m_connectors[4];
    uint8_t m_connectorCount;
    TGLConnectorDesc* m_internalPanel;
    bool m_vbtLoaded;
    bool m_strictVbtMode;
    uint16_t m_vbtVersion;
    uint16_t m_bdbVersion;
    uint8_t m_opregionMajor;
    uint8_t m_opregionMinor;
    uint32_t m_opregionMboxes;
    uint64_t m_opregionPhys;
    uint32_t m_opregionHeaderOffset;
    uint64_t m_opregionRvda;
    uint32_t m_opregionRvds;
    bool m_opregionSignatureValid;
    char m_opregionSource[32];
    uint32_t m_vbtHeaderOffset;
    uint64_t m_vbtPhys;
    size_t m_vbtLength;
    uint8_t m_vbtStorage[kFakeIrisXEMaxVbtBytes];
    uint32_t m_panelPowerOnDelay;
    uint32_t m_panelPowerOffDelay;
    uint8_t m_dpcdBacklightCaps;
    bool m_displayTreeReady;
    
    // Register helpers
    uint32_t readReg(uint32_t offset);
    void writeReg(uint32_t offset, uint32_t value);
    
    // DDI Port control registers (Tiger Lake)
    static constexpr uint32_t DDI_BUF_CTL_A   = 0x64000;
    static constexpr uint32_t DDI_BUF_CTL_B   = 0x64100;
    static constexpr uint32_t DDI_BUF_CTL_C   = 0x64200;
    static constexpr uint32_t DDI_BUF_CTL_D   = 0x64300;
    static constexpr uint32_t DDI_BUF_CTL_TC1 = 0x64400;
    static constexpr uint32_t DDI_BUF_CTL_TC2 = 0x64500;
    static constexpr uint32_t DDI_BUF_CTL_TC3 = 0x64600;
    static constexpr uint32_t DDI_BUF_CTL_TC4 = 0x64700;
    
    // Transcoder function control
    static constexpr uint32_t TRANS_DDI_FUNC_CTL_A = 0x60400;
    static constexpr uint32_t TRANS_DDI_FUNC_CTL_B = 0x61400;
    static constexpr uint32_t TRANS_DDI_FUNC_CTL_C = 0x62400;
    static constexpr uint32_t TRANS_DDI_FUNC_CTL_D = 0x63400;
    
    // Pipe configuration
    static constexpr uint32_t PIPECONF_A = 0x70008;
    static constexpr uint32_t PIPECONF_B = 0x7000C;
    static constexpr uint32_t PIPECONF_C = 0x70010;
    
    // Transcoder configuration
    static constexpr uint32_t TRANS_CONF_A = 0x60000;
    static constexpr uint32_t TRANS_CONF_B = 0x61000;
    static constexpr uint32_t TRANS_CONF_C = 0x62000;
    
    // AUX channel registers
    static constexpr uint32_t AUX_CTL_A = 0x64010;
    static constexpr uint32_t AUX_CTL_B = 0x64110;
    static constexpr uint32_t AUX_CTL_C = 0x64210;
    static constexpr uint32_t AUX_CTL_D = 0x64310;
    static constexpr uint32_t AUX_CTL_TC1 = 0x64410;
    static constexpr uint32_t AUX_CTL_TC2 = 0x64510;
    static constexpr uint32_t AUX_CTL_TC3 = 0x64610;
    static constexpr uint32_t AUX_CTL_TC4 = 0x64710;
    
    // Hotplug detect registers
    static constexpr uint32_t PCH_PORT_HPDCF = 0xC4030;
    static constexpr uint32_t PCH_PORT_HPDSF = 0xC4034;
    static constexpr uint32_t PCH_PORT_HPDCNTF = 0xC4038;
    static constexpr uint32_t PCH_PORT_HPDSTS = 0xC403C;
    static constexpr uint32_t PCH_PORT_HPDEN = 0xC4040;
    
    // DP/eDP registers
    static constexpr uint32_t DP_A = 0x64100;
    static constexpr uint32_t DP_B = 0x64200;
    static constexpr uint32_t DP_C = 0x64300;
    static constexpr uint32_t DP_D = 0x64400;
    
    // Panel power
    static constexpr uint32_t PCH_PP_CONTROL = 0xC7200;
    static constexpr uint32_t PCH_PP_STATUS = 0xC7204;
    static constexpr uint32_t PCH_PP_ON_DELAYS = 0xC7208;
    static constexpr uint32_t PCH_PP_OFF_DELAYS = 0xC720C;
    static constexpr uint32_t PCH_PP_DIVISOR = 0xC7210;
    
    // Get DDI register offset from port
    uint32_t getDDIBufferControl(TGLDDIPort port);
    uint32_t getTranscoderFunctionControl(TGLDDIPort port);
    uint32_t getAUXControl(TGLAUXChannel aux);
    uint32_t getAUXData(TGLAUXChannel aux, uint32_t index);
    uint32_t getPipeConfig(uint8_t pipe);
    uint32_t getTranscoderConfig(uint8_t transcoder);
    
    // Check if port is enabled
    bool isDDIEnabled(TGLDDIPort port);
    bool isPipeEnabled(uint8_t pipe);
    bool isTranscoderEnabled(uint8_t transcoder);
    
    // HPD detection
    bool checkHPD(TGLConnectorDesc& conn);

    // AUX/DPCD/EDID helpers
    bool auxTransfer(TGLAUXChannel aux,
                     const uint8_t* send,
                     uint32_t sendBytes,
                     uint8_t* recv,
                     uint32_t& recvBytes);
    bool auxNativeRead(TGLAUXChannel aux, uint32_t address, uint8_t* data, uint32_t size);
    bool auxI2CWrite(TGLAUXChannel aux, uint8_t address, const uint8_t* data, uint32_t size, bool mot);
    bool auxI2CRead(TGLAUXChannel aux, uint8_t address, uint8_t* data, uint32_t size, bool mot);
    bool readDPCD(TGLConnectorDesc& conn, uint32_t address, uint8_t* data, uint32_t size);
    bool readEDID(TGLConnectorDesc& conn);
    void checkDpcdBacklightCaps(TGLConnectorDesc& conn);

    // VBT discovery helpers
    bool loadVBT();
    bool loadVBTFromOpRegion();
    bool loadVBTFromRegistry();
    bool validateVBTBlob(const uint8_t* bytes, size_t length, const char* source) const;
    bool adoptVBTWindow(const uint8_t* bytes, uint32_t length, const char* source, uint64_t physBase, bool updateOpRegionSource);
    bool parseVBTConnectors();
    bool applyVBTChildDevice(const uint8_t* childBytes, uint8_t childSize, uint8_t slotIndex);
    bool decodeChildDeviceToConnector(const uint8_t* childBytes, uint8_t childSize, TGLConnectorDesc& outConn) const;
    const uint8_t* findBDBSection(uint8_t blockId, uint16_t* outSize) const;
    
    // Initialize default/fallback connector map
    void initDefaultConnectorMap();
    
    // Enable DDI clock/buffer
    bool enableDDI(TGLDDIPort port);
    
    // Panel power sequencing for eDP
    bool powerUpEDPPanel();
    bool powerDownEDPPanel();
    bool isEDPPanelPowered();
};

// Inline helpers
inline uint32_t FakeIrisXEConnectorManager::readReg(uint32_t offset)
{
    if (!m_mmioBase) return 0xFFFFFFFF;
    return *(volatile uint32_t*)(m_mmioBase + offset);
}

inline void FakeIrisXEConnectorManager::writeReg(uint32_t offset, uint32_t value)
{
    if (!m_mmioBase) return;
    *(volatile uint32_t*)(m_mmioBase + offset) = value;
    // Memory barrier
    __asm__ __volatile__("" ::: "memory");
}

#endif /* FakeIrisXEConnectorManager_hpp */
