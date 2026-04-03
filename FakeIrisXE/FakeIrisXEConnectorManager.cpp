#include "FakeIrisXEConnectorManager.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>

namespace {

static const uint32_t kAuxCtlSendBusy = (1u << 31);
static const uint32_t kAuxCtlDone = (1u << 30);
static const uint32_t kAuxCtlInterrupt = (1u << 29);
static const uint32_t kAuxCtlTimeoutError = (1u << 28);
static const uint32_t kAuxCtlTimeoutMax = (3u << 26);
static const uint32_t kAuxCtlReceiveError = (1u << 25);
static const uint32_t kAuxCtlMessageSizeShift = 20u;
static const uint32_t kAuxCtlMessageSizeMask = (0x1Fu << kAuxCtlMessageSizeShift);
static const uint32_t kAuxCtlFwSyncPulseShift = 5u;
static const uint32_t kAuxCtlFwSyncPulseMask = (0x1Fu << kAuxCtlFwSyncPulseShift);
static const uint32_t kAuxCtlSyncPulseMask = 0x1Fu;

static const uint8_t kDpAuxNativeWrite = 0x8;
static const uint8_t kDpAuxNativeRead = 0x9;
static const uint8_t kDpAuxI2CWrite = 0x0;
static const uint8_t kDpAuxI2CRead = 0x1;
static const uint8_t kDpAuxI2CMot = 0x4;

static const uint8_t kDpAuxReplyAck = 0x0;
static const uint8_t kDpAuxReplyNack = 0x1;
static const uint8_t kDpAuxReplyDefer = 0x2;

static const uint32_t kOpRegionSize = 8u * 1024u;
static const uint32_t kOpRegionVbtOffset = 0x400u;
static const uint32_t kOpRegionAsleOffset = 0x300u;
static const uint32_t kOpRegionAsleExtOffset = 0x1C00u;
static const uint32_t kMboxAsle = (1u << 2);
static const uint32_t kMboxAsleExt = (1u << 4);

static const uint8_t kVbtBlockGeneralDefinitions = 2;
static const uint8_t kVbtBlockEdp = 27;
static const uint8_t kVbtBlockLfpOptions = 40;
static const uint8_t kVbtBlockPowerSeq = 42;

static const uint16_t kDeviceHandleLfp1 = 0x0008;
static const uint16_t kDeviceHandleLfp2 = 0x0080;
static const uint16_t kDeviceTypeHdmi = 0x60D2;
static const uint16_t kDeviceTypeDp = 0x68C6;
static const uint16_t kDeviceTypeEDp = 0x78C6;
static const uint16_t kDeviceTypeIntLfp = 0x1022;
static const uint16_t kDeviceTypeInternalConnector = (1u << 12);
static const uint16_t kDeviceTypeDisplayPortOutput = (1u << 2);
static const uint16_t kDeviceTypeTmdsOutput = (1u << 4);

static const uint8_t kDvoPortHdmia = 0;
static const uint8_t kDvoPortHdmib = 1;
static const uint8_t kDvoPortHdmic = 2;
static const uint8_t kDvoPortHdmid = 3;
static const uint8_t kDvoPortLvds = 4;
static const uint8_t kDvoPortDpb = 7;
static const uint8_t kDvoPortDpc = 8;
static const uint8_t kDvoPortDpd = 9;
static const uint8_t kDvoPortDpa = 10;
static const uint8_t kDvoPortDpe = 11;
static const uint8_t kDvoPortHdmie = 12;
static const uint8_t kDvoPortDpf = 13;
static const uint8_t kDvoPortHdmif = 14;
static const uint8_t kDvoPortDpg = 15;
static const uint8_t kDvoPortHdmig = 16;
static const uint8_t kDvoPortDph = 17;
static const uint8_t kDvoPortHdmih = 18;
static const uint8_t kDvoPortDpi = 19;
static const uint8_t kDvoPortHdmii = 20;

static const uint8_t kDpAuxA = 0x40;
static const uint8_t kDpAuxB = 0x10;
static const uint8_t kDpAuxC = 0x20;
static const uint8_t kDpAuxD = 0x30;
static const uint8_t kDpAuxE = 0x50;
static const uint8_t kDpAuxF = 0x60;
static const uint8_t kDpAuxG = 0x70;
static const uint8_t kDpAuxH = 0x80;
static const uint8_t kDpAuxI = 0x90;

struct __attribute__((packed)) VbtHeader {
    char signature[20];
    uint16_t version;
    uint16_t headerSize;
    uint16_t vbtSize;
    uint8_t checksum;
    uint8_t reserved0;
    uint32_t bdbOffset;
    uint32_t aimOffset[4];
};

struct __attribute__((packed)) OpRegionHeader {
    char signature[16];
    uint32_t sizeKb;
    struct {
        uint8_t revision;
        uint8_t minor;
        uint8_t major;
        uint8_t reserved;
    } over;
    uint8_t biosVer[32];
    uint8_t vbiosVer[16];
    uint8_t driverVer[16];
    uint32_t mboxes;
    uint32_t driverModel;
    uint32_t pcon;
    uint8_t dver[32];
    uint8_t reserved[124];
};

struct __attribute__((packed)) OpRegionAsle {
    uint32_t ardy;
    uint32_t aslc;
    uint32_t tche;
    uint32_t alsi;
    uint32_t bclp;
    uint32_t pfit;
    uint32_t cblv;
    uint16_t bclm[20];
    uint32_t cpfm;
    uint32_t epfm;
    uint8_t plut[74];
    uint32_t pfmb;
    uint32_t cddv;
    uint32_t pcft;
    uint32_t srot;
    uint32_t iuer;
    uint64_t fdss;
    uint32_t fdsp;
    uint32_t stat;
    uint64_t rvda;
    uint32_t rvds;
    uint8_t reserved[58];
};

struct __attribute__((packed)) BdbHeader {
    char signature[16];
    uint16_t version;
    uint16_t headerSize;
    uint16_t bdbSize;
};

struct __attribute__((packed)) BdbLfpOptions {
    uint8_t panelType;
    uint8_t panelType2;
};

struct __attribute__((packed)) EdpPowerSeq {
    uint16_t t1t3;
    uint16_t t8;
    uint16_t t9;
    uint16_t t10;
    uint16_t t11t12;
};

struct __attribute__((packed)) EdpFastLinkParams {
    uint8_t rate : 4;
    uint8_t lanes : 4;
    uint8_t preemphasis : 4;
    uint8_t vswing : 4;
};

struct __attribute__((packed)) BdbEdp {
    EdpPowerSeq powerSeqs[16];
    uint32_t colorDepth;
    EdpFastLinkParams fastLinkParams[16];
};

static uint16_t readLe16(const void* p)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

static uint32_t readLe32(const void* p)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(b[0]) |
           (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

static uint32_t packAuxBytes(const uint8_t* src, uint32_t count)
{
    uint32_t value = 0;
    if (count > 4u) {
        count = 4u;
    }
    for (uint32_t i = 0; i < count; ++i) {
        value |= static_cast<uint32_t>(src[i]) << ((3u - i) * 8u);
    }
    return value;
}

static void unpackAuxBytes(uint32_t value, uint8_t* dst, uint32_t count)
{
    if (count > 4u) {
        count = 4u;
    }
    for (uint32_t i = 0; i < count; ++i) {
        dst[i] = static_cast<uint8_t>(value >> ((3u - i) * 8u));
    }
}

static uint32_t auxMessageSize(uint32_t bytes)
{
    return (bytes << kAuxCtlMessageSizeShift) & kAuxCtlMessageSizeMask;
}

static uint32_t auxSyncPulse(uint32_t count)
{
    if (count == 0u) {
        count = 1u;
    }
    return (count - 1u) & kAuxCtlSyncPulseMask;
}

static uint32_t auxFwSyncPulse(uint32_t count)
{
    if (count == 0u) {
        count = 1u;
    }
    return ((count - 1u) << kAuxCtlFwSyncPulseShift) & kAuxCtlFwSyncPulseMask;
}

static uint8_t childByte(const uint8_t* childBytes, uint8_t childSize, uint8_t offset)
{
    if (!childBytes || offset >= childSize) {
        return 0;
    }
    return childBytes[offset];
}

static uint16_t childLe16(const uint8_t* childBytes, uint8_t childSize, uint8_t offset)
{
    if (!childBytes || offset + 1u >= childSize) {
        return 0;
    }
    return readLe16(childBytes + offset);
}

static const char* connectorTypeName(TGLConnectorType type)
{
    switch (type) {
        case TGLConnectorType::eDP:
            return "eDP";
        case TGLConnectorType::HDMI:
            return "HDMI";
        case TGLConnectorType::DP:
            return "DP";
        case TGLConnectorType::USB4TypeC:
            return "USB4-TypeC";
        default:
            return "Unknown";
    }
}

static const char* ddiName(TGLDDIPort port)
{
    switch (port) {
        case TGLDDIPort::DDI_A:
            return "DDI_A";
        case TGLDDIPort::DDI_B:
            return "DDI_B";
        case TGLDDIPort::DDI_C:
            return "DDI_C";
        case TGLDDIPort::DDI_D:
            return "DDI_D";
        case TGLDDIPort::DDI_TC1:
            return "TC1";
        case TGLDDIPort::DDI_TC2:
            return "TC2";
        case TGLDDIPort::DDI_TC3:
            return "TC3";
        case TGLDDIPort::DDI_TC4:
            return "TC4";
        default:
            return "Unknown";
    }
}

static bool isValidEdidHeader(const uint8_t* edid)
{
    static const uint8_t kHeader[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    return edid && memcmp(edid, kHeader, sizeof(kHeader)) == 0;
}

static bool isValidEdidChecksum(const uint8_t* edid)
{
    if (!edid) {
        return false;
    }

    uint8_t sum = 0;
    for (uint32_t i = 0; i < 128u; ++i) {
        sum = static_cast<uint8_t>(sum + edid[i]);
    }
    return sum == 0;
}

static const char* auxReplyName(uint8_t reply)
{
    switch (reply) {
        case kDpAuxReplyAck:
            return "ACK";
        case kDpAuxReplyNack:
            return "NACK";
        case kDpAuxReplyDefer:
            return "DEFER";
        default:
            return "UNKNOWN";
    }
}

static uint16_t dpcdMaxLinkRateMbps(uint8_t encodedRate)
{
    switch (encodedRate) {
        case 0x06:
            return 1620;
        case 0x08:
            return 2160;
        case 0x09:
            return 2430;
        case 0x0A:
            return 2700;
        case 0x0C:
            return 3240;
        case 0x14:
            return 5400;
        case 0x1E:
            return 8100;
        default:
            return 0;
    }
}

static void logRawBytes(const char* prefix, const uint8_t* bytes, size_t length)
{
    if (!prefix || !bytes || length == 0) {
        return;
    }

    char line[3 * 16 + 1] = { 0 };
    const size_t dumpLength = (length > 64u) ? 64u : length;
    for (size_t offset = 0; offset < dumpLength; offset += 16u) {
        size_t lineIndex = 0;
        const size_t chunk = (dumpLength - offset > 16u) ? 16u : (dumpLength - offset);
        for (size_t i = 0; i < chunk && (lineIndex + 3u) < sizeof(line); ++i) {
            const int written = snprintf(line + lineIndex, sizeof(line) - lineIndex, "%02X%s", bytes[offset + i], (i + 1u < chunk) ? " " : "");
            if (written <= 0) {
                break;
            }
            lineIndex += static_cast<size_t>(written);
        }
        IOLog("[TGL-Connector] %s +0x%02llX: %s\n",
              prefix,
              static_cast<unsigned long long>(offset),
              line);
    }
}

static OSObject* copyPropertyFromServiceChain(IOService* service, const char* key)
{
    if (!service || !key) {
        return nullptr;
    }

    IOService* current = service;
    for (uint32_t depth = 0; current && depth < 6u; ++depth) {
        OSObject* object = current->getProperty(key);
        if (object) {
            object->retain();
            return object;
        }
        current = OSDynamicCast(IOService, current->getProvider());
    }

    return nullptr;
}

static uint32_t findOpRegionHeaderOffset(const uint8_t* bytes, uint32_t length)
{
    if (!bytes || length < 16u) {
        return UINT32_MAX;
    }

    const uint32_t scanLimit = (length < 0x100u) ? length : 0x100u;
    for (uint32_t offset = 0; offset + 16u <= scanLimit; ++offset) {
        if (memcmp(bytes + offset, "IntelGraphicsMem", 16) == 0) {
            return offset;
        }
    }

    return UINT32_MAX;
}

static uint32_t findVbtHeaderOffset(const uint8_t* bytes, uint32_t length)
{
    if (!bytes || length < sizeof(VbtHeader)) {
        return UINT32_MAX;
    }

    const uint32_t scanLimit = (length < 0x400u) ? length : 0x400u;
    for (uint32_t offset = 0; offset + sizeof(VbtHeader) <= scanLimit; ++offset) {
        if (memcmp(bytes + offset, "$VBT", 4) == 0) {
            return offset;
        }
    }

    return UINT32_MAX;
}

} // namespace

FakeIrisXEConnectorManager::FakeIrisXEConnectorManager()
    : m_mmioBase(nullptr),
      m_provider(nullptr),
      m_connectorCount(0),
      m_internalPanel(nullptr),
      m_vbtLoaded(false),
      m_vbtVersion(0),
      m_bdbVersion(0),
      m_opregionMajor(0),
      m_opregionMinor(0),
      m_opregionMboxes(0),
      m_opregionPhys(0),
      m_opregionHeaderOffset(0),
      m_opregionRvda(0),
      m_opregionRvds(0),
      m_opregionSignatureValid(false),
      m_vbtHeaderOffset(0),
      m_vbtPhys(0),
      m_vbtLength(0),
      m_strictVbtMode(false),
      m_panelPowerOnDelay(0),
      m_panelPowerOffDelay(0),
      m_dpcdBacklightCaps(0),
      m_displayTreeReady(false)
{
    bzero(m_connectors, sizeof(m_connectors));
    bzero(m_vbtStorage, sizeof(m_vbtStorage));
    bzero(m_opregionSource, sizeof(m_opregionSource));
}

FakeIrisXEConnectorManager::~FakeIrisXEConnectorManager()
{
}

bool FakeIrisXEConnectorManager::init(volatile uint8_t* mmioBase, IOPCIDevice* provider)
{
    if (!mmioBase) {
        IOLog("[TGL-Connector] ERROR: NULL MMIO base\n");
        return false;
    }

    m_mmioBase = mmioBase;
    m_provider = provider;
    IOLog("[TGL-Connector] Initialized with MMIO base %p provider=%p\n", mmioBase, provider);

    return true;
}

void FakeIrisXEConnectorManager::discoverConnectors()
{
    IOLog("[TGL-Connector] Starting connector discovery...\n");

    bzero(m_connectors, sizeof(m_connectors));
    m_connectorCount = 0;
    m_internalPanel = nullptr;

    logHPDStatus();
    logDDIRegisters();

    bool haveVbtMap = loadVBT() && parseVBTConnectors();
    if (haveVbtMap) {
        m_strictVbtMode = true;
        IOLog("[TGL-Connector] Strict VBT mode enabled - fallback map disabled\n");
    } else {
        IOLog("[TGL-Connector] No valid VBT connector map found, using fallback map\n");
        initDefaultConnectorMap();
    }

    probeConnectors();

    if (m_internalPanel && m_internalPanel->present) {
        checkDpcdBacklightCaps(*m_internalPanel);
    }

    logConnectorInfo();
}

void FakeIrisXEConnectorManager::initDefaultConnectorMap()
{
    IOLog("[TGL-Connector] Initializing default connector map (Tiger Lake fallback)...\n");

    bzero(m_connectors, sizeof(m_connectors));

    m_connectors[0].index = 0;
    m_connectors[0].type = TGLConnectorType::eDP;
    m_connectors[0].ddiPort = TGLDDIPort::DDI_A;
    m_connectors[0].auxChannel = TGLAUXChannel::AUX_A;
    m_connectors[0].hpdPin = TGLHPDPin::HPD_NONE;
    m_connectors[0].maxLanes = 4;
    m_connectors[0].maxBitRate = 10100;
    m_connectors[0].isInternal = true;
    m_connectors[0].supportsAudio = false;
    m_connectors[0].hdpBit = 0;

    m_connectors[1].index = 1;
    m_connectors[1].type = TGLConnectorType::HDMI;
    m_connectors[1].ddiPort = TGLDDIPort::DDI_B;
    m_connectors[1].auxChannel = TGLAUXChannel::AUX_B;
    m_connectors[1].hpdPin = TGLHPDPin::HPD_PIN_1;
    m_connectors[1].maxLanes = 4;
    m_connectors[1].maxBitRate = 5940;
    m_connectors[1].isInternal = false;
    m_connectors[1].supportsAudio = true;
    m_connectors[1].hdpBit = (1u << 1);

    m_connectors[2].index = 2;
    m_connectors[2].type = TGLConnectorType::DP;
    m_connectors[2].ddiPort = TGLDDIPort::DDI_C;
    m_connectors[2].auxChannel = TGLAUXChannel::AUX_C;
    m_connectors[2].hpdPin = TGLHPDPin::HPD_PIN_2;
    m_connectors[2].maxLanes = 4;
    m_connectors[2].maxBitRate = 10100;
    m_connectors[2].isInternal = false;
    m_connectors[2].supportsAudio = true;
    m_connectors[2].hdpBit = (1u << 2);

    m_connectors[3].index = 3;
    m_connectors[3].type = TGLConnectorType::USB4TypeC;
    m_connectors[3].ddiPort = TGLDDIPort::DDI_TC1;
    m_connectors[3].auxChannel = TGLAUXChannel::AUX_TC1;
    m_connectors[3].hpdPin = TGLHPDPin::HPD_PIN_3;
    m_connectors[3].maxLanes = 4;
    m_connectors[3].maxBitRate = 10100;
    m_connectors[3].isInternal = false;
    m_connectors[3].supportsAudio = true;
    m_connectors[3].hdpBit = (1u << 3);

    m_connectorCount = 4;
    m_internalPanel = &m_connectors[0];

    IOLog("[TGL-Connector] Default map: con0=eDP(DDI_A), con1=HDMI(DDI_B), con2=DP(DDI_C), con3=TypeC(TC1)\n");
}

void FakeIrisXEConnectorManager::probeConnectors()
{
    IOLog("[TGL-Connector] Probing connectors for presence and EDID/DPCD...\n");

    for (uint8_t i = 0; i < m_connectorCount; ++i) {
        TGLConnectorDesc& conn = m_connectors[i];
        conn.present = false;
        conn.hasDpcd = false;
        conn.hasEdid = false;
        conn.edidLength = 0;
        bzero(conn.dpcd, sizeof(conn.dpcd));
        bzero(conn.edid, sizeof(conn.edid));

    bool hpdPresent = false;
    for (uint32_t hpdPoll = 0; hpdPoll < 3; ++hpdPoll) {
        hpdPresent = conn.isInternal ? isEDPPanelPowered() : checkHPD(conn);
        if (hpdPresent) {
            break;
        }
        IOSleep(50);
    }
        bool auxCandidate = conn.isInternal || hpdPresent;

        if (conn.type == TGLConnectorType::HDMI) {
            const bool ddiActive = isDDIEnabled(conn.ddiPort);
            conn.present = hpdPresent && ddiActive;
            IOLog("[TGL-Connector] Connector %u HDMI hpd=%u ddiActive=%u present=%u (conservative HPD policy)\n",
                  i,
                  hpdPresent ? 1u : 0u,
                  ddiActive ? 1u : 0u,
                  conn.present ? 1u : 0u);
            continue;
        }

        if (auxCandidate && readDPCD(conn, 0x00000u, conn.dpcd, sizeof(conn.dpcd))) {
            conn.hasDpcd = true;
            conn.present = true;
            const uint8_t dpcdRev = conn.dpcd[0];
            const uint16_t maxLinkRate = dpcdMaxLinkRateMbps(conn.dpcd[1]);
            const uint8_t maxLanes = conn.dpcd[2] & 0x1Fu;
            if (maxLinkRate) {
                conn.maxBitRate = maxLinkRate;
            }
            if (maxLanes >= 1u && maxLanes <= 4u) {
                conn.maxLanes = maxLanes;
            }
            IOLog("[TGL-Connector] Connector %u %s DPCD rev=%u.%u maxLink=0x%02X lanes=0x%02X\n",
                  i,
                  connectorTypeName(conn.type),
                  dpcdRev >> 4,
                  dpcdRev & 0x0Fu,
                  conn.dpcd[1],
                  conn.dpcd[2]);
            uint8_t mstCaps = 0;
            if (auxNativeRead(conn.auxChannel, 0x021u, &mstCaps, 1u)) {
                IOLog("[TGL-Connector] Connector %u %s MST caps=0x%02X\n",
                      i,
                      connectorTypeName(conn.type),
                      mstCaps);
            }
        } else if (conn.isInternal && isEDPPanelPowered()) {
            conn.present = true;
        }

        if (conn.present && readEDID(conn)) {
            conn.hasEdid = true;
            IOLog("[TGL-Connector] Connector %u %s EDID acquired over AUX (%u bytes)\n",
                  i,
                  connectorTypeName(conn.type),
                  conn.edidLength);
        } else if (conn.present) {
            IOLog("[TGL-Connector] Connector %u %s present but AUX EDID read failed\n",
                  i,
                  connectorTypeName(conn.type));
        } else {
            IOLog("[TGL-Connector] Connector %u %s not present\n",
                  i,
                  connectorTypeName(conn.type));
        }
    }

    for (uint8_t i = 0; i < m_connectorCount; ++i) {
        TGLConnectorDesc& conn = m_connectors[i];
        if (conn.hasDpcd && conn.discoveredFromVbt) {
            bool vbtMatchesDpcd = true;
            if (conn.maxLanes > 0 && conn.maxLanes != (conn.dpcd[2] & 0x1Fu)) {
                vbtMatchesDpcd = false;
            }
            IOLog("[TGL-Connector] VBT vs DPCD cross-check con%u: lanes_match=%u dpcd_lanes=%u vbt_lanes=%u\n",
                  i,
                  vbtMatchesDpcd ? 1u : 0u,
                  conn.dpcd[2] & 0x1Fu,
                  conn.maxLanes);
        }
    }
}

void FakeIrisXEConnectorManager::checkDpcdBacklightCaps(TGLConnectorDesc& conn)
{
    if (!conn.isInternal || !conn.present) {
        m_dpcdBacklightCaps = 0;
        return;
    }

    uint8_t caps = 0;
    if (auxNativeRead(conn.auxChannel, 0x700u, &caps, 1)) {
        m_dpcdBacklightCaps = caps;
        IOLog("[TGL-Connector] DPCD backlight caps=0x%02X (AUX backlight %ssupported)\n",
              caps,
              (caps & 0x1u) ? "" : "not ");
    } else {
        m_dpcdBacklightCaps = 0;
        IOLog("[TGL-Connector] DPCD backlight read failed, assuming PWM only\n");
    }
}

bool FakeIrisXEConnectorManager::checkHPD(TGLConnectorDesc& conn)
{
    uint32_t hpdSts = readReg(PCH_PORT_HPDSTS);
    if (conn.hdpBit != 0u) {
        return (hpdSts & conn.hdpBit) != 0u;
    }
    return false;
}

TGLConnectorDesc* FakeIrisXEConnectorManager::getConnector(uint8_t index)
{
    if (index >= m_connectorCount) {
        return nullptr;
    }
    return &m_connectors[index];
}

TGLConnectorDesc* FakeIrisXEConnectorManager::getInternalPanel()
{
    return m_internalPanel;
}

const uint8_t* FakeIrisXEConnectorManager::getConnectorEDID(uint8_t index, uint16_t* outLength) const
{
    if (outLength) {
        *outLength = 0;
    }
    if (index >= m_connectorCount || !m_connectors[index].hasEdid) {
        return nullptr;
    }
    if (outLength) {
        *outLength = m_connectors[index].edidLength;
    }
    return m_connectors[index].edid;
}

const uint8_t* FakeIrisXEConnectorManager::getPrimaryDisplayEDID(uint16_t* outLength, TGLConnectorDesc** outConnector) const
{
    if (outLength) {
        *outLength = 0;
    }
    if (outConnector) {
        *outConnector = nullptr;
    }

    for (uint8_t i = 0; i < m_connectorCount; ++i) {
        const TGLConnectorDesc& conn = m_connectors[i];
        if (!conn.hasEdid || !conn.isInternal) {
            continue;
        }

        if (outLength) {
            *outLength = conn.edidLength;
        }
        if (outConnector) {
            *outConnector = const_cast<TGLConnectorDesc*>(&conn);
        }
        return conn.edid;
    }

    for (uint8_t i = 0; i < m_connectorCount; ++i) {
        const TGLConnectorDesc& conn = m_connectors[i];
        if (!conn.hasEdid) {
            continue;
        }

        if (outLength) {
            *outLength = conn.edidLength;
        }
        if (outConnector) {
            *outConnector = const_cast<TGLConnectorDesc*>(&conn);
        }
        return conn.edid;
    }

    return nullptr;
}

void FakeIrisXEConnectorManager::logConnectorInfo()
{
    IOLog("========== TGL Connector Info ==========\n");
    IOLog("  Source: %s (VBT version=%u BDB version=%u)\n",
          m_vbtLoaded ? "real VBT" : "fallback map",
          m_vbtVersion,
          m_bdbVersion);
    for (uint8_t i = 0; i < m_connectorCount; ++i) {
        TGLConnectorDesc& conn = m_connectors[i];
        IOLog("  Connector %u: type=%s port=%s internal=%s present=%s edid=%s dpcd=%s source=%s\n",
              conn.index,
              connectorTypeName(conn.type),
              ddiName(conn.ddiPort),
              conn.isInternal ? "yes" : "no",
              conn.present ? "yes" : "no",
              conn.hasEdid ? "yes" : "no",
              conn.hasDpcd ? "yes" : "no",
              conn.discoveredFromVbt ? "VBT" : "fallback");
    }
    IOLog("========================================\n");
}

void FakeIrisXEConnectorManager::logDDIRegisters()
{
    IOLog("[TGL-Connector] DDI Buffer Control Registers:\n");
    IOLog("  DDI_BUF_CTL_A = 0x%08X\n", readReg(DDI_BUF_CTL_A));
    IOLog("  DDI_BUF_CTL_B = 0x%08X\n", readReg(DDI_BUF_CTL_B));
    IOLog("  DDI_BUF_CTL_C = 0x%08X\n", readReg(DDI_BUF_CTL_C));
    IOLog("  DDI_BUF_CTL_D = 0x%08X\n", readReg(DDI_BUF_CTL_D));
}

void FakeIrisXEConnectorManager::logHPDStatus()
{
    IOLog("[TGL-Connector] HPD Status Registers:\n");
    IOLog("  PCH_PORT_HPDSTS = 0x%08X\n", readReg(PCH_PORT_HPDSTS));
    IOLog("  PCH_PORT_HPDCNTF = 0x%08X\n", readReg(PCH_PORT_HPDCNTF));
    IOLog("  PCH_PORT_HPDEN = 0x%08X\n", readReg(PCH_PORT_HPDEN));
}

uint32_t FakeIrisXEConnectorManager::getDDIBufferControl(TGLDDIPort port)
{
    switch (port) {
        case TGLDDIPort::DDI_A:
            return DDI_BUF_CTL_A;
        case TGLDDIPort::DDI_B:
            return DDI_BUF_CTL_B;
        case TGLDDIPort::DDI_C:
            return DDI_BUF_CTL_C;
        case TGLDDIPort::DDI_D:
            return DDI_BUF_CTL_D;
        case TGLDDIPort::DDI_TC1:
            return DDI_BUF_CTL_TC1;
        case TGLDDIPort::DDI_TC2:
            return DDI_BUF_CTL_TC2;
        case TGLDDIPort::DDI_TC3:
            return DDI_BUF_CTL_TC3;
        case TGLDDIPort::DDI_TC4:
            return DDI_BUF_CTL_TC4;
        default:
            return 0;
    }
}

uint32_t FakeIrisXEConnectorManager::getTranscoderFunctionControl(TGLDDIPort port)
{
    switch (port) {
        case TGLDDIPort::DDI_A:
            return TRANS_DDI_FUNC_CTL_A;
        case TGLDDIPort::DDI_B:
            return TRANS_DDI_FUNC_CTL_B;
        case TGLDDIPort::DDI_C:
            return TRANS_DDI_FUNC_CTL_C;
        case TGLDDIPort::DDI_D:
            return TRANS_DDI_FUNC_CTL_D;
        default:
            return 0;
    }
}

uint32_t FakeIrisXEConnectorManager::getAUXControl(TGLAUXChannel aux)
{
    switch (aux) {
        case TGLAUXChannel::AUX_A:
            return AUX_CTL_A;
        case TGLAUXChannel::AUX_B:
            return AUX_CTL_B;
        case TGLAUXChannel::AUX_C:
            return AUX_CTL_C;
        case TGLAUXChannel::AUX_D:
            return AUX_CTL_D;
        case TGLAUXChannel::AUX_TC1:
            return AUX_CTL_TC1;
        case TGLAUXChannel::AUX_TC2:
            return AUX_CTL_TC2;
        case TGLAUXChannel::AUX_TC3:
            return AUX_CTL_TC3;
        case TGLAUXChannel::AUX_TC4:
            return AUX_CTL_TC4;
        default:
            return 0;
    }
}

uint32_t FakeIrisXEConnectorManager::getAUXData(TGLAUXChannel aux, uint32_t index)
{
    uint32_t ctl = getAUXControl(aux);
    if (!ctl || index >= 5u) {
        return 0;
    }
    return ctl + 4u + (index * 4u);
}

uint32_t FakeIrisXEConnectorManager::getPipeConfig(uint8_t pipe)
{
    switch (pipe) {
        case 0:
            return PIPECONF_A;
        case 1:
            return PIPECONF_B;
        case 2:
            return PIPECONF_C;
        default:
            return 0;
    }
}

uint32_t FakeIrisXEConnectorManager::getTranscoderConfig(uint8_t transcoder)
{
    switch (transcoder) {
        case 0:
            return TRANS_CONF_A;
        case 1:
            return TRANS_CONF_B;
        case 2:
            return TRANS_CONF_C;
        default:
            return 0;
    }
}

bool FakeIrisXEConnectorManager::isDDIEnabled(TGLDDIPort port)
{
    uint32_t reg = getDDIBufferControl(port);
    return reg ? ((readReg(reg) & (1u << 31)) != 0u) : false;
}

bool FakeIrisXEConnectorManager::isPipeEnabled(uint8_t pipe)
{
    uint32_t reg = getPipeConfig(pipe);
    return reg ? ((readReg(reg) & (1u << 31)) != 0u) : false;
}

bool FakeIrisXEConnectorManager::isTranscoderEnabled(uint8_t transcoder)
{
    uint32_t reg = getTranscoderConfig(transcoder);
    return reg ? ((readReg(reg) & (1u << 31)) != 0u) : false;
}

bool FakeIrisXEConnectorManager::powerUpEDPPanel()
{
    const uint32_t onDelayUs = m_panelPowerOnDelay ? m_panelPowerOnDelay : 100000u;
    const uint32_t offDelayUs = m_panelPowerOffDelay ? m_panelPowerOffDelay : 100000u;
    IOLog("[TGL-Connector] Powering up eDP panel (t_on=%uus t_off=%uus)...\n", onDelayUs, offDelayUs);

    uint32_t ppCtrl = readReg(PCH_PP_CONTROL);
    uint32_t ppStatus = readReg(PCH_PP_STATUS);
    if ((ppStatus & (1u << 30)) == 0u) {
        writeReg(PCH_PP_CONTROL, ppCtrl & ~(1u << 0));
        IOSleep((offDelayUs + 999u) / 1000u);
    }

    writeReg(PCH_PP_CONTROL, ppCtrl | (1u << 0));
    const uint32_t pollDelayUs = onDelayUs ? onDelayUs / 10u : 1000u;
    for (uint32_t timeout = 0; timeout < 100u; ++timeout) {
        ppStatus = readReg(PCH_PP_STATUS);
        if ((ppStatus & (1u << 31)) == 0u) {
            IOLog("[TGL-Connector] eDP panel power sequence complete PP_CONTROL=0x%08X PP_STATUS=0x%08X\n",
                  readReg(PCH_PP_CONTROL),
                  ppStatus);
            return true;
        }
        IODelay(pollDelayUs ? pollDelayUs : 1000u);
    }
    IOLog("[TGL-Connector] eDP power up timed out PP_CONTROL=0x%08X PP_STATUS=0x%08X\n",
          readReg(PCH_PP_CONTROL),
          readReg(PCH_PP_STATUS));
    return false;
}

bool FakeIrisXEConnectorManager::powerDownEDPPanel()
{
    writeReg(PCH_PP_CONTROL, readReg(PCH_PP_CONTROL) & ~(1u << 0));
    return true;
}

bool FakeIrisXEConnectorManager::isEDPPanelPowered()
{
    return (readReg(PCH_PP_STATUS) & (1u << 30)) != 0u;
}

bool FakeIrisXEConnectorManager::enableDDI(TGLDDIPort port)
{
    uint32_t ddiReg = getDDIBufferControl(port);
    if (!ddiReg) {
        IOLog("[TGL-Connector] ERROR: Invalid DDI port %d\n", static_cast<int>(port));
        return false;
    }

    uint32_t ddiVal = readReg(ddiReg);
    ddiVal |= (1u << 31);
    writeReg(ddiReg, ddiVal);
    IOLog("[TGL-Connector] Enabled %s -> 0x%08X\n", ddiName(port), readReg(ddiReg));
    return true;
}

bool FakeIrisXEConnectorManager::initEDPConnector(TGLConnectorDesc& conn)
{
    IOLog("[TGL-Connector] Initializing eDP connector on %s\n", ddiName(conn.ddiPort));
    powerUpEDPPanel();
    return enableDDI(conn.ddiPort);
}

bool FakeIrisXEConnectorManager::initHDMIConnector(TGLConnectorDesc& conn)
{
    IOLog("[TGL-Connector] Initializing HDMI connector on %s\n", ddiName(conn.ddiPort));
    if (!enableDDI(conn.ddiPort)) {
        return false;
    }

    uint32_t transFunc = getTranscoderFunctionControl(conn.ddiPort);
    if (transFunc) {
        uint32_t val = readReg(transFunc);
        val &= ~0xFu;
        val |= static_cast<uint32_t>(conn.ddiPort) & 0xFu;
        val &= ~(1u << 4);
        writeReg(transFunc, val);
    }

    return true;
}

bool FakeIrisXEConnectorManager::initDPConnector(TGLConnectorDesc& conn)
{
    IOLog("[TGL-Connector] Initializing DP connector on %s\n", ddiName(conn.ddiPort));
    if (!enableDDI(conn.ddiPort)) {
        return false;
    }

    uint32_t transFunc = getTranscoderFunctionControl(conn.ddiPort);
    if (transFunc) {
        uint32_t val = readReg(transFunc);
        val &= ~0xFu;
        val |= static_cast<uint32_t>(conn.ddiPort) & 0xFu;
        val |= (1u << 4);
        writeReg(transFunc, val);
    }

    return true;
}

bool FakeIrisXEConnectorManager::initTypeCConnector(TGLConnectorDesc& conn)
{
    IOLog("[TGL-Connector] Initializing Type-C connector on %s\n", ddiName(conn.ddiPort));
    return enableDDI(conn.ddiPort);
}

bool FakeIrisXEConnectorManager::enablePipeAndTranscoder(uint8_t pipe, uint8_t transcoder, TGLDDIPort ddi)
{
    uint32_t pipeConf = getPipeConfig(pipe);
    if (pipeConf) {
        writeReg(pipeConf, readReg(pipeConf) | (1u << 31));
    }

    uint32_t transConf = getTranscoderConfig(transcoder);
    if (transConf) {
        writeReg(transConf, readReg(transConf) | (1u << 31));
    }

    uint32_t transFunc = getTranscoderFunctionControl(ddi);
    if (transFunc) {
        uint32_t val = readReg(transFunc);
        val |= (1u << 31);
        val &= ~0xFu;
        val |= static_cast<uint32_t>(ddi) & 0xFu;
        writeReg(transFunc, val);
    }

    return true;
}

void FakeIrisXEConnectorManager::publishConnectorProperties()
{
    IOLog("[TGL-Connector] Connector properties are published by the framebuffer\n");
}

bool FakeIrisXEConnectorManager::auxTransfer(TGLAUXChannel aux,
                                            const uint8_t* send,
                                            uint32_t sendBytes,
                                            uint8_t* recv,
                                            uint32_t& recvBytes)
{
    uint32_t ctlReg = getAUXControl(aux);
    if (!ctlReg || !send || sendBytes == 0u || sendBytes > 20u || recvBytes > 20u) {
        return false;
    }

    const uint32_t recvCapacity = recvBytes;
    recvBytes = 0;

    for (uint32_t wait = 0; wait < 3u; ++wait) {
        uint32_t status = readReg(ctlReg);
        if ((status & kAuxCtlSendBusy) == 0u) {
            break;
        }
        IOSleep(1);
        if (wait == 2u) {
            IOLog("[TGL-Connector] AUX %u busy, status=0x%08X\n", static_cast<uint32_t>(aux), status);
            return false;
        }
    }

    for (uint32_t i = 0; i < 5u; ++i) {
        uint8_t packed[4] = { 0, 0, 0, 0 };
        uint32_t remaining = (sendBytes > i * 4u) ? (sendBytes - i * 4u) : 0u;
        if (remaining) {
            uint32_t copy = remaining > 4u ? 4u : remaining;
            memcpy(packed, send + (i * 4u), copy);
        }
        writeReg(getAUXData(aux, i), packAuxBytes(packed, 4u));
    }

    const uint32_t sendCtl = kAuxCtlSendBusy |
                             kAuxCtlDone |
                             kAuxCtlInterrupt |
                             kAuxCtlTimeoutError |
                             kAuxCtlTimeoutMax |
                             kAuxCtlReceiveError |
                             auxMessageSize(sendBytes) |
                             auxFwSyncPulse(20u) |
                             auxSyncPulse(32u);
    writeReg(ctlReg, sendCtl);

    uint32_t status = 0;
    bool complete = false;
    for (uint32_t poll = 0; poll < 1000u; ++poll) {
        status = readReg(ctlReg);
        if ((status & kAuxCtlSendBusy) == 0u) {
            complete = true;
            break;
        }
        IODelay(10);
    }

    if (!complete) {
        IOLog("[TGL-Connector] AUX %u timed out waiting for SEND_BUSY clear\n", static_cast<uint32_t>(aux));
        return false;
    }

    writeReg(ctlReg, status | kAuxCtlDone | kAuxCtlTimeoutError | kAuxCtlReceiveError);

    if (status & kAuxCtlTimeoutError) {
        return false;
    }
    if ((status & kAuxCtlDone) == 0u || (status & kAuxCtlReceiveError) != 0u) {
        IOLog("[TGL-Connector] AUX %u failed, status=0x%08X\n", static_cast<uint32_t>(aux), status);
        return false;
    }

    uint32_t reportedBytes = (status & kAuxCtlMessageSizeMask) >> kAuxCtlMessageSizeShift;
    if (reportedBytes == 0u || reportedBytes > 20u) {
        IOLog("[TGL-Connector] AUX %u returned invalid size %u status=0x%08X\n",
              static_cast<uint32_t>(aux),
              reportedBytes,
              status);
        return false;
    }

    recvBytes = (reportedBytes > recvCapacity) ? recvCapacity : reportedBytes;
    if (!recv || recvBytes == 0u) {
        return true;
    }

    for (uint32_t i = 0; i < 5u && (i * 4u) < recvBytes; ++i) {
        uint8_t unpacked[4] = { 0, 0, 0, 0 };
        unpackAuxBytes(readReg(getAUXData(aux, i)), unpacked, 4u);
        uint32_t copy = recvBytes - (i * 4u);
        if (copy > 4u) {
            copy = 4u;
        }
        memcpy(recv + (i * 4u), unpacked, copy);
    }

    return true;
}

bool FakeIrisXEConnectorManager::auxNativeRead(TGLAUXChannel aux, uint32_t address, uint8_t* data, uint32_t size)
{
    if (!data || size == 0u) {
        return false;
    }

    uint32_t offset = 0;
    while (offset < size) {
        const uint32_t chunk = (size - offset > 16u) ? 16u : (size - offset);
        uint8_t send[4] = {
            static_cast<uint8_t>((kDpAuxNativeRead << 4) | ((address >> 16) & 0x0Fu)),
            static_cast<uint8_t>((address >> 8) & 0xFFu),
            static_cast<uint8_t>(address & 0xFFu),
            static_cast<uint8_t>(chunk - 1u)
        };
        uint8_t recv[20] = { 0 };
        uint32_t recvBytes = sizeof(recv);
        bool ok = false;

        for (uint32_t attempt = 0; attempt < 5u; ++attempt) {
            if (!auxTransfer(aux, send, sizeof(send), recv, recvBytes) || recvBytes < 1u) {
                continue;
            }

            const uint8_t reply = recv[0] >> 4;
            if (reply == kDpAuxReplyDefer) {
                IOSleep(1);
                recvBytes = sizeof(recv);
                continue;
            }
            if (reply != kDpAuxReplyAck || recvBytes < chunk + 1u) {
                return false;
            }

            memcpy(data + offset, recv + 1, chunk);
            ok = true;
            break;
        }

        if (!ok) {
            return false;
        }

        address += chunk;
        offset += chunk;
    }

    return true;
}

bool FakeIrisXEConnectorManager::auxI2CWrite(TGLAUXChannel aux, uint8_t address, const uint8_t* data, uint32_t size, bool mot)
{
    if (size > 16u) {
        return false;
    }

    uint8_t send[20] = { 0 };
    uint32_t sendBytes = (size > 0u) ? (4u + size) : 3u;
    uint8_t request = static_cast<uint8_t>(kDpAuxI2CWrite | (mot ? kDpAuxI2CMot : 0u));
    send[0] = static_cast<uint8_t>((request << 4) | ((address >> 16) & 0x0Fu));
    send[1] = 0;
    send[2] = address;
    if (size > 0u) {
        send[3] = static_cast<uint8_t>(size - 1u);
        memcpy(send + 4, data, size);
    }

    uint8_t recv[8] = { 0 };
    uint32_t recvBytes = sizeof(recv);
    for (uint32_t attempt = 0; attempt < 5u; ++attempt) {
        if (!auxTransfer(aux, send, sendBytes, recv, recvBytes) || recvBytes < 1u) {
            IOLog("[TGL-Connector] AUX I2C write req=0x%X addr=0x%02X bytes=%u mot=%u attempt=%u transfer failed\n",
                  request,
                  address,
                  size,
                  mot ? 1u : 0u,
                  attempt + 1u);
            recvBytes = sizeof(recv);
            continue;
        }

        const uint8_t reply = recv[0] >> 4;
        IOLog("[TGL-Connector] AUX I2C write req=0x%X addr=0x%02X bytes=%u mot=%u attempt=%u reply=%s(0x%X) payload=%u\n",
              request,
              address,
              size,
              mot ? 1u : 0u,
              attempt + 1u,
              auxReplyName(reply),
              reply,
              recvBytes);
        if (reply == kDpAuxReplyDefer) {
            IOSleep(1);
            recvBytes = sizeof(recv);
            continue;
        }
        if (reply == kDpAuxReplyAck) {
            return true;
        }
        if (reply == kDpAuxReplyNack) {
            return false;
        }
    }

    return false;
}

bool FakeIrisXEConnectorManager::auxI2CRead(TGLAUXChannel aux, uint8_t address, uint8_t* data, uint32_t size, bool mot)
{
    if (!data || size == 0u) {
        return false;
    }

    uint32_t offset = 0;
    while (offset < size) {
        const uint32_t chunk = (size - offset > 16u) ? 16u : (size - offset);
        uint8_t send[4] = {
            static_cast<uint8_t>(((kDpAuxI2CRead | (mot ? kDpAuxI2CMot : 0u)) << 4) | ((address >> 16) & 0x0Fu)),
            0,
            address,
            static_cast<uint8_t>(chunk - 1u)
        };
        uint8_t recv[20] = { 0 };
        uint32_t recvBytes = sizeof(recv);
        bool ok = false;

        for (uint32_t attempt = 0; attempt < 5u; ++attempt) {
            if (!auxTransfer(aux, send, sizeof(send), recv, recvBytes) || recvBytes < 1u) {
                IOLog("[TGL-Connector] AUX I2C read addr=0x%02X bytes=%u mot=%u attempt=%u transfer failed\n",
                      address,
                      chunk,
                      mot ? 1u : 0u,
                      attempt + 1u);
                recvBytes = sizeof(recv);
                continue;
            }

            const uint8_t reply = recv[0] >> 4;
            IOLog("[TGL-Connector] AUX I2C read addr=0x%02X bytes=%u mot=%u attempt=%u reply=%s(0x%X) payload=%u\n",
                  address,
                  chunk,
                  mot ? 1u : 0u,
                  attempt + 1u,
                  auxReplyName(reply),
                  reply,
                  recvBytes);
            if (reply == kDpAuxReplyDefer) {
                IOSleep(1);
                recvBytes = sizeof(recv);
                continue;
            }
            if (reply == kDpAuxReplyNack) {
                return false;
            }
            if (reply != kDpAuxReplyAck || recvBytes < chunk + 1u) {
                return false;
            }

            memcpy(data + offset, recv + 1, chunk);
            ok = true;
            break;
        }

        if (!ok) {
            return false;
        }

        offset += chunk;
    }

    return true;
}

bool FakeIrisXEConnectorManager::readDPCD(TGLConnectorDesc& conn, uint32_t address, uint8_t* data, uint32_t size)
{
    if (!data || size == 0u) {
        return false;
    }

    return auxNativeRead(conn.auxChannel, address, data, size);
}

bool FakeIrisXEConnectorManager::readEDID(TGLConnectorDesc& conn)
{
    if (conn.type == TGLConnectorType::HDMI) {
        return false;
    }

    uint8_t block[128] = { 0 };
    auto readEdidBlock = [&](uint8_t blockIndex, uint8_t* out) -> bool {
        const uint8_t segment = static_cast<uint8_t>(blockIndex / 2u);
        uint8_t offset = static_cast<uint8_t>((blockIndex % 2u) ? 0x80u : 0x00u);

        if (blockIndex >= 2u) {
            if (!auxI2CWrite(conn.auxChannel, 0x30u, &segment, 1u, false)) {
                IOLog("[TGL-Connector] EDID segment write failed for connector %u block %u segment=%u\n",
                      conn.index,
                      blockIndex,
                      segment);
                return false;
            }
        }
        if (!auxI2CWrite(conn.auxChannel, 0x50u, &offset, 1u, true)) {
            IOLog("[TGL-Connector] EDID offset write failed for connector %u block %u offset=0x%02X\n",
                  conn.index,
                  blockIndex,
                  offset);
            return false;
        }
        if (!auxI2CRead(conn.auxChannel, 0x50u, out, 128u, false)) {
            IOLog("[TGL-Connector] EDID read failed for connector %u block %u\n", conn.index, blockIndex);
            return false;
        }
        return true;
    };

    if (!readEdidBlock(0u, block) || !isValidEdidHeader(block) || !isValidEdidChecksum(block)) {
        return false;
    }

    memcpy(conn.edid, block, sizeof(block));
    uint8_t extensionCount = block[126];
    if (extensionCount > 3u) {
        extensionCount = 3u;
    }

    conn.edidLength = 128u;
    for (uint8_t i = 0; i < extensionCount; ++i) {
        uint8_t* dst = conn.edid + conn.edidLength;
        if (!readEdidBlock(static_cast<uint8_t>(i + 1u), dst) || !isValidEdidChecksum(dst)) {
            IOLog("[TGL-Connector] Ignoring invalid EDID extension block %u on connector %u\n", i + 1u, conn.index);
            break;
        }
        conn.edidLength = static_cast<uint16_t>(conn.edidLength + 128u);
    }

    return true;
}

bool FakeIrisXEConnectorManager::loadVBT()
{
    if (m_vbtLoaded) {
        return true;
    }

    if (loadVBTFromOpRegion()) {
        return true;
    }

    return loadVBTFromRegistry();
}

bool FakeIrisXEConnectorManager::adoptVBTWindow(const uint8_t* bytes,
                                                uint32_t length,
                                                const char* source,
                                                uint64_t physBase,
                                                bool updateOpRegionSource)
{
    if (!bytes || length < sizeof(VbtHeader)) {
        return false;
    }

    const uint32_t headerOffset = findVbtHeaderOffset(bytes, length);
    if (headerOffset == UINT32_MAX) {
        IOLog("[TGL-Connector] %s VBT rejected: missing $VBT signature\n", source ? source : "unknown");
        return false;
    }

    if (!validateVBTBlob(bytes + headerOffset, length - headerOffset, source)) {
        return false;
    }

    const VbtHeader* header = reinterpret_cast<const VbtHeader*>(bytes + headerOffset);
    const uint16_t vbtSize = readLe16(&header->vbtSize);
    m_vbtLength = (vbtSize > kFakeIrisXEMaxVbtBytes) ? kFakeIrisXEMaxVbtBytes : vbtSize;
    memcpy(m_vbtStorage, bytes + headerOffset, m_vbtLength);
    m_vbtVersion = readLe16(&header->version);
    m_vbtLoaded = true;
    m_vbtHeaderOffset = headerOffset;
    m_vbtPhys = physBase ? (physBase + headerOffset) : 0;
    if (updateOpRegionSource && source) {
        strlcpy(m_opregionSource, source, sizeof(m_opregionSource));
    }
    IOLog("[TGL-Connector] %s VBT accepted: header_offset=0x%X phys=0x%llX size=0x%X version=%u\n",
          source ? source : "unknown",
          headerOffset,
          static_cast<unsigned long long>(m_vbtPhys),
          m_vbtLength,
          m_vbtVersion);
    return true;
}

bool FakeIrisXEConnectorManager::validateVBTBlob(const uint8_t* bytes, size_t length, const char* source) const
{
    if (!bytes || length < sizeof(VbtHeader)) {
        IOLog("[TGL-Connector] %s VBT rejected: blob too small (%llu bytes)\n",
              source ? source : "unknown",
              static_cast<unsigned long long>(length));
        return false;
    }

    if (memcmp(bytes, "$VBT", 4) != 0) {
        IOLog("[TGL-Connector] %s VBT rejected: missing $VBT signature\n", source ? source : "unknown");
        return false;
    }

    const VbtHeader* header = reinterpret_cast<const VbtHeader*>(bytes);
    const uint16_t headerSize = readLe16(&header->headerSize);
    const uint16_t vbtSize = readLe16(&header->vbtSize);
    const uint32_t bdbOffset = readLe32(&header->bdbOffset);
    if (headerSize < sizeof(VbtHeader) || vbtSize < headerSize || vbtSize > length) {
        IOLog("[TGL-Connector] %s VBT rejected: headerSize=%u vbtSize=%u blobLen=%llu\n",
              source ? source : "unknown",
              headerSize,
              vbtSize,
              static_cast<unsigned long long>(length));
        return false;
    }

    if (bdbOffset < headerSize || bdbOffset + sizeof(BdbHeader) > vbtSize) {
        IOLog("[TGL-Connector] %s VBT rejected: invalid BDB offset 0x%X (header=%u size=%u)\n",
              source ? source : "unknown",
              bdbOffset,
              headerSize,
              vbtSize);
        return false;
    }

    const BdbHeader* bdbHeader = reinterpret_cast<const BdbHeader*>(bytes + bdbOffset);
    const uint16_t bdbHeaderSize = readLe16(&bdbHeader->headerSize);
    const uint16_t bdbSize = readLe16(&bdbHeader->bdbSize);
    if (bdbHeaderSize < sizeof(BdbHeader) || bdbSize < bdbHeaderSize || bdbOffset + bdbSize > vbtSize) {
        IOLog("[TGL-Connector] %s VBT rejected: invalid BDB header size=%u total=%u vbtSize=%u\n",
              source ? source : "unknown",
              bdbHeaderSize,
              bdbSize,
              vbtSize);
        return false;
    }

    return true;
}

bool FakeIrisXEConnectorManager::loadVBTFromOpRegion()
{
    if (!m_provider) {
        return false;
    }

    m_opregionMajor = 0;
    m_opregionMinor = 0;
    m_opregionMboxes = 0;
    m_opregionPhys = 0;
    m_opregionHeaderOffset = 0;
    m_opregionRvda = 0;
    m_opregionRvds = 0;
    m_opregionSignatureValid = false;
    m_vbtHeaderOffset = 0;
    m_vbtPhys = 0;
    bzero(m_opregionSource, sizeof(m_opregionSource));

    auto loadFromMappedOpRegion = [&](const uint8_t* bytes, uint32_t length, const char* source, uint64_t physBase) -> bool {
        if (!bytes || length < sizeof(OpRegionHeader)) {
            return false;
        }

        logRawBytes(source ? source : "opregion", bytes, length);
        const uint32_t headerOffset = findOpRegionHeaderOffset(bytes, length);
        if (headerOffset == UINT32_MAX || headerOffset + sizeof(OpRegionHeader) > length) {
            IOLog("[TGL-Connector] %s rejected: OpRegion signature mismatch\n", source ? source : "opregion");
            return false;
        }

        const OpRegionHeader* opregion = reinterpret_cast<const OpRegionHeader*>(bytes + headerOffset);
        const uint64_t adjustedPhysBase = physBase ? (physBase + headerOffset) : 0;

        m_opregionSignatureValid = true;
        m_opregionPhys = adjustedPhysBase;
        m_opregionHeaderOffset = headerOffset;
        m_opregionMajor = opregion->over.major;
        m_opregionMinor = opregion->over.minor;
        m_opregionMboxes = opregion->mboxes;
        if (source) {
            strlcpy(m_opregionSource, source, sizeof(m_opregionSource));
        }

        IOLog("[TGL-Connector] %s accepted: header_offset=0x%X adjusted_phys=0x%llX v%u.%u size=%uKB mboxes=0x%08X\n",
              source ? source : "opregion",
              headerOffset,
              static_cast<unsigned long long>(adjustedPhysBase),
              m_opregionMajor,
              m_opregionMinor,
              opregion->sizeKb,
              m_opregionMboxes);

        if ((m_opregionMboxes & kMboxAsle) != 0u && headerOffset + kOpRegionAsleOffset + sizeof(OpRegionAsle) <= length) {
            const OpRegionAsle* asle = reinterpret_cast<const OpRegionAsle*>(bytes + headerOffset + kOpRegionAsleOffset);
            uint64_t rvda = asle->rvda;
            m_opregionRvds = asle->rvds;
            if (rvda != 0u && m_opregionRvds >= sizeof(VbtHeader)) {
                uint64_t rvdaCandidates[2] = { 0, 0 };
                uint32_t rvdaCandidateCount = 0;
                if (adjustedPhysBase) {
                    rvdaCandidates[rvdaCandidateCount++] = adjustedPhysBase + rvda;
                }
                rvdaCandidates[rvdaCandidateCount++] = rvda;

                for (uint32_t candidateIndex = 0; candidateIndex < rvdaCandidateCount; ++candidateIndex) {
                    const uint64_t rvdaPhys = rvdaCandidates[candidateIndex];
                    m_opregionRvda = rvdaPhys;
                    IOLog("[TGL-Connector] %s RVDA candidate[%u]=0x%llX RVDS=0x%X\n",
                          source ? source : "opregion",
                          candidateIndex,
                          static_cast<unsigned long long>(m_opregionRvda),
                          m_opregionRvds);

                    IOMemoryDescriptor* vbtDesc = IOMemoryDescriptor::withPhysicalAddress(m_opregionRvda, m_opregionRvds, kIODirectionInOut);
                    if (!vbtDesc) {
                        IOLog("[TGL-Connector] %s failed: could not create RVDA descriptor at 0x%llX size=0x%X\n",
                              source ? source : "opregion",
                              static_cast<unsigned long long>(m_opregionRvda),
                              m_opregionRvds);
                        continue;
                    }

                    IOMemoryMap* vbtMap = vbtDesc->map();
                    if (vbtMap) {
                        const uint8_t* rvdaBytes = reinterpret_cast<const uint8_t*>(vbtMap->getVirtualAddress());
                        const uint32_t rvdaLen = static_cast<uint32_t>(vbtMap->getLength());
                        logRawBytes(candidateIndex == 0 ? "opregion-rvda-vbt-rel" : "opregion-rvda-vbt-raw",
                                    rvdaBytes,
                                    rvdaLen > 256u ? 256u : rvdaLen);
                        if (adoptVBTWindow(rvdaBytes,
                                           rvdaLen,
                                           candidateIndex == 0 ? "opregion-rvda-rel" : "opregion-rvda-raw",
                                           m_opregionRvda,
                                           true)) {
                            vbtMap->release();
                            vbtDesc->release();
                            return true;
                        }
                        vbtMap->release();
                    } else {
                        IOLog("[TGL-Connector] %s failed: could not map RVDA blob at 0x%llX size=0x%X\n",
                              source ? source : "opregion",
                              static_cast<unsigned long long>(m_opregionRvda),
                              m_opregionRvds);
                    }
                    vbtDesc->release();
                }
            }
        }

        const uint32_t inlineVbtLimit = ((m_opregionMboxes & kMboxAsleExt) != 0u) ? kOpRegionAsleExtOffset : (length - headerOffset);
        if (inlineVbtLimit > kOpRegionVbtOffset && headerOffset + inlineVbtLimit <= length &&
            adoptVBTWindow(bytes + headerOffset + kOpRegionVbtOffset,
                           inlineVbtLimit - kOpRegionVbtOffset,
                           "opregion-inline",
                           adjustedPhysBase ? (adjustedPhysBase + kOpRegionVbtOffset) : 0,
                           true)) {
            return true;
        }

        return false;
    };

    auto tryPhysicalOpRegion = [&](uint64_t phys, const char* source) -> bool {
        if (!phys) {
            return false;
        }
        IOMemoryDescriptor* desc = IOMemoryDescriptor::withPhysicalAddress(phys, kOpRegionSize, kIODirectionInOut);
        if (!desc) {
            IOLog("[TGL-Connector] %s failed: could not create descriptor at 0x%llX\n",
                  source ? source : "opregion-phys",
                  static_cast<unsigned long long>(phys));
            return false;
        }
        IOMemoryMap* map = desc->map();
        if (!map) {
            desc->release();
            IOLog("[TGL-Connector] %s failed: could not map 0x%llX\n",
                  source ? source : "opregion-phys",
                  static_cast<unsigned long long>(phys));
            return false;
        }
        const bool loaded = loadFromMappedOpRegion(reinterpret_cast<const uint8_t*>(map->getVirtualAddress()),
                                                   static_cast<uint32_t>(map->getLength()),
                                                   source,
                                                   phys);
        map->release();
        desc->release();
        return loaded;
    };

    const uint32_t asls = m_provider->configRead32(0xFC) & ~0xFFFu;
    if (asls && tryPhysicalOpRegion(asls, "pci-asls")) {
        return true;
    }

    static const char* kOpRegionBlobKeys[] = {
        "AAPL,OpRegion",
        "AAPL00,OpRegion",
        "opregion",
        "OpRegion",
        "AAPL,IGPUOpRegion",
        "AAPL,GraphicsMem",
    };
    for (uint32_t i = 0; i < sizeof(kOpRegionBlobKeys) / sizeof(kOpRegionBlobKeys[0]); ++i) {
        OSObject* object = copyPropertyFromServiceChain(m_provider, kOpRegionBlobKeys[i]);
        OSData* data = OSDynamicCast(OSData, object);
        bool loaded = false;
        if (data && data->getLength() >= sizeof(OpRegionHeader)) {
            loaded = loadFromMappedOpRegion(reinterpret_cast<const uint8_t*>(data->getBytesNoCopy()),
                                            static_cast<uint32_t>(data->getLength()),
                                            kOpRegionBlobKeys[i],
                                            0);
        }
        if (object) {
            object->release();
        }
        if (loaded) {
            return true;
        }
    }

    static const char* kOpRegionPhysKeys[] = {
        "AAPL,OpRegionAddress",
        "AAPL00,OpRegionAddress",
        "opregion-address",
        "OpRegionAddress",
    };
    for (uint32_t i = 0; i < sizeof(kOpRegionPhysKeys) / sizeof(kOpRegionPhysKeys[0]); ++i) {
        OSObject* object = copyPropertyFromServiceChain(m_provider, kOpRegionPhysKeys[i]);
        uint64_t phys = 0;
        if (OSNumber* number = OSDynamicCast(OSNumber, object)) {
            phys = number->unsigned64BitValue();
        } else if (OSData* data = OSDynamicCast(OSData, object)) {
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(data->getBytesNoCopy());
            if (raw && data->getLength() >= 8u) {
                phys = static_cast<uint64_t>(readLe32(raw)) | (static_cast<uint64_t>(readLe32(raw + 4u)) << 32);
            } else if (raw && data->getLength() >= 4u) {
                phys = readLe32(raw);
            }
        }
        if (object) {
            object->release();
        }
        if (phys && tryPhysicalOpRegion(phys & ~0xFFFULL, kOpRegionPhysKeys[i])) {
            return true;
        }
    }

    IOLog("[TGL-Connector] No valid VBT found in OpRegion (source=%s major=%u minor=%u mboxes=0x%08X phys=0x%llX rvda=0x%llX rvds=0x%X)\n",
          m_opregionSource[0] ? m_opregionSource : "none",
          m_opregionMajor,
          m_opregionMinor,
          m_opregionMboxes,
          static_cast<unsigned long long>(m_opregionPhys),
          static_cast<unsigned long long>(m_opregionRvda),
          m_opregionRvds);
    return false;
}

bool FakeIrisXEConnectorManager::loadVBTFromRegistry()
{
    if (!m_provider) {
        return false;
    }

    static const char* kKeys[] = {
        "AAPL,VBT",
        "AAPL00,VBT",
        "VBT",
    };

    for (uint32_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
        OSData* data = OSDynamicCast(OSData, m_provider->getProperty(kKeys[i]));
        if (!data || data->getLength() < sizeof(VbtHeader)) {
            continue;
        }

        if (adoptVBTWindow(reinterpret_cast<const uint8_t*>(data->getBytesNoCopy()),
                           static_cast<uint32_t>(data->getLength()),
                           kKeys[i],
                           0,
                           false)) {
            IOLog("[TGL-Connector] Loaded VBT from provider property %s\n", kKeys[i]);
            return true;
        }
    }

    return false;
}

const uint8_t* FakeIrisXEConnectorManager::findBDBSection(uint8_t blockId, uint16_t* outSize) const
{
    if (outSize) {
        *outSize = 0;
    }
    if (!m_vbtLoaded || m_vbtLength < sizeof(VbtHeader)) {
        return nullptr;
    }

    const VbtHeader* header = reinterpret_cast<const VbtHeader*>(m_vbtStorage);
    const uint32_t bdbOffset = readLe32(&header->bdbOffset);
    if (bdbOffset + sizeof(BdbHeader) > m_vbtLength) {
        return nullptr;
    }

    const uint8_t* bdbBase = m_vbtStorage + bdbOffset;
    const BdbHeader* bdbHeader = reinterpret_cast<const BdbHeader*>(bdbBase);
    const uint16_t bdbSize = readLe16(&bdbHeader->bdbSize);
    const uint16_t bdbHeaderSize = readLe16(&bdbHeader->headerSize);
    if (bdbSize < sizeof(BdbHeader) || bdbOffset + bdbSize > m_vbtLength || bdbHeaderSize >= bdbSize) {
        return nullptr;
    }

    uint32_t index = bdbHeaderSize;
    while (index + 3u <= bdbSize) {
        uint8_t currentId = bdbBase[index];
        uint16_t currentSize = readLe16(bdbBase + index + 1u);
        index += 3u;
        if (index + currentSize > bdbSize) {
            return nullptr;
        }
        if (currentId == blockId) {
            if (outSize) {
                *outSize = currentSize;
            }
            return bdbBase + index;
        }
        index += currentSize;
    }

    return nullptr;
}

bool FakeIrisXEConnectorManager::decodeChildDeviceToConnector(const uint8_t* childBytes,
                                                              uint8_t childSize,
                                                              TGLConnectorDesc& outConn) const
{
    if (!childBytes || childSize < 23u) {
        return false;
    }

    const uint16_t handle = childLe16(childBytes, childSize, 0u);
    const uint16_t deviceType = childLe16(childBytes, childSize, 2u);
    const uint8_t dvoPort = childByte(childBytes, childSize, 16u);
    const uint8_t laneInfo = childByte(childBytes, childSize, 23u);
    const uint8_t supportInfo = childByte(childBytes, childSize, 24u);
    const uint8_t auxChannel = childByte(childBytes, childSize, 25u);
    const uint8_t dongleDetect = childByte(childBytes, childSize, 26u);
    const uint8_t dvoWiring = childByte(childBytes, childSize, 28u);
    const uint16_t extendedType = childLe16(childBytes, childSize, 30u);
    const uint8_t dvoFunction = childByte(childBytes, childSize, 32u);
    const uint8_t typeCFlags = childByte(childBytes, childSize, 33u);
    const uint8_t dpMaxRateField = childByte(childBytes, childSize, 38u) & 0x7u;

    const bool isInternal = (handle == kDeviceHandleLfp1 || handle == kDeviceHandleLfp2 ||
                             deviceType == kDeviceTypeEDp || deviceType == kDeviceTypeIntLfp ||
                             (deviceType & kDeviceTypeInternalConnector) != 0u);
    const bool dpSupport = deviceType == kDeviceTypeDp ||
                           deviceType == kDeviceTypeEDp ||
                           (deviceType & kDeviceTypeDisplayPortOutput) != 0u ||
                           (supportInfo & (1u << 1)) != 0u ||
                           auxChannel != 0u;
    const bool hdmiSupport = deviceType == kDeviceTypeHdmi ||
                             (deviceType & kDeviceTypeTmdsOutput) != 0u ||
                             (supportInfo & (1u << 0)) != 0u;
    const bool dpUsbTypeC = (typeCFlags & 0x1u) != 0u;

    TGLConnectorType type = TGLConnectorType::Unknown;
    if (isInternal && dpSupport) {
        type = TGLConnectorType::eDP;
    } else if (dpSupport && dpUsbTypeC) {
        type = TGLConnectorType::USB4TypeC;
    } else if (dpSupport) {
        type = TGLConnectorType::DP;
    } else if (hdmiSupport) {
        type = TGLConnectorType::HDMI;
    } else {
        switch (dvoPort) {
            case kDvoPortDpa:
            case kDvoPortDpb:
            case kDvoPortDpc:
            case kDvoPortDpd:
            case kDvoPortDpe:
            case kDvoPortDpf:
            case kDvoPortDpg:
            case kDvoPortDph:
            case kDvoPortDpi:
                type = isInternal ? TGLConnectorType::eDP : TGLConnectorType::DP;
                break;
            case kDvoPortHdmia:
            case kDvoPortHdmib:
            case kDvoPortHdmic:
            case kDvoPortHdmid:
            case kDvoPortHdmie:
            case kDvoPortHdmif:
            case kDvoPortHdmig:
            case kDvoPortHdmih:
            case kDvoPortHdmii:
                type = TGLConnectorType::HDMI;
                break;
            default:
                type = TGLConnectorType::Unknown;
                break;
        }
    }

    if (type == TGLConnectorType::Unknown) {
        return false;
    }

    TGLAUXChannel mappedAux = TGLAUXChannel::AUX_A;
    switch (auxChannel) {
        case kDpAuxA:
            mappedAux = TGLAUXChannel::AUX_A;
            break;
        case kDpAuxB:
            mappedAux = TGLAUXChannel::AUX_B;
            break;
        case kDpAuxC:
            mappedAux = TGLAUXChannel::AUX_C;
            break;
        case kDpAuxD:
            mappedAux = dpUsbTypeC ? TGLAUXChannel::AUX_TC1 : TGLAUXChannel::AUX_D;
            break;
        case kDpAuxE:
            mappedAux = TGLAUXChannel::AUX_TC2;
            break;
        case kDpAuxF:
            mappedAux = TGLAUXChannel::AUX_TC3;
            break;
        case kDpAuxG:
        case kDpAuxH:
        case kDpAuxI:
            mappedAux = TGLAUXChannel::AUX_TC4;
            break;
        default:
            mappedAux = isInternal ? TGLAUXChannel::AUX_A : TGLAUXChannel::AUX_B;
            break;
    }

    TGLDDIPort mappedDdi = TGLDDIPort::DDI_A;
    switch (dvoPort) {
        case kDvoPortDpa:
        case kDvoPortHdmia:
        case kDvoPortLvds:
            mappedDdi = TGLDDIPort::DDI_A;
            break;
        case kDvoPortDpb:
        case kDvoPortHdmib:
            mappedDdi = TGLDDIPort::DDI_B;
            break;
        case kDvoPortDpc:
        case kDvoPortHdmic:
            mappedDdi = TGLDDIPort::DDI_C;
            break;
        case kDvoPortDpd:
        case kDvoPortHdmid:
            mappedDdi = dpUsbTypeC ? TGLDDIPort::DDI_TC1 : TGLDDIPort::DDI_D;
            break;
        case kDvoPortDpe:
        case kDvoPortHdmie:
            mappedDdi = TGLDDIPort::DDI_TC2;
            break;
        case kDvoPortDpf:
        case kDvoPortHdmif:
            mappedDdi = TGLDDIPort::DDI_TC3;
            break;
        case kDvoPortDpg:
        case kDvoPortHdmig:
        case kDvoPortDph:
        case kDvoPortHdmih:
        case kDvoPortDpi:
        case kDvoPortHdmii:
            mappedDdi = TGLDDIPort::DDI_TC4;
            break;
        default:
            mappedDdi = isInternal ? TGLDDIPort::DDI_A : TGLDDIPort::DDI_B;
            break;
    }

    TGLHPDPin hpdPin = TGLHPDPin::HPD_NONE;
    uint32_t hpdBit = 0;
    if (!isInternal) {
        switch (mappedDdi) {
            case TGLDDIPort::DDI_B:
                hpdPin = TGLHPDPin::HPD_PIN_1;
                hpdBit = (1u << 1);
                break;
            case TGLDDIPort::DDI_C:
                hpdPin = TGLHPDPin::HPD_PIN_2;
                hpdBit = (1u << 2);
                break;
            case TGLDDIPort::DDI_D:
            case TGLDDIPort::DDI_TC1:
                hpdPin = TGLHPDPin::HPD_PIN_3;
                hpdBit = (1u << 3);
                break;
            case TGLDDIPort::DDI_TC2:
                hpdPin = TGLHPDPin::HPD_PIN_4;
                hpdBit = (1u << 4);
                break;
            case TGLDDIPort::DDI_TC3:
                hpdPin = TGLHPDPin::HPD_PIN_5;
                hpdBit = (1u << 5);
                break;
            case TGLDDIPort::DDI_TC4:
                hpdPin = TGLHPDPin::HPD_PIN_6;
                hpdBit = (1u << 6);
                break;
            default:
                break;
        }
    }

    uint8_t maxLanes = 4;
    const uint8_t laneEncoding = static_cast<uint8_t>((laneInfo >> 6) & 0x3u);
    if (laneEncoding == 1u) {
        maxLanes = 1;
    } else if (laneEncoding == 2u) {
        maxLanes = 2;
    } else if (laneEncoding == 3u) {
        maxLanes = 4;
    }

    bzero(&outConn, sizeof(outConn));
    outConn.type = type;
    outConn.ddiPort = mappedDdi;
    outConn.auxChannel = mappedAux;
    outConn.hpdPin = hpdPin;
    outConn.hdpBit = hpdBit;
    outConn.maxLanes = maxLanes;
    outConn.maxBitRate = (type == TGLConnectorType::HDMI) ? 5940 : 10100;
    switch (dpMaxRateField) {
        case 1:
            outConn.maxBitRate = 1620;
            break;
        case 2:
            outConn.maxBitRate = 2700;
            break;
        case 3:
            outConn.maxBitRate = 5400;
            break;
        case 4:
            outConn.maxBitRate = 8100;
            break;
        case 5:
            outConn.maxBitRate = 10000;
            break;
        default:
            break;
    }
    outConn.isInternal = isInternal;
    outConn.supportsAudio = (type != TGLConnectorType::eDP);
    outConn.discoveredFromVbt = true;
    outConn.panelType = 0xFFu;

    IOLog("[TGL-Connector] VBT child handle=0x%04X type=0x%04X dvo=0x%02X aux=0x%02X ddc=0x%02X typeC=%u support=0x%02X detect=0x%02X wire=0x%02X ext=0x%04X func=0x%02X -> %s on %s\n",
          handle,
          deviceType,
          dvoPort,
          auxChannel,
          childByte(childBytes, childSize, 19u),
          dpUsbTypeC ? 1u : 0u,
          supportInfo,
          dongleDetect,
          dvoWiring,
          extendedType,
          dvoFunction,
          connectorTypeName(type),
          ddiName(mappedDdi));

    return true;
}

bool FakeIrisXEConnectorManager::applyVBTChildDevice(const uint8_t* childBytes, uint8_t childSize, uint8_t slotIndex)
{
    if (slotIndex >= 4u) {
        return false;
    }

    TGLConnectorDesc decoded;
    if (!decodeChildDeviceToConnector(childBytes, childSize, decoded)) {
        return false;
    }

    decoded.index = slotIndex;
    m_connectors[slotIndex] = decoded;
    if (decoded.isInternal && !m_internalPanel) {
        m_internalPanel = &m_connectors[slotIndex];
    }
    return true;
}

bool FakeIrisXEConnectorManager::parseVBTConnectors()
{
    if (!m_vbtLoaded) {
        return false;
    }

    const VbtHeader* header = reinterpret_cast<const VbtHeader*>(m_vbtStorage);
    const uint32_t bdbOffset = readLe32(&header->bdbOffset);
    if (bdbOffset + sizeof(BdbHeader) > m_vbtLength) {
        return false;
    }

    const BdbHeader* bdbHeader = reinterpret_cast<const BdbHeader*>(m_vbtStorage + bdbOffset);
    m_bdbVersion = readLe16(&bdbHeader->version);

    uint16_t generalDefsSize = 0;
    const uint8_t* generalDefs = findBDBSection(kVbtBlockGeneralDefinitions, &generalDefsSize);
    if (!generalDefs || generalDefsSize < 5u) {
        IOLog("[TGL-Connector] VBT missing GENERAL_DEFINITIONS block\n");
        return false;
    }

    const uint8_t childDevSize = generalDefs[4];
    const uint8_t advertisedChildCount = (generalDefsSize > 5u) ? generalDefs[5] : 0u;
    if (childDevSize < 23u || generalDefsSize <= 5u) {
        IOLog("[TGL-Connector] VBT GENERAL_DEFINITIONS child size invalid: %u\n", childDevSize);
        return false;
    }

    bzero(m_connectors, sizeof(m_connectors));
    m_connectorCount = 0;
    m_internalPanel = nullptr;

    const uint8_t* children = generalDefs + 5u;
    const uint16_t childBytes = generalDefsSize - 5u;
    const uint16_t childCount = childBytes / childDevSize;
    IOLog("[TGL-Connector] VBT GENERAL_DEFINITIONS childSize=%u advertisedCount=%u computedCount=%u bootDisplay=0x%02X%02X\n",
          childDevSize,
          advertisedChildCount,
          childCount,
          generalDefs[3],
          generalDefs[2]);

    uint8_t nextSlot = 0;
    for (uint16_t pass = 0; pass < 2 && nextSlot < 4u; ++pass) {
        for (uint16_t i = 0; i < childCount && nextSlot < 4u; ++i) {
            const uint8_t* child = children + (i * childDevSize);
            TGLConnectorDesc decoded;
            if (!decodeChildDeviceToConnector(child, childDevSize, decoded)) {
                continue;
            }
            if ((pass == 0 && !decoded.isInternal) || (pass == 1 && decoded.isInternal)) {
                continue;
            }
            if (applyVBTChildDevice(child, childDevSize, nextSlot)) {
                ++nextSlot;
            }
        }
    }

    if (nextSlot == 0u) {
        IOLog("[TGL-Connector] VBT parsed successfully but contained no usable connectors\n");
        return false;
    }

    m_connectorCount = nextSlot;

    uint16_t lfpOptionsSize = 0;
    const BdbLfpOptions* lfpOptions = reinterpret_cast<const BdbLfpOptions*>(findBDBSection(kVbtBlockLfpOptions, &lfpOptionsSize));
    uint16_t edpSize = 0;
    const BdbEdp* edp = reinterpret_cast<const BdbEdp*>(findBDBSection(kVbtBlockEdp, &edpSize));
    if (m_internalPanel && lfpOptions && lfpOptionsSize >= sizeof(BdbLfpOptions)) {
        m_internalPanel->panelType = lfpOptions->panelType;
        if (m_internalPanel->panelType > 15u) {
            m_internalPanel->panelType = 0u;
        }
    }
    if (m_internalPanel && edp && edpSize >= sizeof(BdbEdp)) {
        uint8_t panelType = m_internalPanel->panelType;
        if (panelType < 16u) {
            switch (edp->fastLinkParams[panelType].lanes) {
                case 0:
                    m_internalPanel->maxLanes = 1;
                    break;
                case 1:
                    m_internalPanel->maxLanes = 2;
                    break;
                case 3:
                    m_internalPanel->maxLanes = 4;
                    break;
                default:
                    break;
            }
            switch (edp->fastLinkParams[panelType].rate) {
                case 0:
                    m_internalPanel->maxBitRate = 1620;
                    break;
                case 1:
                    m_internalPanel->maxBitRate = 2700;
                    break;
                case 2:
                    m_internalPanel->maxBitRate = 5400;
                    break;
                default:
                    break;
            }
        }
    }

    uint16_t powerSeqSize = 0;
    const EdpPowerSeq* powerSeq = reinterpret_cast<const EdpPowerSeq*>(findBDBSection(kVbtBlockPowerSeq, &powerSeqSize));
    if (m_internalPanel && powerSeq && powerSeqSize >= sizeof(EdpPowerSeq)) {
        uint8_t panelType = m_internalPanel->panelType;
        if (panelType < 16u) {
            const EdpPowerSeq& seq = powerSeq[panelType];
            m_panelPowerOnDelay = seq.t1t3;
            m_panelPowerOffDelay = seq.t8;
            IOLog("[TGL-Connector] VBT panel timing: T1+T3=%uus T8=%uus T9=%uus T10=%uus T11+T12=%uus\n",
                  seq.t1t3, seq.t8, seq.t9, seq.t10, seq.t11t12);
        }
    }

    const bool ubuntuShape = (m_vbtLength == 0x21C0u) && (m_bdbVersion == 244u) && (childDevSize == 39u);
    IOLog("[TGL-Connector] VBT cross-check: ubuntuShape=%u vbtSize=0x%llX bdb=%u childSize=%u\n",
          ubuntuShape ? 1u : 0u,
          static_cast<unsigned long long>(m_vbtLength),
          m_bdbVersion,
          childDevSize);
    if (m_internalPanel) {
        IOLog("[TGL-Connector] VBT internal panel: ddi=%s aux=%u panelType=%u lanes=%u rate=%u present=%u\n",
              ddiName(m_internalPanel->ddiPort),
              static_cast<unsigned>(m_internalPanel->auxChannel),
              m_internalPanel->panelType,
              m_internalPanel->maxLanes,
              m_internalPanel->maxBitRate,
              m_internalPanel->present ? 1u : 0u);
    }

    IOLog("[TGL-Connector] Parsed %u connectors from real VBT (VBT=%u BDB=%u)\n",
          m_connectorCount,
          m_vbtVersion,
          m_bdbVersion);
    return true;
}
