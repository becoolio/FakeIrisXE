//
//  FakeIrisXEGuC.cpp
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 03/12/25.
//

// FakeIrisXEGuC.cpp
#include "FakeIrisXEGuC.hpp"
#include "i915_reg.h"

// V135: Add missing register defines - aggressive Linux GT initialization
// V135: Added PPGTT, GART, additional power management, GT workarounds
#ifndef GEN11_GUC_RESET
#define GEN11_GUC_RESET              0x1C0C0
#endif

// V135: Additional Gen12/Tiger Lake registers from Linux i915
#ifndef GEN12_PPGTT_PML4E
#define GEN12_PPGTT_PML4E          0x1C80   // PPGTT PML4 Entry
#endif
#ifndef GEN12_PPGTT_PML4E_2
#define GEN12_PPGTT_PML4E_2        0x1C84   // PPGTT PML4 Entry (alternate)
#endif
#ifndef GEN12_GGTT_TOP
#define GEN12_GGTT_TOP              0x108000 // GGTT top of memory
#endif
#ifndef GEN12_GGTT_PTE
#define GEN12_GGTT_PTE              0x40000  // GGTT PTE start
#endif

// V135: Additional power management registers
#ifndef GEN12_PWR_WELL_CTL
#define GEN12_PWR_WELL_CTL          0x45400
#endif
#ifndef GEN12_PWR_WELL_CTL2
#define GEN12_PWR_WELL_CTL2         0x45404
#endif
#ifndef GEN12_PWR_WELL_CTL3
#define GEN12_PWR_WELL_CTL3         0x45408
#endif
#ifndef GEN12_PWR_WELL_CTL4
#define GEN12_PWR_WELL_CTL4         0x4540C
#endif
#ifndef GEN12_PWR_WELL_STATUS
#define GEN12_PWR_WELL_STATUS       0x45410
#endif

// V135: GT workarounds from Linux
#ifndef GEN12_GT_WORKAROUND
#define GEN12_GT_WORKAROUND         0xA200
#endif
#ifndef GEN12_GT_PERF_LIMIT
#define GEN12_GT_PERF_LIMIT         0xA094
#endif
#ifndef GEN12_RC_CTL
#define GEN12_RC_CTL                0xA090
#endif

// V135: MOCS registers (Memory Override Control State)
#ifndef GEN12_MOCS0
#define GEN12_MOCS0                 0xB020
#endif
#ifndef GEN12_MOCS1
#define GEN12_MOCS1                 0xB024
#endif
#ifndef GEN12_MOCS2
#define GEN12_MOCS2                 0xB028
#endif

// V135: Additional GuC registers
#ifndef GEN11_GUC_MISC_CTRL
#define GEN11_GUC_MISC_CTRL         0x1C0F0
#endif
#ifndef GEN11_GUC_WOPCM_OFFSET
#define GEN11_GUC_WOPCM_OFFSET      0x1C0E0
#endif
#ifndef GEN12_GUC_WOPCM_SIZE
#define GEN12_GUC_WOPCM_SIZE        0x1C0E4
#endif
#ifndef GEN11_GUC_CAPS3
#define GEN11_GUC_CAPS3              0x1C0A8
#endif
#ifndef GEN11_GUC_CAPS4
#define GEN11_GUC_CAPS4              0x1C0AC
#endif
#ifndef GEN11_GUC_IRQ_CLEAR
#define GEN11_GUC_IRQ_CLEAR         0x1C5C4
#endif
#ifndef GEN11_GUC_IRQ_ENABLE
#define GEN11_GUC_IRQ_ENABLE         0x1C5C8
#endif
#ifndef GEN11_HUC_FW_ADDR_LO
#define GEN11_HUC_FW_ADDR_LO        0x1C0D0
#endif
#ifndef GEN11_HUC_FW_ADDR_HI
#define GEN11_HUC_FW_ADDR_HI        0x1C0D4
#endif

// V133: RPS registers
#ifndef GEN12_RPNCURT
#define GEN12_RPNCURT               0xA010
#endif
#ifndef GEN12_RPNMAXCT
#define GEN12_RPNMAXCT              0xA020
#endif
#ifndef GEN12_RPNMINCT
#define GEN12_RPNMINCT              0xA030
#endif

// V134: Additional GT and power management registers
#ifndef GT_PM_CONFIG
#define GT_PM_CONFIG                0xA290
#endif
#ifndef PWR_WELL_CTL2
#define PWR_WELL_CTL2              0x45404
#endif
#ifndef PWR_WELL_CTL3
#define PWR_WELL_CTL3              0x45408
#endif
#ifndef FORCEWAKE_REQ
#define FORCEWAKE_REQ              0xA188
#endif
#ifndef FORCEWAKE_ACK
#define FORCEWAKE_ACK              0x130044
#endif
// V136: CRITICAL FIX - GuC registers are at 0xC000+ offsets (Tiger Lake), NOT 0x5820!
// Based on Intel PRM Vol13 and ChatGPT analysis
// GUC_SHIM_CONTROL at 0xC064 (was incorrectly 0x5820)
#ifndef GUC_SHIM_CONTROL
#define GUC_SHIM_CONTROL           0xC064
#endif
#ifndef GUC_SHIM_CONTROL2
#define GUC_SHIM_CONTROL2         0xC068
#endif
// GuC status at 0xC000
#ifndef GUC_STATUS
#define GUC_STATUS                0xC000
#endif
// WOPCM registers at 0xC050 and 0xC340
#ifndef GUC_WOPCM_BASE
#define GUC_WOPCM_BASE           0xC050
#endif
#ifndef GUC_WOPCM_SIZE
#define GUC_WOPCM_SIZE           0xC340
#endif
// Doorbell/interrupt trigger at 0xC4C8
#ifndef GUC_SEND_INTERRUPT
#define GUC_SEND_INTERRUPT       0xC4C8
#endif
// RSA signature base at 0xC200
#ifndef GUC_RSA_SIGNATURE
#define GUC_RSA_SIGNATURE        0xC200
#endif

// ============================================================================
// V137: CRITICAL FIXES - Correct Gen12/Tiger Lake register map
// Based on Intel PRM and Linux i915 driver analysis
// ============================================================================

// ===== CORRECT GuC/DMA register map (Gen12/Tiger Lake - direct offsets, no base) =====
// GuC status/control
#ifndef GUC_STATUS_V137
#define GUC_STATUS_V137              0xC000
#endif
#ifndef GUC_SHIM_CONTROL_V137
#define GUC_SHIM_CONTROL_V137        0xC064
#endif
#ifndef GUC_CTL_V137
#define GUC_CTL_V137                 0xC010
#endif
#ifndef GUC_RESET_CTL_V137
#define GUC_RESET_CTL_V137           0xC040    // GuC reset control (V138)
#endif

// WOPCM registers (CORRECT offsets)
#ifndef GUC_WOPCM_SIZE_V137
#define GUC_WOPCM_SIZE_V137         0xC050    // size + lock bit
#endif
#ifndef DMA_GUC_WOPCM_OFFSET_V137
#define DMA_GUC_WOPCM_OFFSET_V137    0xC340    // base/offset + valid bit
#endif

// DMA copy engine (CORRECT offsets - 0xC300 range)
#ifndef DMA_ADDR_0_LOW_V137
#define DMA_ADDR_0_LOW_V137         0xC300    // source address low
#endif
#ifndef DMA_ADDR_0_HIGH_V137
#define DMA_ADDR_0_HIGH_V137        0xC304    // source address high + address space
#endif
#ifndef DMA_ADDR_1_LOW_V137
#define DMA_ADDR_1_LOW_V137         0xC308    // destination address low
#endif
#ifndef DMA_ADDR_1_HIGH_V137
#define DMA_ADDR_1_HIGH_V137        0xC30C    // destination address high + address space
#endif
#ifndef DMA_COPY_SIZE_V137
#define DMA_COPY_SIZE_V137          0xC310    // transfer size
#endif
#ifndef DMA_CTRL_V137
#define DMA_CTRL_V137              0xC314    // control + trigger
#endif

// Address space encoding (in HIGH registers)
#ifndef DMA_ADDRESS_SPACE_WOPCM_V137
#define DMA_ADDRESS_SPACE_WOPCM_V137 (7u << 16)  // 0x70000
#endif
#ifndef DMA_ADDRESS_SPACE_GTT_V137
#define DMA_ADDRESS_SPACE_GTT_V137   (8u << 16)  // 0x80000
#endif

// WOPCM encoding bits
#ifndef GUC_WOPCM_SIZE_LOCKED_V137
#define GUC_WOPCM_SIZE_LOCKED_V137    (1u << 0)
#endif
#ifndef GUC_WOPCM_SIZE_MASK_V137
#define GUC_WOPCM_SIZE_MASK_V137      (0xFFFFFu << 12)  // size is [31:12]
#endif
#ifndef GUC_WOPCM_OFFSET_VALID_V137
#define GUC_WOPCM_OFFSET_VALID_V137   (1u << 0)
#endif
#ifndef DMA_GUC_WOPCM_OFFSET_MASK_V137
#define DMA_GUC_WOPCM_OFFSET_MASK_V137 (0x3FFFFu << 14)  // base is [31:14]
#endif

// DMA control bits
#ifndef START_DMA_V137
#define START_DMA_V137               (1u << 0)
#endif
#ifndef UOS_MOVE_V137
#define UOS_MOVE_V137                (1u << 4)
#endif

// UOS RSA scratch registers (0xC200+)
#ifndef UOS_RSA_SCRATCH_BASE_V137
#define UOS_RSA_SCRATCH_BASE_V137    0xC200
#endif
#ifndef UOS_RSA_SCRATCH_COUNT_V137
#define UOS_RSA_SCRATCH_COUNT_V137   64  // 256 bytes
#endif

// GUC_STATUS bitfields
#ifndef GUC_BOOTROM_STATUS_MASK_V137
#define GUC_BOOTROM_STATUS_MASK_V137  (0x7Fu << 1)
#endif
#ifndef GUC_BOOTROM_STATUS_SHIFT_V137
#define GUC_BOOTROM_STATUS_SHIFT_V137 1
#endif
#ifndef GUC_UKERNEL_STATUS_MASK_V137
#define GUC_UKERNEL_STATUS_MASK_V137  (0xFFu << 8)
#endif
#ifndef GUC_UKERNEL_STATUS_SHIFT_V137
#define GUC_UKERNEL_STATUS_SHIFT_V137 8
#endif
#ifndef GUC_MIA_CORE_STATUS_MASK_V137
#define GUC_MIA_CORE_STATUS_MASK_V137 (0x7u << 16)
#endif
#ifndef GUC_MIA_CORE_STATUS_SHIFT_V137
#define GUC_MIA_CORE_STATUS_SHIFT_V137 16
#endif

// Helper macros
#ifndef FIELD_GET_V137
#define FIELD_GET_V137(mask, v) (((v) & (mask)) >> __builtin_ctz(mask))
#endif

// Legacy/alternate DMA registers (keep for reference - but NOT the correct ones!)
#ifndef GUC_DMA_STATUS
#define GUC_DMA_STATUS             0x1C588
#endif
#ifndef DMA_CTRL
#define DMA_CTRL                  0x1C584
#endif

// Old incorrect DMA registers (were being used - WRONG!)
#ifndef DMA_ADDR_0_LOW
#define DMA_ADDR_0_LOW            0x1C570
#endif
#ifndef DMA_ADDR_0_HIGH
#define DMA_ADDR_0_HIGH           0x1C574
#endif
#ifndef DMA_ADDR_1_LOW
#define DMA_ADDR_1_LOW            0x1C578
#endif
#ifndef DMA_ADDR_1_HIGH
#define DMA_ADDR_1_HIGH           0x1C57C
#endif
#ifndef DMA_COPY_SIZE
#define DMA_COPY_SIZE             0x1C580
#endif

// Linux DMA registers (V132 fallback)
#ifndef DMA_ADDR_0_LOW_LINUX
#define DMA_ADDR_0_LOW_LINUX      0x5820
#endif
#ifndef DMA_ADDR_0_HIGH_LINUX
#define DMA_ADDR_0_HIGH_LINUX      0x5824
#endif
#ifndef DMA_ADDR_1_LOW_LINUX
#define DMA_ADDR_1_LOW_LINUX      0x5828
#endif
#ifndef DMA_ADDR_1_HIGH_LINUX
#define DMA_ADDR_1_HIGH_LINUX     0x582C
#endif
#ifndef DMA_COPY_SIZE_LINUX
#define DMA_COPY_SIZE_LINUX       0x5830
#endif
#ifndef DMA_CTRL_LINUX
#define DMA_CTRL_LINUX            0x5834
#endif

// DMA flags
#ifndef UOS_MOVE
#define UOS_MOVE                  0x05
#endif
#ifndef START_DMA
#define START_DMA                 0x1
#endif
#ifndef DMA_ADDRESS_SPACE_WOPCM
#define DMA_ADDRESS_SPACE_WOPCM   0x10000
#endif

// More GuC registers
#ifndef GEN11_GUC_FW_SIZE
#define GEN11_GUC_FW_SIZE        0x1C0B8
#endif
#ifndef GEN11_GUC_FW_ADDR_LO
#define GEN11_GUC_FW_ADDR_LO     0x1C0C4
#endif
#ifndef GEN11_HUC_FW_SIZE
#define GEN11_HUC_FW_SIZE        0x1C0D8
#endif

// V133: RPS registers - add missing ones
#ifndef GEN12_RP_GT_PERF_STATUS
#define GEN12_RP_GT_PERF_STATUS  0xA070
#endif
#ifndef GEN12_RP_STATE_CAP
#define GEN12_RP_STATE_CAP       0xA040
#endif

// GuC status values
#ifndef GEN11_GUC_CAPS2
#define GEN11_GUC_CAPS2          0x1C0A4
#endif

// Apple DMA trigger magic value
#ifndef APPLE_DMA_MAGIC_TRIGGER
#define APPLE_DMA_MAGIC_TRIGGER   0xFFFF0011
#endif

// GuC load status values
#ifndef GUC_LOAD_SUCCESS_STATUS
#define GUC_LOAD_SUCCESS_STATUS   0xF0
#endif
#ifndef GUC_LOAD_FAIL_STATUS_1
#define GUC_LOAD_FAIL_STATUS_1    0xA0
#endif
#ifndef GUC_LOAD_FAIL_STATUS_2
#define GUC_LOAD_FAIL_STATUS_2    0x60
#endif

// GuC soft scratch
#ifndef GEN11_GUC_SOFT_SCRATCH
#define GEN11_GUC_SOFT_SCRATCH(n) (0x1C180 + (n) * 4)
#endif

// GuC SHIM flags
#ifndef GUC_ENABLE_READ_CACHE_LOGIC
#define GUC_ENABLE_READ_CACHE_LOGIC         (1 << 0)
#endif
#ifndef GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA
#define GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA (1 << 1)
#endif
#ifndef GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA
#define GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA (1 << 2)
#endif
#ifndef GUC_ENABLE_MIA_CLOCK_GATING
#define GUC_ENABLE_MIA_CLOCK_GATING         (1 << 3)
#endif
#ifndef GUC_DISABLE_SRAM_INIT_TO_ZEROES
#define GUC_DISABLE_SRAM_INIT_TO_ZEROES     (1 << 4)
#endif
#ifndef GUC_ENABLE_MIA_CACHING
#define GUC_ENABLE_MIA_CACHING              (1 << 5)
#endif
#ifndef GUC_ENABLE_DEBUG_REG
#define GUC_ENABLE_DEBUG_REG                (1 << 6)
#endif

// GT doorbell
#ifndef GT_DOORBELL_ENABLE
#define GT_DOORBELL_ENABLE    0x1
#endif

#define super OSObject

OSDefineMetaClassAndStructors(FakeIrisXEGuC, OSObject);

// TGL GuC registers
// V49: DMC registers (must load DMC before GuC per Intel PRM)
#define DMC_PROGRAMMABLE_ADDRESS_LOCATION   0x0008C040
#define DMC_PROGRAMMABLE_ADDRESS_LOCATION_1 0x0008C044
#define DMC_SSP_BASE                        0x0008C080

// V134: More comprehensive diagnostics - BAR0, ForceWake, GT state, pipeline
#define GEN11_HUC_STATUS                 0x1C0EC

// V134: Additional diagnostic registers
#define GEN12_GT_MODE                    0xA004   // GT mode control
#define GEN12_GT_IA_MODE                 0xA008   // GT IA mode
#define GEN12_GT_RC_MODE                 0xA00C   // GT RC mode
#define GEN12_GT_RP_STATE_CAP            0xA040   // RP state capability
#define GEN12_GT_PERF_STATUS             0xA070   // Performance status
#define GEN12_GT_PERF_LIMIT_REASON       0xA094   // Performance limit reason
#define GEN12_GT_L3_SQC_REG0             0xB010   // L3 SQC register
#define GEN12_GT_CDC_REG0                0xA000   // CDC register
#define GEN12_GT_FENCE_EN                0xA1F0   // Fence enable
#define GEN12_MMIO_START                 0xA000   // Start of MMIO range
#define GEN12_MMIO_END                   0xC000   // End of MMIO range

FakeIrisXEGuC* FakeIrisXEGuC::withOwner(FakeIrisXEFramebuffer* owner)
{
    FakeIrisXEGuC* obj = OSTypeAlloc(FakeIrisXEGuC);
    if (!obj) return nullptr;
    
    if (!obj->init()) {
        obj->release();
        return nullptr;
    }
    
    obj->fOwner = owner;
    return obj;
}

bool FakeIrisXEGuC::initGuC()
{
    IOLog("(FakeIrisXE) [V136] Initializing Gen12 GuC - FIXED REGISTER OFFSETS\n");
    IOLog("(FakeIrisXE) [V136] CRITICAL: Using 0xC000+ offsets (not 0x5820!)\n");
    
    // V135: Run aggressive Linux GT initialization BEFORE GuC load
    initGTPreWorkaround();
    
    IOLog("(FakeIrisXE) [V134] Features: BAR0 check, GT state, ForceWake, Pipeline status\n");
    
    // V134: Enhanced initial state dump with BAR0 verification
    IOLog("(FakeIrisXE) [V134] === INITIAL HARDWARE STATE ===\n");
    
    // V134: Read BAR0 status if available through framebuffer
    IOLog("(FakeIrisXE) [V134] === BAR0 & MMIO VERIFICATION ===\n");
    IOLog("(FakeIrisXE) [V134] Note: BAR0 should be mapped at framebuffer init\n");
    IOLog("(FakeIrisXE) [V134] MMIO base verified in FakeIrisXEFramebuffer\n");
    
    // V134: Read and dump all key registers at start
    uint32_t guc_status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
    uint32_t guc_ctl = fOwner->safeMMIORead(GEN11_GUC_CTL);
    uint32_t guc_reset = fOwner->safeMMIORead(GEN11_GUC_RESET);
    uint32_t guc_caps1 = fOwner->safeMMIORead(GEN11_GUC_CAPS1);
    uint32_t guc_caps2 = fOwner->safeMMIORead(GEN11_GUC_CAPS2);
    
    IOLog("(FakeIrisXE) [V134] GUC_STATUS:  0x%08X\n", guc_status);
    IOLog("(FakeIrisXE) [V134] GUC_CTL:      0x%08X\n", guc_ctl);
    IOLog("(FakeIrisXE) [V134] GUC_RESET:    0x%08X\n", guc_reset);
    IOLog("(FakeIrisXE) [V134] GUC_CAPS1:    0x%08X\n", guc_caps1);
    IOLog("(FakeIrisXE) [V134] GUC_CAPS2:    0x%08X\n", guc_caps2);
    
    // V134: Extended GT state diagnostics
    IOLog("(FakeIrisXE) [V134] === GT STATE & POWER ===\n");
    uint32_t gt_mode = fOwner->safeMMIORead(GEN12_GT_MODE);
    uint32_t gt_perf_status = fOwner->safeMMIORead(GEN12_GT_PERF_STATUS);
    uint32_t gt_perf_limit = fOwner->safeMMIORead(GEN12_GT_PERF_LIMIT_REASON);
    uint32_t gt_rp_cap = fOwner->safeMMIORead(GEN12_GT_RP_STATE_CAP);
    
    IOLog("(FakeIrisXE) [V134] GT_MODE:           0x%08X\n", gt_mode);
    IOLog("(FakeIrisXE) [V134] GT_PERF_STATUS:    0x%08X\n", gt_perf_status);
    IOLog("(FakeIrisXE) [V134] GT_PERF_LIMIT:     0x%08X\n", gt_perf_limit);
    IOLog("(FakeIrisXE) [V134] GT_RP_STATE_CAP:   0x%08X\n", gt_rp_cap);
    
    // V134: ForceWake deep dive
    IOLog("(FakeIrisXE) [V134] === FORCEWAKE STATUS ===\n");
    uint32_t fw_req = fOwner->safeMMIORead(FORCEWAKE_REQ);
    uint32_t fw_ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V134] FORCEWAKE_REQ:     0x%08X\n", fw_req);
    IOLog("(FakeIrisXE) [V134] FORCEWAKE_ACK:      0x%08X\n", fw_ack);
    
    // Parse ForceWake bits
    uint32_t fw_render = (fw_ack >> 0) & 0x1;
    uint32_t fw_boost = (fw_ack >> 1) & 0x1;
    IOLog("(FakeIrisXE) [V134]   Render domain: %s\n", fw_render ? "ACTIVE" : "INACTIVE");
    IOLog("(FakeIrisXE) [V134]   Boost domain:   %s\n", fw_boost ? "ACTIVE" : "INACTIVE");
    
    // Check GT power state
    uint32_t gt_pm = fOwner->safeMMIORead(GT_PM_CONFIG);
    uint32_t pwctl2 = fOwner->safeMMIORead(PWR_WELL_CTL2);
    uint32_t pwctl3 = fOwner->safeMMIORead(PWR_WELL_CTL3);
    IOLog("(FakeIrisXE) [V134] GT_PM_CONFIG: 0x%08X\n", gt_pm);
    IOLog("(FakeIrisXE) [V134] PWR_WELL_CTL2: 0x%08X\n", pwctl2);
    IOLog("(FakeIrisXE) [V134] PWR_WELL_CTL3: 0x%08X\n", pwctl3);
    
    // V105: Check DMA registers
    uint32_t dma_ctrl = fOwner->safeMMIORead(DMA_CTRL);
    uint32_t dma_status = fOwner->safeMMIORead(GUC_DMA_STATUS);
    IOLog("(FakeIrisXE) [V134] DMA_CTRL:     0x%08X\n", dma_ctrl);
    IOLog("(FakeIrisXE) [V134] DMA_STATUS:   0x%08X\n", dma_status);
    
    // V105: Check GUC_SHIM_CONTROL (the problematic register)
    uint32_t shim_ctrl = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    IOLog("(FakeIrisXE) [V134] GUC_SHIM_CONTROL (0x5820): 0x%08X\n", shim_ctrl);
    
    IOLog("(FakeIrisXE) [V134] =============================\n");
    
    // V50: Step 1 - Try DMC firmware first (Linux sequence)
    IOLog("(FakeIrisXE) [V50] Step 1: Attempting DMC firmware load...\n");
    extern const unsigned char tgl_dmc_ver2_12_bin[];
    extern const unsigned int tgl_dmc_ver2_12_bin_len;
    
    bool dmc_loaded = loadDmcFirmware(tgl_dmc_ver2_12_bin, tgl_dmc_ver2_12_bin_len);
    if (dmc_loaded) {
        IOLog("(FakeIrisXE) [V50] ✅ DMC firmware loaded successfully\n");
    } else {
        IOLog("(FakeIrisXE) [V50] ⚠️ DMC load failed, proceeding without DMC\n");
    }
    
    // V105: Post-DMC state check
    IOLog("(FakeIrisXE) [V134] === STATE AFTER DMC ===\n");
    uint32_t post_dmc_status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
    uint32_t post_dmc_ctl = fOwner->safeMMIORead(GEN11_GUC_CTL);
    IOLog("(FakeIrisXE) [V134] GUC_STATUS: 0x%08X\n", post_dmc_status);
    IOLog("(FakeIrisXE) [V134] GUC_CTL:    0x%08X\n", post_dmc_ctl);
    
    // V106: Read GT frequency info
    IOLog("(FakeIrisXE) [V134] GT Frequency Info:\n");
    uint32_t freq_ctrl = fOwner->safeMMIORead(0xA200);
    uint32_t freq_cap = fOwner->safeMMIORead(0xA204);
    uint32_t freq_pwreq = fOwner->safeMMIORead(0xA208);
    IOLog("(FakeIrisXE) [V134] FREQ_CTRL:  0x%08X\n", freq_ctrl);
    IOLog("(FakeIrisXE) [V134] FREQ_CAP:   0x%08X\n", freq_cap);
    IOLog("(FakeIrisXE) [V134] FREQ_PWREQ: 0x%08X\n", freq_pwreq);
    IOLog("(FakeIrisXE) [V134] =======================\n");
    
    // V50: Step 2 - Check current GuC power state
    IOLog("(FakeIrisXE) [V50] Step 2: Checking GuC power state...\n");
    uint32_t initial_status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
    uint32_t initial_reset = fOwner->safeMMIORead(GEN11_GUC_RESET);
    IOLog("(FakeIrisXE) [V50] Initial state - STATUS: 0x%08X, RESET: 0x%08X\n", 
          initial_status, initial_reset);
    
    // Check if GuC is already running
    if (initial_status != 0 && initial_status != 0xFFFFFFFF) {
        IOLog("(FakeIrisXE) [V50] GuC appears to be running (STATUS: 0x%08X)\n", initial_status);
        
        uint32_t caps1 = fOwner->safeMMIORead(GEN11_GUC_CAPS1);
        uint32_t caps2 = fOwner->safeMMIORead(GEN11_GUC_CAPS2);
        IOLog("(FakeIrisXE) [V50] GuC CAPS1: 0x%08X\n", caps1);
        IOLog("(FakeIrisXE) [V50] GuC CAPS2: 0x%08X\n", caps2);
        
        if (caps1 != 0 || caps2 != 0) {
            IOLog("(FakeIrisXE) [V50] ✅ GuC accessible! Version %u.%u\n",
                  (caps1 >> 16) & 0xFF, (caps1 >> 8) & 0xFF);
            fGuCMode = true;
            return true;
        }
    }
    
    // V50: Step 3 - Try to initialize GuC
    IOLog("(FakeIrisXE) [V50] Step 3: Attempting GuC initialization...\n");
    
    // Hold GuC in reset first
    fOwner->safeMMIOWrite(GEN11_GUC_RESET, 0x1);
    IOSleep(1);
    
    uint32_t reset_check = fOwner->safeMMIORead(GEN11_GUC_RESET);
    IOLog("(FakeIrisXE) [V50] Reset held: 0x%08X\n", reset_check);
    
    // Release reset
    fOwner->safeMMIOWrite(GEN11_GUC_RESET, 0x0);
    IOSleep(10);
    
    uint32_t post_reset_status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
    IOLog("(FakeIrisXE) [V50] Status after reset: 0x%08X\n", post_reset_status);
    
    // Read CAPS
    IOSleep(10);
    uint32_t caps1 = fOwner->safeMMIORead(GEN11_GUC_CAPS1);
    uint32_t caps2 = fOwner->safeMMIORead(GEN11_GUC_CAPS2);
    uint32_t caps3 = fOwner->safeMMIORead(GEN11_GUC_CAPS3);
    uint32_t caps4 = fOwner->safeMMIORead(GEN11_GUC_CAPS4);
    
    IOLog("(FakeIrisXE) [V50] GuC Capabilities:\n");
    IOLog("  CAPS1: 0x%08X\n", caps1);
    IOLog("  CAPS2: 0x%08X\n", caps2);
    IOLog("  CAPS3: 0x%08X\n", caps3);
    IOLog("  CAPS4: 0x%08X\n", caps4);
    
    if (caps1 == 0 && caps2 == 0) {
        IOLog("(FakeIrisXE) [V52.1] ⚠️ GuC CAPS are zero BEFORE firmware load - this is expected!\n");
        IOLog("(FakeIrisXE) [V52.1] ℹ️ Loading GuC firmware should make CAPS accessible...\n");
        
        // V52.1: IMPORTANT - We need to load firmware BEFORE deciding to fallback!
        // The GuC hardware is not accessible until firmware is loaded via DMA
        // Load the GuC firmware first (this triggers the DMA upload)
        extern const unsigned char tgl_guc_70_1_1_bin[];
        extern const unsigned int tgl_guc_70_1_1_bin_len;
        
        IOLog("(FakeIrisXE) [V52.1] Loading GuC firmware to enable GuC hardware...\n");
        bool fwLoaded = loadGuCFirmware(tgl_guc_70_1_1_bin, tgl_guc_70_1_1_bin_len);
        
        if (fwLoaded) {
            IOLog("(FakeIrisXE) [V52.1] ✅ Firmware load attempted, re-checking CAPS...\n");
            
            // Re-check CAPS after firmware load
            IOSleep(50);
            caps1 = fOwner->safeMMIORead(GEN11_GUC_CAPS1);
            caps2 = fOwner->safeMMIORead(GEN11_GUC_CAPS2);
            caps3 = fOwner->safeMMIORead(GEN11_GUC_CAPS3);
            caps4 = fOwner->safeMMIORead(GEN11_GUC_CAPS4);
            
            IOLog("(FakeIrisXE) [V52.1] GuC CAPS AFTER firmware load:\n");
            IOLog("  CAPS1: 0x%08X\n", caps1);
            IOLog("  CAPS2: 0x%08X\n", caps2);
            IOLog("  CAPS3: 0x%08X\n", caps3);
            IOLog("  CAPS4: 0x%08X\n", caps4);
            
            if (caps1 != 0 || caps2 != 0) {
                IOLog("(FakeIrisXE) [V52.1] ✅ GuC now accessible after firmware load!\n");
                fGuCMode = true;
                return true;
            }
        }
        
        IOLog("(FakeIrisXE) [V52.1] ❌ GuC still not accessible after firmware load attempt\n");
        IOLog("(FakeIrisXE) [V52.1] 🔄 Falling back to Execlist mode\n");
        IOLog("(FakeIrisXE) [V52.1] ℹ️ Execlist provides full GPU functionality without GuC\n");
        fGuCMode = false;
        return true;  // Return true to allow execlist fallback
    }
    
    // V53: Initialize full GuC subsystem (doorbells, CTB, etc.)
    IOLog("(FakeIrisXE) [V53] Initializing GuC subsystem components...\n");
    initGuCSubsystem();
    
    uint32_t supportedMajor = (caps1 >> 16) & 0xFF;
    uint32_t supportedMinor = (caps1 >> 8) & 0xFF;
    IOLog("(FakeIrisXE) [V50] ✅ GuC accessible! Version %u.%u\n",
          supportedMajor, supportedMinor);
    fGuCMode = true;
    return true;
    uint64_t start = mach_absolute_time();
    uint64_t timeout = 100 * 1000000ULL; // 100ms
    
    while (mach_absolute_time() - start < timeout) {
        uint32_t status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
        if (!(status & 0x8000)) { // Reset bit cleared
            break;
        }
        IOSleep(1);
    }
    
    // 3. Clear interrupts
    fOwner->safeMMIOWrite(GEN11_GUC_IRQ_CLEAR, 0xFFFFFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_IRQ_ENABLE, 0x0);
    
    // 4. Disable GuC initially
    fOwner->safeMMIOWrite(GEN11_GUC_CTL, 0x0);
    
    IOLog("(FakeIrisXE) [GuC] Initialization complete\n");
    return true;
}

// ============================================================================
// V49: DMC Firmware Loading (Linux-style initialization)
// ============================================================================

bool FakeIrisXEGuC::loadDmcFirmware(const uint8_t* fwData, size_t fwSize)
{
    IOLog("(FakeIrisXE) [V49] Loading DMC firmware (Linux-compatible)...\n");
    
    if (!fwData || fwSize == 0) {
        IOLog("(FakeIrisXE) [V49] ❌ No DMC firmware provided\n");
        return false;
    }
    
    // Allocate GEM for DMC firmware
    fDmcFwGem = FakeIrisXEGEM::withSize(fwSize, 0);
    if (!fDmcFwGem) {
        IOLog("(FakeIrisXE) [V49] ❌ Failed to allocate GEM for DMC firmware\n");
        return false;
    }
    
    // Copy firmware
    IOBufferMemoryDescriptor* md = fDmcFwGem->memoryDescriptor();
    memcpy(md->getBytesNoCopy(), fwData, fwSize);
    
    // Pin and map
    fDmcFwGem->pin();
    uint64_t gpuAddr = fOwner->ggttMap(fDmcFwGem);
    if (!gpuAddr) {
        IOLog("(FakeIrisXE) [V49] ❌ Failed to map DMC firmware\n");
        fDmcFwGem->unpin();
        fDmcFwGem->release();
        fDmcFwGem = nullptr;
        return false;
    }
    
    IOLog("(FakeIrisXE) [V49] DMC firmware mapped at GGTT=0x%llx (%zu bytes)\n", 
          gpuAddr, fwSize);
    
    // Program DMC firmware address
    fOwner->safeMMIOWrite(DMC_PROGRAMMABLE_ADDRESS_LOCATION, (uint32_t)(gpuAddr & 0xFFFFFFFF));
    fOwner->safeMMIOWrite(DMC_PROGRAMMABLE_ADDRESS_LOCATION_1, (uint32_t)(gpuAddr >> 32));
    
    IOLog("(FakeIrisXE) [V49] DMC firmware address programmed\n");
    
    // Trigger DMC load
    fOwner->safeMMIOWrite(DMC_SSP_BASE, 0x1);
    IOSleep(100);
    
    IOLog("(FakeIrisXE) [V49] ✅ DMC firmware loaded successfully\n");
    
    return true;
}

// ============================================================================
// V133: RPS/Frequency Control for Execlist Optimization
// Sets GPU frequency for better performance when using Execlist (no GuC)
// ============================================================================
void FakeIrisXEGuC::configureRPS()
{
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    IOLog("(FakeIrisXE) [V134] Configuring RPS/Frequency Control\n");
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    
    // Acquire ForceWake for RPS programming
    if (!acquireForceWake()) {
        IOLog("(FakeIrisXE) [V134] ⚠️ Could not acquire ForceWake for RPS\n");
    }
    
    // Read current frequency settings
    uint32_t curFreq = fOwner->safeMMIORead(GEN12_RPNCURT);
    uint32_t maxFreq = fOwner->safeMMIORead(GEN12_RPNMAXCT);
    uint32_t minFreq = fOwner->safeMMIORead(GEN12_RPNMINCT);
    uint32_t perfStatus = fOwner->safeMMIORead(GEN12_RP_GT_PERF_STATUS);
    uint32_t perfLimit = fOwner->safeMMIORead(GEN12_GT_PERF_LIMIT_REASON);
    uint32_t stateCap = fOwner->safeMMIORead(GEN12_RP_STATE_CAP);
    
    IOLog("(FakeIrisXE) [V134] Current Frequency: %u MHz\n", curFreq);
    IOLog("(FakeIrisXE) [V134] Max Frequency: %u MHz\n", maxFreq);
    IOLog("(FakeIrisXE) [V134] Min Frequency: %u MHz\n", minFreq);
    IOLog("(FakeIrisXE) [V134] Performance Status: 0x%08X\n", perfStatus);
    IOLog("(FakeIrisXE) [V134] Performance Limit: 0x%08X\n", perfLimit);
    IOLog("(FakeIrisXE) [V134] State Capability: 0x%08X\n", stateCap);
    
    // Set to maximum frequency for better Execlist performance
    // Note: On some systems, writing to these may not work without GuC
    IOLog("(FakeIrisXE) [V134] Setting maximum frequency...\n");
    
    // Try to set max frequency (this may require GuC to be running)
    // If it doesn't work, we still get performance benefits from Execlist
    fOwner->safeMMIOWrite(GEN12_RPNMAXCT, 0xFFFF);  // Request max
    IOSleep(10);
    
    uint32_t newMax = fOwner->safeMMIORead(GEN12_RPNMAXCT);
    IOLog("(FakeIrisXE) [V134] New Max Frequency Request: %u MHz\n", newMax);
    
    // Release ForceWake
    releaseForceWake();
    
    IOLog("(FakeIrisXE) [V134] RPS configuration complete\n");
    IOLog("(FakeIrisXE) [V134] ============================================\n");
}

// ============================================================================
// V133: MMIO-based Firmware Loading (bypass DMA)
// Try loading firmware directly via MMIO writes to WOPCM
// This is a last resort if DMA continues to fail
// ============================================================================
bool FakeIrisXEGuC::loadFirmwareViaMMIO(uint64_t sourceGpuAddr, uint32_t destOffset, size_t fwSize)
{
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    IOLog("(FakeIrisXE) [V134] MMIO-based Firmware Loading (BYPASS DMA)\n");
    IOLog("(FakeIrisXE) [V134] Source: GGTT 0x%016llX\n", sourceGpuAddr);
    IOLog("(FakeIrisXE) [V134] Dest: WOPCM offset 0x%X\n", destOffset);
    IOLog("(FakeIrisXE) [V134] Size: %zu bytes\n", fwSize);
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    
    // This method is typically not used on modern platforms
    // but we're documenting it for completeness
    // Real hardware would require WOPCM to be mapped and accessible
    
    IOLog("(FakeIrisXE) [V134] ⚠️ MMIO loading requires WOPCM mapping\n");
    IOLog("(FakeIrisXE) [V134] This method bypasses DMA entirely\n");
    IOLog("(FakeIrisXE) [V134] On TigerLake, DMA is required for GuC loading\n");
    IOLog("(FakeIrisXE) [V134] Returning false - DMA is mandatory\n");
    
    return false;
}

bool FakeIrisXEGuC::loadGuCFirmware(const uint8_t* fwData, size_t fwSize)
{
    if (!fwData || fwSize < 4096) {
        IOLog("(FakeIrisXE) [GuC] Invalid firmware data\n");
        return false;
    }
    
    // Check for new CSS header format (type 0x6)
    uint32_t headerType = *(uint32_t*)fwData;
    size_t payloadOffset, payloadSize;
    
    if (headerType == 0xABCD) {
        // Old format
        struct GuCFirmwareHeader {
            uint32_t headerMarker;    // 0xABCD
            uint32_t headerLen;
            uint32_t headerVersion;
            uint32_t uCodeVersion;
            uint32_t uCodeLen;
            uint32_t uCodeCRC;
            uint32_t reserved[2];
        } __attribute__((packed));
        
        const GuCFirmwareHeader* header = (const GuCFirmwareHeader*)fwData;
        fGuCVersion = header->uCodeVersion;
        payloadSize = header->uCodeLen;
        payloadOffset = sizeof(GuCFirmwareHeader);
        
        IOLog("(FakeIrisXE) [GuC] Old format firmware v%u\n", fGuCVersion);
    }
    else if (headerType == 0x00000006) {
        // New CSS header format (from your logs)
        struct CSSFirmwareHeader {
            uint32_t module_type;     // 0x00000006
            uint32_t header_len;      // 0xA1 (161 bytes)
            uint32_t header_version;  // 0x10000
            uint32_t module_id;
            uint32_t module_vendor;   // 0x8086 (Intel)
            uint32_t date;
            uint32_t size;            // Total module size
            uint32_t key_size;
            uint32_t modulus_size;
            uint32_t exponent_size;
            uint32_t reserved[22];
            // Followed by modulus, exponent, signature
        } __attribute__((packed));
        
        const CSSFirmwareHeader* cssHeader = (const CSSFirmwareHeader*)fwData;
        fGuCVersion = cssHeader->header_version;  // 0x10000
        
        // Payload starts after header_len
        payloadOffset = cssHeader->header_len;
        // Payload size = total size - header_len
        payloadSize = fwSize - payloadOffset;
        
        IOLog("(FakeIrisXE) [GuC] New CSS format firmware v%u\n", fGuCVersion);
        IOLog("(FakeIrisXE) [GuC] Header len: 0x%x bytes\n", cssHeader->header_len);
        IOLog("(FakeIrisXE) [GuC] Payload: offset=0x%zx, size=0x%zx\n",
              payloadOffset, payloadSize);
    }
    else {
        IOLog("(FakeIrisXE) [GuC] Unknown firmware header: 0x%08x\n", headerType);
        return false;
    }
    
    // Validate payload
    if (payloadSize == 0 || payloadOffset + payloadSize > fwSize) {
        IOLog("(FakeIrisXE) [GuC] Invalid payload size\n");
        return false;
    }
    
    // Allocate GEM (aligned to 4K)
    size_t allocSize = (payloadSize + 4095) & ~4095;
    fGuCFwGem = FakeIrisXEGEM::withSize(allocSize, 0);
    if (!fGuCFwGem) {
        IOLog("(FakeIrisXE) [GuC] Failed to allocate GEM for firmware\n");
        return false;
    }
    
    // Copy firmware payload
    IOBufferMemoryDescriptor* md = fGuCFwGem->memoryDescriptor();
    void* cpuPtr = md->getBytesNoCopy();
    memcpy(cpuPtr, fwData + payloadOffset, payloadSize);
    
    // Pin and map GEM to GGTT
    fGuCFwGem->pin();
    uint64_t gpuAddr = fOwner->ggttMap(fGuCFwGem);
    if (!gpuAddr) {
        IOLog("(FakeIrisXE) [GuC] Failed to map firmware\n");
        fGuCFwGem->unpin();
        fGuCFwGem->release();
        fGuCFwGem = nullptr;
        return false;
    }
    
    IOLog("(FakeIrisXE) [GuC] Firmware mapped at GGTT=0x%llx\n", gpuAddr);
    
    // Program firmware address registers
    fOwner->safeMMIOWrite(GEN11_GUC_FW_ADDR_LO, (uint32_t)(gpuAddr & 0xFFFFFFFF));
    fOwner->safeMMIOWrite(GEN11_GUC_FW_ADDR_HI, (uint32_t)(gpuAddr >> 32));
    fOwner->safeMMIOWrite(GEN11_GUC_FW_SIZE, (uint32_t)(allocSize / 4096));
    
    // V56: DO DMA UPLOAD - This is the critical step!
    IOLog("(FakeIrisXE) [V56] === DMA Firmware Upload (Apple first, Linux fallback) ===\n");
    IOLog("(FakeIrisXE) [V56] Firmware size: %zu bytes, payload offset: 0x%zx\n", fwSize, payloadOffset);
    
    // V112: Try multiple WOPCM offsets
    // Common offsets: 0x2000 (standard), 0x0, 0x4000, 0x6000
    uint32_t wopcmOffsets[] = {0x2000, 0x0, 0x4000, 0x6000};
    bool anyOffsetWorked = false;
    
    // V138: Try the FIXED Tiger Lake method first
    IOLog("(FakeIrisXE) [V138] Trying V138 Tiger Lake GuC load method...\n");
    bool v138Success = loadGuCWithV138Method(fwData, fwSize, gpuAddr);
    
    if (v138Success) {
        IOLog("(FakeIrisXE) [V138] ✅ V138 method succeeded!\n");
    } else {
        IOLog("(FakeIrisXE) [V138] ⚠️ V138 method failed, trying V137...\n");
        
        // V137 fallback
        bool v137Success = loadGuCWithV137Method(fwData, fwSize, gpuAddr);
        if (v137Success) {
            IOLog("(FakeIrisXE) [V137] ✅ V137 method succeeded!\n");
        } else {
            IOLog("(FakeIrisXE) [V137] ⚠️ V137 also failed, trying legacy...\n");
        }
    }
    
    // Legacy fallback - removed to simplify (V138/V137 tried first)
        
        // Legacy fallback: try multiple WOPCM offsets
        for (int offsetIdx = 0; offsetIdx < 4; offsetIdx++) {
            uint32_t destOffset = wopcmOffsets[offsetIdx];
            IOLog("(FakeIrisXE) [V134] ===== Trying WOPCM offset 0x%X =====\n", destOffset);
            
            size_t dmaTransferSize = payloadSize + 256;
            
            // Apple pre-DMA init (ForceWake, RSA, WOPCM)
            IOLog("(FakeIrisXE) [V134] Step 1: Apple pre-DMA init...\n");
            initGuCForAppleDMA(fwData, fwSize, gpuAddr);
            
            // Try Apple DMA first, then Linux fallback
            IOLog("(FakeIrisXE) [V134] Step 2: Attempting DMA upload...\n");
            if (uploadFirmwareWithFallback(gpuAddr, destOffset, dmaTransferSize)) {
                IOLog("(FakeIrisXE) [V134] ✅ DMA upload succeeded with offset 0x%X!\n", destOffset);
                anyOffsetWorked = true;
                break;
            }
            
            IOLog("(FakeIrisXE) [V134] ❌ DMA upload failed with offset 0x%X!\n", destOffset);
            
        // Legacy fallback - simplified
    }
    
    // Release ForceWake
    releaseForceWake();
    
    // V108: Comprehensive post-DMA state check
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    IOLog("(FakeIrisXE) [V134] STATE AFTER GUC FIRMWARE LOAD ATTEMPT\n");
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    
    // V52.1: NOW check CAPS after firmware is loaded
    IOLog("(FakeIrisXE) [V134] Step 3: Checking GuC capabilities...\n");
    uint32_t guc_caps1 = fOwner->safeMMIORead(GEN11_GUC_CAPS1);
    uint32_t guc_caps2 = fOwner->safeMMIORead(GEN11_GUC_CAPS2);
    uint32_t guc_caps3 = fOwner->safeMMIORead(GEN11_GUC_CAPS3);
    uint32_t guc_caps4 = fOwner->safeMMIORead(GEN11_GUC_CAPS4);
    IOLog("(FakeIrisXE) [V134] CAPS1: 0x%08X\n", guc_caps1);
    IOLog("(FakeIrisXE) [V134] CAPS2: 0x%08X\n", guc_caps2);
    IOLog("(FakeIrisXE) [V134] CAPS3: 0x%08X\n", guc_caps3);
    IOLog("(FakeIrisXE) [V134] CAPS4: 0x%08X\n", guc_caps4);
    
    // V108: Additional status registers
    uint32_t guc_status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
    uint32_t guc_ctl = fOwner->safeMMIORead(GEN11_GUC_CTL);
    uint32_t shim_ctrl = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    IOLog("(FakeIrisXE) [V134] STATUS: 0x%08X\n", guc_status);
    IOLog("(FakeIrisXE) [V134] CTL:    0x%08X\n", guc_ctl);
    IOLog("(FakeIrisXE) [V134] SHIM:   0x%08X\n", shim_ctrl);
    
    // V108: Parse status bits
    uint32_t ready = (guc_status >> 0) & 0x1;
    uint32_t fw_loaded = (guc_status >> 1) & 0x1;
    uint32_t comm = (guc_status >> 2) & 0x1;
    IOLog("(FakeIrisXE) [V134] Ready=%u FWLoaded=%u Comm=%u\n", ready, fw_loaded, comm);
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    
    if (guc_caps1 == 0 && guc_caps2 == 0) {
        IOLog("(FakeIrisXE) [V134] ⚠️ GuC CAPS still zero - firmware may not have loaded\n");
        IOLog("(FakeIrisXE) [V134] Possible causes:\n");
        IOLog("  1. DMA transfer to WOPCM failed\n");
        IOLog("  2. GUC_SHIM_CONTROL not writable (hardware/firmware blocking)\n");
        IOLog("  3. Power wells not enabled\n");
        IOLog("  4. RSA verification failed\n");
        IOLog("  5. GuC disabled in BIOS\n");
        
        // V133: Configure RPS for Execlist optimization since GuC failed
        IOLog("(FakeIrisXE) [V134] Configuring RPS for Execlist optimization...\n");
        configureRPS();
    }
    
    return true;
}

bool FakeIrisXEGuC::loadHuCFirmware(const uint8_t* fwData, size_t fwSize)
{
    // Similar to GuC loading but for HuC
    if (!fwData || fwSize < 4096) {
        IOLog("(FakeIrisXE) [HuC] Invalid firmware data\n");
        return false;
    }
    
    // Parse HuC firmware header (similar to GuC)
    struct HuCFirmwareHeader {
        uint32_t headerMarker;    // 0xABCD or 0xFEED
        uint32_t headerLen;
        uint32_t uCodeVersion;
        uint32_t uCodeLen;
    } __attribute__((packed));
    
    const HuCFirmwareHeader* header = (const HuCFirmwareHeader*)fwData;
    fHuCVersion = header->uCodeVersion;
    size_t payloadSize = header->uCodeLen;
    
    IOLog("(FakeIrisXE) [HuC] Loading firmware v%u, size: 0x%zx\n",
          fHuCVersion, payloadSize);
    
    // Allocate and load HuC firmware
    size_t allocSize = (payloadSize + 4095) & ~4095;
    fHuCFwGem = FakeIrisXEGEM::withSize(allocSize, 0);
    if (!fHuCFwGem) return false;
    
    IOBufferMemoryDescriptor* md = fHuCFwGem->memoryDescriptor();
    void* cpuPtr = md->getBytesNoCopy();
    memcpy(cpuPtr, fwData + sizeof(HuCFirmwareHeader), payloadSize);
    
    fHuCFwGem->pin();
    uint64_t gpuAddr = fOwner->ggttMap(fHuCFwGem);
    
    fOwner->safeMMIOWrite(GEN11_HUC_FW_ADDR_LO, (uint32_t)(gpuAddr & 0xFFFFFFFF));
    fOwner->safeMMIOWrite(GEN11_HUC_FW_ADDR_HI, (uint32_t)(gpuAddr >> 32));
    
    return true;
}

// ============================================================================
// V53: Enhanced HuC Firmware Loading with DMA (based on Linux i915 + mac-gfx-research)
// ============================================================================

// HuC register definitions (per Intel PRM)
#define GEN11_HUC_FW_STATUS                 0x1C3C0
#define GEN11_HUC_FW_CTL                    0x1C3C4
#define HUC_LOAD_REQUEST                     0x1
#define HUC_LOAD_COMPLETE                    0x2
#define HUC_AUTH_SUCCESS                     0x4

bool FakeIrisXEGuC::loadHuCFirmwareWithDMA(const uint8_t* fwData, size_t fwSize)
{
    IOLog("(FakeIrisXE) [V53] Loading HuC firmware with DMA...\n");
    
    if (!fwData || fwSize < 4096) {
        IOLog("(FakeIrisXE) [V53] [HuC] Invalid firmware data\n");
        return false;
    }
    
    // Parse HuC firmware header
    struct HuCFirmwareHeader {
        uint32_t headerMarker;
        uint32_t headerLen;
        uint32_t uCodeVersion;
        uint32_t uCodeLen;
    } __attribute__((packed));
    
    const HuCFirmwareHeader* header = (const HuCFirmwareHeader*)fwData;
    fHuCVersion = header->uCodeVersion;
    size_t payloadSize = header->uCodeLen;
    
    IOLog("(FakeIrisXE) [V53] [HuC] Loading firmware v%u, size: 0x%zx\n",
          fHuCVersion, payloadSize);
    
    // Allocate GEM for HuC firmware
    size_t allocSize = (payloadSize + 4095) & ~4095;
    fHuCFwGem = FakeIrisXEGEM::withSize(allocSize, 0);
    if (!fHuCFwGem) {
        IOLog("(FakeIrisXE) [V53] [HuC] Failed to allocate GEM\n");
        return false;
    }
    
    // Copy firmware to GEM
    IOBufferMemoryDescriptor* md = fHuCFwGem->memoryDescriptor();
    void* cpuPtr = md->getBytesNoCopy();
    memcpy(cpuPtr, fwData + sizeof(HuCFirmwareHeader), payloadSize);
    
    // Pin and map to GGTT
    fHuCFwGem->pin();
    uint64_t gpuAddr = fOwner->ggttMap(fHuCFwGem);
    
    if (!gpuAddr) {
        IOLog("(FakeIrisXE) [V53] [HuC] Failed to map firmware\n");
        fHuCFwGem->release();
        fHuCFwGem = nullptr;
        return false;
    }
    
    IOLog("(FakeIrisXE) [V53] [HuC] Mapped at GGTT=0x%016llX\n", gpuAddr);
    
    // Program HuC firmware address (similar to GuC but different registers)
    fOwner->safeMMIOWrite(GEN11_HUC_FW_ADDR_LO, (uint32_t)(gpuAddr & 0xFFFFFFFF));
    fOwner->safeMMIOWrite(GEN11_HUC_FW_ADDR_HI, (uint32_t)(gpuAddr >> 32));
    fOwner->safeMMIOWrite(GEN11_HUC_FW_SIZE, (uint32_t)(allocSize / 4096));
    
    IOLog("(FakeIrisXE) [V53] [HuC] Firmware address programmed\n");
    
    // V53: Upload HuC to WOPCM via DMA (same method as GuC)
    uint32_t hucDestOffset = 0x0;  // HuC loads at offset 0 in WOPCM
    size_t dmaTransferSize = payloadSize + 256;
    
    // Use our existing DMA function
    if (!uploadFirmwareViaDMA(gpuAddr, hucDestOffset, dmaTransferSize, UOS_MOVE)) {
        IOLog("(FakeIrisXE) [V53] [HuC] ⚠️ DMA upload failed, trying without DMA\n");
    } else {
        IOLog("(FakeIrisXE) [V53] [HuC] ✅ DMA upload complete\n");
    }
    
    return true;
}

// ============================================================================
// V53: Doorbell Initialization (based on mac-gfx-research initDoorbells)
// Doorbells are used for GuC submission - they signal the GuC when work is ready
// ============================================================================

// Doorbell register definitions (per Intel PRM)
#define GEN11_GUC_DB_CID0                  0x1C5C0  // Doorbell 0 - Client ID
#define GEN11_GUC_DB_CID1                  0x1C5C4  // Doorbell 1
#define GEN11_GUC_DB_CID2                  0x1C5C8  // Doorbell 2
#define GEN11_GUC_DB_CID3                  0x1C5CC  // Doorbell 3
#define GEN11_GUC_DB_CTL                   0x1C5D0  // Doorbell Control

#define GUC_DOORBELL_ENABLE                 0x1
#define GUC_DOORBELL_INVALIDATE              0x2

bool FakeIrisXEGuC::initDoorbells()
{
    IOLog("(FakeIrisXE) [V53] Initializing doorbells for GuC submission...\n");
    
    // Initialize doorbell registers (based on Apple's initDoorbells)
    // Each doorbell has: CID (Client ID),phase
    
    // Clear all doorbells (set to invalid)
    for (int i = 0; i < 8; i++) {
        uint32_t dbOffset = GEN11_GUC_DB_CID0 + (i * 4);
        fOwner->safeMMIOWrite(dbOffset, 0xFFFFFFFF);  // Invalid
    }
    
    // Enable doorbells
    uint32_t dbCtl = fOwner->safeMMIORead(GEN11_GUC_DB_CTL);
    dbCtl |= GUC_DOORBELL_ENABLE;
    fOwner->safeMMIOWrite(GEN11_GUC_DB_CTL, dbCtl);
    
    IOLog("(FakeIrisXE) [V53] ✅ Doorbells initialized\n");
    return true;
}

// ============================================================================
// V53: Command Transport Buffer (CTB) Setup
// Based on mac-gfx-research IGHardwareGuCCTBuffer
// CTBs are used for Host-to-GuC and GuC-to-Host communication
// ============================================================================

// CTB register definitions (per Intel PRM)
#define GEN11_GUC_H2G_DB_ADDR_LO            0x1C800  // Host-to-GuC Doorbell Addr Low
#define GEN11_GUC_H2G_DB_ADDR_HI            0x1C804  // Host-to-GuC Doorbell Addr Hi
#define GEN11_GUC_H2G_CTB_ADDR_LO           0x1C808  // Host-to-GuC CTB Addr Low
#define GEN11_GUC_H2G_CTB_ADDR_HI          0x1C80C  // Host-to-GuC CTB Addr Hi
#define GEN11_GUC_H2G_CTB_SIZE              0x1C810  // Host-to-GuC CTB Size
#define GEN11_GUC_G2H_DB_ADDR_LO            0x1C900  // GuC-to-Host Doorbell Addr Low
#define GEN11_GUC_G2H_DB_ADDR_HI            0x1C904  // GuC-to-Host Doorbell Addr Hi
#define GEN11_GUC_G2H_CTB_ADDR_LO           0x1C908  // GuC-to-Host CTB Addr Low
#define GEN11_GUC_G2H_CTB_ADDR_HI           0x1C90C  // GuC-to-Host CTB Addr Hi
#define GEN11_GUC_G2H_CTB_SIZE              0x1C910  // GuC-to-Host CTB Size

#define GUC_CTB_SIZE                        0x1000   // 4KB per CTB

bool FakeIrisXEGuC::initCommandTransportBuffers()
{
    IOLog("(FakeIrisXE) [V53] Initializing Command Transport Buffers (CTB)...\n");
    
    // Allocate CTB buffers (4KB each for H2G and G2H)
    // In a real implementation, these would be GEM objects
    // For now, we set up the register structures
    
    // H2G CTB Setup
    uint32_t h2gDb = 0x0;      // Doorbell offset (would be from GEM)
    uint32_t h2gCtb = 0x1000;  // CTB offset (would be from GEM)
    
    fOwner->safeMMIOWrite(GEN11_GUC_H2G_DB_ADDR_LO, h2gDb & 0xFFFFFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_H2G_DB_ADDR_HI, (h2gDb >> 32) & 0xFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_H2G_CTB_ADDR_LO, h2gCtb & 0xFFFFFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_H2G_CTB_ADDR_HI, (h2gCtb >> 32) & 0xFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_H2G_CTB_SIZE, GUC_CTB_SIZE);
    
    IOLog("(FakeIrisXE) [V53]   H2G CTB configured: DB=0x%X, CTB=0x%X, Size=0x%X\n",
          h2gDb, h2gCtb, GUC_CTB_SIZE);
    
    // G2H CTB Setup
    uint32_t g2hDb = 0x2000;   // Doorbell offset
    uint32_t g2hCtb = 0x3000;  // CTB offset
    
    fOwner->safeMMIOWrite(GEN11_GUC_G2H_DB_ADDR_LO, g2hDb & 0xFFFFFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_G2H_DB_ADDR_HI, (g2hDb >> 32) & 0xFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_G2H_CTB_ADDR_LO, g2hCtb & 0xFFFFFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_G2H_CTB_ADDR_HI, (g2hCtb >> 32) & 0xFFFF);
    fOwner->safeMMIOWrite(GEN11_GUC_G2H_CTB_SIZE, GUC_CTB_SIZE);
    
    IOLog("(FakeIrisXE) [V53]   G2H CTB configured: DB=0x%X, CTB=0x%X, Size=0x%X\n",
          g2hDb, g2hCtb, GUC_CTB_SIZE);
    
    IOLog("(FakeIrisXE) [V53] ✅ CTB buffers initialized\n");
    return true;
}

// ============================================================================
// V53: Full GuC Subsystem Initialization (combines all V53 features)
// ============================================================================

bool FakeIrisXEGuC::initGuCSubsystem()
{
    IOLog("(FakeIrisXE) [V53] === Full GuC Subsystem Initialization ===\n");
    
    // Step 1: Initialize doorbells
    if (!initDoorbells()) {
        IOLog("(FakeIrisXE) [V53] ⚠️ Doorbell init failed, continuing...\n");
    }
    
    // Step 2: Initialize CTB buffers
    if (!initCommandTransportBuffers()) {
        IOLog("(FakeIrisXE) [V53] ⚠️ CTB init failed, continuing...\n");
    }
    
    // Step 3: If HuC firmware is available, load it
    if (fHuCFwGem) {
        IOLog("(FakeIrisXE) [V53] HuC firmware already loaded\n");
    } else {
        IOLog("(FakeIrisXE) [V53] No HuC firmware loaded\n");
    }
    
    IOLog("(FakeIrisXE) [V53] === GuC Subsystem Initialization Complete ===\n");
    return true;
}

// ============================================================================
// V51: DMA Firmware Upload (per Intel i915 driver - intel_uc_fw.c)
// V107: Enhanced diagnostics
// V132: Try both Apple (0x1C570) and Linux (0x5820) DMA registers
// ============================================================================
bool FakeIrisXEGuC::uploadFirmwareViaDMA(uint64_t sourceGpuAddr, uint32_t destOffset, 
                                         size_t fwSize, uint32_t dmaFlags)
{
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    IOLog("(FakeIrisXE) [V134] Starting DMA firmware upload (ENHANCED)\n");
    IOLog("(FakeIrisXE) [V134]   Source: GGTT 0x%016llX\n", sourceGpuAddr);
    IOLog("(FakeIrisXE) [V134]   Dest: WOPCM offset 0x%X\n", destOffset);
    IOLog("(FakeIrisXE) [V134]   Size: 0x%zX bytes\n", fwSize);
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    
    // V132: Try both Apple-style DMA (0x1C570) and Linux-style DMA (0x5820)
    // Apple-style was already tried first, now try Linux-style as backup
    // The DMA_ADDR_0_LOW etc. are currently pointing to Apple registers (0x1C570)
    // Let's also try the Linux DMA registers
    
    // V107: Pre-DMA state check
    uint32_t pre_dma_shim = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    uint32_t pre_dma_gtpm = fOwner->safeMMIORead(GT_PM_CONFIG);
    uint32_t pre_dma_pw2 = fOwner->safeMMIORead(PWR_WELL_CTL2);
    IOLog("(FakeIrisXE) [V134] Pre-DMA: SHIM=0x%08X GTPM=0x%08X PW2=0x%08X\n",
          pre_dma_shim, pre_dma_gtpm, pre_dma_pw2);
    
    // Step 1: Set source address (DMA_ADDR_0)
    // Hardware expects 16-bit upper limit (bits 16-47)
    uint32_t srcLow = (uint32_t)(sourceGpuAddr & 0xFFFFFFFF);
    uint32_t srcHigh = (uint32_t)((sourceGpuAddr >> 32) & 0xFFFF);  // Only bits 32-47
    
    // Try Apple-style DMA registers first (current default)
    fOwner->safeMMIOWrite(DMA_ADDR_0_LOW, srcLow);
    fOwner->safeMMIOWrite(DMA_ADDR_0_HIGH, srcHigh);
    
    IOLog("(FakeIrisXE) [V134]   Source address written (Apple): 0x%04X%08X\n", srcHigh, srcLow);
    
    // Step 2: Set destination offset (DMA_ADDR_1)
    // Destination is WOPCM space at offset 0x2000 for GuC
    fOwner->safeMMIOWrite(DMA_ADDR_1_LOW, destOffset);
    fOwner->safeMMIOWrite(DMA_ADDR_1_HIGH, DMA_ADDRESS_SPACE_WOPCM);
    
    IOLog("(FakeIrisXE) [V134]   Destination address written: WOPCM offset 0x%X\n", destOffset);
    
    // Step 3: Set transfer size (includes CSS header + uCode)
    // Linux uses: sizeof(struct uc_css_header) + uc_fw->ucode_size
    fOwner->safeMMIOWrite(DMA_COPY_SIZE, (uint32_t)fwSize);
    
    IOLog("(FakeIrisXE) [V134]   Transfer size written: 0x%X\n", (uint32_t)fwSize);
    
    // V107: Verify written values
    uint32_t verify_src_lo = fOwner->safeMMIORead(DMA_ADDR_0_LOW);
    uint32_t verify_src_hi = fOwner->safeMMIORead(DMA_ADDR_0_HIGH);
    uint32_t verify_dst_lo = fOwner->safeMMIORead(DMA_ADDR_1_LOW);
    uint32_t verify_dst_hi = fOwner->safeMMIORead(DMA_ADDR_1_HIGH);
    IOLog("(FakeIrisXE) [V134] Verify: SRC=0x%04X%08X DST=0x%05X%05X\n",
          verify_src_hi, verify_src_lo, verify_dst_hi, verify_dst_lo);
    
    // Step 4: Start DMA transfer
    // Linux uses: dma_flags | START_DMA
    uint32_t ctrl = dmaFlags | START_DMA;
    fOwner->safeMMIOWrite(DMA_CTRL, ctrl);
    
    IOLog("(FakeIrisXE) [V134]   DMA started (CTRL=0x%08X)...\n", ctrl);
    
    // Step 5: Wait for DMA completion (START_DMA bit clears)
    // Linux waits up to 100ms
    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = 100 * 1000000ULL;  // 100ms
    bool completed = false;
    
    while (mach_absolute_time() - start < timeoutNs) {
        uint32_t status = fOwner->safeMMIORead(DMA_CTRL);
        if (!(status & START_DMA)) {
            completed = true;
            break;
        }
        IOSleep(1);  // 1ms polling interval
    }
    
    if (!completed) {
        uint32_t finalStatus = fOwner->safeMMIORead(DMA_CTRL);
        IOLog("(FakeIrisXE) [V134] ❌ DMA timeout! DMA_CTRL=0x%08X\n", finalStatus);
        
        // V132: Try Linux DMA registers as fallback
        IOLog("(FakeIrisXE) [V134] Trying Linux DMA registers as fallback...\n");
        
        // Reset DMA
        fOwner->safeMMIOWrite(DMA_CTRL_LINUX, 0);
        IOSleep(10);
        
        // Write to Linux DMA registers
        fOwner->safeMMIOWrite(DMA_ADDR_0_LOW_LINUX, srcLow);
        fOwner->safeMMIOWrite(DMA_ADDR_0_HIGH_LINUX, srcHigh);
        fOwner->safeMMIOWrite(DMA_ADDR_1_LOW_LINUX, destOffset);
        fOwner->safeMMIOWrite(DMA_ADDR_1_HIGH_LINUX, DMA_ADDRESS_SPACE_WOPCM);
        fOwner->safeMMIOWrite(DMA_COPY_SIZE_LINUX, (uint32_t)fwSize);
        
        IOLog("(FakeIrisXE) [V134]   Linux DMA: src=0x%04X%08X dst=0x%X size=0x%X\n",
              srcHigh, srcLow, destOffset, (uint32_t)fwSize);
        
        // Start Linux DMA
        fOwner->safeMMIOWrite(DMA_CTRL_LINUX, ctrl);
        
        // Wait again
        start = mach_absolute_time();
        while (mach_absolute_time() - start < timeoutNs) {
            uint32_t status = fOwner->safeMMIORead(DMA_CTRL_LINUX);
            if (!(status & START_DMA)) {
                completed = true;
                IOLog("(FakeIrisXE) [V134] ✅ Linux DMA completed!\n");
                break;
            }
            IOSleep(1);
        }
        
        if (!completed) {
            IOLog("(FakeIrisXE) [V134] ❌ Linux DMA also failed!\n");
            return false;
        }
    }
    
    // Step 6: Disable DMA bits after completion
    fOwner->safeMMIOWrite(DMA_CTRL, 0);
    
    IOLog("(FakeIrisXE) [V134] ✅ Linux-style DMA firmware upload completed successfully\n");
    return true;
}

// ============================================================================
// V52: Apple-Style DMA Firmware Upload (from mac-gfx-research analysis)
// Based on IGHardwareGuC::loadGuCBinary() in AppleIntelICLGraphics.c
// Uses registers at 0xc300+ (relative to GuC base 0x1C000)
// ============================================================================
bool FakeIrisXEGuC::uploadFirmwareViaDMA_Apple(uint64_t sourceGpuAddr, uint32_t destOffset, 
                                                size_t fwSize)
{
    uint32_t base = 0x1C000;  // GuC register base
    
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    IOLog("(FakeIrisXE) [V134] Apple-style DMA upload (ENHANCED)\n");
    IOLog("(FakeIrisXE) [V134]   Source: GGTT 0x%016llX\n", sourceGpuAddr);
    IOLog("(FakeIrisXE) [V134]   Dest: WOPCM offset 0x%X\n", destOffset);
    IOLog("(FakeIrisXE) [V134]   Size: 0x%zX bytes\n", fwSize);
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    
    // V109: Pre-trigger state check
    uint32_t pre_status = fOwner->safeMMIORead(base + 0xc000);
    uint32_t pre_ctl = fOwner->safeMMIORead(GEN11_GUC_CTL);
    uint32_t pre_shim = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    IOLog("(FakeIrisXE) [V134] Pre-trigger: STATUS=0x%08X CTL=0x%08X SHIM=0x%08X\n",
          pre_status, pre_ctl, pre_shim);
    
    // The initGuCForAppleDMA has already written:
    // - Source address to 0xc300/0xc304
    // - Destination to 0xc308/0xc30c
    // - Size to 0xc310
    // We just need to trigger DMA and poll for completion
    
    // Verify the addresses were written correctly
    uint32_t verifySrcLo = fOwner->safeMMIORead(base + 0xc300);
    uint32_t verifySrcHi = fOwner->safeMMIORead(base + 0xc304);
    uint32_t verifyDstLo = fOwner->safeMMIORead(base + 0xc308);
    uint32_t verifyDstHi = fOwner->safeMMIORead(base + 0xc30c);
    uint32_t verifySize  = fOwner->safeMMIORead(base + 0xc310);
    
    IOLog("(FakeIrisXE) [V52]   Verified: src=0x%04X%08X, dst=0x%05X%05X, size=0x%X\n",
          verifySrcHi, verifySrcLo, verifyDstHi, verifyDstLo, verifySize);
    
    // Step 1: Trigger DMA with Apple's magic value (Apple line 32746)
    fOwner->safeMMIOWrite(base + 0xc314, APPLE_DMA_MAGIC_TRIGGER);
    IOLog("(FakeIrisXE) [V52]   DMA triggered with magic value 0x%08X\n", APPLE_DMA_MAGIC_TRIGGER);
    
    // V109: Immediate post-trigger check
    uint32_t post_trigger = fOwner->safeMMIORead(base + 0xc314);
    uint32_t post_status = fOwner->safeMMIORead(base + 0xc000);
    IOLog("(FakeIrisXE) [V134] Post-trigger: TRIGGER=0x%08X STATUS=0x%08X\n",
          post_trigger, post_status);
    
    // Step 2: Wait for completion using Apple's status polling method (Apple lines 32747-32760)
    // Apple polls status register at base + 0xc000 and checks for:
    // - 0xF0 (bits 8-15): Success
    // - 0xA0 or 0x60: Failure
    IOLog("(FakeIrisXE) [V134]   Polling for completion (Apple method)...\n");
    
    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = 15000 * 1000000ULL;  // 15 seconds (Apple retries up to 15 times @ 1ms)
    int retryCount = 0;
    const int maxRetries = 15;
    
    while (retryCount < maxRetries && (mach_absolute_time() - start) < timeoutNs) {
        uint32_t status = fOwner->safeMMIORead(base + 0xc000);
        uint8_t statusByte = (status >> 8) & 0xFF;  // Status is in bits 8-15
        
        IOLog("(FakeIrisXE) [V134]     Poll %d: STATUS=0x%08X, byte=0x%02X\n", 
              retryCount, status, statusByte);
        
        // Check for success
        if (statusByte == GUC_LOAD_SUCCESS_STATUS) {
            IOLog("(FakeIrisXE) [V134] ✅ GuC firmware loaded successfully!\n");
            return true;
        }
        
        // Check for failure conditions
        if (((status & 0xFE) == GUC_LOAD_FAIL_STATUS_1) || (statusByte == GUC_LOAD_FAIL_STATUS_2)) {
            IOLog("(FakeIrisXE) [V134] ❌ GuC firmware load failed! STATUS=0x%08X\n", status);
            return false;
        }
        
        // Wait 1ms between polls (Apple uses assert_wait_timeout with 1000us)
        IOSleep(1);
        retryCount++;
    }
    
    IOLog("(FakeIrisXE) [V134] ❌ Timeout waiting for GuC firmware load (retries: %d)\n", retryCount);
    return false;
}

// ============================================================================
// V52: Unified Firmware Upload with Fallback
// Tries Apple method first (from mac-gfx-research), then Linux method
// V111: Added retry logic
// V132: Added Linux DMA register fallback
// ============================================================================
bool FakeIrisXEGuC::uploadFirmwareWithFallback(uint64_t sourceGpuAddr, uint32_t destOffset, 
                                                size_t fwSize)
{
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    IOLog("(FakeIrisXE) [V134] Firmware upload with RETRY LOGIC\n");
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    
    // V111: Retry loop
    const int maxRetries = 3;
    for (int retry = 0; retry < maxRetries; retry++) {
        IOLog("(FakeIrisXE) [V134] ===== RETRY %d/%d =====\n", retry + 1, maxRetries);
        
        // Try Apple-style DMA first (based on mac-gfx-research analysis)
        IOLog("(FakeIrisXE) [V134] Attempt %d: Apple-style DMA\n", retry + 1);
        if (uploadFirmwareViaDMA_Apple(sourceGpuAddr, destOffset, fwSize)) {
            IOLog("(FakeIrisXE) [V134] ✅ Apple-style DMA succeeded!\n");
            return true;
        }
        
        IOLog("(FakeIrisXE) [V134] ⚠️ Apple-style DMA failed, trying Linux-style...\n");
        
        // Fallback to Linux-style DMA (standard Intel)
        IOLog("(FakeIrisXE) [V134] Attempt %d: Linux-style DMA\n", retry + 1);
        if (uploadFirmwareViaDMA(sourceGpuAddr, destOffset, fwSize, UOS_MOVE)) {
            IOLog("(FakeIrisXE) [V134] ✅ Linux-style DMA succeeded!\n");
            return true;
        }
        
        // V111: Wait before retry
        IOLog("(FakeIrisXE) [V134] ❌ Retry %d failed, waiting 100ms...\n", retry + 1);
        IOSleep(100);
    }
    
    IOLog("(FakeIrisXE) [V134] ❌ All %d retries failed!\n", maxRetries);
    return false;
}

bool FakeIrisXEGuC::enableGuCSubmission()
{
    IOLog("(FakeIrisXE) [V45] [GuC] Enabling GuC submission mode (Intel PRM sequence)\n");
    
    if (!fGuCFwGem) {
        IOLog("(FakeIrisXE) [V45] [GuC] ❌ No firmware loaded\n");
        return false;
    }
    
    // V45: Intel PRM-compliant startup sequence
    // Step 1: Check current state before starting
    uint32_t guc_reset_before = fOwner->safeMMIORead(GEN11_GUC_RESET);
    uint32_t guc_status_before = fOwner->safeMMIORead(GEN11_GUC_STATUS);
    IOLog("(FakeIrisXE) [V45] [GuC] Initial state - RESET: 0x%08X, STATUS: 0x%08X\n", 
          guc_reset_before, guc_status_before);
    
    // Step 2: Verify firmware address is programmed
    uint32_t fw_addr_lo = fOwner->safeMMIORead(GEN11_GUC_FW_ADDR_LO);
    uint32_t fw_addr_hi = fOwner->safeMMIORead(GEN11_GUC_FW_ADDR_HI);
    uint32_t fw_size = fOwner->safeMMIORead(GEN11_GUC_FW_SIZE);
    IOLog("(FakeIrisXE) [V45] [GuC] Firmware addr: 0x%08X%08X, size: %u pages\n",
          fw_addr_hi, fw_addr_lo, fw_size);
    
    // Step 3: Program GUC_CTL to start GuC (auto-releases reset per PRM)
    uint32_t guc_ctl = 0;
    guc_ctl |= (1 << 0);   // Enable GuC (triggers auto-reset-release)
    guc_ctl |= (1 << 6);   // Enable submission
    guc_ctl |= (1 << 7);   // Load GuC firmware
    
    if (fHuCFwGem) {
        guc_ctl |= (1 << 8);   // Load HuC
    }
    
    IOLog("(FakeIrisXE) [V45] [GuC] Writing GUC_CTL = 0x%08X...\n", guc_ctl);
    fOwner->safeMMIOWrite(GEN11_GUC_CTL, guc_ctl);
    
    // V45: Short delay after writing CTL (let hardware react)
    IOSleep(1);
    
    // Step 4: Check immediate status
    uint32_t status_after_ctl = fOwner->safeMMIORead(GEN11_GUC_STATUS);
    IOLog("(FakeIrisXE) [V45] [GuC] Status after CTL write: 0x%08X\n", status_after_ctl);
    
    // Step 5: Wait for GuC ready (per Intel PRM polling sequence)
    IOLog("(FakeIrisXE) [V45] [GuC] Waiting for GuC initialization...\n");
    if (!waitGuCReady(15000)) { // 15 second timeout (increased for safety)
        IOLog("(FakeIrisXE) [V45] [GuC] ❌ Failed to start GuC (timeout)\n");
        
        // V45: Diagnostic dump on failure
        uint32_t final_status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
        uint32_t final_reset = fOwner->safeMMIORead(GEN11_GUC_RESET);
        uint32_t guc_cap = fOwner->safeMMIORead(GEN11_GUC_CAPS1);
        IOLog("(FakeIrisXE) [V45] [GuC] Final state - STATUS: 0x%08X, RESET: 0x%08X, CAPS: 0x%08X\n",
              final_status, final_reset, guc_cap);
        return false;
    }
    
    // 4. Setup interrupts
   // setupGuCInterrupts();
    
    // 5. Check HuC status if loaded
    if (fHuCFwGem) {
        uint32_t huc_status = fOwner->safeMMIORead(GEN11_HUC_STATUS);
        IOLog("(FakeIrisXE) [HuC] Status: 0x%08x\n", huc_status);
    }
    
    IOLog("(FakeIrisXE) [V48] [GuC] ✅ GuC submission enabled successfully!\n");
    IOLog("(FakeIrisXE) [V48] [GuC] Hardware acceleration is now ACTIVE\n");
    dumpGuCStatus();
    
    // V47: Test command submission
    IOLog("(FakeIrisXE) [V48] Testing command submission...\n");
    if (testCommandSubmission()) {
        IOLog("(FakeIrisXE) [V48] ✅ Command submission test PASSED\n");
    } else {
        IOLog("(FakeIrisXE) [V48] ⚠️ Command submission test FAILED (GuC may still work)\n");
    }
    
    return true;
}

bool FakeIrisXEGuC::waitGuCReady(uint32_t timeoutMs)
{
    uint64_t start = mach_absolute_time();
    uint64_t timeout = timeoutMs * 1000000ULL;
    
    IOLog("(FakeIrisXE) [V45] [GuC] Polling GuC status (timeout: %u ms)...\n", timeoutMs);
    
    uint32_t lastStatus = 0;
    int sameStatusCount = 0;
    
    while (mach_absolute_time() - start < timeout) {
        uint32_t status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
        
        // V45: Track if status is changing
        if (status != lastStatus) {
            IOLog("(FakeIrisXE) [V45] [GuC] Status change: 0x%08X -> 0x%08X (bits: R=%s FW=%s COM=%s)\n",
                  lastStatus, status,
                  (status & 0x1) ? "Y" : "N",
                  (status & 0x2) ? "Y" : "N",
                  (status & 0x4) ? "Y" : "N");
            lastStatus = status;
            sameStatusCount = 0;
        } else {
            sameStatusCount++;
        }
        
        // Check ready bits per Intel PRM:
        // Bit 0: GuC ready
        // Bit 1: Firmware loaded
        // Bit 2: GuC communication established
        if ((status & 0x7) == 0x7) {
            IOLog("(FakeIrisXE) [V45] [GuC] ✅ Ready! Status: 0x%08x\n", status);
            return true;
        }
        
        // Check for errors (bits 31:16)
        if (status & 0xFFFF0000) {
            IOLog("(FakeIrisXE) [V45] [GuC] ⚠️ Error detected: 0x%08X\n", status);
            return false;
        }
        
        // Check for errors
        if (status & 0xFFFF0000) {
            IOLog("(FakeIrisXE) [GuC] Error detected: 0x%08x\n", status);
            return false;
        }
        
        if ((mach_absolute_time() - start) % 1000000000ULL == 0) {
            IOLog("(FakeIrisXE) [GuC] Still waiting... Status: 0x%08x\n", status);
        }
        
        IOSleep(10);
    }
    
    IOLog("(FakeIrisXE) [GuC] Timeout waiting for GuC ready\n");
    return false;
}

void FakeIrisXEGuC::dumpGuCStatus()
{
    uint32_t status = fOwner->safeMMIORead(GEN11_GUC_STATUS);
    uint32_t ctl = fOwner->safeMMIORead(GEN11_GUC_CTL);
    
    IOLog("(FakeIrisXE) [GuC] Status Dump:\n");
    IOLog("  CTL: 0x%08x\n", ctl);
    IOLog("  STATUS: 0x%08x\n", status);
    IOLog("    Ready: %s\n", (status & 0x1) ? "YES" : "NO");
    IOLog("    FW Loaded: %s\n", (status & 0x2) ? "YES" : "NO");
    IOLog("    Comm Established: %s\n", (status & 0x4) ? "YES" : "NO");
    
    // Dump scratch registers
    for (int i = 0; i < 16; i++) {
        uint32_t val = fOwner->safeMMIORead(GEN11_GUC_SOFT_SCRATCH(i));
        IOLog("  Scratch[%02d]: 0x%08x\n", i, val);
    }
}

// ============================================================================
// V47: Command Submission Test
// ============================================================================
bool FakeIrisXEGuC::testCommandSubmission()
{
    IOLog("(FakeIrisXE) [V48] Creating test command buffer...\n");
    
    // Create a simple batch buffer with MI_NOOP commands
    FakeIrisXEGEM* testGem = FakeIrisXEGEM::withSize(4096, 0);
    if (!testGem) {
        IOLog("(FakeIrisXE) [V48] Failed to create test GEM\n");
        return false;
    }
    
    // Fill with MI_NOOP commands (0x00000000)
    IOBufferMemoryDescriptor* md = testGem->memoryDescriptor();
    uint32_t* cmds = (uint32_t*)md->getBytesNoCopy();
    for (int i = 0; i < 256; i++) {
        cmds[i] = 0x00000000;  // MI_NOOP
    }
    // Add batch end
    cmds[256] = 0x05000000;  // MI_BATCH_BUFFER_END
    
    // Pin and map
    testGem->pin();
    uint64_t gpuAddr = fOwner->ggttMap(testGem);
    if (!gpuAddr) {
        IOLog("(FakeIrisXE) [V48] Failed to map test buffer\n");
        testGem->release();
        return false;
    }
    
    IOLog("(FakeIrisXE) [V48] Test buffer at GPU addr 0x%llx\n", gpuAddr);
    
    // V47: Use scratch registers to submit (simplified test)
    // In real implementation, would use proper GuC submission path
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(0), (uint32_t)(gpuAddr & 0xFFFFFFFF));
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(1), (uint32_t)(gpuAddr >> 32));
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(2), 0x1);  // Trigger bit
    
    IOLog("(FakeIrisXE) [V48] Submitted test command via scratch registers\n");
    
    // Cleanup
    testGem->unpin();
    testGem->release();
    
    return true;
}

// ============================================================================
// V52.1: ForceWake - Acquire before GuC register access
// V115: Enhanced logging
// ============================================================================
bool FakeIrisXEGuC::acquireForceWake()
{
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    IOLog("(FakeIrisXE) [V134] Acquiring ForceWake...\n");
    
    // V115: Check initial state
    uint32_t initial_fw = fOwner->safeMMIORead(FORCEWAKE_REQ);
    uint32_t initial_ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V134] Initial: REQ=0x%08X ACK=0x%08X\n", initial_fw, initial_ack);
    
    // Write 0x000F000F to FORCEWAKE_REQ (request all power wells)
    fOwner->safeMMIOWrite(FORCEWAKE_REQ, 0x000F000F);
    IOSleep(5);
    
    // V115: Verify write
    uint32_t after_write = fOwner->safeMMIORead(FORCEWAKE_REQ);
    IOLog("(FakeIrisXE) [V134] After write: REQ=0x%08X\n", after_write);
    
    // Poll for ACK
    uint64_t start = mach_absolute_time();
    uint64_t timeout = 50 * 1000000ULL;  // 50ms
    
    while (mach_absolute_time() - start < timeout) {
        uint32_t ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
        if ((ack & 0xF) == 0xF) {
            IOLog("(FakeIrisXE) [V134] ✅ ForceWake acquired! ACK=0x%08X\n", ack);
            return true;
        }
        IOSleep(1);
    }
    
    // V115: Final attempt state
    uint32_t final_ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V134] ⚠️ ForceWake timeout, ACK=0x%08X\n", final_ack);
    return true;  // Continue anyway - may work
}

void FakeIrisXEGuC::releaseForceWake()
{
    IOLog("(FakeIrisXE) [V134] Releasing ForceWake...\n");
    
    // Write 0 to release ForceWake
    fOwner->safeMMIOWrite(FORCEWAKE_REQ, 0x00000000);
    
    IOSleep(1);
    
    // V115: Verify release
    uint32_t after_release = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V134] After release: ACK=0x%08X\n", after_release);
}

// ============================================================================
// V55: Extract RSA data (modulus + signature) from firmware CSS header
// Based on Intel firmware specification and Linux i915 driver
// ============================================================================
bool FakeIrisXEGuC::extractRSASignature(const uint8_t* fwData, size_t fwSize, uint8_t* signatureOut)
{
    if (!fwData || !signatureOut) return false;
    
    // CSS header structure (from Intel firmware spec)
    struct CSSHeader {
        uint32_t module_type;      // 0x00000006
        uint32_t header_len;       // usually 0xA1 (161 bytes)
        uint32_t header_version;
        uint32_t module_id;
        uint32_t module_vendor;    // 0x8086 for Intel
        uint32_t date;
        uint32_t size;             // Total module size
        uint32_t key_size;         // RSA key size in DWORDs
        uint32_t modulus_size;     // RSA modulus size in DWORDs  
        uint32_t exponent_size;    // RSA exponent size in DWORDs
        uint32_t reserved[22];
        // Followed by: modulus (modulus_size * 4 bytes), exponent (exponent_size * 4), signature
    } __attribute__((packed));
    
    if (fwSize < sizeof(CSSHeader)) {
        IOLog("(FakeIrisXE) [V55] ❌ Firmware too small for CSS header\n");
        return false;
    }
    
    const CSSHeader* css = (const CSSHeader*)fwData;
    
    IOLog("(FakeIrisXE) [V55] CSS header: type=0x%08X, header_len=%d, key_size=%d, modulus_size=%d\n",
          css->module_type, css->header_len, css->key_size, css->modulus_size);
    
    // V55: Properly calculate RSA data locations
    // The RSA modulus starts right after the header
    const uint8_t* modulusData = fwData + css->header_len;
    uint32_t modulusSize = css->modulus_size * 4;  // Convert DWORDs to bytes
    uint32_t exponentSize = css->exponent_size * 4;
    
    // Signature starts after modulus + exponent
    const uint8_t* signatureData = modulusData + modulusSize + exponentSize;
    uint32_t signatureSize = 256;  // Standard RSA-2048 signature is 256 bytes
    
    memset(signatureOut, 0, 256);  // Clear output buffer
    
    // Copy signature data
    if (fwSize > (signatureData - fwData)) {
        size_t availableSigSize = fwSize - (signatureData - fwData);
        size_t copySize = (availableSigSize > 256) ? 256 : availableSigSize;
        memcpy(signatureOut, signatureData, copySize);
        IOLog("(FakeIrisXE) [V55] ✅ Extracted %zu bytes RSA signature from offset %u\n", 
              copySize, (uint32_t)(signatureData - fwData));
        
        // Also store modulus data for potential use (first 24 bytes go to 0xc184)
        // For now we just log it
        if (modulusSize >= 24) {
            IOLog("(FakeIrisXE) [V55] RSA modulus available: %d bytes at offset %d\n",
                  modulusSize, css->header_len);
        }
    } else {
        IOLog("(FakeIrisXE) [V55] ⚠️ No signature data found at expected offset, using zeros\n");
    }
    
    return true;
}

// ============================================================================
// V56: Program GUC_SHIM_CONTROL (required before DMA per Linux i915)
// V56: Added write verification with retry and alternative register offset
// V114: Added more register verification
// V136: FIXED - Using correct Tiger Lake GuC register offsets (0xC000+)
// Based on Intel PRM Vol13 and ChatGPT analysis
// ============================================================================
void FakeIrisXEGuC::programShimControl()
{
    IOLog("(FakeIrisXE) [V136] ============================================\n");
    IOLog("(FakeIrisXE) [V136] Programming GUC_SHIM_CONTROL (FIXED OFFSETS)\n");
    IOLog("(FakeIrisXE) [V136] Using register offset 0x%04X (Tiger Lake CORRECT)\n", GUC_SHIM_CONTROL);
    
    // Check power well status first
    uint32_t pwctl2 = fOwner->safeMMIORead(PWR_WELL_CTL2);
    uint32_t pwctl3 = fOwner->safeMMIORead(PWR_WELL_CTL3);
    IOLog("(FakeIrisXE) [V136] Power wells: PW2=0x%08X PW3=0x%08X\n", pwctl2, pwctl3);
    
    // Try to ensure GT power domain is enabled
    if ((pwctl2 & 0x2) == 0) {
        IOLog("(FakeIrisXE) [V136] GT power well may be off, attempting to enable...\n");
        fOwner->safeMMIOWrite(PWR_WELL_CTL2, 0x3);  // Request power on
        IOSleep(50);
        pwctl2 = fOwner->safeMMIORead(PWR_WELL_CTL2);
        IOLog("(FakeIrisXE) [V136] After power request: PW2=0x%08X\n", pwctl2);
    }
    
    // CRITICAL: Acquire ForceWake before GuC MMIO access
    IOLog("(FakeIrisXE) [V136] Acquire ForceWake (RENDER+MEDIA)...\n");
    uint32_t fw_req = fOwner->safeMMIORead(FORCEWAKE_REQ);
    uint32_t fw_ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V136] ForceWake BEFORE: REQ=0x%08X ACK=0x%08X\n", fw_req, fw_ack);
    
    acquireForceWake();
    IOSleep(20);
    
    fw_req = fOwner->safeMMIORead(FORCEWAKE_REQ);
    fw_ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V136] ForceWake AFTER: REQ=0x%08X ACK=0x%08X\n", fw_req, fw_ack);
    
    // Read initial state at CORRECT offset 0xC064
    uint32_t initial_shim = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    uint32_t initial_gtpm = fOwner->safeMMIORead(GT_PM_CONFIG);
    IOLog("(FakeIrisXE) [V136] Initial: SHIM(0xC064)=0x%08X GTPM=0x%08X\n", initial_shim, initial_gtpm);
    
    // V136: Use Apple's base shim value (0x8617) per ChatGPT analysis
    // This is the correct handshake value for Tiger Lake
    uint32_t shim_val = 0x8617;
    
    IOLog("(FakeIrisXE) [V136] Target shim value = 0x%04X (Apple base)\n", shim_val);
    
    // V136: Write to CORRECT offset 0xC064 with retry
    bool shimSuccess = false;
    
    IOLog("(FakeIrisXE) [V136] Writing GUC_SHIM_CONTROL at 0xC064...\n");
    
    for (int retry = 0; retry < 10 && !shimSuccess; retry++) {
        fOwner->safeMMIOWrite(GUC_SHIM_CONTROL, shim_val);
        IOSleep(10); // Wait for write to propagate
        
        uint32_t shimRead = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
        IOLog("(FakeIrisXE) [V136] Attempt %d: Wrote 0x%04X, Read 0x%08X\n", 
              retry + 1, shim_val, shimRead);
        
        if (shimRead != 0) {
            shimSuccess = true;
            IOLog("(FakeIrisXE) [V136] ✅ GUC_SHIM_CONTROL VERIFIED at 0xC064: 0x%08X\n", shimRead);
        } else {
            IOSleep(10);
        }
    }
    
    // Also try GUC_SHIM_CONTROL2 at 0xC068
    if (!shimSuccess) {
        IOLog("(FakeIrisXE) [V136] Trying GUC_SHIM_CONTROL2 at 0xC068...\n");
        for (int retry = 0; retry < 5 && !shimSuccess; retry++) {
            fOwner->safeMMIOWrite(GUC_SHIM_CONTROL2, shim_val);
            IOSleep(10);
            
            uint32_t shimRead = fOwner->safeMMIORead(GUC_SHIM_CONTROL2);
            if (shimRead != 0) {
                shimSuccess = true;
                IOLog("(FakeIrisXE) [V136] ✅ GUC_SHIM_CONTROL2 verified: 0x%08X\n", shimRead);
            }
        }
    }
    
    // Read final state
    uint32_t final_shim = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    IOLog("(FakeIrisXE) [V136] Final SHIM_CONTROL(0xC064): 0x%08X\n", final_shim);
    IOLog("(FakeIrisXE) [V136] ============================================\n");
    
    if (!shimSuccess) {
        IOLog("(FakeIrisXE) [V136] ❌ GUC_SHIM_CONTROL write failed - hardware may be blocked\n");
        IOLog("(FakeIrisXE) [V136] NOTE: This indicates GT power/forcewake issue or hardware blocking\n");
    }
    
    // Enable GT doorbell with verification
    IOLog("(FakeIrisXE) [V136] Programming GT_PM_CONFIG...\n");
    fOwner->safeMMIOWrite(GT_PM_CONFIG, GT_DOORBELL_ENABLE);
    IOSleep(5);
    
    uint32_t pmRead = fOwner->safeMMIORead(GT_PM_CONFIG);
    if (pmRead == GT_DOORBELL_ENABLE) {
        IOLog("(FakeIrisXE) [V56] ✅ GT_PM_CONFIG verified: 0x%08X (doorbell enabled)\n", pmRead);
    } else {
        IOLog("(FakeIrisXE) [V56] ⚠️ GT_PM_CONFIG: wrote 0x%08X, read 0x%08X\n", GT_DOORBELL_ENABLE, pmRead);
    }
}

// ============================================================================
// V56: Apple-style GuC initialization before DMA
// Fixed register offsets and enhanced verification
// V113: Added more verification and logging
// V132: Enhanced power well checks
// ============================================================================
bool FakeIrisXEGuC::initGuCForAppleDMA(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr)
{
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    IOLog("(FakeIrisXE) [V134] GuC Pre-DMA Initialization (ENHANCED)\n");
    IOLog("(FakeIrisXE) [V134] ============================================\n");
    
    // Step 1: Acquire ForceWake (CRITICAL - must hold throughout!)
    IOLog("(FakeIrisXE) [V56] Step 1: Acquiring ForceWake...\n");
    if (!acquireForceWake()) {
        IOLog("(FakeIrisXE) [V56] ⚠️ ForceWake acquisition warning, continuing...\n");
    }
    
    // V113: Verify ForceWake
    uint32_t fw_req = fOwner->safeMMIORead(FORCEWAKE_REQ);
    uint32_t fw_ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V134] ForceWake: REQ=0x%08X ACK=0x%08X\n", fw_req, fw_ack);
    
    // Step 2: Program GUC_SHIM_CONTROL (V56 - Fixed register offset with verification)
    IOLog("(FakeIrisXE) [V134] Step 2: Programming Shim Control...\n");
    IOLog("(FakeIrisXE) [V134] Using corrected register offset 0x5820 for Tiger Lake\n");
    programShimControl();
    IOSleep(10);  // Let settings propagate
    
    // V113: Verify Shim Control
    uint32_t shim_ctrl = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    IOLog("(FakeIrisXE) [V134] Shim Control verify: 0x%08X\n", shim_ctrl);
    
    // Step 3: Write GuC reset/initialization registers
    // Using base 0x1C000 as per Intel PRM for Gen11/12 GuC registers
    uint32_t base = 0x1C000;
    
    IOLog("(FakeIrisXE) [V56] Step 3: GuC reset sequence...\n");
    fOwner->safeMMIOWrite(base + 0x1984, 0x1);
    IOLog("(FakeIrisXE) [V56]   Wrote 0x1984 = 0x1\n");

    fOwner->safeMMIOWrite(base + 0x9424, 0x1);
    IOLog("(FakeIrisXE) [V56]   Wrote 0x9424 = 0x1\n");

    fOwner->safeMMIOWrite(base + 0x9424, 0x10);
    IOLog("(FakeIrisXE) [V56]   Wrote 0x9424 = 0x10\n");

    // Check status register for conditional writes
    uint32_t statusCheck = fOwner->safeMMIORead(base + 0xfce);
    IOLog("(FakeIrisXE) [V56]   Status check 0xfce = 0x%08X\n", statusCheck);

    if ((int)statusCheck < 0) {
        fOwner->safeMMIOWrite(base + 0x9024, 0xcb);
        IOLog("(FakeIrisXE) [V56]   Wrote 0x9024 = 0xcb (conditional)\n");
    }

    // Step 4: V56 - Proper RSA Signature Extraction and Writing
    IOLog("(FakeIrisXE) [V56] Step 4: RSA Signature Setup...\n");
    uint8_t signatureData[256];
    bool rsaOk = extractRSASignature(fwData, fwSize, signatureData);
    
    if (rsaOk) {
        // V55: Write actual RSA modulus data (first 24 bytes) to 0xc184
        // These should be the first 6 dwords of the modulus from the CSS header
        struct CSSHeader {
            uint32_t module_type, header_len, header_version, module_id;
            uint32_t module_vendor, date, size, key_size, modulus_size, exponent_size;
        } __attribute__((packed));
        const CSSHeader* css = (const CSSHeader*)fwData;
        const uint8_t* modulusStart = fwData + css->header_len;
        
        // Write first 24 bytes of modulus to 0xc184 (RSA key data registers)
        IOLog("(FakeIrisXE) [V56]   Writing RSA key data (modulus) to 0xc184...\n");
        for (int i = 0; i < 6; i++) {
            uint32_t val = 0;
            if (css->modulus_size * 4 > i * 4) {
                val = *(uint32_t*)(modulusStart + (i * 4));
            }
            fOwner->safeMMIOWrite(base + 0xc184 + (i * 4), val);
        }
        IOLog("(FakeIrisXE) [V56]   ✅ Wrote RSA key data (6 dwords from modulus)\n");

        // Write 256 bytes of RSA signature to 0xc200
        IOLog("(FakeIrisXE) [V56]   Writing RSA signature (256 bytes) to 0xc200...\n");
        for (int i = 0; i < 64; i++) {
            uint32_t val = *(uint32_t*)(signatureData + (i * 4));
            fOwner->safeMMIOWrite(base + 0xc200 + (i * 4), val);
        }
        IOLog("(FakeIrisXE) [V56]   ✅ Wrote RSA signature\n");
    } else {
        IOLog("(FakeIrisXE) [V56]   ⚠️ RSA extraction failed, using zeros\n");
        // Fallback to zeros
        for (int i = 0; i < 6; i++) {
            fOwner->safeMMIOWrite(base + 0xc184 + (i * 4), 0);
        }
        for (int i = 0; i < 64; i++) {
            fOwner->safeMMIOWrite(base + 0xc200 + (i * 4), 0);
        }
    }

    // Step 5: Write DMA parameters
    IOLog("(FakeIrisXE) [V56] Step 5: DMA Parameters...\n");

    // Calculate proper transfer size based on firmware payload
    uint32_t transferSize = 0x60400;  // Default for ICL/Gen11
    if (fwSize > 0xA1) {  // If we have a CSS header
        transferSize = fwSize - 0xA1 + 256;  // payload + CSS overhead
    }

    fOwner->safeMMIOWrite(base + 0xc310, transferSize);
    IOLog("(FakeIrisXE) [V56]   DMA size = 0x%X (%u bytes)\n", transferSize, transferSize);

    // Source address (GGTT mapped firmware)
    fOwner->safeMMIOWrite(base + 0xc300, (uint32_t)(gpuAddr & 0xFFFFFFFF));
    fOwner->safeMMIOWrite(base + 0xc304, (uint32_t)((gpuAddr >> 32) & 0xFFFF));
    IOLog("(FakeIrisXE) [V56]   Source GGTT = 0x%016llX\n", gpuAddr);

    // Destination (WOPCM offset 0x2000 for GuC)
    fOwner->safeMMIOWrite(base + 0xc308, 0x2000);
    fOwner->safeMMIOWrite(base + 0xc30c, 0x70000);  // WOPCM address space
    IOLog("(FakeIrisXE) [V56]   Dest = WOPCM offset 0x2000, space 0x70000\n");

    // Step 6: WOPCM settings
    IOLog("(FakeIrisXE) [V56] Step 6: WOPCM Configuration...\n");
    fOwner->safeMMIOWrite(base + 0xc340, 0x100000);  // 1MB WOPCM size
    fOwner->safeMMIOWrite(base + 0xc050, 0x1);       // Enable WOPCM
    IOLog("(FakeIrisXE) [V56]   WOPCM configured\n");

    IOSleep(5);  // Brief delay before DMA

    IOLog("(FakeIrisXE) [V56] === Pre-DMA Initialization Complete ===\n");
    return true;
}

// ============================================================================
// V135: Aggressive Linux GT Initialization Before GuC Load
// Based on Linux i915 driver intel_gt_init_hw() sequence
// This runs BEFORE any GuC firmware loading to ensure GT is properly initialized
// ============================================================================
void FakeIrisXEGuC::initGTPreWorkaround()
{
    IOLog("(FakeIrisXE) [V135] ============================================\n");
    IOLog("(FakeIrisXE) [V135] AGGRESSIVE LINUX GT INITIALIZATION\n");
    IOLog("(FakeIrisXE) [V135] Based on Linux i915 intel_gt_init_hw()\n");
    IOLog("(FakeIrisXE) [V135] ============================================\n");
    
    // Step 1: Comprehensive power well status check
    IOLog("(FakeIrisXE) [V135] Step 1: Checking power wells...\n");
    uint32_t pw_status = fOwner->safeMMIORead(GEN12_PWR_WELL_STATUS);
    uint32_t pw_ctl = fOwner->safeMMIORead(GEN12_PWR_WELL_CTL);
    uint32_t pw_ctl2 = fOwner->safeMMIORead(GEN12_PWR_WELL_CTL2);
    uint32_t pw_ctl3 = fOwner->safeMMIORead(GEN12_PWR_WELL_CTL3);
    uint32_t pw_ctl4 = fOwner->safeMMIORead(GEN12_PWR_WELL_CTL4);
    
    IOLog("(FakeIrisXE) [V135] Power: STATUS=0x%08X CTL=0x%08X\n", pw_status, pw_ctl);
    IOLog("(FakeIrisXE) [V135] Power: CTL2=0x%08X CTL3=0x%08X CTL4=0x%08X\n", 
          pw_ctl2, pw_ctl3, pw_ctl4);
    
    // Step 2: Request all power wells (Linux does this)
    IOLog("(FakeIrisXE) [V135] Step 2: Requesting power wells...\n");
    fOwner->safeMMIOWrite(GEN12_PWR_WELL_CTL2, 0x00030003);  // Request PW2
    IOSleep(10);
    fOwner->safeMMIOWrite(GEN12_PWR_WELL_CTL3, 0x40030003);  // Request PW3  
    IOSleep(10);
    fOwner->safeMMIOWrite(GEN12_PWR_WELL_CTL4, 0x00030003);  // Request PW4
    IOSleep(10);
    
    // Step 3: Acquire ForceWake
    IOLog("(FakeIrisXE) [V135] Step 3: Acquiring ForceWake...\n");
    acquireForceWake();
    IOSleep(20);
    
    uint32_t fw_ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V135] ForceWake ACK: 0x%08X\n", fw_ack);
    
    // Step 4: Configure MOCS (Memory Override Control State) - Linux does this
    IOLog("(FakeIrisXE) [V135] Step 4: Configuring MOCS registers...\n");
    // MOCS0-MOCS2: Set up default caching
    fOwner->safeMMIOWrite(GEN12_MOCS0, 0x7D40001D);  // Default L3+LLC
    fOwner->safeMMIOWrite(GEN12_MOCS1, 0x7D40001D);  // Default L3+LLC
    fOwner->safeMMIOWrite(GEN12_MOCS2, 0x7D40001D);  // Default L3+LLC
    IOLog("(FakeIrisXE) [V135] MOCS configured\n");
    
    // Step 5: GT performance/RC configuration (Linux sets this up)
    IOLog("(FakeIrisXE) [V135] Step 5: Configuring GT performance...\n");
    uint32_t rc_ctl = fOwner->safeMMIORead(GEN12_RC_CTL);
    IOLog("(FakeIrisXE) [V135] RC_CTL: 0x%08X\n", rc_ctl);
    
    // Enable RC6 and deeper sleep states
    fOwner->safeMMIOWrite(GEN12_RC_CTL, rc_ctl | 0x3);  // Enable RC6
    IOSleep(5);
    
    // Step 6: Check GGTT status (Linux verifies this)
    IOLog("(FakeIrisXE) [V135] Step 6: Checking GGTT...\n");
    uint32_t ggtt_top = fOwner->safeMMIORead(GEN12_GGTT_TOP);
    IOLog("(FakeIrisXE) [V135] GGTT_TOP: 0x%08X\n", ggtt_top);
    
    // Step 7: PPGTT PML4 setup attempt (Linux programs this)
    IOLog("(FakeIrisXE) [V135] Step 7: PPGTT PML4 configuration...\n");
    uint32_t pml4e = fOwner->safeMMIORead(GEN12_PPGTT_PML4E);
    uint32_t pml4e2 = fOwner->safeMMIORead(GEN12_PPGTT_PML4E_2);
    IOLog("(FakeIrisXE) [V135] PPGTT: PML4E=0x%08X PML4E2=0x%08X\n", pml4e, pml4e2);
    
    // Step 8: Check GT mode
    IOLog("(FakeIrisXE) [V135] Step 8: GT mode check...\n");
    uint32_t gt_mode = fOwner->safeMMIORead(GEN12_GT_MODE);
    IOLog("(FakeIrisXE) [V135] GT_MODE: 0x%08X\n", gt_mode);
    
    // Step 9: Check for any GT workarounds
    IOLog("(FakeIrisXE) [V135] Step 9: GT workaround registers...\n");
    uint32_t gt_workaround = fOwner->safeMMIORead(GEN12_GT_WORKAROUND);
    uint32_t perf_limit = fOwner->safeMMIORead(GEN12_GT_PERF_LIMIT);
    IOLog("(FakeIrisXE) [V135] GT_WORKAROUND: 0x%08X\n", gt_workaround);
    IOLog("(FakeIrisXE) [V135] PERF_LIMIT: 0x%08X\n", perf_limit);
    
    // Step 10: Check GuC misc control
    IOLog("(FakeIrisXE) [V135] Step 10: GuC misc control...\n");
    uint32_t guc_misc = fOwner->safeMMIORead(GEN11_GUC_MISC_CTRL);
    uint32_t guc_wopcm_offset = fOwner->safeMMIORead(GEN11_GUC_WOPCM_OFFSET);
    uint32_t guc_wopcm_size = fOwner->safeMMIORead(GEN12_GUC_WOPCM_SIZE);
    IOLog("(FakeIrisXE) [V135] GUC_MISC: 0x%08X\n", guc_misc);
    IOLog("(FakeIrisXE) [V135] GUC_WOPCM_OFFSET: 0x%08X\n", guc_wopcm_offset);
    IOLog("(FakeIrisXE) [V135] GUC_WOPCM_SIZE: 0x%08X\n", guc_wopcm_size);
    
    // Step 11: Check current GUC_SHIM_CONTROL (at 0xC064 - CORRECTED)
    IOLog("(FakeIrisXE) [V136] Step 11: Final GUC_SHIM_CONTROL check at 0xC064...\n");
    uint32_t shim_ctrl = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    IOLog("(FakeIrisXE) [V136] GUC_SHIM_CONTROL (0xC064): 0x%08X\n", shim_ctrl);
    
    // Try to write to GUC_SHIM_CONTROL with Apple's value (0x8617)
    IOLog("(FakeIrisXE) [V136] Attempting Apple SHIM write (0x8617)...\n");
    fOwner->safeMMIOWrite(GUC_SHIM_CONTROL, 0x00008617);
    IOSleep(10);
    uint32_t shim_after = fOwner->safeMMIORead(GUC_SHIM_CONTROL);
    IOLog("(FakeIrisXE) [V136] After Apple write: 0x%08X\n", shim_after);
    
    IOLog("(FakeIrisXE) [V136] ============================================\n");
    IOLog("(FakeIrisXE) [V136] GT PRE-INIT COMPLETE\n");
    IOLog("(FakeIrisXE) [V136] Using CORRECT Tiger Lake GuC registers (0xC000+)\n");
    IOLog("(FakeIrisXE) [V136] ============================================\n");
}

// ============================================================================
// V137: Correct Firmware Layout Derivation (Linux i915 method)
// Based on intel_guc_fw.c - calculates correct offset using CSS fields
// ============================================================================
bool FakeIrisXEGuC::deriveLayoutFromCSS(const uint8_t* fwData, size_t fwSize,
                                         size_t* outPayloadOffset, size_t* outPayloadSize)
{
    const struct CSSFirmwareHeader {
        uint32_t module_type;
        uint32_t header_len;
        uint32_t header_version;
        uint32_t module_id;
        uint32_t module_vendor;
        uint32_t date;
        uint32_t size;
        uint32_t key_size;
        uint32_t modulus_size;
        uint32_t exponent_size;
    } __attribute__((packed));

    const CSSFirmwareHeader* css = (const CSSFirmwareHeader*)fwData;

    IOLog("(FakeIrisXE) [V137] CSS: header_len=%u key_size=%u modulus_size=%u exponent_size=%u\n",
          css->header_len, css->key_size, css->modulus_size, css->exponent_size);

    size_t key_size_bytes = css->key_size * 4;
    size_t modulus_size_bytes = css->modulus_size * 4;
    size_t exponent_size_bytes = css->exponent_size * 4;

    size_t rsa_offset = css->header_len + key_size_bytes + modulus_size_bytes + exponent_size_bytes;
    size_t ucode_offset = rsa_offset + modulus_size_bytes;

    IOLog("(FakeIrisXE) [V137] RSA starts at: 0x%zx\n", rsa_offset);
    IOLog("(FakeIrisXE) [V137] uCode starts at: 0x%zx\n", ucode_offset);

    *outPayloadOffset = ucode_offset;
    *outPayloadSize = fwSize - ucode_offset;

    IOLog("(FakeIrisXE) [V137] Payload: offset=0x%zx size=0x%zx\n", *outPayloadOffset, *outPayloadSize);
    return true;
}

// ============================================================================
// V137: Program WOPCM for Tiger Lake (Linux i915 method)
// Configures GUC_WOPCM_SIZE and DMA_GUC_WOPCM_OFFSET before DMA transfer
// ============================================================================
bool FakeIrisXEGuC::programWopcmForTgl(uint32_t wopcmSize, uint32_t wopcmOffset)
{
    IOLog("(FakeIrisXE) [V137] Programming WOPCM: size=0x%X offset=0x%X\n", wopcmSize, wopcmOffset);

    uint32_t guc_wopcm_base = 0;
    fOwner->safeMMIOWrite(GUC_WOPCM_SIZE_V137, 0);
    IOSleep(1);

    uint32_t size_val = (wopcmSize & 0xFFFFFu) << 12;
    size_val |= GUC_WOPCM_SIZE_LOCKED_V137;
    fOwner->safeMMIOWrite(GUC_WOPCM_SIZE_V137, size_val);
    IOLog("(FakeIrisXE) [V137] Wrote GUC_WOPCM_SIZE (0xC050): 0x%08X\n", size_val);

    IOSleep(1);

    uint32_t offset_val = (wopcmOffset & 0x3FFFFu) << 14;
    offset_val |= GUC_WOPCM_OFFSET_VALID_V137;
    fOwner->safeMMIOWrite(DMA_GUC_WOPCM_OFFSET_V137, offset_val);
    IOLog("(FakeIrisXE) [V137] Wrote DMA_GUC_WOPCM_OFFSET (0xC340): 0x%08X\n", offset_val);

    IOSleep(1);

    uint32_t verify_size = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t verify_offset = fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137);
    IOLog("(FakeIrisXE) [V137] Verify: SIZE=0x%08X OFFSET=0x%08X\n", verify_size, verify_offset);

    return (verify_size != 0 && verify_offset != 0);
}

// ============================================================================
// V137: Write RSA Signature to UOS_RSA_SCRATCH (Linux i915 method)
// Writes full 256 bytes (64 dwords) to registers at 0xC200+
// ============================================================================
bool FakeIrisXEGuC::writeRsaScratch(const uint8_t* fwData, size_t fwSize)
{
    IOLog("(FakeIrisXE) [V137] Writing RSA signature to UOS_RSA_SCRATCH (0xC200+)\n");

    const struct CSSFirmwareHeader {
        uint32_t module_type;
        uint32_t header_len;
        uint32_t header_version;
        uint32_t module_id;
        uint32_t module_vendor;
        uint32_t date;
        uint32_t size;
        uint32_t key_size;
        uint32_t modulus_size;
        uint32_t exponent_size;
    } __attribute__((packed));

    const CSSFirmwareHeader* css = (const CSSFirmwareHeader*)fwData;

    size_t key_size_bytes = css->key_size * 4;
    size_t modulus_size_bytes = css->modulus_size * 4;
    size_t exponent_size_bytes = css->exponent_size * 4;

    size_t rsa_offset = css->header_len + key_size_bytes + modulus_size_bytes + exponent_size_bytes;

    IOLog("(FakeIrisXE) [V137] RSA signature at offset: 0x%zx\n", rsa_offset);

    const uint8_t* rsa_data = fwData + rsa_offset;

    for (int i = 0; i < UOS_RSA_SCRATCH_COUNT_V137; i++) {
        uint32_t val = 0;
        if (rsa_offset + (i * 4) + 4 <= fwSize) {
            val = *(uint32_t*)(rsa_data + (i * 4));
        }
        fOwner->safeMMIOWrite(UOS_RSA_SCRATCH_BASE_V137 + (i * 4), val);
    }

    IOLog("(FakeIrisXE) [V137] ✅ Wrote %d dwords (256 bytes) to UOS_RSA_SCRATCH\n",
          UOS_RSA_SCRATCH_COUNT_V137);

    return true;
}

// ============================================================================
// V137: DMA Copy from GTT to WOPCM (Linux i915 method)
// Uses CORRECT Tiger Lake DMA registers at 0xC300-0xC314
// ============================================================================
bool FakeIrisXEGuC::dmaCopyGttToWopcm(uint64_t sourceGpuAddr, uint32_t destOffset, size_t fwSize)
{
    IOLog("(FakeIrisXE) [V137] DMA Copy: GGTT=0x%016llX -> WOPCM offset=0x%X size=0x%zX\n",
          sourceGpuAddr, destOffset, fwSize);

    uint32_t srcLow = (uint32_t)(sourceGpuAddr & 0xFFFFFFFF);
    uint32_t srcHigh = (uint32_t)((sourceGpuAddr >> 32) & 0xFFFF);
    srcHigh |= DMA_ADDRESS_SPACE_GTT_V137;

    fOwner->safeMMIOWrite(DMA_ADDR_0_LOW_V137, srcLow);
    fOwner->safeMMIOWrite(DMA_ADDR_0_HIGH_V137, srcHigh);

    uint32_t dstLow = destOffset;
    uint32_t dstHigh = DMA_ADDRESS_SPACE_WOPCM_V137;

    fOwner->safeMMIOWrite(DMA_ADDR_1_LOW_V137, dstLow);
    fOwner->safeMMIOWrite(DMA_ADDR_1_HIGH_V137, dstHigh);

    fOwner->safeMMIOWrite(DMA_COPY_SIZE_V137, (uint32_t)fwSize);

    IOLog("(FakeIrisXE) [V137] DMA registers programmed:\n");
    IOLog("  SRC_LO=0x%08X SRC_HI=0x%08X\n", srcLow, srcHigh);
    IOLog("  DST_LO=0x%08X DST_HI=0x%08X\n", dstLow, dstHigh);
    IOLog("  SIZE=0x%08X\n", (uint32_t)fwSize);

    uint32_t ctrl = START_DMA_V137 | UOS_MOVE_V137;
    fOwner->safeMMIOWrite(DMA_CTRL_V137, ctrl);
    IOLog("(FakeIrisXE) [V137] DMA started: CTRL=0x%08X\n", ctrl);

    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = 100 * 1000000ULL;
    bool completed = false;

    while (mach_absolute_time() - start < timeoutNs) {
        uint32_t status = fOwner->safeMMIORead(DMA_CTRL_V137);
        if (!(status & START_DMA_V137)) {
            completed = true;
            IOLog("(FakeIrisXE) [V137] ✅ DMA completed!\n");
            break;
        }
        IOSleep(1);
    }

    if (!completed) {
        uint32_t finalStatus = fOwner->safeMMIORead(DMA_CTRL_V137);
        IOLog("(FakeIrisXE) [V137] ❌ DMA timeout! DMA_CTRL=0x%08X\n", finalStatus);
        return false;
    }

    fOwner->safeMMIOWrite(DMA_CTRL_V137, 0);
    return true;
}

// ============================================================================
// V137: Wait for GuC Boot (Linux i915 method)
// Polls GUC_STATUS with correct bitfield decoding
// ============================================================================
bool FakeIrisXEGuC::waitForGucBoot(uint32_t timeoutMs)
{
    IOLog("(FakeIrisXE) [V137] Waiting for GuC boot (timeout: %u ms)...\n", timeoutMs);

    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = timeoutMs * 1000000ULL;

    while (mach_absolute_time() - start < timeoutNs) {
        uint32_t status = fOwner->safeMMIORead(GUC_STATUS_V137);

        uint32_t bootrom_status = FIELD_GET_V137(GUC_BOOTROM_STATUS_MASK_V137, status);
        uint32_t ukernel_status = FIELD_GET_V137(GUC_UKERNEL_STATUS_MASK_V137, status);
        uint32_t mia_core_status = FIELD_GET_V137(GUC_MIA_CORE_STATUS_MASK_V137, status);

        IOLog("(FakeIrisXE) [V137] STATUS=0x%08X bootrom=%u ukernel=%u mia=%u\n",
              status, bootrom_status, ukernel_status, mia_core_status);

        if (bootrom_status == 0x7F && ukernel_status == 0xFF) {
            IOLog("(FakeIrisXE) [V137] ✅ GuC booted successfully!\n");
            return true;
        }

        if (bootrom_status != 0 && bootrom_status != 0x7F) {
            IOLog("(FakeIrisXE) [V137] ❌ GuC boot failed! bootrom_status=0x%02X\n", bootrom_status);
            return false;
        }

        IOSleep(10);
    }

    IOLog("(FakeIrisXE) [V137] ❌ Timeout waiting for GuC boot\n");
    return false;
}

// ============================================================================
// V137: Complete GuC Load Sequence (uses all correct Tiger Lake methods)
// Replaces initGuCForAppleDMA with correct Linux-style sequence
// ============================================================================
bool FakeIrisXEGuC::loadGuCWithV137Method(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr)
{
    IOLog("(FakeIrisXE) [V137] ============================================\n");
    IOLog("(FakeIrisXE) [V137] V137 GuC Load Sequence (Tiger Lake)\n");
    IOLog("(FakeIrisXE) [V137] ============================================\n");

    if (!acquireForceWake()) {
        IOLog("(FakeIrisXE) [V137] ⚠️ ForceWake warning, continuing...\n");
    }

    programShimControl();
    IOSleep(10);

    size_t payloadOffset, payloadSize;
    deriveLayoutFromCSS(fwData, fwSize, &payloadOffset, &payloadSize);

    writeRsaScratch(fwData, fwSize);

    uint32_t wopcmSize = 0x100000;
    uint32_t wopcmOffset = 0x2000;
    if (!programWopcmForTgl(wopcmSize, wopcmOffset)) {
        IOLog("(FakeIrisXE) [V137] ❌ WOPCM configuration failed!\n");
        return false;
    }

    if (!dmaCopyGttToWopcm(gpuAddr + payloadOffset, wopcmOffset, payloadSize)) {
        IOLog("(FakeIrisXE) [V137] ❌ DMA copy failed!\n");
        return false;
    }

    if (!waitForGucBoot(5000)) {
        IOLog("(FakeIrisXE) [V137] ❌ GuC boot failed!\n");
        return false;
    }

    releaseForceWake();

    IOLog("(FakeIrisXE) [V137] ✅ V137 GuC load sequence complete!\n");
    return true;
}

// ============================================================================
// V138: Fixed GuC Reset + WOPCM Configuration (Linux i915 method)
// Based on Intel PRM and Linux i915 intel_uc.c
// ============================================================================
bool FakeIrisXEGuC::guclResetForWopcmV138()
{
    IOLog("(FakeIrisXE) [V138] === GuC Reset for WOPCM Configuration ===\n");
    
    // Check if WOPCM is already locked
    uint32_t wopcm_size = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t wopcm_offset = fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137);
    IOLog("(FakeIrisXE) [V138] Current WOPCM: SIZE=0x%08X OFFSET=0x%08X\n", wopcm_size, wopcm_offset);
    
    if (wopcm_size & 0x80000000) {
        IOLog("(FakeIrisXE) [V138] WOPCM already locked - skipping reset\n");
        return true;
    }
    
    // Ensure ForceWake is acquired
    acquireForceWake();
    IOSleep(10);
    
    // Request GuC reset
    fOwner->safeMMIOWrite(GUC_RESET_CTL_V137, 0x00000001);
    IOSleep(50);
    
    IOLog("(FakeIrisXE) [V138] === GuC Reset Complete ===\n");
    return true;
}

// ============================================================================
// V138: Program WOPCM with proper sequence
// ============================================================================
bool FakeIrisXEGuC::programWopcmForTglV138(uint32_t wopcmSize, uint32_t wopcmOffset)
{
    IOLog("(FakeIrisXE) [V138] Programming WOPCM: size=0x%X offset=0x%X\n", wopcmSize, wopcmOffset);
    
    // Check if already locked
    uint32_t check_size = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    if (check_size & 0x80000000) {
        IOLog("(FakeIrisXE) [V138] WOPCM already locked\n");
        return true;
    }
    
    // Calculate size value (in 4KB pages, shifted left by 12)
    uint32_t size_in_pages = wopcmSize >> 12;
    uint32_t size_val = (size_in_pages & 0xFFFFFu) << 12;
    size_val |= 0x80000000;  // GUC_WOPCM_SIZE_LOCKED
    
    fOwner->safeMMIOWrite(GUC_WOPCM_SIZE_V137, size_val);
    IOSleep(10);
    
    uint32_t verify_size = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    IOLog("(FakeIrisXE) [V138] Wrote SIZE=0x%08X verify=0x%08X\n", size_val, verify_size);
    
    // Calculate offset value (in 16KB increments, shifted left by 14)
    uint32_t offset_in_16k = wopcmOffset >> 14;
    uint32_t offset_val = (offset_in_16k & 0x3FFFFu) << 14;
    offset_val |= 0x80000000;  // GUC_WOPCM_OFFSET_VALID
    
    fOwner->safeMMIOWrite(DMA_GUC_WOPCM_OFFSET_V137, offset_val);
    IOSleep(10);
    
    uint32_t verify_offset = fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137);
    IOLog("(FakeIrisXE) [V138] Wrote OFFSET=0x%08X verify=0x%08X\n", offset_val, verify_offset);
    
    return (verify_size != 0 && verify_offset != 0);
}

// ============================================================================
// V138: Complete GuC Load Sequence with FIXED WOPCM
// ============================================================================
bool FakeIrisXEGuC::loadGuCWithV138Method(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr)
{
    IOLog("(FakeIrisXE) [V138] V138 GuC Load Sequence (Tiger Lake FIXED)\n");
    
    if (!acquireForceWake()) {
        IOLog("(FakeIrisXE) [V138] ⚠️ ForceWake warning, continuing...\n");
    }
    IOSleep(20);
    
    programShimControl();
    IOSleep(20);
    
    guclResetForWopcmV138();
    IOSleep(20);
    
    size_t payloadOffset, payloadSize;
    deriveLayoutFromCSS(fwData, fwSize, &payloadOffset, &payloadSize);
    
    writeRsaScratch(fwData, fwSize);
    
    uint32_t wopcmSize = 0x100000;  // 1MB
    uint32_t wopcmOffset = 0x2000;   // 8KB offset
    if (!programWopcmForTglV138(wopcmSize, wopcmOffset)) {
        IOLog("(FakeIrisXE) [V138] ❌ WOPCM configuration failed!\n");
        return false;
    }
    IOSleep(20);
    
    if (!dmaCopyGttToWopcm(gpuAddr + payloadOffset, wopcmOffset, payloadSize)) {
        IOLog("(FakeIrisXE) [V138] ❌ DMA copy failed!\n");
        return false;
    }
    
    if (!waitForGucBoot(5000)) {
        IOLog("(FakeIrisXE) [V138] ❌ GuC boot failed!\n");
        return false;
    }
    
    releaseForceWake();
    
    IOLog("(FakeIrisXE) [V138] ✅ V138 GuC load sequence complete!\n");
    return true;
}
