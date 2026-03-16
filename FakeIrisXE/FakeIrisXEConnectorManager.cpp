//
//  FakeIrisXEConnectorManager.cpp
//  FakeIrisXEFramebuffer
//
//  Tiger Lake Connector Manager - Real connector discovery and enablement
//  Reference: Intel PRM Vol 12, Linux i915 intel_bios.c, intel_ddi.c, intel_dp.c
//

#include "FakeIrisXEConnectorManager.hpp"

// IOLog is already declared in FakeIrisXEFramebuffer.hpp via IOKit headers
// Just use it directly

FakeIrisXEConnectorManager::FakeIrisXEConnectorManager()
    : m_mmioBase(nullptr),
      m_connectorCount(0),
      m_internalPanel(nullptr)
{
    // Initialize connector array
    for (int i = 0; i < 4; i++) {
        m_connectors[i] = {};
    }
}

FakeIrisXEConnectorManager::~FakeIrisXEConnectorManager()
{
}

bool FakeIrisXEConnectorManager::init(volatile uint8_t* mmioBase)
{
    if (!mmioBase) {
        IOLog("[TGL-Connector] ERROR: NULL MMIO base\n");
        return false;
    }
    
    m_mmioBase = mmioBase;
    IOLog("[TGL-Connector] Initialized with MMIO base %p\n", mmioBase);
    
    return true;
}

void FakeIrisXEConnectorManager::discoverConnectors()
{
    IOLog("[TGL-Connector] Starting connector discovery...\n");
    
    // Log current HDP status to see what's connected
    logHPDStatus();
    
    // Log DDI register states
    logDDIRegisters();
    
    // Initialize default connector map (fallback if VBT not available)
    initDefaultConnectorMap();
    
    // Probe each connector for presence
    probeConnectors();
    
    // Log discovered connectors
    logConnectorInfo();
}

void FakeIrisXEConnectorManager::initDefaultConnectorMap()
{
    IOLog("[TGL-Connector] Initializing default connector map (Tiger Lake laptop fallback)...\n");
    
    // Tiger Lake laptop default: Port A = eDP (internal), Port B = HDMI, Port C = DP, Port D = USB-C
    // This is a safe fallback but real hardware may differ
    
    // Connector 0: Port A - eDP (internal panel)
    m_connectors[0] = {
        .index = 0,
        .type = TGLConnectorType::eDP,
        .ddiPort = TGLDDIPort::DDI_A,
        .auxChannel = TGLAUXChannel::AUX_A,
        .hpdPin = TGLHPDPin::HPD_NONE,  // Internal panel, no hotplug
        .maxLanes = 4,
        .maxBitRate = 10100,  // 10.1 Gbps
        .isInternal = true,
        .supportsAudio = false,
        .hdpBit = 0
    };
    m_internalPanel = &m_connectors[0];
    
    // Connector 1: Port B - HDMI
    m_connectors[1] = {
        .index = 1,
        .type = TGLConnectorType::HDMI,
        .ddiPort = TGLDDIPort::DDI_B,
        .auxChannel = TGLAUXChannel::AUX_B,
        .hpdPin = TGLHPDPin::HPD_PIN_1,
        .maxLanes = 4,
        .maxBitRate = 5940,  // 5.94 Gbps (HDMI 2.0)
        .isInternal = false,
        .supportsAudio = true,
        .hdpBit = (1 << 1)
    };
    
    // Connector 2: Port C - DP
    m_connectors[2] = {
        .index = 2,
        .type = TGLConnectorType::DP,
        .ddiPort = TGLDDIPort::DDI_C,
        .auxChannel = TGLAUXChannel::AUX_C,
        .hpdPin = TGLHPDPin::HPD_PIN_2,
        .maxLanes = 4,
        .maxBitRate = 10100,  // 10.1 Gbps
        .isInternal = false,
        .supportsAudio = true,
        .hdpBit = (1 << 2)
    };
    
    // Connector 3: Port D - USB4/Type-C (DP Alt Mode)
    m_connectors[3] = {
        .index = 3,
        .type = TGLConnectorType::USB4TypeC,
        .ddiPort = TGLDDIPort::DDI_TC1,
        .auxChannel = TGLAUXChannel::AUX_TC1,
        .hpdPin = TGLHPDPin::HPD_PIN_3,
        .maxLanes = 4,
        .maxBitRate = 10100,  // 10.1 Gbps
        .isInternal = false,
        .supportsAudio = true,
        .hdpBit = (1 << 3)
    };
    
    m_connectorCount = 4;
    
    IOLog("[TGL-Connector] Default map: con0=eDP(DDI_A), con1=HDMI(DDI_B), con2=DP(DDI_C), con3=TypeC(TC1)\n");
}

void FakeIrisXEConnectorManager::probeConnectors()
{
    IOLog("[TGL-Connector] Probing connectors for presence...\n");
    
    for (int i = 0; i < m_connectorCount; i++) {
        TGLConnectorDesc& conn = m_connectors[i];
        
        if (conn.type == TGLConnectorType::eDP) {
            // eDP: Check if panel is powered
            if (isEDPPanelPowered()) {
                IOLog("[TGL-Connector] Connector %d: eDP panel DETECTED (powered)\n", i);
                conn.hdpBit = 0xFFFFFFFF;  // Mark as present
            } else {
                IOLog("[TGL-Connector] Connector %d: eDP panel NOT detected\n", i);
            }
        } else {
            // HDMI/DP/USB-C: Check HPD
            if (checkHPD(conn)) {
                IOLog("[TGL-Connector] Connector %d: %s DETECTED via HPD\n", i,
                      conn.type == TGLConnectorType::HDMI ? "HDMI" :
                      conn.type == TGLConnectorType::DP ? "DP" : "TypeC");
            } else {
                IOLog("[TGL-Connector] Connector %d: %s NOT connected\n", i,
                      conn.type == TGLConnectorType::HDMI ? "HDMI" :
                      conn.type == TGLConnectorType::DP ? "DP" : "TypeC");
            }
        }
    }
}

bool FakeIrisXEConnectorManager::checkHPD(TGLConnectorDesc& conn)
{
    // Read HPD status from PCH
    uint32_t hpdSts = readReg(PCH_PORT_HPDSTS);
    
    IOLog("[TGL-Connector] HPD status register = 0x%08X\n", hpdSts);
    
    // Check the specific HDP bit for this connector
    if (conn.hdpBit != 0 && conn.hdpBit != 0xFFFFFFFF) {
        return (hpdSts & conn.hdpBit) != 0;
    }
    
    return false;
}

TGLConnectorDesc* FakeIrisXEConnectorManager::getConnector(uint8_t index)
{
    if (index >= 4) return nullptr;
    return &m_connectors[index];
}

TGLConnectorDesc* FakeIrisXEConnectorManager::getInternalPanel()
{
    return m_internalPanel;
}

void FakeIrisXEConnectorManager::logConnectorInfo()
{
    IOLog("========== TGL Connector Info ==========\n");
    for (int i = 0; i < m_connectorCount; i++) {
        TGLConnectorDesc& conn = m_connectors[i];
        const char* typeStr = conn.type == TGLConnectorType::eDP ? "eDP" :
                              conn.type == TGLConnectorType::HDMI ? "HDMI" :
                              conn.type == TGLConnectorType::DP ? "DP" :
                              conn.type == TGLConnectorType::USB4TypeC ? "USB4-TypeC" : "Unknown";
        const char* ddiStr = conn.ddiPort == TGLDDIPort::DDI_A ? "DDI_A" :
                             conn.ddiPort == TGLDDIPort::DDI_B ? "DDI_B" :
                             conn.ddiPort == TGLDDIPort::DDI_C ? "DDI_C" :
                             conn.ddiPort == TGLDDIPort::DDI_D ? "DDI_D" :
                             conn.ddiPort == TGLDDIPort::DDI_TC1 ? "TC1" :
                             conn.ddiPort == TGLDDIPort::DDI_TC2 ? "TC2" : "Unknown";
        
        IOLog("  Connector %d: Type=%s, Port=%s, Lanes=%d, Bitrate=%d Gbps, Internal=%s\n",
              conn.index, typeStr, ddiStr, conn.maxLanes, conn.maxBitRate / 1000,
              conn.isInternal ? "yes" : "no");
    }
    IOLog("========================================\n");
}

void FakeIrisXEConnectorManager::logDDIRegisters()
{
    IOLog("[TGL-Connector] DDI Buffer Control Registers:\n");
    IOLog("  DDI_BUF_CTL_A = 0x%08X (DDI_A %s)\n",
          readReg(DDI_BUF_CTL_A),
          isDDIEnabled(TGLDDIPort::DDI_A) ? "ENABLED" : "disabled");
    IOLog("  DDI_BUF_CTL_B = 0x%08X (DDI_B %s)\n",
          readReg(DDI_BUF_CTL_B),
          isDDIEnabled(TGLDDIPort::DDI_B) ? "ENABLED" : "disabled");
    IOLog("  DDI_BUF_CTL_C = 0x%08X (DDI_C %s)\n",
          readReg(DDI_BUF_CTL_C),
          isDDIEnabled(TGLDDIPort::DDI_C) ? "ENABLED" : "disabled");
    IOLog("  DDI_BUF_CTL_D = 0x%08X (DDI_D %s)\n",
          readReg(DDI_BUF_CTL_D),
          isDDIEnabled(TGLDDIPort::DDI_D) ? "ENABLED" : "disabled");
}

void FakeIrisXEConnectorManager::logHPDStatus()
{
    IOLog("[TGL-Connector] HPD Status Registers:\n");
    IOLog("  PCH_PORT_HPDSTS = 0x%08X\n", readReg(PCH_PORT_HPDSTS));
    IOLog("  PCH_PORT_HPDCNTF = 0x%08X\n", readReg(PCH_PORT_HPDCNTF));
    IOLog("  PCH_PORT_HPDEN = 0x%08X\n", readReg(PCH_PORT_HPDEN));
}

// =============================================
// Port/Register helper implementations
// =============================================

uint32_t FakeIrisXEConnectorManager::getDDIBufferControl(TGLDDIPort port)
{
    switch (port) {
        case TGLDDIPort::DDI_A: return DDI_BUF_CTL_A;
        case TGLDDIPort::DDI_B: return DDI_BUF_CTL_B;
        case TGLDDIPort::DDI_C: return DDI_BUF_CTL_C;
        case TGLDDIPort::DDI_D: return DDI_BUF_CTL_D;
        case TGLDDIPort::DDI_TC1: return DDI_BUF_CTL_TC1;
        case TGLDDIPort::DDI_TC2: return DDI_BUF_CTL_TC2;
        case TGLDDIPort::DDI_TC3: return DDI_BUF_CTL_TC3;
        case TGLDDIPort::DDI_TC4: return DDI_BUF_CTL_TC4;
        default: return 0;
    }
}

uint32_t FakeIrisXEConnectorManager::getTranscoderFunctionControl(TGLDDIPort port)
{
    switch (port) {
        case TGLDDIPort::DDI_A: return TRANS_DDI_FUNC_CTL_A;
        case TGLDDIPort::DDI_B: return TRANS_DDI_FUNC_CTL_B;
        case TGLDDIPort::DDI_C: return TRANS_DDI_FUNC_CTL_C;
        case TGLDDIPort::DDI_D: return TRANS_DDI_FUNC_CTL_D;
        default: return 0;
    }
}

uint32_t FakeIrisXEConnectorManager::getAUXControl(TGLAUXChannel aux)
{
    switch (aux) {
        case TGLAUXChannel::AUX_A: return AUX_CTL_A;
        case TGLAUXChannel::AUX_B: return AUX_CTL_B;
        case TGLAUXChannel::AUX_C: return AUX_CTL_C;
        case TGLAUXChannel::AUX_D: return AUX_CTL_D;
        case TGLAUXChannel::AUX_TC1: return AUX_CTL_TC1;
        case TGLAUXChannel::AUX_TC2: return AUX_CTL_TC2;
        case TGLAUXChannel::AUX_TC3: return AUX_CTL_TC3;
        case TGLAUXChannel::AUX_TC4: return AUX_CTL_TC4;
        default: return 0;
    }
}

uint32_t FakeIrisXEConnectorManager::getPipeConfig(uint8_t pipe)
{
    switch (pipe) {
        case 0: return PIPECONF_A;
        case 1: return PIPECONF_B;
        case 2: return PIPECONF_C;
        default: return 0;
    }
}

uint32_t FakeIrisXEConnectorManager::getTranscoderConfig(uint8_t transcoder)
{
    switch (transcoder) {
        case 0: return TRANS_CONF_A;
        case 1: return TRANS_CONF_B;
        case 2: return TRANS_CONF_C;
        default: return 0;
    }
}

bool FakeIrisXEConnectorManager::isDDIEnabled(TGLDDIPort port)
{
    uint32_t ddiReg = getDDIBufferControl(port);
    if (!ddiReg) return false;
    
    uint32_t val = readReg(ddiReg);
    return (val & (1 << 31)) != 0;  // Bit 31 = DDI Buffer Enable
}

bool FakeIrisXEConnectorManager::isPipeEnabled(uint8_t pipe)
{
    uint32_t pipeConf = getPipeConfig(pipe);
    if (!pipeConf) return false;
    
    uint32_t val = readReg(pipeConf);
    return (val & (1 << 31)) != 0;  // Bit 31 = Pipe Enable
}

bool FakeIrisXEConnectorManager::isTranscoderEnabled(uint8_t transcoder)
{
    uint32_t transConf = getTranscoderConfig(transcoder);
    if (!transConf) return false;
    
    uint32_t val = readReg(transConf);
    return (val & (1 << 31)) != 0;  // Bit 31 = Transcoder Enable
}

// =============================================
// Panel Power Control (for eDP)
// =============================================

bool FakeIrisXEConnectorManager::powerUpEDPPanel()
{
    IOLog("[TGL-Connector] Powering up eDP panel...\n");
    
    // Panel power on
    uint32_t ppControl = readReg(PCH_PP_CONTROL);
    writeReg(PCH_PP_CONTROL, ppControl | (1 << 0));  // Power on
    
    // Wait for power up
    int timeout = 100;
    while (timeout-- > 0) {
        uint32_t ppStatus = readReg(PCH_PP_STATUS);
        if (ppStatus & (1 << 2)) {  // Power down timeout
            IOLog("[TGL-Connector] eDP power up timeout\n");
            return false;
        }
        if ((ppStatus & (1 << 31)) == 0) {  // Power sequence complete
            IOLog("[TGL-Connector] eDP panel powered on\n");
            return true;
        }
        // Simple delay
        for (volatile int i = 0; i < 10000; i++);
    }
    
    IOLog("[TGL-Connector] eDP power up timed out\n");
    return false;
}

bool FakeIrisXEConnectorManager::powerDownEDPPanel()
{
    IOLog("[TGL-Connector] Powering down eDP panel...\n");
    
    // Panel power off
    uint32_t ppControl = readReg(PCH_PP_CONTROL);
    writeReg(PCH_PP_CONTROL, ppControl & ~(1 << 0));  // Power off
    
    return true;
}

bool FakeIrisXEConnectorManager::isEDPPanelPowered()
{
    uint32_t ppStatus = readReg(PCH_PP_STATUS);
    // Bit 31 = Power sequence in progress
    // Bit 30 = Power target state (1=on, 0=off)
    // Bit 29 = Power cycle in progress
    return ((ppStatus & (1 << 30)) != 0);
}

// =============================================
// DDI Initialization
// =============================================

bool FakeIrisXEConnectorManager::enableDDI(TGLDDIPort port)
{
    IOLog("[TGL-Connector] Enabling DDI port %d\n", (int)port);
    
    uint32_t ddiReg = getDDIBufferControl(port);
    if (!ddiReg) {
        IOLog("[TGL-Connector] ERROR: Invalid DDI port %d\n", (int)port);
        return false;
    }
    
    uint32_t ddiVal = readReg(ddiReg);
    IOLog("[TGL-Connector] Current DDI_BUF_CTL = 0x%08X\n", ddiVal);
    
    // Enable DDI buffer
    ddiVal |= (1 << 31);  // Bit 31: DDI Buffer Enable
    ddiVal &= ~((1 << 1) | (1 << 0));  // Clear port select (for eDP)
    
    writeReg(ddiReg, ddiVal);
    
    IOLog("[TGL-Connector] DDI_BUF_CTL set to 0x%08X\n", readReg(ddiReg));
    
    return true;
}

// =============================================
// Connector Initialization
// =============================================

bool FakeIrisXEConnectorManager::initEDPConnector(TGLConnectorDesc& conn)
{
    IOLog("[TGL-Connector] Initializing eDP connector on DDI_%c\n",
          'A' + (int)conn.ddiPort);
    
    // Power up the panel
    if (!powerUpEDPPanel()) {
        IOLog("[TGL-Connector] WARNING: Panel may not be powered\n");
    }
    
    // Enable DDI
    if (!enableDDI(conn.ddiPort)) {
        IOLog("[TGL-Connector] ERROR: Failed to enable DDI\n");
        return false;
    }
    
    IOLog("[TGL-Connector] eDP connector initialized\n");
    return true;
}

bool FakeIrisXEConnectorManager::initHDMIConnector(TGLConnectorDesc& conn)
{
    IOLog("[TGL-Connector] Initializing HDMI connector on DDI_%c\n",
          'A' + (int)conn.ddiPort);
    
    // Enable DDI
    if (!enableDDI(conn.ddiPort)) {
        IOLog("[TGL-Connector] ERROR: Failed to enable DDI\n");
        return false;
    }
    
    // Set up HDMI function control
    uint32_t transFunc = getTranscoderFunctionControl(conn.ddiPort);
    if (transFunc) {
        uint32_t val = readReg(transFunc);
        val &= ~0xF;  // Clear port select
        val |= (int)conn.ddiPort & 0xF;  // Set port
        val &= ~(1 << 4);  // Clear HDMI mode (set to 0 for HDMI)
        writeReg(transFunc, val);
        IOLog("[TGL-Connector] TRANS_DDI_FUNC_CTL set to 0x%08X\n", readReg(transFunc));
    }
    
    IOLog("[TGL-Connector] HDMI connector initialized\n");
    return true;
}

bool FakeIrisXEConnectorManager::initDPConnector(TGLConnectorDesc& conn)
{
    IOLog("[TGL-Connector] Initializing DP connector on DDI_%c\n",
          'A' + (int)conn.ddiPort);
    
    // Enable DDI
    if (!enableDDI(conn.ddiPort)) {
        IOLog("[TGL-Connector] ERROR: Failed to enable DDI\n");
        return false;
    }
    
    // Set up DP function control
    uint32_t transFunc = getTranscoderFunctionControl(conn.ddiPort);
    if (transFunc) {
        uint32_t val = readReg(transFunc);
        val &= ~0xF;  // Clear port select
        val |= (int)conn.ddiPort & 0xF;  // Set port
        val |= (1 << 4);  // Set DP mode (bit 4 = 1 for DP)
        writeReg(transFunc, val);
        IOLog("[TGL-Connector] TRANS_DDI_FUNC_CTL set to 0x%08X\n", readReg(transFunc));
    }
    
    IOLog("[TGL-Connector] DP connector initialized\n");
    return true;
}

bool FakeIrisXEConnectorManager::initTypeCConnector(TGLConnectorDesc& conn)
{
    IOLog("[TGL-Connector] Initializing USB4/Type-C connector on TC%d\n",
          (int)conn.ddiPort - (int)TGLDDIPort::DDI_TC1 + 1);
    
    // Type-C is similar to DP but with USB4/Thunderbolt
    // Enable the port
    if (!enableDDI(conn.ddiPort)) {
        IOLog("[TGL-Connector] ERROR: Failed to enable Type-C DDI\n");
        return false;
    }
    
    // Type-C specific: configure for DP Alt Mode
    // This typically involves:
    // 1. USB-C policy control
    // 2. AUX channel setup
    // 3. USB/DP role switching
    
    IOLog("[TGL-Connector] Type-C connector initialized\n");
    return true;
}

bool FakeIrisXEConnectorManager::enablePipeAndTranscoder(uint8_t pipe, uint8_t transcoder, TGLDDIPort ddi)
{
    IOLog("[TGL-Connector] Enabling Pipe %d, Transcoder %d for DDI_%c\n",
          pipe, transcoder, 'A' + (int)ddi);
    
    // Enable pipe
    uint32_t pipeConf = getPipeConfig(pipe);
    if (pipeConf) {
        uint32_t val = readReg(pipeConf);
        val |= (1 << 31);  // Pipe Enable
        val &= ~(1 << 23); // Disable interlacing
        writeReg(pipeConf, val);
        IOLog("[TGL-Connector] PIPECONF_%c = 0x%08X\n", 'A' + pipe, readReg(pipeConf));
    }
    
    // Enable transcoder
    uint32_t transConf = getTranscoderConfig(transcoder);
    if (transConf) {
        uint32_t val = readReg(transConf);
        val |= (1 << 31);  // Transcoder Enable
        writeReg(transConf, val);
        IOLog("[TGL-Connector] TRANS_CONF_%c = 0x%08X\n", 'A' + transcoder, readReg(transConf));
    }
    
    // Connect transcoder to DDI
    uint32_t transFunc = getTranscoderFunctionControl(ddi);
    if (transFunc) {
        uint32_t val = readReg(transFunc);
        val |= (1 << 31);  // DDI Function Enable
        val &= ~0xF;       // Clear port select
        val |= (int)ddi & 0xF;
        writeReg(transFunc, val);
        IOLog("[TGL-Connector] TRANS_DDI_FUNC_CTL_%c = 0x%08X\n", 'A' + (int)ddi, readReg(transFunc));
    }
    
    return true;
}

// =============================================
// Property Publishing (for compatibility)
// =============================================

void FakeIrisXEConnectorManager::publishConnectorProperties()
{
    // This would publish framebuffer-conX properties
    // The actual publishing happens in FakeIrisXEFramebuffer::setProperties
    // This method is here for reference
    
    IOLog("[TGL-Connector] Connector properties should be published from framebuffer\n");
}
