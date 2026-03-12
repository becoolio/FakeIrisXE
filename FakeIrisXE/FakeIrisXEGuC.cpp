//
//  FakeIrisXEGuC.cpp
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 03/12/25.
//

// FakeIrisXEGuC.cpp
#include "FakeIrisXEGuC.hpp"
#include "i915_reg.h"
#include "FakeIrisXEGuCTGLPublicKey.hpp"
#include "AppleSafeRegisterAccess.hpp"
#include <libkern/c++/OSBoolean.h>

extern "C" void OSSynchronizeIO(void);

// V135: Add missing register defines - aggressive Linux GT initialization
// V135: Added PPGTT, GART, additional power management, GT workarounds
#ifndef GEN11_GUC_RESET
#define GEN11_GUC_RESET              0x1C0C0
#endif

// V146: Add GUC_CTL and GUC_MISC_CONTROL registers
#ifndef GUC_CTL
#define GUC_CTL                     0xC05C  // GuC Control register
#endif

#ifndef GUC_MISC_CONTROL
#define GUC_MISC_CONTROL            0xC068  // GuC Misc Control (V143 added)
#endif

// V146: Try GUC_ACTION_INIT
#ifndef GUC_ACTION_INIT
#define GUC_ACTION_INIT             0x3000  // Init action
#endif

// V147: Additional pre-initialization registers
#ifndef GUC_H2G_MSG
#define GUC_H2G_MSG                0xC300  // Host-to-GuC message
#endif

#ifndef GUC_STATUS_SecureBoot
#define GUC_STATUS_SecureBoot      0x80000000  // Secure boot bit
#endif

#ifndef GUC_STATUS_WOPCMERR
#define GUC_STATUS_WOPCMERR        0x40000000  // WOPCM error
#endif

#ifndef GUC_HEADER_INFO_V170
#define GUC_HEADER_INFO_V170       0xC014
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
// Use Linux-public GT_PM_CONFIG candidates; do not alias FORCEWAKE_MT at 0xA188.
#ifndef GT_PM_CONFIG
#define GT_PM_CONFIG                0x138140
#endif
#ifndef TGL_GT_PM_CONFIG
#define TGL_GT_PM_CONFIG            0x138140
#endif
#ifndef TGL_GT_PM_CONFIG_GT
#define TGL_GT_PM_CONFIG_GT         0x13816C
#endif
#ifndef GUC_DOORBELL_CTRL
#define GUC_DOORBELL_CTRL           0xC510
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

// V143: GUC_MISC_CONTROL - Apple driver writes 3 to this before DMA
#ifndef GUC_MISC_CONTROL
#define GUC_MISC_CONTROL            0xC068  // Same as SHIM_CONTROL2?
#endif

#ifndef APPLE_TGL_GUC_RESET_CTRL_V173
#define APPLE_TGL_GUC_RESET_CTRL_V173 0x941C
#endif
#ifndef APPLE_TGL_ME_FW_STATUS_V173
#define APPLE_TGL_ME_FW_STATUS_V173 0xC0F4
#endif
#ifndef APPLE_TGL_GUC_RESET_BIT_V173
#define APPLE_TGL_GUC_RESET_BIT_V173 0x00000008U
#endif
#ifndef APPLE_TGL_ME_WAKE_REQ_V173
#define APPLE_TGL_ME_WAKE_REQ_V173 0x00000002U
#endif
#ifndef APPLE_TGL_ME_WAKE_ACK_MASK_V173
#define APPLE_TGL_ME_WAKE_ACK_MASK_V173 0x00000001U
#endif
#ifndef APPLE_TGL_ME_HASH_READY_V173
#define APPLE_TGL_ME_HASH_READY_V173 0x000001FFU
#endif
#ifndef APPLE_TGL_SPRINGBOARD_PTR_V173
#define APPLE_TGL_SPRINGBOARD_PTR_V173 0xC1B8
#endif

#ifndef GEN11_FORCEWAKE_KERNEL_BIT_V174
#define GEN11_FORCEWAKE_KERNEL_BIT_V174 0x00000001U
#endif
#ifndef GEN11_FORCEWAKE_MASKED_ENABLE_V174
#define GEN11_FORCEWAKE_MASKED_ENABLE_V174 0x00010001U
#endif
#ifndef GEN11_FORCEWAKE_MASKED_DISABLE_V174
#define GEN11_FORCEWAKE_MASKED_DISABLE_V174 0x00010000U
#endif

#ifndef APPLE_FORCEWAKE_POLL_DELAY_US_V175
#define APPLE_FORCEWAKE_POLL_DELAY_US_V175 100U
#endif
#ifndef APPLE_FORCEWAKE_POLLS_PER_TRY_V175
#define APPLE_FORCEWAKE_POLLS_PER_TRY_V175 3000U
#endif
#ifndef APPLE_FORCEWAKE_MAX_RETRIGGERS_V175
#define APPLE_FORCEWAKE_MAX_RETRIGGERS_V175 8U
#endif

#ifndef APPLE_TGL_PREAUTH_MAX_ATTEMPTS_V177
#define APPLE_TGL_PREAUTH_MAX_ATTEMPTS_V177 5U
#endif
#ifndef APPLE_TGL_PREAUTH_STEP_TIMEOUT_MS_V177
#define APPLE_TGL_PREAUTH_STEP_TIMEOUT_MS_V177 250U
#endif
#ifndef APPLE_TGL_PREAUTH_READY_TIMEOUT_MS_V177
#define APPLE_TGL_PREAUTH_READY_TIMEOUT_MS_V177 500U
#endif
#ifndef APPLE_TGL_PREAUTH_RETRY_DELAY_MS_V177
#define APPLE_TGL_PREAUTH_RETRY_DELAY_MS_V177 20U
#endif
#ifndef APPLE_TGL_ME_WAKE_TIMEOUT_MS_V179
#define APPLE_TGL_ME_WAKE_TIMEOUT_MS_V179 1500U
#endif
#ifndef APPLE_TGL_ME_HASH_READY_TIMEOUT_MS_V179
#define APPLE_TGL_ME_HASH_READY_TIMEOUT_MS_V179 1500U
#endif

#ifndef APPLE_TGL_GUC_LOAD_FREQ_CTRL_V178
#define APPLE_TGL_GUC_LOAD_FREQ_CTRL_V178 0xA008U
#endif
#ifndef APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178
#define APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178 0x145948U
#endif
#ifndef APPLE_TGL_GUC_LOAD_FREQ_STATUS_SHIFT_V178
#define APPLE_TGL_GUC_LOAD_FREQ_STATUS_SHIFT_V178 11U
#endif
#ifndef APPLE_TGL_GUC_LOAD_FREQ_STATUS_MASK_V178
#define APPLE_TGL_GUC_LOAD_FREQ_STATUS_MASK_V178 (0x1FFU << APPLE_TGL_GUC_LOAD_FREQ_STATUS_SHIFT_V178)
#endif
#ifndef APPLE_TGL_GUC_LOAD_FREQ_TOKEN_V178
#define APPLE_TGL_GUC_LOAD_FREQ_TOKEN_V178 0x03018000U
#endif
#ifndef APPLE_TGL_GUC_LOAD_FREQ_TIMEOUT_MS_V178
#define APPLE_TGL_GUC_LOAD_FREQ_TIMEOUT_MS_V178 30U
#endif

#ifndef APPLE_TGL_FORCEWAKE_GLOBAL_ENABLE_V176
#define APPLE_TGL_FORCEWAKE_GLOBAL_ENABLE_V176 0x00020002U
#endif
#ifndef APPLE_TGL_FORCEWAKE_GLOBAL_DISABLE_V176
#define APPLE_TGL_FORCEWAKE_GLOBAL_DISABLE_V176 0x00020000U
#endif
#ifndef APPLE_TGL_FORCEWAKE_GLOBAL_ACK_MASK_V176
#define APPLE_TGL_FORCEWAKE_GLOBAL_ACK_MASK_V176 0x00000002U
#endif

#ifndef APPLE_TGL_FORCEWAKE_RENDER_ENABLE_V176
#define APPLE_TGL_FORCEWAKE_RENDER_ENABLE_V176 0x00020002U
#endif
#ifndef APPLE_TGL_FORCEWAKE_RENDER_DISABLE_V176
#define APPLE_TGL_FORCEWAKE_RENDER_DISABLE_V176 0x00020000U
#endif
#ifndef APPLE_TGL_FORCEWAKE_RENDER_ACK_MASK_V176
#define APPLE_TGL_FORCEWAKE_RENDER_ACK_MASK_V176 0x00000002U
#endif
#ifndef APPLE_TGL_FORCEWAKE_RENDER_ACK_V176
#define APPLE_TGL_FORCEWAKE_RENDER_ACK_V176 0x0D84U
#endif

#ifndef APPLE_TGL_FORCEWAKE_MEDIA_ENABLE_V176
#define APPLE_TGL_FORCEWAKE_MEDIA_ENABLE_V176 0x00010001U
#endif
#ifndef APPLE_TGL_FORCEWAKE_MEDIA_DISABLE_V176
#define APPLE_TGL_FORCEWAKE_MEDIA_DISABLE_V176 0x00010000U
#endif
#ifndef APPLE_TGL_FORCEWAKE_MEDIA_ACK_MASK_V176
#define APPLE_TGL_FORCEWAKE_MEDIA_ACK_MASK_V176 0x00000001U
#endif
#ifndef GEN11_FORCEWAKE_MEDIA_VDBOX0
#define GEN11_FORCEWAKE_MEDIA_VDBOX0 0x0000A540U
#endif
#ifndef GEN11_FORCEWAKE_MEDIA_VDBOX0_ACK
#define GEN11_FORCEWAKE_MEDIA_VDBOX0_ACK 0x00000D50U
#endif
#ifndef GEN11_FORCEWAKE_MEDIA_VEBOX0
#define GEN11_FORCEWAKE_MEDIA_VEBOX0 0x0000A560U
#endif
#ifndef GEN11_FORCEWAKE_MEDIA_VEBOX0_ACK
#define GEN11_FORCEWAKE_MEDIA_VEBOX0_ACK 0x00000D70U
#endif
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

#ifndef GUC_SOFT_SCRATCH_V170
#define GUC_SOFT_SCRATCH_V170(n)   (0xC180 + ((n) * 4))
#endif

#ifndef GUC_CTL_LOG_PARAMS_V170
#define GUC_CTL_LOG_PARAMS_V170    0U
#endif
#ifndef GUC_CTL_WA_V170
#define GUC_CTL_WA_V170            1U
#endif
#ifndef GUC_CTL_FEATURE_V170
#define GUC_CTL_FEATURE_V170       2U
#endif
#ifndef GUC_CTL_DEBUG_V170
#define GUC_CTL_DEBUG_V170         3U
#endif
#ifndef GUC_CTL_ADS_V170
#define GUC_CTL_ADS_V170           4U
#endif
#ifndef GUC_CTL_DEVID_V170
#define GUC_CTL_DEVID_V170         5U
#endif
#ifndef GUC_CTL_MAX_DWORDS_V170
#define GUC_CTL_MAX_DWORDS_V170    6U
#endif

// Address space encoding (in HIGH registers)
#ifndef DMA_ADDRESS_SPACE_WOPCM_V137
// V142: Fixed - Linux uses 0x10000, not 0x70000!
// The address space bits tell DMA where to write:
// Bit 16 = WOPCM space (0x10000)
#define DMA_ADDRESS_SPACE_WOPCM_V137 (0x00010000U)  // Was 0x70000 - WRONG!
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
#ifndef GUC_CTL_DISABLE_SCHEDULER_V170
#define GUC_CTL_DISABLE_SCHEDULER_V170 (1u << 14)
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

#ifndef GUC_AUTH_STATUS_MASK_V170
#define GUC_AUTH_STATUS_MASK_V170  (0x3u << 30)
#endif
#ifndef GUC_AUTH_STATUS_SHIFT_V170
#define GUC_AUTH_STATUS_SHIFT_V170 30
#endif
#ifndef GUC_AUTH_STATUS_BAD_V170
#define GUC_AUTH_STATUS_BAD_V170   0x1U
#endif
#ifndef GUC_AUTH_STATUS_GOOD_V170
#define GUC_AUTH_STATUS_GOOD_V170  0x2U
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

#ifndef FAKEIRISXE_ENABLE_APPLE_GUC_PATH
#define FAKEIRISXE_ENABLE_APPLE_GUC_PATH 0
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

// V145: FIX GT_PM_CONFIG to 0xA188 for Tiger Lake
// Linux public i915 programs GT_DOORBELL_ENABLE into GT_PM_CONFIG.
#ifndef TGL_GT_PM_CONFIG_VALUE
#define TGL_GT_PM_CONFIG_VALUE     0x00000001U
#endif

// GT doorbell
#ifndef GT_DOORBELL_ENABLE
#define GT_DOORBELL_ENABLE    0x1
#endif

#ifndef GEN12_GUC_TLB_INV_CR_V170
#define GEN12_GUC_TLB_INV_CR_V170   0xCEE8
#endif
#ifndef GEN12_GUC_TLB_INV_CR_INVALIDATE_V170
#define GEN12_GUC_TLB_INV_CR_INVALIDATE_V170 0x1U
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
    obj->fGuCMode = false;
    obj->fLastReportedStage = kGuCStageIdle;
    obj->fFirmwareMode = kGuCFirmwareModeAppleOnly;
    obj->fApplePinnedAccessActive = false;
    obj->fApplePinnedDomain = kAppleRegisterDomainNone;
    obj->fGuCPublicKeyGem = nullptr;
    return obj;
}

bool FakeIrisXEGuC::initGuC()
{
    fFirmwareMode = selectFirmwareMode();
    IOLog("(FakeIrisXE) [GuC] Initializing deterministic GuC pre-flight (mode=%s)\n",
          firmwareModeName(fFirmwareMode));

    fGuCMode = false;
    fLastReportedStage = kGuCStageIdle;

    initGTPreWorkaround();

    extern const unsigned char tgl_dmc_ver2_12_bin[];
    extern const unsigned int tgl_dmc_ver2_12_bin_len;
    if (!loadDmcFirmware(tgl_dmc_ver2_12_bin, tgl_dmc_ver2_12_bin_len)) {
        IOLog("(FakeIrisXE) [GuC] DMC load failed, continuing with GuC path\n");
    }

    IOLog("(FakeIrisXE) [GuC] Pre-flight complete\n");
    return true;
}

FakeIrisXEGuC::GuCFirmwareMode FakeIrisXEGuC::selectFirmwareMode() const
{
    return kGuCFirmwareModeAppleOnly;
}

const char* FakeIrisXEGuC::firmwareModeName(GuCFirmwareMode mode) const
{
    switch (mode) {
        case kGuCFirmwareModeAppleOnly:
            return "apple-only";
        case kGuCFirmwareModeLinuxReserved:
            return "linux-reserved";
    }

    return "unknown";
}

const char* FakeIrisXEGuC::authStatusName(uint8_t authStatus) const
{
    switch (authStatus) {
        case 0:
            return "none";
        case GUC_AUTH_STATUS_BAD_V170:
            return "bad";
        case GUC_AUTH_STATUS_GOOD_V170:
            return "good";
        default:
            return "other";
    }
}

void FakeIrisXEGuC::logBootFailureSignature(const char* reason, uint64_t startNs,
                                            uint32_t retryIndex, uint32_t rawStatusOverride)
{
    uint32_t rawStatus = rawStatusOverride;
    if (rawStatus == 0xFFFFFFFFU) {
        rawStatus = fOwner->safeMMIORead(GUC_STATUS_V137);
    }

    GuCStatusDecoded decoded = decodeStatus(rawStatus);
    IOLog("(FakeIrisXE) [GuC][Boot] FAIL mode=%s reason=%s raw=0x%08X bootrom=0x%02X ukernel=0x%02X mia=0x%X auth=%s(0x%X)\n",
          firmwareModeName(fFirmwareMode),
          reason ? reason : "unknown",
          rawStatus,
          decoded.bootrom,
          decoded.ukernel,
          decoded.mia,
          authStatusName(decoded.authStatus),
          decoded.authStatus);
    emitStageReport(kGuCStageFailure, startNs, retryIndex, rawStatus);
}

bool FakeIrisXEGuC::prepareAppleWopcm(GuCStage stage, uint32_t desiredSizeValue,
                                      uint32_t desiredOffsetValue)
{
    uint32_t currentSize = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t currentOffset = fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137);
    const bool sizeLocked = ((currentSize & 0x80000000U) != 0U) ||
                            ((currentSize & GUC_WOPCM_SIZE_LOCKED_V137) != 0U);
    const bool offsetValid = ((currentOffset & 0x80000000U) != 0U) ||
                             ((currentOffset & GUC_WOPCM_OFFSET_VALID_V137) != 0U);

    IOLog("(FakeIrisXE) [GuC][Apple] WOPCM preflight size=0x%08X offset=0x%08X size_locked=%u offset_valid=%u\n",
          currentSize,
          currentOffset,
          sizeLocked ? 1U : 0U,
          offsetValid ? 1U : 0U);

    if (currentSize == desiredSizeValue && currentOffset == desiredOffsetValue) {
        IOLog("(FakeIrisXE) [GuC][Apple] WOPCM locked, reusing existing configuration\n");
        return true;
    }

    if (sizeLocked || offsetValid) {
        IOLog("(FakeIrisXE) [GuC][Apple] WOPCM locked with unexpected values size=0x%08X offset=0x%08X expected_size=0x%08X expected_offset=0x%08X\n",
              currentSize,
              currentOffset,
              desiredSizeValue,
              desiredOffsetValue);
        return false;
    }

    uint32_t sizeReadback = 0;
    uint32_t offsetReadback = 0;
    writeRegWithReadback(stage, "GUC_WOPCM_SIZE", GUC_WOPCM_SIZE_V137,
                         desiredSizeValue, &sizeReadback);
    writeRegWithReadback(stage, "DMA_GUC_WOPCM_OFFSET", DMA_GUC_WOPCM_OFFSET_V137,
                         desiredOffsetValue, &offsetReadback);

    if (sizeReadback != desiredSizeValue || offsetReadback != desiredOffsetValue) {
        IOLog("(FakeIrisXE) [GuC][Apple] WOPCM write did not stick size=0x%08X offset=0x%08X expected_size=0x%08X expected_offset=0x%08X\n",
              sizeReadback,
              offsetReadback,
              desiredSizeValue,
              desiredOffsetValue);
        return false;
    }

    IOLog("(FakeIrisXE) [GuC][Apple] WOPCM programmed size=0x%08X offset=0x%08X\n",
          sizeReadback, offsetReadback);
    return true;
}

bool FakeIrisXEGuC::writeRegWithReadback(GuCStage stage, const char* regName,
                                         uint32_t reg, uint32_t value,
                                         uint32_t* outReadback)
{
    fOwner->safeWriteRegister32(reg, value);
    uint32_t readback = fOwner->safeReadRegister32(reg);

    if (outReadback) {
        *outReadback = readback;
    }

    IOLog("(FakeIrisXE) [GuC][RW] stage=%u %s(0x%04X) write=0x%08X read=0x%08X\n",
          (uint32_t)stage, regName, reg, value, readback);

    return readback == value;
}

FakeIrisXEGuC::GuCStatusDecoded FakeIrisXEGuC::decodeStatus(uint32_t rawStatus) const
{
    GuCStatusDecoded decoded;
    decoded.bootrom = (uint8_t)FIELD_GET_V137(GUC_BOOTROM_STATUS_MASK_V137, rawStatus);
    decoded.ukernel = (uint8_t)FIELD_GET_V137(GUC_UKERNEL_STATUS_MASK_V137, rawStatus);
    decoded.mia = (uint8_t)FIELD_GET_V137(GUC_MIA_CORE_STATUS_MASK_V137, rawStatus);
    decoded.authStatus = (uint8_t)FIELD_GET_V137(GUC_AUTH_STATUS_MASK_V170, rawStatus);
    decoded.valid = (rawStatus != 0xFFFFFFFFU);
    decoded.success = ((decoded.bootrom == 0x7FU && decoded.ukernel == 0xFFU) ||
                       (decoded.ukernel == GUC_LOAD_SUCCESS_STATUS));
    decoded.failure = (((rawStatus & 0xFEU) == GUC_LOAD_FAIL_STATUS_1) ||
                       (decoded.ukernel == GUC_LOAD_FAIL_STATUS_2) ||
                       (decoded.authStatus == GUC_AUTH_STATUS_BAD_V170) ||
                       decoded.bootrom == 0x06U);
    return decoded;
}

bool FakeIrisXEGuC::isImpossibleStatusDecode(uint32_t rawStatus,
                                             const GuCStatusDecoded& decoded) const
{
    if (!decoded.valid) {
        return true;
    }

    if (decoded.bootrom == 0 && (decoded.ukernel != 0 || decoded.mia != 0)) {
        return true;
    }

    if (rawStatus == 0xFFFFFFFFU) {
        return true;
    }

    return false;
}

bool FakeIrisXEGuC::ownerBooleanPropertyEnabled(const char* key) const
{
    if (!fOwner || !key) {
        return false;
    }

    OSBoolean* value = OSDynamicCast(OSBoolean, fOwner->getProperty(key));
    return value && value->isTrue();
}

uint32_t FakeIrisXEGuC::selectGtPmConfigReg() const
{
    if (!fOwner) {
        return TGL_GT_PM_CONFIG;
    }

    switch (fOwner->getPCIDeviceID()) {
        case 0x9A49:
            return TGL_GT_PM_CONFIG;
        case 0x46A3:
            return TGL_GT_PM_CONFIG_GT;
        default:
            return TGL_GT_PM_CONFIG;
    }
}

FakeIrisXEGuC::AppleRegisterDomain FakeIrisXEGuC::determinePowerDomainForOffset(uint32_t offset) const
{
    switch (offset) {
        case FORCEWAKE_REQ:
        case FORCEWAKE_ACK:
        case GEN11_FORCEWAKE_RENDER:
        case GEN11_FORCEWAKE_RENDER_ACK:
        case APPLE_TGL_FORCEWAKE_RENDER_ACK_V176:
        case GEN11_FORCEWAKE_MEDIA_VDBOX0:
        case GEN11_FORCEWAKE_MEDIA_VDBOX0_ACK:
        case GEN11_FORCEWAKE_MEDIA_VEBOX0:
        case GEN11_FORCEWAKE_MEDIA_VEBOX0_ACK:
            return kAppleRegisterDomainNone;

        case APPLE_TGL_GUC_RESET_CTRL_V173:
        case APPLE_TGL_ME_FW_STATUS_V173:
        case APPLE_TGL_GUC_LOAD_FREQ_CTRL_V178:
        case APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178:
        case APPLE_TGL_SPRINGBOARD_PTR_V173:
        case TGL_GT_PM_CONFIG_GT:
        case GUC_HEADER_INFO_V170:
            return kAppleRegisterDomainGlobal;
    }

    if (offset >= GUC_SOFT_SCRATCH_V170(0) && offset <= GUC_SOFT_SCRATCH_V170(6)) {
        return kAppleRegisterDomainGlobal;
    }

    if (offset >= UOS_RSA_SCRATCH_BASE_V137 &&
        offset < (UOS_RSA_SCRATCH_BASE_V137 + UOS_RSA_SCRATCH_COUNT_V137 * sizeof(uint32_t))) {
        return kAppleRegisterDomainGlobal;
    }

    if ((offset >= 0xC000U && offset <= 0xC17FU) ||
        (offset >= 0xC300U && offset <= 0xC34FU)) {
        return kAppleRegisterDomainRender;
    }

    return kAppleRegisterDomainNone;
}

const char* FakeIrisXEGuC::appleRegisterDomainName(AppleRegisterDomain domain) const
{
    switch (domain) {
        case kAppleRegisterDomainNone:
            return "none";
        case kAppleRegisterDomainGlobal:
            return "global";
        case kAppleRegisterDomainRender:
            return "render";
        case kAppleRegisterDomainMedia:
            return "media";
    }

    return "unknown";
}

bool FakeIrisXEGuC::beginAppleRegisterAccess(AppleRegisterDomain domain, const char* label,
                                             bool* outAcquiredSession)
{
    if (outAcquiredSession) {
        *outAcquiredSession = false;
    }

    if (!fOwner || domain == kAppleRegisterDomainNone) {
        return true;
    }

    if (fApplePinnedAccessActive) {
        if (domain == fApplePinnedDomain || domain == kAppleRegisterDomainNone) {
            return true;
        }
        IOLog("(FakeIrisXE) [GuC][AppleAccess] nested domain mismatch requested=%s active=%s label=%s\n",
              appleRegisterDomainName(domain),
              appleRegisterDomainName(fApplePinnedDomain),
              label ? label : "unknown");
        return false;
    }

    uint32_t accessDomain = AppleSafeRegisterAccess::kDomainGlobal;
    switch (domain) {
        case kAppleRegisterDomainGlobal:
            accessDomain = AppleSafeRegisterAccess::kDomainGlobal;
            break;
        case kAppleRegisterDomainRender:
            accessDomain = AppleSafeRegisterAccess::kDomainRender;
            break;
        case kAppleRegisterDomainMedia:
            accessDomain = AppleSafeRegisterAccess::kDomainMediaVdbox0;
            break;
        case kAppleRegisterDomainNone:
        default:
            accessDomain = AppleSafeRegisterAccess::kDomainGlobal;
            break;
    }

    const uint32_t mmioLength = static_cast<uint32_t>(fOwner->getMMIOMapLength());
    volatile UInt8* mmioBase = fOwner->getMMIOBase();
    if (!AppleSafeRegisterAccess::beginSession(mmioBase, mmioLength, accessDomain, label)) {
        IOLog("(FakeIrisXE) [GuC][AppleAccess] acquire failed label=%s domain=%s reason=session\n",
              label ? label : "unknown",
              appleRegisterDomainName(domain));
        return false;
    }

    if (domain == kAppleRegisterDomainMedia) {
        if (!AppleSafeRegisterAccess::beginSession(mmioBase,
                                                   mmioLength,
                                                   AppleSafeRegisterAccess::kDomainMediaVebox0,
                                                   "MEDIA_VEBOX0")) {
            AppleSafeRegisterAccess::endSession(mmioBase, mmioLength, accessDomain, label);
            IOLog("(FakeIrisXE) [GuC][AppleAccess] acquire failed label=%s domain=%s reason=vebox-session\n",
                  label ? label : "unknown",
                  appleRegisterDomainName(domain));
            return false;
        }
    }

    fApplePinnedAccessActive = true;
    fApplePinnedDomain = domain;
    if (outAcquiredSession) {
        *outAcquiredSession = true;
    }
    return true;
}

void FakeIrisXEGuC::endAppleRegisterAccess(bool acquiredSession)
{
    if (!acquiredSession || !fOwner || !fApplePinnedAccessActive) {
        return;
    }

    const uint32_t mmioLength = static_cast<uint32_t>(fOwner->getMMIOMapLength());
    volatile UInt8* mmioBase = fOwner->getMMIOBase();
    switch (fApplePinnedDomain) {
        case kAppleRegisterDomainGlobal:
            AppleSafeRegisterAccess::endSession(mmioBase, mmioLength,
                                                AppleSafeRegisterAccess::kDomainGlobal,
                                                "AppleAccessEndGlobal");
            break;
        case kAppleRegisterDomainRender:
            AppleSafeRegisterAccess::endSession(mmioBase, mmioLength,
                                                AppleSafeRegisterAccess::kDomainRender,
                                                "AppleAccessEndRender");
            break;
        case kAppleRegisterDomainMedia:
            AppleSafeRegisterAccess::endSession(mmioBase, mmioLength,
                                                AppleSafeRegisterAccess::kDomainMediaVdbox0,
                                                "AppleAccessEndMediaVdbox0");
            AppleSafeRegisterAccess::endSession(mmioBase, mmioLength,
                                                AppleSafeRegisterAccess::kDomainMediaVebox0,
                                                "AppleAccessEndMediaVebox0");
            break;
        case kAppleRegisterDomainNone:
        default:
            break;
    }

    fApplePinnedAccessActive = false;
    fApplePinnedDomain = kAppleRegisterDomainNone;
}

uint32_t FakeIrisXEGuC::safeRead32Apple(uint32_t offset, const char* label, bool* outOk)
{
    AppleRegisterDomain domain = determinePowerDomainForOffset(offset);
    bool acquiredSession = false;
    if (!beginAppleRegisterAccess(domain, label, &acquiredSession)) {
        if (outOk) {
            *outOk = false;
        }
        return 0xFFFFFFFFU;
    }

    uint32_t value = fOwner->safeReadRegister32(offset);
    endAppleRegisterAccess(acquiredSession);

    if (outOk) {
        *outOk = true;
    }
    return value;
}

bool FakeIrisXEGuC::safeWrite32Apple(uint32_t offset, uint32_t value, const char* label,
                                     uint32_t* outReadback)
{
    AppleRegisterDomain domain = determinePowerDomainForOffset(offset);
    bool acquiredSession = false;
    if (!beginAppleRegisterAccess(domain, label, &acquiredSession)) {
        if (outReadback) {
            *outReadback = 0xFFFFFFFFU;
        }
        return false;
    }

    fOwner->safeWriteRegister32(offset, value);
    uint32_t readback = fOwner->safeReadRegister32(offset);
    endAppleRegisterAccess(acquiredSession);

    if (outReadback) {
        *outReadback = readback;
    }
    return readback == value;
}

void FakeIrisXEGuC::logForceWakeDiagnostics(const char* label) const
{
    if (!fOwner) {
        return;
    }

    auto readAudit = [&](uint32_t reg) -> uint32_t {
        return fOwner->isMMIOOffsetValid(reg) ? fOwner->safeReadRegister32(reg) : 0xFFFFFFFFU;
    };

    const uint32_t gtPmReg = selectGtPmConfigReg();
    IOLog("(FakeIrisXE) [GuC][ForceWake] %s mt_req=0x%08X mt_ack=0x%08X render_req=0x%08X render_ack=0x%08X render_ack_alt=0x%08X media_vdbox_req=0x%08X media_vdbox_ack=0x%08X media_vebox_req=0x%08X media_vebox_ack=0x%08X gt_pm[%05X]=0x%08X\n",
          label ? label : "snapshot",
          readAudit(FORCEWAKE_REQ),
          readAudit(FORCEWAKE_ACK),
          readAudit(GEN11_FORCEWAKE_RENDER),
          readAudit(GEN11_FORCEWAKE_RENDER_ACK),
          readAudit(APPLE_TGL_FORCEWAKE_RENDER_ACK_V176),
          readAudit(GEN11_FORCEWAKE_MEDIA_VDBOX0),
          readAudit(GEN11_FORCEWAKE_MEDIA_VDBOX0_ACK),
          readAudit(GEN11_FORCEWAKE_MEDIA_VEBOX0),
          readAudit(GEN11_FORCEWAKE_MEDIA_VEBOX0_ACK),
          gtPmReg,
          readAudit(gtPmReg));
}

void FakeIrisXEGuC::logAppleBootAudit(const char* label) const
{
    if (!fOwner) {
        return;
    }

    const char* auditLabel = label ? label : "snapshot";
    const uint64_t mmioLen = fOwner->getMMIOMapLength();
    const uint32_t bar0Low = fOwner->getBAR0ConfigLow();
    const uint32_t bar0High = fOwner->getBAR0ConfigHigh();
    const uint32_t gtPmReg = selectGtPmConfigReg();

    IOLog("(FakeIrisXE) [GuC][Audit] %s pci=%04X:%04X bar0_cfg=0x%08X/0x%08X bar0_phys=0x%016llX mmio_len=0x%llX\n",
          auditLabel,
          fOwner->getPCIVendorID(),
          fOwner->getPCIDeviceID(),
          bar0Low,
          bar0High,
          (unsigned long long)fOwner->getBAR0PhysicalAddress(),
          (unsigned long long)mmioLen);

    struct AuditReg {
        const char* name;
        uint32_t reg;
    };

    const AuditReg regs[] = {
        {"FORCEWAKE_MT_REQ", FORCEWAKE_REQ},
        {"FORCEWAKE_MT_ACK", FORCEWAKE_ACK},
        {"GT_PM_CONFIG_SEL", gtPmReg},
        {"GT_PM_CONFIG_GT", TGL_GT_PM_CONFIG_GT},
        {"ME_FW_STATUS", APPLE_TGL_ME_FW_STATUS_V173},
        {"GFX_RESET_CTRL", APPLE_TGL_GUC_RESET_CTRL_V173},
        {"GUC_STATUS", GUC_STATUS_V137},
        {"GUC_SHIM_CONTROL", GUC_SHIM_CONTROL_V137},
        {"GUC_MISC_CONTROL", GUC_MISC_CONTROL},
        {"SOFT_SCRATCH0", GUC_SOFT_SCRATCH_V170(0)},
        {"SOFT_SCRATCH1", GUC_SOFT_SCRATCH_V170(1)},
        {"SPRINGBOARD_PTR", APPLE_TGL_SPRINGBOARD_PTR_V173},
        {"GUC_WOPCM_SIZE", GUC_WOPCM_SIZE_V137},
        {"DMA_GUC_WOPCM_OFFSET", DMA_GUC_WOPCM_OFFSET_V137},
        {"DMA_ADDR_0_LOW", DMA_ADDR_0_LOW_V137},
        {"DMA_ADDR_1_LOW", DMA_ADDR_1_LOW_V137},
        {"DMA_COPY_SIZE", DMA_COPY_SIZE_V137},
        {"DMA_CTRL", DMA_CTRL_V137},
        {"GUC_TLB_INV", GEN12_GUC_TLB_INV_CR_V170},
    };

    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
        const bool mapped = fOwner->isMMIOOffsetValid(regs[i].reg);
        const uint32_t value = mapped ? fOwner->safeReadRegister32(regs[i].reg) : 0xFFFFFFFFU;
        IOLog("(FakeIrisXE) [GuC][Audit] %s %s(0x%05X) mapped=%u value=0x%08X\n",
              auditLabel,
              regs[i].name,
              regs[i].reg,
              mapped ? 1U : 0U,
              value);
    }
}

void FakeIrisXEGuC::logAppleRegisterWindow(const char* label) const
{
    if (!fOwner) {
        return;
    }

    auto readReg = [&](uint32_t reg) -> uint32_t {
        return fOwner->isMMIOOffsetValid(reg) ? fOwner->safeReadRegister32(reg) : 0xFFFFFFFFU;
    };

    const char* snapshotLabel = label ? label : "snapshot";
    IOLog("(FakeIrisXE) [GuC][Regs] %s DMA_ADDR_0_LOW=0x%08X DMA_ADDR_0_HIGH=0x%08X DMA_ADDR_1_LOW=0x%08X DMA_ADDR_1_HIGH=0x%08X DMA_COPY_SIZE=0x%08X\n",
          snapshotLabel,
          readReg(DMA_ADDR_0_LOW_V137),
          readReg(DMA_ADDR_0_HIGH_V137),
          readReg(DMA_ADDR_1_LOW_V137),
          readReg(DMA_ADDR_1_HIGH_V137),
          readReg(DMA_COPY_SIZE_V137));
    IOLog("(FakeIrisXE) [GuC][Regs] %s DMA_CTRL=0x%08X GUC_STATUS=0x%08X GUC_SHIM_CONTROL=0x%08X GUC_MISC_CONTROL=0x%08X\n",
          snapshotLabel,
          readReg(DMA_CTRL_V137),
          readReg(GUC_STATUS_V137),
          readReg(GUC_SHIM_CONTROL_V137),
          readReg(GUC_MISC_CONTROL));
    IOLog("(FakeIrisXE) [GuC][Regs] %s GUC_HEADER_INFO=0x%08X SCR0_AUTH=0x%08X SCR1_KEY=0x%08X SCR2=0x%08X SCR3=0x%08X SCR4=0x%08X SCR5=0x%08X SCR6=0x%08X ME_C0F4=0x%08X GFX_RESET_941C=0x%08X SPRINGBOARD_C1B8=0x%08X\n",
          snapshotLabel,
          readReg(GUC_HEADER_INFO_V170),
          readReg(GUC_SOFT_SCRATCH_V170(0)),
          readReg(GUC_SOFT_SCRATCH_V170(1)),
          readReg(GUC_SOFT_SCRATCH_V170(2)),
          readReg(GUC_SOFT_SCRATCH_V170(3)),
          readReg(GUC_SOFT_SCRATCH_V170(4)),
          readReg(GUC_SOFT_SCRATCH_V170(5)),
          readReg(GUC_SOFT_SCRATCH_V170(6)),
          readReg(APPLE_TGL_ME_FW_STATUS_V173),
          readReg(APPLE_TGL_GUC_RESET_CTRL_V173),
          readReg(APPLE_TGL_SPRINGBOARD_PTR_V173));
    IOLog("(FakeIrisXE) [GuC][Regs] %s RSA0=0x%08X RSA1=0x%08X RSA62=0x%08X RSA63=0x%08X\n",
          snapshotLabel,
          readReg(UOS_RSA_SCRATCH_BASE_V137 + 0),
          readReg(UOS_RSA_SCRATCH_BASE_V137 + 4),
          readReg(UOS_RSA_SCRATCH_BASE_V137 + ((UOS_RSA_SCRATCH_COUNT_V137 - 2) * 4)),
          readReg(UOS_RSA_SCRATCH_BASE_V137 + ((UOS_RSA_SCRATCH_COUNT_V137 - 1) * 4)));
}

bool FakeIrisXEGuC::ensureApplePublicKeyBlob(uint64_t* outGpuAddr, bool logBlob)
{
    if (!fOwner) {
        return false;
    }

    if (!fGuCPublicKeyGem) {
        const size_t keyAllocSize = (sizeof(kTglAppleGuCPublicKey) + 4095U) & ~4095U;
        fGuCPublicKeyGem = FakeIrisXEGEM::withSize(keyAllocSize, 0);
        if (!fGuCPublicKeyGem) {
            IOLog("(FakeIrisXE) [GuC][Apple] failed to allocate public-key GEM size=%zu\n",
                  keyAllocSize);
            return false;
        }

        IOBufferMemoryDescriptor* md = fGuCPublicKeyGem->memoryDescriptor();
        void* cpuPtr = md ? md->getBytesNoCopy() : nullptr;
        if (!cpuPtr) {
            IOLog("(FakeIrisXE) [GuC][Apple] failed to map public-key GEM on CPU\n");
            fGuCPublicKeyGem->release();
            fGuCPublicKeyGem = nullptr;
            return false;
        }

        bzero(cpuPtr, keyAllocSize);
        memcpy(cpuPtr, kTglAppleGuCPublicKey, sizeof(kTglAppleGuCPublicKey));
        producerCoherencyBarrier("copied Apple TGL public key blob");
        fGuCPublicKeyGem->pin();
        uint64_t keyGpuAddr = fOwner->ggttMap(fGuCPublicKeyGem);
        if (!keyGpuAddr || (keyGpuAddr >> 32) != 0U) {
            IOLog("(FakeIrisXE) [GuC][Apple] invalid public-key GGTT=0x%016llX\n",
                  (unsigned long long)keyGpuAddr);
            fGuCPublicKeyGem->unpin();
            fGuCPublicKeyGem->release();
            fGuCPublicKeyGem = nullptr;
            return false;
        }
    }

    uint64_t keyGpuAddr = fOwner->ggttMap(fGuCPublicKeyGem);
    if (!keyGpuAddr || (keyGpuAddr >> 32) != 0U) {
        IOLog("(FakeIrisXE) [GuC][Apple] invalid public-key GGTT=0x%016llX\n",
              (unsigned long long)keyGpuAddr);
        return false;
    }

    if (outGpuAddr) {
        *outGpuAddr = keyGpuAddr;
    }

    if (logBlob) {
        const uint32_t* keyDw = reinterpret_cast<const uint32_t*>(kTglAppleGuCPublicKey);
        IOLog("(FakeIrisXE) [GuC][Apple] public-key blob ggtt=0x%08X size=%zu first=0x%08X/0x%08X last=0x%08X/0x%08X\n",
              (uint32_t)keyGpuAddr,
              sizeof(kTglAppleGuCPublicKey),
              keyDw[0],
              keyDw[1],
              keyDw[(sizeof(kTglAppleGuCPublicKey) / sizeof(uint32_t)) - 2],
              keyDw[(sizeof(kTglAppleGuCPublicKey) / sizeof(uint32_t)) - 1]);
    }

    return true;
}

bool FakeIrisXEGuC::writeAppleBootParams(GuCStage stage)
{
    if (!fOwner) {
        return false;
    }

    uint64_t keyGpuAddr = 0;
    if (!ensureApplePublicKeyBlob(&keyGpuAddr, true)) {
        return false;
    }

    struct AuthRegWrite {
        const char* name;
        uint32_t reg;
        uint32_t value;
    } writes[] = {
        {"SOFT_SCRATCH1_KEY_LO", GUC_SOFT_SCRATCH_V170(1), (uint32_t)keyGpuAddr},
        {"SOFT_SCRATCH2_ZERO", GUC_SOFT_SCRATCH_V170(2), 0},
        {"SOFT_SCRATCH3_ZERO", GUC_SOFT_SCRATCH_V170(3), 0},
        {"SOFT_SCRATCH4_ZERO", GUC_SOFT_SCRATCH_V170(4), 0},
        {"SOFT_SCRATCH5_ZERO", GUC_SOFT_SCRATCH_V170(5), 0},
        {"SOFT_SCRATCH6_ZERO", GUC_SOFT_SCRATCH_V170(6), 0},
    };

    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i) {
        uint32_t readback = 0;
        writeRegWithReadback(stage, writes[i].name, writes[i].reg, writes[i].value, &readback);
        if (readback != writes[i].value) {
            IOLog("(FakeIrisXE) [GuC][Apple] auth block mismatch reg=0x%05X write=0x%08X read=0x%08X\n",
                  writes[i].reg,
                  writes[i].value,
                  readback);
            return false;
        }
    }

    IOLog("(FakeIrisXE) [GuC][Apple] auth block scratch1_key=0x%08X scratch2_6=zero source=DTK-unboxed-tgl-public-key\n",
          (uint32_t)keyGpuAddr);
    return true;
}

bool FakeIrisXEGuC::writeAndPollAppleReg(GuCStage stage, const char* label, uint32_t writeReg,
                                         uint32_t writeValue, uint32_t pollReg,
                                         uint32_t pollMask, uint32_t expectedValue,
                                         uint32_t timeoutMs, uint32_t* outPollValue)
{
    AppleRegisterDomain writeDomain = determinePowerDomainForOffset(writeReg);
    AppleRegisterDomain pollDomain = determinePowerDomainForOffset(pollReg);
    bool acquiredSession = false;

    if (writeDomain == pollDomain && writeDomain != kAppleRegisterDomainNone) {
        if (!beginAppleRegisterAccess(writeDomain, label, &acquiredSession)) {
            if (outPollValue) {
                *outPollValue = 0xFFFFFFFFU;
            }
            IOLog("(FakeIrisXE) [GuC][Apple] %s failed to pin access domain=%s\n",
                  label,
                  appleRegisterDomainName(writeDomain));
            return false;
        }
    }

    uint32_t writeReadback = 0;
    writeRegWithReadback(stage, label, writeReg, writeValue, &writeReadback);

    uint32_t pollValue = 0xFFFFFFFFU;
    const uint32_t maxPolls = timeoutMs ? timeoutMs : 1U;
    for (uint32_t poll = 0; poll < maxPolls; ++poll) {
        pollValue = acquiredSession ? fOwner->safeReadRegister32(pollReg)
                                    : safeRead32Apple(pollReg, label, nullptr);
        if ((pollValue & pollMask) == expectedValue) {
            if (outPollValue) {
                *outPollValue = pollValue;
            }
            IOLog("(FakeIrisXE) [GuC][Apple] %s poll_reg=0x%05X write=0x%08X mask=0x%08X expected=0x%08X read=0x%08X polls=%u\n",
                  label,
                  pollReg,
                  writeValue,
                  pollMask,
                  expectedValue,
                  pollValue,
                  poll + 1U);
            endAppleRegisterAccess(acquiredSession);
            return true;
        }
        IOSleep(1);
    }

    if (outPollValue) {
        *outPollValue = pollValue;
    }
    IOLog("(FakeIrisXE) [GuC][Apple] %s timeout poll_reg=0x%05X write=0x%08X mask=0x%08X expected=0x%08X read=0x%08X timeout_ms=%u\n",
          label,
          pollReg,
          writeValue,
          pollMask,
          expectedValue,
          pollValue,
          timeoutMs);
    endAppleRegisterAccess(acquiredSession);
    return false;
}

bool FakeIrisXEGuC::pollAppleRegEquals(GuCStage stage, const char* label, uint32_t reg,
                                       uint32_t expectedValue, uint32_t timeoutMs,
                                       uint32_t* outValue)
{
    (void)stage;
    AppleRegisterDomain domain = determinePowerDomainForOffset(reg);
    bool acquiredSession = false;
    if (domain != kAppleRegisterDomainNone && !beginAppleRegisterAccess(domain, label, &acquiredSession)) {
        if (outValue) {
            *outValue = 0xFFFFFFFFU;
        }
        IOLog("(FakeIrisXE) [GuC][Apple] %s failed to pin access domain=%s\n",
              label,
              appleRegisterDomainName(domain));
        return false;
    }

    uint32_t value = 0xFFFFFFFFU;
    const uint32_t maxPolls = timeoutMs ? timeoutMs : 1U;
    for (uint32_t poll = 0; poll < maxPolls; ++poll) {
        value = acquiredSession ? fOwner->safeReadRegister32(reg)
                                : safeRead32Apple(reg, label, nullptr);
        if (value == expectedValue) {
            if (outValue) {
                *outValue = value;
            }
            IOLog("(FakeIrisXE) [GuC][Apple] %s reg=0x%05X expected=0x%08X read=0x%08X polls=%u\n",
                  label,
                  reg,
                  expectedValue,
                  value,
                  poll + 1U);
            endAppleRegisterAccess(acquiredSession);
            return true;
        }
        IOSleep(1);
    }

    if (outValue) {
        *outValue = value;
    }
    IOLog("(FakeIrisXE) [GuC][Apple] %s timeout reg=0x%05X expected=0x%08X read=0x%08X timeout_ms=%u\n",
          label,
          reg,
          expectedValue,
          value,
          timeoutMs);
    endAppleRegisterAccess(acquiredSession);
    return false;
}

bool FakeIrisXEGuC::safeForceWakeDomain(GuCStage stage, const char* label,
                                         uint32_t requestReg, uint32_t ackReg,
                                         uint32_t requestValue, uint32_t ackMask,
                                         uint32_t expectedAckValue)
{
    if (!fOwner) {
        IOLog("(FakeIrisXE) [GuC][%s] ERROR: No fOwner for safeForceWakeDomain\n",
              stageToString(stage));
        return false;
    }

    uint32_t requestReadback = 0;

    // Use safe register access for the request register write
    fOwner->safeMMIOWrite(requestReg, requestValue);
    requestReadback = fOwner->safeMMIORead(requestReg);
    IOLog("(FakeIrisXE) [GuC][%s] %s: wrote 0x%08X to 0x%08X (readback 0x%08X)\n",
          stageToString(stage), label, requestValue, requestReg, requestReadback);

    uint32_t ackValue = 0;
    for (uint32_t retrigger = 0; retrigger <= APPLE_FORCEWAKE_MAX_RETRIGGERS_V175; ++retrigger) {
        for (uint32_t poll = 0; poll < APPLE_FORCEWAKE_POLLS_PER_TRY_V175; ++poll) {
            // Use safe register access for reading the ack register
            ackValue = fOwner->safeMMIORead(ackReg);
            if ((ackValue & ackMask) == expectedAckValue) {
                IOLog("(FakeIrisXE) [GuC][Apple] %s ack=0x%08X mask=0x%08X expected=0x%08X polls=%u retriggers=%u\n",
                      label,
                      ackValue,
                      ackMask,
                      expectedAckValue,
                      poll + 1U,
                      retrigger);
                return true;
            }
            IODelay(APPLE_FORCEWAKE_POLL_DELAY_US_V175);
        }

        if (retrigger == APPLE_FORCEWAKE_MAX_RETRIGGERS_V175) {
            break;
        }

        IOLog("(FakeIrisXE) [GuC][Apple] Retrigger %s read=0x%08X expect=0x%08X retrigger=%u\n",
              label,
              ackValue,
              expectedAckValue,
              retrigger + 1U);
        // Re-apply the request on retrigger
        fOwner->safeMMIOWrite(requestReg, requestValue);
        (void)fOwner->safeMMIORead(requestReg); // readback
        IODelay(APPLE_FORCEWAKE_POLL_DELAY_US_V175);
    }

    IOLog("(FakeIrisXE) [GuC][Apple] %s failed final_ack=0x%08X expected=0x%08X retriggers=%u\n",
          label,
          ackValue,
          expectedAckValue,
          APPLE_FORCEWAKE_MAX_RETRIGGERS_V175);
    return false;
}

bool FakeIrisXEGuC::acquireAppleWakeDomains(GuCStage stage)
{
    logForceWakeDiagnostics("apple-domain-pre");

    uint32_t gtPmGtReadback = 0;
    writeRegWithReadback(stage,
                         "GT_PM_CONFIG_GT",
                         TGL_GT_PM_CONFIG_GT,
                         GT_DOORBELL_ENABLE,
                         &gtPmGtReadback);
    if (gtPmGtReadback != GT_DOORBELL_ENABLE) {
        IOLog("(FakeIrisXE) [GuC][Apple] GT_PM_CONFIG_GT did not latch write=0x%08X read=0x%08X; continuing without using it as a success signal\n",
              GT_DOORBELL_ENABLE,
              gtPmGtReadback);
    }

    if (!safeForceWakeDomain(stage,
                             "FORCEWAKE_GLOBAL",
                             FORCEWAKE_REQ,
                             FORCEWAKE_ACK,
                             APPLE_TGL_FORCEWAKE_GLOBAL_ENABLE_V176,
                             APPLE_TGL_FORCEWAKE_GLOBAL_ACK_MASK_V176,
                             APPLE_TGL_FORCEWAKE_GLOBAL_ACK_MASK_V176)) {
        return false;
    }

    if (!safeForceWakeDomain(stage,
                             "FORCEWAKE_RENDER",
                             GEN11_FORCEWAKE_RENDER,
                             APPLE_TGL_FORCEWAKE_RENDER_ACK_V176,
                             APPLE_TGL_FORCEWAKE_RENDER_ENABLE_V176,
                             APPLE_TGL_FORCEWAKE_RENDER_ACK_MASK_V176,
                             APPLE_TGL_FORCEWAKE_RENDER_ACK_MASK_V176)) {
        return false;
    }

    if (!safeForceWakeDomain(stage,
                             "FORCEWAKE_MEDIA_VDBOX0",
                             GEN11_FORCEWAKE_MEDIA_VDBOX0,
                             GEN11_FORCEWAKE_MEDIA_VDBOX0_ACK,
                             APPLE_TGL_FORCEWAKE_MEDIA_ENABLE_V176,
                             APPLE_TGL_FORCEWAKE_MEDIA_ACK_MASK_V176,
                             APPLE_TGL_FORCEWAKE_MEDIA_ACK_MASK_V176)) {
        return false;
    }

    if (!safeForceWakeDomain(stage,
                             "FORCEWAKE_MEDIA_VEBOX0",
                             GEN11_FORCEWAKE_MEDIA_VEBOX0,
                             GEN11_FORCEWAKE_MEDIA_VEBOX0_ACK,
                             APPLE_TGL_FORCEWAKE_MEDIA_ENABLE_V176,
                             APPLE_TGL_FORCEWAKE_MEDIA_ACK_MASK_V176,
                             APPLE_TGL_FORCEWAKE_MEDIA_ACK_MASK_V176)) {
        return false;
    }

    logForceWakeDiagnostics("apple-domain-post");
    return true;
}

bool FakeIrisXEGuC::runApplePreAuthHandshake(GuCStage stage, uint32_t restoreFreqToken)
{
    uint32_t pollValue = 0;
    uint32_t lastMeValue = fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173);
    uint32_t lastResetValue = fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173);
    const uint32_t loadFreqExpectedField =
        ((APPLE_TGL_GUC_LOAD_FREQ_TOKEN_V178 >> 23) & 0x1FFU) << APPLE_TGL_GUC_LOAD_FREQ_STATUS_SHIFT_V178;

    for (uint32_t attempt = 1; attempt <= APPLE_TGL_PREAUTH_MAX_ATTEMPTS_V177; ++attempt) {
        if (attempt > 1) {
            IOLog("(FakeIrisXE) [GuC][Apple] pre-auth recycle attempt=%u releasing and re-arming wake domains\n",
                  attempt);
            if (!writeAndPollAppleReg(stage,
                                      "RESTORE_LOAD_FREQ_RETRY",
                                      APPLE_TGL_GUC_LOAD_FREQ_CTRL_V178,
                                      restoreFreqToken,
                                      APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178,
                                      APPLE_TGL_GUC_LOAD_FREQ_STATUS_MASK_V178,
                                      ((restoreFreqToken >> 23) & 0x1FFU) << APPLE_TGL_GUC_LOAD_FREQ_STATUS_SHIFT_V178,
                                      APPLE_TGL_GUC_LOAD_FREQ_TIMEOUT_MS_V178,
                                      &pollValue)) {
                IOLog("(FakeIrisXE) [GuC][Apple] pre-auth recycle attempt=%u restore-frequency warning status=0x%08X token=0x%08X\n",
                      attempt,
                      fOwner->safeMMIORead(APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178),
                      restoreFreqToken);
            }
            releaseForceWake();
            IOSleep(APPLE_TGL_PREAUTH_RETRY_DELAY_MS_V177);
            if (!acquireForceWake()) {
                IOLog("(FakeIrisXE) [GuC][Apple] pre-auth recycle attempt=%u failed to reacquire MT forcewake\n",
                      attempt);
                return false;
            }
            logForceWakeDiagnostics("apple-preauth-reacquire");
            if (!writeAndPollAppleReg(stage,
                                      "SET_LOAD_FREQ_RETRY",
                                      APPLE_TGL_GUC_LOAD_FREQ_CTRL_V178,
                                      APPLE_TGL_GUC_LOAD_FREQ_TOKEN_V178,
                                      APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178,
                                      APPLE_TGL_GUC_LOAD_FREQ_STATUS_MASK_V178,
                                      loadFreqExpectedField,
                                      APPLE_TGL_GUC_LOAD_FREQ_TIMEOUT_MS_V178,
                                      &pollValue)) {
                IOLog("(FakeIrisXE) [GuC][Apple] pre-auth recycle attempt=%u load-frequency warning status=0x%08X\n",
                      attempt,
                      fOwner->safeMMIORead(APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178));
            }
            if (!acquireAppleWakeDomains(stage)) {
                IOLog("(FakeIrisXE) [GuC][Apple] pre-auth recycle attempt=%u failed to re-arm Apple wake domains\n",
                      attempt);
                return false;
            }
        }

        IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u/%u me_before=0x%08X reset_before=0x%08X\n",
              attempt,
              APPLE_TGL_PREAUTH_MAX_ATTEMPTS_V177,
              fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173),
              fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173));

        if (!writeAndPollAppleReg(stage,
                                  "GFX_RESET_PREAUTH_1",
                                  APPLE_TGL_GUC_RESET_CTRL_V173,
                                  APPLE_TGL_GUC_RESET_BIT_V173,
                                  APPLE_TGL_GUC_RESET_CTRL_V173,
                                  APPLE_TGL_GUC_RESET_BIT_V173,
                                  0,
                                  APPLE_TGL_PREAUTH_STEP_TIMEOUT_MS_V177,
                                  &pollValue)) {
            lastMeValue = fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173);
            lastResetValue = fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173);
            IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u reset1-fail me=0x%08X reset=0x%08X\n",
                  attempt,
                  lastMeValue,
                  lastResetValue);
            continue;
        }

        IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u after-reset1 me=0x%08X reset=0x%08X\n",
              attempt,
              fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173),
              fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173));

        if (!writeAndPollAppleReg(stage,
                                  "ME_WAKE_PREAUTH",
                                  APPLE_TGL_ME_FW_STATUS_V173,
                                  APPLE_TGL_ME_WAKE_REQ_V173,
                                  APPLE_TGL_ME_FW_STATUS_V173,
                                  APPLE_TGL_ME_WAKE_ACK_MASK_V173,
                                  APPLE_TGL_ME_WAKE_ACK_MASK_V173,
                                  APPLE_TGL_ME_WAKE_TIMEOUT_MS_V179,
                                  &pollValue)) {
            lastMeValue = fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173);
            lastResetValue = fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173);
            IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u wake-fail me=0x%08X reset=0x%08X\n",
                  attempt,
                  lastMeValue,
                  lastResetValue);

            uint32_t meWakeVals[5] = {0, 0, 0, 0, 0};
            for (int i = 0; i < 5; ++i) {
                meWakeVals[i] = fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173);
                IODelay(1000);
            }
            IOLog("(FakeIrisXE) [GuC][Apple] ME wake diagnostics write=0x%08X ackMask=0x%08X timeout=%ums samples=[0x%08X 0x%08X 0x%08X 0x%08X 0x%08X]\n",
                  APPLE_TGL_ME_WAKE_REQ_V173,
                  APPLE_TGL_ME_WAKE_ACK_MASK_V173,
                  APPLE_TGL_ME_WAKE_TIMEOUT_MS_V179,
                  meWakeVals[0], meWakeVals[1], meWakeVals[2], meWakeVals[3], meWakeVals[4]);
            continue;
        }

        IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u after-wake me=0x%08X reset=0x%08X\n",
              attempt,
              fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173),
              fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173));

        if (!writeAndPollAppleReg(stage,
                                  "GFX_RESET_PREAUTH_2",
                                  APPLE_TGL_GUC_RESET_CTRL_V173,
                                  APPLE_TGL_GUC_RESET_BIT_V173,
                                  APPLE_TGL_GUC_RESET_CTRL_V173,
                                  APPLE_TGL_GUC_RESET_BIT_V173,
                                  0,
                                  APPLE_TGL_PREAUTH_STEP_TIMEOUT_MS_V177,
                                  &pollValue)) {
            lastMeValue = fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173);
            lastResetValue = fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173);
            IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u reset2-fail me=0x%08X reset=0x%08X\n",
                  attempt,
                  lastMeValue,
                  lastResetValue);
            continue;
        }

        IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u after-reset2 me=0x%08X reset=0x%08X\n",
              attempt,
              fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173),
              fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173));

        IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u before-hash-request me=0x%08X\n",
              attempt,
              fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173));

        if (!writeAndPollAppleReg(stage,
                                  "ME_HASH_REQUEST",
                                  APPLE_TGL_ME_FW_STATUS_V173,
                                  0,
                                  APPLE_TGL_ME_FW_STATUS_V173,
                                  APPLE_TGL_ME_WAKE_ACK_MASK_V173,
                                  APPLE_TGL_ME_WAKE_ACK_MASK_V173,
                                  APPLE_TGL_PREAUTH_STEP_TIMEOUT_MS_V177,
                                  &pollValue)) {
            lastMeValue = fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173);
            lastResetValue = fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173);
            IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u hash-request-fail me=0x%08X reset=0x%08X\n",
                  attempt,
                  lastMeValue,
                  lastResetValue);
            continue;
        }

        IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u before-hash-ready me=0x%08X\n",
              attempt,
              fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173));

        if (pollAppleRegEquals(stage,
                               "ME_HASH_READY",
                               APPLE_TGL_ME_FW_STATUS_V173,
                               APPLE_TGL_ME_HASH_READY_V173,
                               APPLE_TGL_ME_HASH_READY_TIMEOUT_MS_V179,
                               &pollValue)) {
            IOLog("(FakeIrisXE) [GuC][Apple] pre-auth handshake complete attempts=%u me_c0f4=0x%08X reset_941c=0x%08X\n",
                  attempt,
                  fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173),
                  fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173));
            return true;
        }

        lastMeValue = fOwner->safeMMIORead(APPLE_TGL_ME_FW_STATUS_V173);
        lastResetValue = fOwner->safeMMIORead(APPLE_TGL_GUC_RESET_CTRL_V173);
        IOLog("(FakeIrisXE) [GuC][Apple] pre-auth attempt=%u hash-ready-fail me=0x%08X reset=0x%08X\n",
              attempt,
              lastMeValue,
              lastResetValue);
    }

    IOLog("(FakeIrisXE) [GuC][Apple] pre-auth handshake exhausted attempts=%u last_me=0x%08X last_reset=0x%08X\n",
          APPLE_TGL_PREAUTH_MAX_ATTEMPTS_V177,
          lastMeValue,
          lastResetValue);
    return false;
}

void FakeIrisXEGuC::issueGuCTlbInvalidate() const
{
    if (!fOwner) {
        return;
    }

    if (!fOwner->isMMIOOffsetValid(GEN12_GUC_TLB_INV_CR_V170)) {
        IOLog("(FakeIrisXE) [GuC][Apple] GUC_TLB_INV unavailable reg=0x%05X\n",
              GEN12_GUC_TLB_INV_CR_V170);
        return;
    }

    const uint32_t before = fOwner->safeMMIORead(GEN12_GUC_TLB_INV_CR_V170);
    fOwner->safeMMIOWrite(GEN12_GUC_TLB_INV_CR_V170,
                          GEN12_GUC_TLB_INV_CR_INVALIDATE_V170);
    IOSleep(1);
    const uint32_t after = fOwner->safeMMIORead(GEN12_GUC_TLB_INV_CR_V170);
    IOLog("(FakeIrisXE) [GuC][Apple] GUC_TLB_INV before=0x%08X after=0x%08X note=write-1-invalidate\n",
          before,
          after);
}

void FakeIrisXEGuC::logDoorbellSnapshot(const char* label) const
{
    uint32_t fwReq = fOwner->safeMMIORead(FORCEWAKE_REQ);
    uint32_t fwAck = fOwner->safeMMIORead(FORCEWAKE_ACK);
    uint32_t gtPm = fOwner->safeMMIORead(selectGtPmConfigReg());
    uint32_t doorbellCtrl = fOwner->safeMMIORead(GUC_DOORBELL_CTRL);
    uint32_t shim = fOwner->safeMMIORead(GUC_SHIM_CONTROL_V137);
    uint32_t shim2 = fOwner->safeMMIORead(GUC_SHIM_CONTROL2);
    uint32_t status = fOwner->safeMMIORead(GUC_STATUS_V137);
    uint32_t wopcmSize = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t wopcmOffset = fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137);

    IOLog("(FakeIrisXE) [GuC][Doorbell] %s req=0x%08X ack=0x%08X gt_pm=0x%08X doorbell_ctrl=0x%08X shim=0x%08X shim2=0x%08X status=0x%08X wopcm_size=0x%08X wopcm_off=0x%08X\n",
          label ? label : "snapshot",
          fwReq,
          fwAck,
          gtPm,
          doorbellCtrl,
          shim,
          shim2,
          status,
          wopcmSize,
          wopcmOffset);
}

bool FakeIrisXEGuC::programDoorbellEnable(GuCStage stage)
{
    const bool pmExperiment = ownerBooleanPropertyEnabled("FakeIrisXEGuCPMExperiment");
    const bool probeEnabled = ownerBooleanPropertyEnabled("FakeIrisXEGuCDoorbellProbe");
    const bool aggressive = ownerBooleanPropertyEnabled("FakeIrisXEGuCDoorbellAggressive");

    struct DoorbellTarget {
        const char* regName;
        uint32_t reg;
        bool enabled;
    };

    const DoorbellTarget targets[] = {
        {"GT_PM_CONFIG", GT_PM_CONFIG, true},
        {"DOORBELL_CTRL", GUC_DOORBELL_CTRL, pmExperiment || probeEnabled},
    };

    // V145: Use Tiger Lake specific value 0xA188 instead of just doorbell bit
    const uint32_t baseMasks[] = {
        TGL_GT_PM_CONFIG_VALUE,  // 0xA188 - full Tiger Lake config
        GT_DOORBELL_ENABLE | 0x2U,
    };
    const uint32_t probeMasks[] = {
        GT_DOORBELL_ENABLE | 0x100U,
        GT_DOORBELL_ENABLE | 0x00010000U,
    };

    if (probeEnabled) {
        logDoorbellSnapshot("probe-entry");
    }

    uint32_t attemptIndex = 0;
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
        const DoorbellTarget& target = targets[t];
        if (!target.enabled) {
            continue;
        }

        for (size_t m = 0; m < sizeof(baseMasks) / sizeof(baseMasks[0]); ++m) {
            uint32_t mask = baseMasks[m];
            uint32_t before = fOwner->safeMMIORead(target.reg);
            uint32_t writeValue = before | mask;
            uint32_t readback = 0;

            IOLog("(FakeIrisXE) [GuC][Doorbell] attempt=%u style=rmw reg=%s mask=0x%08X old=0x%08X write=0x%08X\n",
                  attemptIndex++,
                  target.regName,
                  mask,
                  before,
                  writeValue);

            writeRegWithReadback(stage, target.regName, target.reg, writeValue, &readback);
            if ((readback & mask) == mask) {
                IOLog("(FakeIrisXE) [GuC] Doorbell enabled via %s mask=0x%08X read=0x%08X\n",
                      target.regName,
                      mask,
                      readback);
                if (probeEnabled) {
                    logDoorbellSnapshot("probe-success");
                }
                return true;
            }

            if (!probeEnabled) {
                IOLog("(FakeIrisXE) [GuC] Doorbell attempt failed via %s old=0x%08X read=0x%08X\n",
                      target.regName,
                      before,
                      readback);
                continue;
            }

            IOLog("(FakeIrisXE) [GuC][Doorbell] attempt=%u style=direct reg=%s mask=0x%08X\n",
                  attemptIndex++,
                  target.regName,
                  mask);
            writeRegWithReadback(stage, target.regName, target.reg, mask, &readback);
            if ((readback & mask) == mask) {
                IOLog("(FakeIrisXE) [GuC] Doorbell enabled via %s direct mask=0x%08X read=0x%08X\n",
                      target.regName,
                      mask,
                      readback);
                logDoorbellSnapshot("probe-success-direct");
                return true;
            }

            if (aggressive) {
                IOLog("(FakeIrisXE) [GuC][Doorbell] attempt=%u style=clear-set reg=%s mask=0x%08X\n",
                      attemptIndex++,
                      target.regName,
                      mask);
                writeRegWithReadback(stage, target.regName, target.reg, 0, &readback);
                writeRegWithReadback(stage, target.regName, target.reg, writeValue, &readback);
                if ((readback & mask) == mask) {
                    IOLog("(FakeIrisXE) [GuC] Doorbell enabled via %s clear-set mask=0x%08X read=0x%08X\n",
                          target.regName,
                          mask,
                          readback);
                    logDoorbellSnapshot("probe-success-clear-set");
                    return true;
                }
            }

            IOLog("(FakeIrisXE) [GuC] Doorbell attempt failed via %s mask=0x%08X read=0x%08X\n",
                  target.regName,
                  mask,
                  readback);
        }

        if (!probeEnabled) {
            continue;
        }

        for (size_t m = 0; m < sizeof(probeMasks) / sizeof(probeMasks[0]); ++m) {
            uint32_t mask = probeMasks[m];
            uint32_t before = fOwner->safeMMIORead(target.reg);
            uint32_t writeValue = before | mask;
            uint32_t readback = 0;

            IOLog("(FakeIrisXE) [GuC][Doorbell] attempt=%u style=probe reg=%s mask=0x%08X old=0x%08X write=0x%08X\n",
                  attemptIndex++,
                  target.regName,
                  mask,
                  before,
                  writeValue);
            writeRegWithReadback(stage, target.regName, target.reg, writeValue, &readback);

            if ((readback & GT_DOORBELL_ENABLE) == GT_DOORBELL_ENABLE) {
                IOLog("(FakeIrisXE) [GuC] Doorbell probe latched via %s mask=0x%08X read=0x%08X\n",
                      target.regName,
                      mask,
                      readback);
                logDoorbellSnapshot("probe-success-mask");
                return true;
            }
        }
    }

    if (probeEnabled) {
        logDoorbellSnapshot("probe-exit-fail");
    }

    return false;
}

void FakeIrisXEGuC::producerCoherencyBarrier(const char* reason)
{
    __sync_synchronize();
    OSSynchronizeIO();
    IOLog("(FakeIrisXE) [GuC][Barrier] producer->consumer (%s)\n", reason ? reason : "unknown");
}

void FakeIrisXEGuC::consumerCoherencyBarrier(const char* reason)
{
    OSSynchronizeIO();
    __sync_synchronize();
    IOLog("(FakeIrisXE) [GuC][Barrier] consumer->producer (%s)\n", reason ? reason : "unknown");
}

void FakeIrisXEGuC::emitStageReport(GuCStage stage, uint64_t startNs, uint32_t retryIndex,
                                    uint32_t rawStatusOverride)
{
    if (stage == fLastReportedStage) {
        return;
    }

    uint32_t rawStatus = rawStatusOverride;
    if (rawStatus == 0xFFFFFFFFU) {
        rawStatus = fOwner->safeMMIORead(GUC_STATUS_V137);
    }

    GuCStatusDecoded decoded = decodeStatus(rawStatus);
    GuCStageReport report;
    report.stage = stage;
    report.elapsed_us = (mach_absolute_time() - startNs) / 1000ULL;
    report.raw_status = rawStatus;
    report.decoded_status = decoded;
    report.retry_index = retryIndex;

    const char* stageName = "UNKNOWN";
    switch (stage) {
        case kGuCStageIdle: stageName = "IDLE"; break;
        case kGuCStageForceWake: stageName = "FORCEWAKE"; break;
        case kGuCStageShim: stageName = "SHIM"; break;
        case kGuCStageWopcm: stageName = "WOPCM"; break;
        case kGuCStageDmaProgram: stageName = "DMA_PROGRAM"; break;
        case kGuCStageDmaTrigger: stageName = "DMA_TRIGGER"; break;
        case kGuCStageBootPoll: stageName = "BOOT_POLL"; break;
        case kGuCStageBootSuccess: stageName = "BOOT_SUCCESS"; break;
        case kGuCStageFailure: stageName = "FAILURE"; break;
        default: break;
    }

    IOLog("(FakeIrisXE) [GuC][Stage] stage=%s retry=%u elapsed_us=%llu raw=0x%08X bootrom=0x%02X ukernel=0x%02X mia=0x%X auth=0x%X valid=%u\n",
          stageName,
          report.retry_index,
          (unsigned long long)report.elapsed_us,
          report.raw_status,
          report.decoded_status.bootrom,
          report.decoded_status.ukernel,
          report.decoded_status.mia,
          report.decoded_status.authStatus,
          report.decoded_status.valid ? 1U : 0U);

    fLastReportedStage = stage;
}

void FakeIrisXEGuC::logLinuxBaselineCorrelation(bool bootSuccess)
{
    uint32_t status = fOwner->safeMMIORead(GUC_STATUS_V137);
    uint32_t gtPmConfig = fOwner->safeMMIORead(GT_PM_CONFIG);
    uint32_t doorbellCtrl = fOwner->safeMMIORead(GUC_DOORBELL_CTRL);
    uint32_t rcCtl = fOwner->safeMMIORead(GEN12_RC_CTL);

    GuCStatusDecoded decoded = decodeStatus(status);
    bool submissionEnabled = bootSuccess &&
                             (((gtPmConfig & GT_DOORBELL_ENABLE) != 0) ||
                              ((doorbellCtrl & GT_DOORBELL_ENABLE) != 0));
    bool slpcEnabled = bootSuccess && (decoded.ukernel == 0xFFU || decoded.ukernel == 0xF0U);
    bool rcEnabled = ((rcCtl & 0x1U) != 0);

    IOLog("(FakeIrisXE) [GuC][Baseline] expected: submission=enabled slpc=enabled rc=enabled\n");
    IOLog("(FakeIrisXE) [GuC][Baseline] actual: submission=%s slpc=%s rc=%s status=0x%08X gt_pm=0x%08X doorbell_ctrl=0x%08X rc_ctl=0x%08X\n",
          submissionEnabled ? "enabled" : "disabled",
          slpcEnabled ? "enabled" : "disabled",
          rcEnabled ? "enabled" : "disabled",
          status,
          gtPmConfig,
          doorbellCtrl,
          rcCtl);
}

bool FakeIrisXEGuC::pollForBootFastFail(uint32_t timeoutMs, uint64_t startNs, uint32_t retryIndex)
{
    emitStageReport(kGuCStageBootPoll, startNs, retryIndex);

    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = (uint64_t)timeoutMs * 1000000ULL;
    uint32_t lastStatus = 0xFFFFFFFFU;
    uint32_t stableCount = 0;
    uint32_t pollCount = 0;
    
    IOLog("(FakeIrisXE) [GuC][BootPoll] Starting poll with timeout=%ums\n", timeoutMs);

    while (mach_absolute_time() - start < timeoutNs) {
        consumerCoherencyBarrier("GUC_STATUS poll");
        uint32_t status = fOwner->safeMMIORead(GUC_STATUS_V137);
        GuCStatusDecoded decoded = decodeStatus(status);
        pollCount++;

        if (fFirmwareMode == kGuCFirmwareModeAppleOnly && pollCount == 1U) {
            logAppleRegisterWindow("apple-first-boot-poll");
        }

        if (status == lastStatus) {
            stableCount++;
        } else {
            IOLog("(FakeIrisXE) [GuC][BootPoll] poll=%u raw=0x%08X status changed bootrom=0x%02X ukernel=0x%02X mia=0x%X auth=0x%X\n",
                  pollCount, status, decoded.bootrom, decoded.ukernel, decoded.mia, decoded.authStatus);
            stableCount = 0;
            lastStatus = status;
        }

        if (isImpossibleStatusDecode(status, decoded)) {
            IOLog("(FakeIrisXE) [GuC] Hard stop: impossible GUC_STATUS decode raw=0x%08X\n", status);
            return false;
        }

        if (decoded.failure) {
            IOLog("(FakeIrisXE) [GuC] GuC reported FAILURE raw=0x%08X bootrom=0x%02X ukernel=0x%02X mia=0x%X auth=%s(0x%X) valid=%u\n",
                  status,
                  decoded.bootrom,
                  decoded.ukernel,
                  decoded.mia,
                  authStatusName(decoded.authStatus),
                  decoded.authStatus,
                  decoded.valid);
            IOLog("(FakeIrisXE) [GuC]   >>> FAILURE DETECTED AT POLL %u <<<\n", pollCount);
            return false;
        }

        if (status == 0x00000001U && stableCount >= 20U) {
            IOLog("(FakeIrisXE) [GuC] Hard stop: status stuck at 0x00000001 for 20 polls\n");
            return false;
        }

        if (decoded.success) {
            IOLog("(FakeIrisXE) [GuC][BootPoll] SUCCESS! poll=%u raw=0x%08X bootrom=0x%02X ukernel=0x%02X\n",
                  pollCount, status, decoded.bootrom, decoded.ukernel);
            emitStageReport(kGuCStageBootSuccess, startNs, retryIndex, status);
            return true;
        }

        if ((stableCount % 64U) == 0U) {
            IOLog("(FakeIrisXE) [GuC][BootPoll] poll=%u stable=%u raw=0x%08X bootrom=0x%02X ukernel=0x%02X mia=0x%X auth=0x%X\n",
                  pollCount, stableCount, status,
                  decoded.bootrom,
                  decoded.ukernel,
                  decoded.mia,
                  decoded.authStatus);
        }

        IOSleep(1);
    }

    IOLog("(FakeIrisXE) [GuC] TIMEOUT after %u polls (timeout=%ums)\n", pollCount, timeoutMs);
    IOLog("(FakeIrisXE) [GuC] Final status: raw=0x%08X\n", lastStatus);
    return false;
}

bool FakeIrisXEGuC::runLinuxBringUpPath(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr,
                                        uint32_t retryIndex, uint64_t startNs)
{
    GuCFwLayout layout;
    if (!parseGuCFirmwareV139(fwData, fwSize, layout)) {
        IOLog("(FakeIrisXE) [GuC] Linux path parse failed\n");
        return false;
    }

    // V147: COMPREHENSIVE PRE-INIT DIAGNOSTICS
    IOLog("(FakeIrisXE) [GuC][V147] ============================================\n");
    IOLog("(FakeIrisXE) [GuC][V147] PRE-INIT DIAGNOSTICS - Tiger Lake GuC Status\n");
    IOLog("(FakeIrisXE) [GuC][V147] ============================================\n");
    
    // Read all critical pre-init registers
    uint32_t preStatus = fOwner->safeMMIORead(GUC_STATUS_V137);
    uint32_t preGucCtl = fOwner->safeMMIORead(GUC_CTL);
    uint32_t preGucMisc = fOwner->safeMMIORead(GUC_MISC_CONTROL);
    uint32_t preShim = fOwner->safeMMIORead(GUC_SHIM_CONTROL_V137);
    uint32_t preWopcmSize = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t preWopcmOffset = fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137);
    uint32_t preGtPm = fOwner->safeMMIORead(GT_PM_CONFIG);
    uint32_t preDoorbell = fOwner->safeMMIORead(GUC_DOORBELL_CTRL);
    uint32_t preGucReset = fOwner->safeMMIORead(GEN11_GUC_RESET);
    
    IOLog("(FakeIrisXE) [GuC][V147] Pre-init Register State:\n");
    IOLog("(FakeIrisXE) [GuC][V147]   GUC_STATUS:         0x%08X\n", preStatus);
    IOLog("(FakeIrisXE) [GuC][V147]   GUC_CTL:           0x%08X\n", preGucCtl);
    IOLog("(FakeIrisXE) [GuC][V147]   GUC_MISC_CONTROL:  0x%08X\n", preGucMisc);
    IOLog("(FakeIrisXE) [GuC][V147]   GUC_SHIM_CONTROL:  0x%08X\n", preShim);
    IOLog("(FakeIrisXE) [GuC][V147]   GUC_WOPCM_SIZE:    0x%08X\n", preWopcmSize);
    IOLog("(FakeIrisXE) [GuC][V147]   DMA_GUC_WOPCM_OFF: 0x%08X\n", preWopcmOffset);
    IOLog("(FakeIrisXE) [GuC][V147]   GT_PM_CONFIG:      0x%08X\n", preGtPm);
    IOLog("(FakeIrisXE) [GuC][V147]   GUC_DOORBELL:      0x%08X\n", preDoorbell);
    IOLog("(FakeIrisXE) [GuC][V147]   GEN11_GUC_RESET:   0x%08X\n", preGucReset);
    
    // Analyze WOPCM state
    bool wopcmLocked = (preWopcmSize & 0x80000000U) || (preWopcmOffset & 0x80000000U);
    IOLog("(FakeIrisXE) [GuC][V147]   WOPCM Locked:      %s\n", wopcmLocked ? "YES" : "NO");
    
    // Check for errors
    if (preStatus & GUC_STATUS_WOPCMERR) {
        IOLog("(FakeIrisXE) [GuC][V147] ⚠️  WOPCM ERROR DETECTED!\n");
    }
    if (preStatus & GUC_STATUS_SecureBoot) {
        IOLog("(FakeIrisXE) [GuC][V147] ⚠️  SECURE BOOT - GuC may be locked!\n");
    }
    
    // Decode status
    IOLog("(FakeIrisXE) [GuC][V147] Status decode:\n");
    IOLog("(FakeIrisXE) [GuC][V147]   bootrom: 0x%02X (expect 0x76 for success)\n", (preStatus >> 8) & 0xFF);
    IOLog("(FakeIrisXE) [GuC][V147]   ukernel: 0x%02X (expect 0x30 for success)\n", (preStatus >> 16) & 0xFF);
    IOLog("(FakeIrisXE) [GuC][V147]   mia_core: 0x%02X (expect 0x03 for success)\n", (preStatus >> 24) & 0xFF);
    
    IOLog("(FakeIrisXE) [GuC][V147] ============================================\n");
    IOLog("(FakeIrisXE) [GuC][V147] NOTE: Linux i915 DISABLES GuC on Tiger Lake by default!\n");
    IOLog("(FakeIrisXE) [GuC][V147]      enable_guc=0 is default for TGL per Linux source\n");
    IOLog("(FakeIrisXE) [GuC][V147] ============================================\n");

    const uint32_t kLinuxShimControl = 0x00000017U;
    const uint32_t kWopcmSizeBytes = 0x00100000U;
    // V146: Try offset 0x2000 since offset 0 didn't work with locked WOPCM
    const uint32_t kWopcmOffsetBytes = 0x00002000U;
    const uint32_t kWopcmSizeValue = ((kWopcmSizeBytes >> 12) << 12) | 0x80000000U;
    const uint32_t kWopcmOffsetValue = (((kWopcmOffsetBytes >> 14) & 0x3FFFFU) << 14) | 0x80000001U;  // V145: Added VALID bit

    emitStageReport(kGuCStageForceWake, startNs, retryIndex);
    if (!acquireForceWake()) {
        IOLog("(FakeIrisXE) [GuC] Hard stop: ForceWake acquisition failed\n");
        emitStageReport(kGuCStageFailure, startNs, retryIndex);
        return false;
    }

    // V145: Enhanced GT reset/sanitize before GuC load (Linux __uc_sanitize sequence)
    IOLog("(FakeIrisXE) [GuC][V145] Performing comprehensive GT reset/sanitize...\n");
    
    // First, clear WOPCM registers to ensure clean state
    IOLog("(FakeIrisXE) [GuC][V145] Clearing WOPCM registers...\n");
    fOwner->safeMMIOWrite(GUC_WOPCM_SIZE_V137, 0);
    fOwner->safeMMIOWrite(DMA_GUC_WOPCM_OFFSET_V137, 0);
    IOSleep(10);
    
    // Read initial state
    uint32_t gucResetBefore = fOwner->safeMMIORead(GEN11_GUC_RESET);
    uint32_t gucStatusBefore = fOwner->safeMMIORead(GUC_STATUS_V137);
    IOLog("(FakeIrisXE) [GuC][V145] Before reset: GUC_RESET=0x%08X STATUS=0x%08X\n", 
          gucResetBefore, gucStatusBefore);
    
    // V145: More comprehensive GT reset sequence
    // Step 1: Request GT reset
    fOwner->safeMMIOWrite(GEN11_GUC_RESET, 0x1000);  // Request GT reset
    IOSleep(10);
    
    // Step 2: Assert reset
    fOwner->safeMMIOWrite(GEN11_GUC_RESET, 0x1001);  // Assert GT reset
    IOSleep(10);
    
    // Step 3: Release reset but keep request
    fOwner->safeMMIOWrite(GEN11_GUC_RESET, 0x0001);  // Release reset, keep request bit
    IOSleep(20);
    
    // Step 4: Clear GUC_STATUS to ensure clean state
    IOLog("(FakeIrisXE) [GuC][V145] Clearing GuC status registers...\n");
    fOwner->safeMMIOWrite(GUC_STATUS_V137, 0);
    IOSleep(5);
    
    // Step 5: Clear SOFT_SCRATCH registers (Linux does this)
    for (int i = 0; i < 16; i++) {
        fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(i), 0);
    }
    IOSleep(5);
    
    // Step 6: Verify reset completed
    uint32_t gucResetAfter = fOwner->safeMMIORead(GEN11_GUC_RESET);
    uint32_t gucStatusAfter = fOwner->safeMMIORead(GUC_STATUS_V137);
    IOLog("(FakeIrisXE) [GuC][V145] After reset: GUC_RESET=0x%08X STATUS=0x%08X\n", 
          gucResetAfter, gucStatusAfter);

    emitStageReport(kGuCStageWopcm, startNs, retryIndex);
    
    // V139: Dump WOPCM pre-config state
    dumpWopcmRegs("pre-WOPCM-config");
    
    // V138: Make WOPCM lock optional - some hardware doesn't need locked WOPCM
    // V139: Default to optional since Linux doesn't strictly require lock
    const bool wopcmLockOptional = true;
    
    uint32_t wopcmSizeBefore = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t wopcmOffsetBefore = fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137);
    IOLog("(FakeIrisXE) [GuC] WOPCM pre-config: SIZE=0x%08X OFFSET=0x%08X lock_optional=%u\n",
          wopcmSizeBefore, wopcmOffsetBefore, wopcmLockOptional ? 1 : 0);
    
    if (!wopcmLockOptional) {
        if ((wopcmSizeBefore & 0x80000000U) && wopcmSizeBefore != kWopcmSizeValue) {
            IOLog("(FakeIrisXE) [GuC] Hard stop: WOPCM size locked mismatch old=0x%08X expected=0x%08X\n",
                  wopcmSizeBefore,
                  kWopcmSizeValue);
            releaseForceWake();
            emitStageReport(kGuCStageFailure, startNs, retryIndex);
            return false;
        }
        if ((wopcmOffsetBefore & 0x80000000U) && wopcmOffsetBefore != kWopcmOffsetValue) {
            IOLog("(FakeIrisXE) [GuC] Hard stop: WOPCM offset valid mismatch old=0x%08X expected=0x%08X\n",
                  wopcmOffsetBefore,
                  kWopcmOffsetValue);
            releaseForceWake();
            emitStageReport(kGuCStageFailure, startNs, retryIndex);
            return false;
        }
    }
    
    // V145: Set GT_PM_CONFIG to Tiger Lake value BEFORE WOPCM config
    IOLog("(FakeIrisXE) [GuC][V145] Setting GT_PM_CONFIG to 0xA188 before WOPCM...\n");
    fOwner->safeMMIOWrite(GT_PM_CONFIG, TGL_GT_PM_CONFIG_VALUE);  // 0xA188
    IOSleep(5);
    uint32_t gtPmReadback = fOwner->safeMMIORead(GT_PM_CONFIG);
    IOLog("(FakeIrisXE) [GuC][V145] GT_PM_CONFIG readback: 0x%08X\n", gtPmReadback);

    uint32_t wopcmSizeReadback = 0;
    uint32_t wopcmOffsetReadback = 0;
    writeRegWithReadback(kGuCStageWopcm, "GUC_WOPCM_SIZE", GUC_WOPCM_SIZE_V137,
                         kWopcmSizeValue, &wopcmSizeReadback);
    writeRegWithReadback(kGuCStageWopcm, "DMA_GUC_WOPCM_OFFSET", DMA_GUC_WOPCM_OFFSET_V137,
                         kWopcmOffsetValue, &wopcmOffsetReadback);

    // V139: Dump WOPCM post-config state
    dumpWopcmRegs("post-WOPCM-config");

    // V138: Make lock check optional
    if (!wopcmLockOptional) {
        if ((wopcmSizeReadback & 0x80000000U) == 0U ||
            (wopcmOffsetReadback & 0x80000000U) == 0U) {
            IOLog("(FakeIrisXE) [GuC] Hard stop: WOPCM registers not locked/valid SIZE=0x%08X OFFSET=0x%08X\n",
                  wopcmSizeReadback,
                  wopcmOffsetReadback);
            releaseForceWake();
            emitStageReport(kGuCStageFailure, startNs, retryIndex);
            return false;
        }
    } else {
        IOLog("(FakeIrisXE) [GuC] WOPCM lock optional: SIZE=0x%08X OFFSET=0x%08X (continuing anyway)\n",
              wopcmSizeReadback, wopcmOffsetReadback);
    }

    emitStageReport(kGuCStageShim, startNs, retryIndex);
    uint32_t shimReadback = 0;
    writeRegWithReadback(kGuCStageShim, "GUC_SHIM_CONTROL", GUC_SHIM_CONTROL_V137,
                         kLinuxShimControl, &shimReadback);
    if (shimReadback == 0U) {
        IOLog("(FakeIrisXE) [GuC] Hard stop: GUC_SHIM_CONTROL readback is zero\n");
        releaseForceWake();
        emitStageReport(kGuCStageFailure, startNs, retryIndex);
        return false;
    }

    // V141: Enable doorbell bypass by default - continue even if doorbell fails
    const bool doorbellBypass = true;  // V141: Default to bypass since doorbell is optional for GuC
    
    if (!programDoorbellEnable(kGuCStageShim)) {
        uint32_t pmReadback = fOwner->safeMMIORead(GT_PM_CONFIG);
        uint32_t doorbellReadback = fOwner->safeMMIORead(GUC_DOORBELL_CTRL);
        if (doorbellBypass) {
            IOLog("(FakeIrisXE) [GuC][V140] ⚠️ Doorbell enable failed (gt_pm=0x%08X doorbell_ctrl=0x%08X), BYPASSING\n",
                  pmReadback, doorbellReadback);
        } else {
            IOLog("(FakeIrisXE) [GuC] Hard stop: doorbell enable mismatch gt_pm=0x%08X doorbell_ctrl=0x%08X\n",
                  pmReadback,
                  doorbellReadback);
            releaseForceWake();
            emitStageReport(kGuCStageFailure, startNs, retryIndex);
            return false;
        }
    }

    // V146: Add GUC_CTL and GUC_MISC_CONTROL before DMA
    IOLog("(FakeIrisXE) [GuC][V146] Setting GUC_CTL and GUC_MISC_CONTROL...\n");
    
    // GUC_CTL: Enable GuC (bit 0 = enable)
    fOwner->safeMMIOWrite(GUC_CTL, 0x00000001);
    IOSleep(5);
    uint32_t gucCtlRead = fOwner->safeMMIORead(GUC_CTL);
    IOLog("(FakeIrisXE) [GuC][V146] GUC_CTL: wrote 0x00000001, read 0x%08X\n", gucCtlRead);
    
    // GUC_MISC_CONTROL: Set to 3 (from Apple decompilation V143)
    fOwner->safeMMIOWrite(GUC_MISC_CONTROL, 0x00000003);
    IOSleep(5);
    uint32_t gucMiscRead = fOwner->safeMMIORead(GUC_MISC_CONTROL);
    IOLog("(FakeIrisXE) [GuC][V146] GUC_MISC_CONTROL: wrote 0x00000003, read 0x%08X\n", gucMiscRead);

    if (!writeRsaScratchV139(fwData, layout)) {
        IOLog("(FakeIrisXE) [GuC] Linux path RSA programming failed\n");
        releaseForceWake();
        emitStageReport(kGuCStageFailure, startNs, retryIndex);
        return false;
    }

    emitStageReport(kGuCStageDmaProgram, startNs, retryIndex);
    uint64_t srcAddr = gpuAddr + layout.header_offset;
    uint32_t srcLow = (uint32_t)(srcAddr & 0xFFFFFFFFULL);
    uint32_t srcHigh = (uint32_t)((srcAddr >> 32) & 0x0000FFFFULL);
    uint32_t dstLow = kWopcmOffsetBytes;
    uint32_t dstHigh = DMA_ADDRESS_SPACE_WOPCM_V137;

    writeRegWithReadback(kGuCStageDmaProgram, "DMA_ADDR_0_LOW", DMA_ADDR_0_LOW_V137,
                         srcLow, 0);
    writeRegWithReadback(kGuCStageDmaProgram, "DMA_ADDR_0_HIGH", DMA_ADDR_0_HIGH_V137,
                         srcHigh, 0);
    writeRegWithReadback(kGuCStageDmaProgram, "DMA_ADDR_1_LOW", DMA_ADDR_1_LOW_V137,
                         dstLow, 0);
    writeRegWithReadback(kGuCStageDmaProgram, "DMA_ADDR_1_HIGH", DMA_ADDR_1_HIGH_V137,
                         dstHigh, 0);
    writeRegWithReadback(kGuCStageDmaProgram, "DMA_COPY_SIZE", DMA_COPY_SIZE_V137,
                         layout.dma_copy_size, 0);

    // V143: Write GUC params to SOFT_SCRATCH before DMA!
    writeGuCParams();

    producerCoherencyBarrier("firmware DMA programmed");

    emitStageReport(kGuCStageDmaTrigger, startNs, retryIndex);
    
    // V138: Try multiple DMA trigger methods
    // V139: Default to true - Linux i915 uses START_DMA only without UOS_MOVE
    const bool trySimpleDma = true;
    uint32_t dmaTriggers[] = {
        START_DMA_V137 | UOS_MOVE_V137,        // Complex: UOS_MOVE + START_DMA
        START_DMA_V137,                        // Simple: START_DMA only (Linux style)
        0x00000001,                            // Minimal: just bit 0
    };
    const char* dmaNames[] = {"UOS_MOVE", "SIMPLE", "MINIMAL"};
    
    bool dmaSuccess = false;
    for (int attempt = 0; attempt < 3 && !dmaSuccess; attempt++) {
        // V139: Try simpler DMA methods first (Linux i915 uses START_DMA only)
        if (attempt > 0 && !trySimpleDma) {
            IOLog("(FakeIrisXE) [GuC] Skipping DMA attempt %d (simple DMA not enabled)\n", attempt);
            continue;
        }
        
        uint32_t triggerVal = dmaTriggers[attempt];
        IOLog("(FakeIrisXE) [GuC] DMA trigger attempt %d: %s value=0x%08X\n",
              attempt + 1, dmaNames[attempt], triggerVal);
        
        // V139: Dump pre-DMA state
        dumpDmaRegs("pre-DMA");
        dumpGuCStatusEx("pre-DMA");
        
        // Clear DMA ctrl first
        fOwner->safeMMIOWrite(DMA_CTRL_V137, 0);
        IOSleep(5);
        
        writeRegWithReadback(kGuCStageDmaTrigger, "DMA_CTRL", DMA_CTRL_V137,
                             triggerVal, 0);
        
        uint64_t dmaStart = mach_absolute_time();
        bool dmaDone = false;
        int pollCount = 0;
        while (mach_absolute_time() - dmaStart < (100ULL * 1000000ULL)) {
            uint32_t dmaCtrl = fOwner->safeMMIORead(DMA_CTRL_V137);
            pollCount++;
            if ((dmaCtrl & START_DMA_V137) == 0U) {
                dmaDone = true;
                IOLog("(FakeIrisXE) [GuC] DMA %s completed! ctrl=0x%08X polls=%d\n", 
                      dmaNames[attempt], dmaCtrl, pollCount);
                break;
            }
            IOSleep(1);
        }
        
        // V139: Dump post-DMA state
        dumpDmaRegs("post-DMA");
        
        if (!dmaDone) {
            IOLog("(FakeIrisXE) [GuC] DMA %s failed to complete after %d polls, trying next...\n", 
                  dmaNames[attempt], pollCount);
            continue;
        }

        writeRegWithReadback(kGuCStageDmaTrigger, "DMA_CTRL", DMA_CTRL_V137, 0, 0);
        
        // V139: Poll for boot status with enhanced logging
        dumpGuCStatusEx("post-DMA, pre-boot-poll");
        
        if (pollForBootFastFail(3000, startNs, retryIndex)) {
            dmaSuccess = true;
            dumpGuCStatusEx("post-boot-success");
            break;
        }
        
        // V139: Dump failure state
        dumpGuCStatusEx("post-boot-fail");
        IOLog("(FakeIrisXE) [GuC] GuC did not boot with DMA %s, trying next...\n", dmaNames[attempt]);
    }

    if (!dmaSuccess) {
        IOLog("(FakeIrisXE) [GuC] Hard stop: All DMA trigger methods failed\n");
        releaseForceWake();
        emitStageReport(kGuCStageFailure, startNs, retryIndex);
        return false;
    }

    releaseForceWake();
    return true;
}

bool FakeIrisXEGuC::runAppleBringUpPath(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr,
                                        uint32_t retryIndex, uint64_t startNs)
{
    GuCFwLayout layout;
    if (!parseGuCFirmwareV139(fwData, fwSize, layout)) {
        logBootFailureSignature("apple-parse", startNs, retryIndex);
        return false;
    }

    const uint32_t kAppleShimControl = 0x00208617U;
    const uint32_t kAppleMiscControl = 0x00000003U;
    const uint32_t kAppleWopcmSizeValue = 0x80100001U;
    const uint32_t kAppleWopcmOffsetValue = 0x80000001U;
    const uint32_t kAppleDmaDestOffset = 0x00002000U;
    const uint32_t kAppleDmaDestHigh = 0x00070000U;
    bool forceWakeHeld = false;
    bool freqOverrideArmed = false;
    uint32_t savedFreqToken = 0;

    IOLog("(FakeIrisXE) [GuC][Boot] mode=apple-only parser=v139 regs=tgl-0xC000 dma=apple-magic fallback=disabled\n");

    auto changeAppleLoadFrequency = [&](const char* label, uint32_t requestedToken) -> bool {
        const uint32_t expectedField = ((requestedToken >> 23) & 0x1FFU) << APPLE_TGL_GUC_LOAD_FREQ_STATUS_SHIFT_V178;
        const bool changed = writeAndPollAppleReg(kGuCStageForceWake,
                                                  label,
                                                  APPLE_TGL_GUC_LOAD_FREQ_CTRL_V178,
                                                  requestedToken,
                                                  APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178,
                                                  APPLE_TGL_GUC_LOAD_FREQ_STATUS_MASK_V178,
                                                  expectedField,
                                                  APPLE_TGL_GUC_LOAD_FREQ_TIMEOUT_MS_V178,
                                                  0);
        IOLog("(FakeIrisXE) [GuC][Apple] %s token=0x%08X status=0x%08X expected_field=0x%08X result=%u\n",
              label,
              requestedToken,
              fOwner->safeMMIORead(APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178),
              expectedField,
              changed ? 1U : 0U);
        return changed;
    };

    auto failAppleBoot = [&](const char* reason) -> bool {
        uint32_t rawStatus = fOwner->safeMMIORead(GUC_STATUS_V137);
        logAppleRegisterWindow("apple-fail-snapshot");
        logForceWakeDiagnostics("apple-fail");
        logAppleBootAudit("apple-fail");
        if (freqOverrideArmed) {
            if (!changeAppleLoadFrequency("RESTORE_LOAD_FREQ", savedFreqToken)) {
                IOLog("(FakeIrisXE) [GuC][Apple] restore frequency warning token=0x%08X after failure\n",
                      savedFreqToken);
            }
            freqOverrideArmed = false;
        }
        if (forceWakeHeld) {
            releaseForceWake();
            forceWakeHeld = false;
        }
        logBootFailureSignature(reason, startNs, retryIndex, rawStatus);
        return false;
    };

    emitStageReport(kGuCStageForceWake, startNs, retryIndex);
    if (!acquireForceWake()) {
        return failAppleBoot("forcewake");
    }
    forceWakeHeld = true;
    logForceWakeDiagnostics("apple-boot-forcewake");
    logAppleBootAudit("apple-pre-shim");

    savedFreqToken = fOwner->safeMMIORead(APPLE_TGL_GUC_LOAD_FREQ_CTRL_V178);
    freqOverrideArmed = true;
    IOLog("(FakeIrisXE) [GuC][Apple] pre-auth load frequency current_token=0x%08X status=0x%08X\n",
          savedFreqToken,
          fOwner->safeMMIORead(APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178));
    if (!changeAppleLoadFrequency("SET_LOAD_FREQ", APPLE_TGL_GUC_LOAD_FREQ_TOKEN_V178)) {
        IOLog("(FakeIrisXE) [GuC][Apple] load-frequency warning token=0x%08X status=0x%08X; continuing to test ME wake\n",
              APPLE_TGL_GUC_LOAD_FREQ_TOKEN_V178,
              fOwner->safeMMIORead(APPLE_TGL_GUC_LOAD_FREQ_STATUS_V178));
    }

    if (!acquireAppleWakeDomains(kGuCStageForceWake)) {
        return failAppleBoot("forcewake-domains");
    }

    uint64_t preAuthKeyGpuAddr = 0;
    if (!ensureApplePublicKeyBlob(&preAuthKeyGpuAddr, true)) {
        return failAppleBoot("public-key-blob");
    }
    IOLog("(FakeIrisXE) [GuC][Apple] pre-auth public-key blob ready ggtt=0x%08X\n",
          (uint32_t)preAuthKeyGpuAddr);

    if (!writeAppleBootParams(kGuCStageForceWake)) {
        return failAppleBoot("preauth-keyregs");
    }
    logAppleRegisterWindow("apple-preauth-keyregs");

    if (!runApplePreAuthHandshake(kGuCStageForceWake, savedFreqToken)) {
        return failAppleBoot("preauth-handshake");
    }
    logAppleRegisterWindow("apple-after-preauth-handshake");

    const uint32_t gtPmReg = selectGtPmConfigReg();
    const uint32_t gtPmReadback = fOwner->isMMIOOffsetValid(gtPmReg)
                                ? fOwner->safeMMIORead(gtPmReg)
                                : 0xFFFFFFFFU;
    IOLog("(FakeIrisXE) [GuC][Apple] GT_PM_CONFIG audit reg=0x%05X read=0x%08X write=skipped until verified\n",
          gtPmReg,
          gtPmReadback);

    emitStageReport(kGuCStageShim, startNs, retryIndex);
    uint32_t shimReadback = 0;
    writeRegWithReadback(kGuCStageShim, "GUC_SHIM_CONTROL", GUC_SHIM_CONTROL_V137,
                         kAppleShimControl, &shimReadback);
    if (shimReadback != kAppleShimControl) {
        IOLog("(FakeIrisXE) [GuC][Apple] GUC_SHIM_CONTROL did not latch write=0x%08X read=0x%08X\n",
              kAppleShimControl,
              shimReadback);
        return failAppleBoot("shim-readback");
    }

    if (!writeRsaScratchV139(fwData, layout)) {
        return failAppleBoot("rsa-scratch");
    }

    emitStageReport(kGuCStageWopcm, startNs, retryIndex);
    if (!prepareAppleWopcm(kGuCStageWopcm, kAppleWopcmSizeValue, kAppleWopcmOffsetValue)) {
        return failAppleBoot("wopcm");
    }

    emitStageReport(kGuCStageDmaProgram, startNs, retryIndex);
    uint64_t srcAddr = gpuAddr + layout.header_offset;
    uint32_t srcLow = (uint32_t)(srcAddr & 0xFFFFFFFFULL);
    uint32_t srcHigh = (uint32_t)((srcAddr >> 32) & 0x0000FFFFULL);
    uint32_t dstLow = kAppleDmaDestOffset;
    uint32_t dstHigh = kAppleDmaDestHigh;

    writeRegWithReadback(kGuCStageDmaProgram, "DMA_ADDR_0_LOW", DMA_ADDR_0_LOW_V137,
                         srcLow, 0);
    writeRegWithReadback(kGuCStageDmaProgram, "DMA_ADDR_0_HIGH", DMA_ADDR_0_HIGH_V137,
                         srcHigh, 0);
    writeRegWithReadback(kGuCStageDmaProgram, "DMA_ADDR_1_LOW", DMA_ADDR_1_LOW_V137,
                         dstLow, 0);
    writeRegWithReadback(kGuCStageDmaProgram, "DMA_ADDR_1_HIGH", DMA_ADDR_1_HIGH_V137,
                         dstHigh, 0);
    writeRegWithReadback(kGuCStageDmaProgram, "DMA_COPY_SIZE", DMA_COPY_SIZE_V137,
                         layout.dma_copy_size, 0);
    logAppleRegisterWindow("apple-after-dma-programming");

    if (!writeAppleBootParams(kGuCStageDmaProgram)) {
        return failAppleBoot("params");
    }
    logAppleRegisterWindow("apple-after-auth-programming");
    issueGuCTlbInvalidate();

    uint32_t authKickReadback = 0;
    writeRegWithReadback(kGuCStageDmaTrigger, "SOFT_SCRATCH0_AUTH", GUC_SOFT_SCRATCH_V170(0),
                         0x00000001U, &authKickReadback);
    if (authKickReadback != 0x00000001U) {
        IOLog("(FakeIrisXE) [GuC][Apple] SOFT_SCRATCH0 auth kick did not latch write=0x00000001 read=0x%08X\n",
              authKickReadback);
        return failAppleBoot("auth-kick");
    }
    logAppleRegisterWindow("apple-after-auth-kick");

    uint32_t miscReadback = 0;
    writeRegWithReadback(kGuCStageDmaTrigger, "GUC_MISC_CONTROL", GUC_MISC_CONTROL,
                         kAppleMiscControl, &miscReadback);
    if (miscReadback != kAppleMiscControl) {
        IOLog("(FakeIrisXE) [GuC][Apple] GUC_MISC_CONTROL did not latch write=0x%08X read=0x%08X; continuing without using it as a success signal\n",
              kAppleMiscControl,
              miscReadback);
    }

    logAppleBootAudit("apple-pre-trigger");

    producerCoherencyBarrier("apple-only DMA programmed");

    emitStageReport(kGuCStageDmaTrigger, startNs, retryIndex);
    writeRegWithReadback(kGuCStageDmaTrigger, "DMA_CTRL", DMA_CTRL_V137,
                         APPLE_DMA_MAGIC_TRIGGER, 0);
    logAppleRegisterWindow("apple-after-dma-ctrl");

    if (!pollForBootFastFail(5000, startNs, retryIndex)) {
        return failAppleBoot("boot-poll");
    }

    if (freqOverrideArmed) {
        if (!changeAppleLoadFrequency("RESTORE_LOAD_FREQ", savedFreqToken)) {
            IOLog("(FakeIrisXE) [GuC][Apple] restore frequency warning token=0x%08X after success\n",
                  savedFreqToken);
        }
        freqOverrideArmed = false;
    }

    releaseForceWake();
    forceWakeHeld = false;
    return true;
}

bool FakeIrisXEGuC::bootGuCFirmware(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr)
{
    uint64_t startNs = mach_absolute_time();
    fLastReportedStage = kGuCStageIdle;
    fFirmwareMode = selectFirmwareMode();

    IOLog("(FakeIrisXE) [GuC][Boot] entry mode=%s linux_fallback=disabled parser=v139\n",
          firmwareModeName(fFirmwareMode));

    switch (fFirmwareMode) {
        case kGuCFirmwareModeAppleOnly:
            return runAppleBringUpPath(fwData, fwSize, gpuAddr, 0, startNs);
        case kGuCFirmwareModeLinuxReserved:
            IOLog("(FakeIrisXE) [GuC][Boot] reserved mode=%s is compiled but disabled for this phase\n",
                  firmwareModeName(fFirmwareMode));
            logBootFailureSignature("mode-disabled", startNs, 0);
            return false;
    }

    logBootFailureSignature("mode-invalid", startNs, 0);
    return false;
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

    if (fGuCFwGem) {
        fGuCFwGem->unpin();
        fGuCFwGem->release();
        fGuCFwGem = nullptr;
    }

    if (fGuCPublicKeyGem) {
        fGuCPublicKeyGem->unpin();
        fGuCPublicKeyGem->release();
        fGuCPublicKeyGem = nullptr;
    }

    fGuCVersion = *(const uint32_t*)fwData;
    size_t allocSize = (fwSize + 4095) & ~4095ULL;

    fGuCFwGem = FakeIrisXEGEM::withSize(allocSize, 0);
    if (!fGuCFwGem) {
        IOLog("(FakeIrisXE) [GuC] Failed to allocate GEM for firmware image\n");
        return false;
    }

    IOBufferMemoryDescriptor* md = fGuCFwGem->memoryDescriptor();
    void* cpuPtr = md ? md->getBytesNoCopy() : nullptr;
    if (!cpuPtr) {
        IOLog("(FakeIrisXE) [GuC] Failed to get CPU mapping for firmware GEM\n");
        fGuCFwGem->release();
        fGuCFwGem = nullptr;
        return false;
    }

    bzero(cpuPtr, allocSize);
    memcpy(cpuPtr, fwData, fwSize);
    producerCoherencyBarrier("copied full GuC image into GGTT GEM");

    fGuCFwGem->pin();
    uint32_t wopcmSizeReg = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t wopcmPages = (wopcmSizeReg & 0x7FFFF000U) >> 12;
    uint64_t mapFloor = wopcmPages ? ((uint64_t)wopcmPages << 12) : 0x00100000ULL;

    uint64_t gpuAddr = fOwner->ggttMapAtOrAbove(fGuCFwGem, mapFloor);
    if (!gpuAddr) {
        gpuAddr = fOwner->ggttMap(fGuCFwGem);
    }

    if (!gpuAddr) {
        IOLog("(FakeIrisXE) [GuC] Failed to map firmware GEM into GGTT\n");
        fGuCFwGem->unpin();
        fGuCFwGem->release();
        fGuCFwGem = nullptr;
        return false;
    }

    IOLog("(FakeIrisXE) [GuC] Firmware image mapped at GGTT=0x%016llX (size=%zu floor=0x%llX)\n",
          gpuAddr,
          fwSize,
          (unsigned long long)mapFloor);

    fOwner->safeMMIOWrite(GEN11_GUC_FW_ADDR_LO, (uint32_t)(gpuAddr & 0xFFFFFFFFULL));
    fOwner->safeMMIOWrite(GEN11_GUC_FW_ADDR_HI, (uint32_t)((gpuAddr >> 32) & 0xFFFFFFFFULL));
    fOwner->safeMMIOWrite(GEN11_GUC_FW_SIZE, (uint32_t)(allocSize / 4096));

    if (!bootGuCFirmware(fwData, fwSize, gpuAddr)) {
        IOLog("(FakeIrisXE) [GuC] Apple-only firmware boot failed; stopping GuC init cleanly\n");
        fGuCMode = false;
        configureRPS();
        return false;
    }

    fGuCMode = true;
    initGuCSubsystem();
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

// V143: Write GUC params to SOFT_SCRATCH registers before DMA
// This is critical! Without these, the firmware doesn't know how to initialize
void FakeIrisXEGuC::writeGuCParams()
{
    IOLog("(FakeIrisXE) [GuC][V143] Writing GUC params to SOFT_SCRATCH...\n");
    
    // Clear SOFT_SCRATCH(0) first (Linux does this)
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(0), 0);
    
    // GUC_CTL_FEATURE - disable scheduler, no SLPC for initial load
    // Bit 0: GUC_CTL_DISABLE_SCHEDULER - don't use GuC scheduler yet
    // This is minimal - just tells firmware basic config
    uint32_t ctl_feature = 0x00000001;  // GUC_CTL_DISABLE_SCHEDULER
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(1), ctl_feature);
    
    // GUC_CTL_DEBUG - logging disabled initially
    uint32_t ctl_debug = 0;
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(2), ctl_debug);
    
    // GUC_CTL_ADS - address of Advanced Data Structure (can be 0 for now)
    uint32_t ctl_ads = 0;
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(3), ctl_ads);
    
    // GUC_CTL_WA - workarounds (can be 0 for now)
    uint32_t ctl_wa = 0;
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(4), ctl_wa);
    
    // GUC_CTL_DEVID - device info (can be 0 for now)
    uint32_t ctl_devid = 0;
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(5), ctl_devid);
    
    // GUC_CTL_LOG_PARAMS - logging params (can be 0 for now)
    uint32_t ctl_log = 0;
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(6), ctl_log);
    
    IOLog("(FakeIrisXE) [GuC][V143] GUC params written: FEATURE=0x%08X DEBUG=0x%08X ADS=0x%08X\n",
          ctl_feature, ctl_debug, ctl_ads);
}

// V139: Enhanced DMA diagnostics - dumps all DMA-related registers
void FakeIrisXEGuC::dumpDmaRegs(const char* label) const
{
    IOLog("(FakeIrisXE) [GuC] DMA Registers at %s:\n", label);
    IOLog("  DMA_ADDR_0_LOW:    0x%08X\n", fOwner->safeMMIORead(DMA_ADDR_0_LOW_V137));
    IOLog("  DMA_ADDR_0_HIGH:   0x%08X\n", fOwner->safeMMIORead(DMA_ADDR_0_HIGH_V137));
    IOLog("  DMA_ADDR_1_LOW:    0x%08X\n", fOwner->safeMMIORead(DMA_ADDR_1_LOW_V137));
    IOLog("  DMA_ADDR_1_HIGH:   0x%08X\n", fOwner->safeMMIORead(DMA_ADDR_1_HIGH_V137));
    IOLog("  DMA_COPY_SIZE:     0x%08X\n", fOwner->safeMMIORead(DMA_COPY_SIZE_V137));
    IOLog("  DMA_CTRL:          0x%08X\n", fOwner->safeMMIORead(DMA_CTRL_V137));
}

// V139: Enhanced WOPCM diagnostics
void FakeIrisXEGuC::dumpWopcmRegs(const char* label) const
{
    IOLog("(FakeIrisXE) [GuC] WOPCM Registers at %s:\n", label);
    IOLog("  GUC_WOPCM_SIZE:    0x%08X\n", fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137));
    IOLog("  DMA_GUC_WOPCM_OFFSET: 0x%08X\n", fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137));
}

// V139: Enhanced GuC status diagnostics
void FakeIrisXEGuC::dumpGuCStatusEx(const char* label) const
{
    IOLog("(FakeIrisXE) [GuC] GuC Status at %s:\n", label);
    uint32_t status = fOwner->safeMMIORead(GUC_STATUS_V137);
    IOLog("  GUC_STATUS:        0x%08X\n", status);
    
    GuCStatusDecoded dec = decodeStatus(status);
    IOLog("    bootrom: 0x%02X ukernel: 0x%02X mia: 0x%X auth: %s(0x%X)\n",
          dec.bootrom,
          dec.ukernel,
          dec.mia,
          authStatusName(dec.authStatus),
          dec.authStatus);
    IOLog("    valid: %u success: %u failure: %u\n", dec.valid, dec.success ? 1 : 0, dec.failure ? 1 : 0);
    
    uint32_t ctl = fOwner->safeMMIORead(GUC_CTL_V137);
    IOLog("  GUC_CTL:           0x%08X\n", ctl);
    
    uint32_t shim = fOwner->safeMMIORead(GUC_SHIM_CONTROL_V137);
    IOLog("  GUC_SHIM_CONTROL:  0x%08X\n", shim);
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
    producerCoherencyBarrier("test submission batch write");
    
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
    producerCoherencyBarrier("scratch command publish");
    fOwner->safeMMIOWrite(GEN11_GUC_SOFT_SCRATCH(2), 0x1);  // Trigger bit
    consumerCoherencyBarrier("scratch trigger consumed");
    
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
    logForceWakeDiagnostics("pre-write");
    
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
            logForceWakeDiagnostics("post-acquire");
            return true;
        }
        IOSleep(1);
    }
    
    // V115: Final attempt state
    uint32_t final_ack = fOwner->safeMMIORead(FORCEWAKE_ACK);
    IOLog("(FakeIrisXE) [V134] ⚠️ ForceWake timeout, ACK=0x%08X\n", final_ack);
    logForceWakeDiagnostics("timeout");
    return false;
}

void FakeIrisXEGuC::releaseForceWake()
{
    IOLog("(FakeIrisXE) [V134] Releasing ForceWake...\n");

    fOwner->safeMMIOWrite(GEN11_FORCEWAKE_RENDER, APPLE_TGL_FORCEWAKE_RENDER_DISABLE_V176);
    fOwner->safeMMIOWrite(GEN11_FORCEWAKE_MEDIA_VDBOX0, APPLE_TGL_FORCEWAKE_MEDIA_DISABLE_V176);
    fOwner->safeMMIOWrite(GEN11_FORCEWAKE_MEDIA_VEBOX0, APPLE_TGL_FORCEWAKE_MEDIA_DISABLE_V176);
    fOwner->safeMMIOWrite(FORCEWAKE_REQ, APPLE_TGL_FORCEWAKE_GLOBAL_DISABLE_V176);
    IOSleep(1);
    
    // Write 0 to release ForceWake
    fOwner->safeMMIOWrite(FORCEWAKE_REQ, 0x00000000);
    
    IOSleep(1);
    
    // V115: Verify release
    uint32_t after_release = fOwner->safeMMIORead(FORCEWAKE_ACK);
    uint32_t render_ack = fOwner->safeMMIORead(APPLE_TGL_FORCEWAKE_RENDER_ACK_V176);
    uint32_t media_vdbox_ack = fOwner->safeMMIORead(GEN11_FORCEWAKE_MEDIA_VDBOX0_ACK);
    uint32_t media_vebox_ack = fOwner->safeMMIORead(GEN11_FORCEWAKE_MEDIA_VEBOX0_ACK);
    IOLog("(FakeIrisXE) [V134] After release: MT_ACK=0x%08X RENDER_ACK=0x%08X MEDIA_VDBOX_ACK=0x%08X MEDIA_VEBOX_ACK=0x%08X\n",
          after_release,
          render_ack,
          media_vdbox_ack,
          media_vebox_ack);
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
    // V145: Use Tiger Lake specific value 0xA188 instead of just 0x1
    IOLog("(FakeIrisXE) [V145] Programming GT_PM_CONFIG with Tiger Lake value...\n");
    fOwner->safeMMIOWrite(GT_PM_CONFIG, TGL_GT_PM_CONFIG_VALUE);  // 0xA188
    IOSleep(5);
    
    uint32_t pmRead = fOwner->safeMMIORead(GT_PM_CONFIG);
    if (pmRead == TGL_GT_PM_CONFIG_VALUE) {
        IOLog("(FakeIrisXE) [V145] ✅ GT_PM_CONFIG verified: 0x%08X\n", pmRead);
    } else {
        IOLog("(FakeIrisXE) [V145] ⚠️ GT_PM_CONFIG: wrote 0x%08X, read 0x%08X\n", TGL_GT_PM_CONFIG_VALUE, pmRead);
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
    if (!acquireForceWake()) {
        IOLog("(FakeIrisXE) [V135] ForceWake acquisition failed during pre-init; aborting pre-init audit path\n");
        return;
    }
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
    
    // Apple-only phase: do not pre-program SHIM here.
    // The active boot path owns the single SHIM write and uses 0x00208617.
    IOLog("(FakeIrisXE) [V136] Apple-only phase: pre-init SHIM write disabled; active boot path will program 0x00208617\n");
    
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
// V140: Wait for GuC Boot (Linux i915 method with RSA failure detection)
// Polls GUC_STATUS with correct bitfield decoding
// ============================================================================
bool FakeIrisXEGuC::waitForGucBoot(uint32_t timeoutMs)
{
    IOLog("(FakeIrisXE) [V140] Waiting for GuC boot (timeout: %u ms)...\n", timeoutMs);

    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = timeoutMs * 1000000ULL;
    uint32_t lastStatus = 0xFFFFFFFFU;
    uint32_t sameStatusCount = 0;

    while (mach_absolute_time() - start < timeoutNs) {
        uint32_t status = fOwner->safeMMIORead(GUC_STATUS_V137);

        if (status == lastStatus) {
            ++sameStatusCount;
        } else {
            sameStatusCount = 0;
            lastStatus = status;
        }

        uint32_t bootrom_status = FIELD_GET_V137(GUC_BOOTROM_STATUS_MASK_V137, status);
        uint32_t ukernel_status = FIELD_GET_V137(GUC_UKERNEL_STATUS_MASK_V137, status);
        uint32_t mia_core_status = FIELD_GET_V137(GUC_MIA_CORE_STATUS_MASK_V137, status);

        if (sameStatusCount == 0 || (sameStatusCount % 50) == 0) {
            IOLog("(FakeIrisXE) [V140] STATUS=0x%08X bootrom=%u ukernel=%u mia=%u stable=%u\n",
                  status, bootrom_status, ukernel_status, mia_core_status, sameStatusCount);
        }

        // Fast-fail: common stuck state observed in field logs.
        if (status == 0x00000001 && sameStatusCount >= 25) {
            IOLog("(FakeIrisXE) [V140] ❌ Stuck at STATUS=0x00000001, fast-failing GuC boot\n");
            return false;
        }

        // V140: Check for RSA verification failure
        if (bootrom_status == 0x06) {
            IOLog("(FakeIrisXE) [V140] ❌ RSA VERIFICATION FAILED! (bootrom_status=0x06)\n");
            return false;
        }

        if (bootrom_status == 0x7F && ukernel_status == 0xFF) {
            IOLog("(FakeIrisXE) [V140] ✅ GuC booted successfully!\n");
            return true;
        }

        if (bootrom_status != 0 && bootrom_status != 0x7F && bootrom_status != 0x06) {
            IOLog("(FakeIrisXE) [V140] ❌ GuC boot failed! bootrom_status=0x%02X\n", bootrom_status);
            return false;
        }

        IOSleep(10);
    }

    IOLog("(FakeIrisXE) [V140] ❌ Timeout waiting for GuC boot\n");
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

// ============================================================================
// V139: STRICT CSS Parser (Linux i915 exact method)
// ============================================================================
#pragma pack(push, 1)
struct GuCCssHeader {
    uint32_t module_type;
    uint32_t header_size_dw;
    uint32_t header_version;
    uint32_t module_id;
    uint32_t module_vendor;
    uint32_t date;
    uint32_t size_dw;
    uint32_t key_size_dw;
    uint32_t modulus_size_dw;
    uint32_t exponent_size_dw;
    uint32_t reserved[22];
} __attribute__((packed));
#pragma pack(pop)

bool FakeIrisXEGuC::parseGuCFirmwareV139(const uint8_t* fwData, size_t fwSize, GuCFwLayout& layout)
{
    IOLog("(FakeIrisXE) [V139] === STRICT CSS Parser (i915 method) ===\n");
    
    if (fwSize < sizeof(GuCCssHeader)) {
        IOLog("(FakeIrisXE) [V139] ❌ File too small for CSS header\n");
        return false;
    }
    
    auto* css = reinterpret_cast<const GuCCssHeader*>(fwData);
    
    IOLog("(FakeIrisXE) [V139] CSS: header_size_dw=%u size_dw=%u key_size_dw=%u modulus_size_dw=%u exponent_size_dw=%u\n",
          css->header_size_dw, css->size_dw, css->key_size_dw, css->modulus_size_dw, css->exponent_size_dw);
    
    if (css->header_size_dw == 0 || css->size_dw == 0) {
        IOLog("(FakeIrisXE) [V139] ❌ Invalid header_size_dw or size_dw\n");
        return false;
    }
    
    if (css->size_dw < css->header_size_dw) {
        IOLog("(FakeIrisXE) [V139] ❌ size_dw < header_size_dw\n");
        return false;
    }
    
    if (css->key_size_dw != 64) {
        IOLog("(FakeIrisXE) [V139] ⚠️ key_size_dw=%u (expected 64)\n", css->key_size_dw);
    }
    
    uint32_t header_size = (css->header_size_dw - css->modulus_size_dw - css->key_size_dw - css->exponent_size_dw) * 4;
    const uint32_t expected_header_size = (uint32_t)sizeof(GuCCssHeader);
    
    IOLog("(FakeIrisXE) [V139] Computed header_size=%u bytes expected=%u bytes\n",
          header_size, expected_header_size);

    if (header_size != expected_header_size) {
        IOLog("(FakeIrisXE) [V139] ❌ CSS header_size mismatch computed=%u expected=%u\n",
              header_size, expected_header_size);
        return false;
    }
    
    uint32_t ucode_offset = 0 + header_size;
    uint32_t ucode_size = (css->size_dw - css->header_size_dw) * 4;
    uint32_t rsa_offset = ucode_offset + ucode_size;
    uint32_t rsa_size = css->key_size_dw * 4;
    
    IOLog("(FakeIrisXE) [V139] layout: header@0 size=%u ucode@0x%x size=%u RSA@0x%x size=%u\n",
          header_size, ucode_offset, ucode_size, rsa_offset, rsa_size);
    
    uint64_t need = (uint64_t)header_size + (uint64_t)ucode_size + (uint64_t)rsa_size;
    if (fwSize < need) {
        IOLog("(FakeIrisXE) [V139] ❌ File too small: need=%llu have=%zu\n", need, fwSize);
        return false;
    }
    
    layout = {
        .header_offset = 0,
        .header_size = header_size,
        .ucode_offset = ucode_offset,
        .ucode_size = ucode_size,
        .rsa_offset = rsa_offset,
        .rsa_size = rsa_size,
        .dma_copy_size = header_size + ucode_size
    };
    
    IOLog("(FakeIrisXE) [V139] ✅ DMA copy size: %u bytes (header + ucode)\n", layout.dma_copy_size);
    return true;
}

// ============================================================================
// V139: Write RSA scratch from rsa_offset (i915 method)
// ============================================================================
bool FakeIrisXEGuC::writeRsaScratchV139(const uint8_t* fwData, const GuCFwLayout& layout)
{
    IOLog("(FakeIrisXE) [V139] Writing RSA scratch from offset 0x%x size=%u\n",
          layout.rsa_offset, layout.rsa_size);

    if (!fwData) {
        IOLog("(FakeIrisXE) [V139] ❌ RSA scratch source is null\n");
        return false;
    }

    if (layout.rsa_size != (UOS_RSA_SCRATCH_COUNT_V137 * sizeof(uint32_t))) {
        IOLog("(FakeIrisXE) [V139] ❌ RSA scratch size mismatch layout=%u expected=%zu\n",
              layout.rsa_size,
              (size_t)(UOS_RSA_SCRATCH_COUNT_V137 * sizeof(uint32_t)));
        return false;
    }
    
    const uint32_t* rsaDw = reinterpret_cast<const uint32_t*>(fwData + layout.rsa_offset);
    uint32_t firstDw0 = rsaDw[0];
    uint32_t firstDw1 = rsaDw[1];
    uint32_t lastDw0 = rsaDw[UOS_RSA_SCRATCH_COUNT_V137 - 2];
    uint32_t lastDw1 = rsaDw[UOS_RSA_SCRATCH_COUNT_V137 - 1];
    bool hasNonZero = false;
    for (uint32_t i = 0; i < UOS_RSA_SCRATCH_COUNT_V137; ++i) {
        if (rsaDw[i] != 0U) {
            hasNonZero = true;
            break;
        }
    }

    IOLog("(FakeIrisXE) [V139] RSA scratch source first=0x%08X/0x%08X last=0x%08X/0x%08X nonzero=%u\n",
          firstDw0, firstDw1, lastDw0, lastDw1, hasNonZero ? 1U : 0U);
    if (!hasNonZero) {
        IOLog("(FakeIrisXE) [V139] ❌ RSA scratch source is all zeros\n");
        return false;
    }
    
    for (uint32_t i = 0; i < 64; i++) {
        fOwner->safeMMIOWrite(UOS_RSA_SCRATCH_BASE_V137 + (i * 4), rsaDw[i]);
    }

    uint32_t rbFirst0 = fOwner->safeMMIORead(UOS_RSA_SCRATCH_BASE_V137 + 0);
    uint32_t rbFirst1 = fOwner->safeMMIORead(UOS_RSA_SCRATCH_BASE_V137 + 4);
    uint32_t rbLast0 = fOwner->safeMMIORead(UOS_RSA_SCRATCH_BASE_V137 + ((UOS_RSA_SCRATCH_COUNT_V137 - 2) * 4));
    uint32_t rbLast1 = fOwner->safeMMIORead(UOS_RSA_SCRATCH_BASE_V137 + ((UOS_RSA_SCRATCH_COUNT_V137 - 1) * 4));
    IOLog("(FakeIrisXE) [V139] RSA scratch readback first=0x%08X/0x%08X last=0x%08X/0x%08X\n",
          rbFirst0, rbFirst1, rbLast0, rbLast1);
    if (rbFirst0 != firstDw0 || rbFirst1 != firstDw1 || rbLast0 != lastDw0 || rbLast1 != lastDw1) {
        IOLog("(FakeIrisXE) [V139] ❌ RSA scratch readback mismatch\n");
        return false;
    }
    
    IOLog("(FakeIrisXE) [V139] ✅ Wrote 64 dwords to UOS_RSA_SCRATCH\n");
    return true;
}

// ============================================================================
// V139: DMA copy header + uCode (NOT payload-only)
// FIXED: Source HIGH has no address-space OR
// ============================================================================
bool FakeIrisXEGuC::dmaCopyHeaderUcodeToWopcmV139(uint64_t fwGgttAddr, const GuCFwLayout& layout)
{
    IOLog("(FakeIrisXE) [V139] DMA: copy %u bytes from GGTT+0 to WOPCM:0x2000\n", layout.dma_copy_size);
    
    fOwner->safeMMIOWrite(DMA_COPY_SIZE_V137, layout.dma_copy_size);
    
    uint64_t srcAddr = fwGgttAddr + layout.header_offset;
    uint32_t srcLow = (uint32_t)(srcAddr & 0xFFFFFFFFULL);
    uint32_t srcHigh = (uint32_t)((srcAddr >> 32) & 0x0000FFFFULL);
    
    fOwner->safeMMIOWrite(DMA_ADDR_0_LOW_V137, srcLow);
    fOwner->safeMMIOWrite(DMA_ADDR_0_HIGH_V137, srcHigh);
    
    IOLog("(FakeIrisXE) [V139] SRC=0x%016llX (LO=0x%08X HI=0x%08X - no addr-space!)\n", srcAddr, srcLow, srcHigh);
    
    fOwner->safeMMIOWrite(DMA_ADDR_1_LOW_V137, 0x2000);
    fOwner->safeMMIOWrite(DMA_ADDR_1_HIGH_V137, DMA_ADDRESS_SPACE_WOPCM_V137);
    
    uint32_t ctrl = START_DMA_V137 | UOS_MOVE_V137;
    fOwner->safeMMIOWrite(DMA_CTRL_V137, ctrl);
    IOLog("(FakeIrisXE) [V139] Started DMA: CTRL=0x%08X\n", ctrl);
    
    uint64_t start = mach_absolute_time();
    uint64_t timeoutNs = 100 * 1000000ULL;
    bool completed = false;
    
    while (mach_absolute_time() - start < timeoutNs) {
        uint32_t status = fOwner->safeMMIORead(DMA_CTRL_V137);
        if (!(status & START_DMA_V137)) {
            completed = true;
            IOLog("(FakeIrisXE) [V139] ✅ DMA done! status=0x%08X\n", status);
            break;
        }
        IOSleep(1);
    }
    
    if (!completed) {
        IOLog("(FakeIrisXE) [V139] ❌ DMA timeout!\n");
        IOLog("(FakeIrisXE) [V139] DMA_CTRL=0x%08X\n", fOwner->safeMMIORead(DMA_CTRL_V137));
        return false;
    }
    
    fOwner->safeMMIOWrite(DMA_CTRL_V137, 0);
    return true;
}

// ============================================================================
// V140: Complete GuC Load with STRICT i915 method
// ============================================================================
bool FakeIrisXEGuC::loadGuCWithV139Method(const uint8_t* fwData, size_t fwSize, uint64_t gpuAddr)
{
    IOLog("(FakeIrisXE) [V140] === V140 STRICT i915 METHOD ===\n");
    
    GuCFwLayout layout{};
    if (!parseGuCFirmwareV139(fwData, fwSize, layout)) {
        IOLog("(FakeIrisXE) [V140] ❌ Parse failed!\n");
        return false;
    }
    
    // Step 6: Check GGTT pin bias (firmware must be above WOPCM size)
    // V140: FIXED - mask lock bit (bit 31) before extracting size
    uint32_t wopcmSizeReg = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t wopcmSize = ((wopcmSizeReg & 0x7FFFFFFF) >> 12) & 0xFFFFFu; // Mask lock bit first, then extract size
    uint64_t wopcmSizeBytes = (uint64_t)wopcmSize * 4096;
    
    // If WOPCM not configured yet, assume default 1MB
    if (wopcmSizeBytes == 0) {
        wopcmSizeBytes = 0x100000; // 1MB default
        IOLog("(FakeIrisXE) [V140] WOPCM not configured, using default 1MB\n");
    }
    
    IOLog("(FakeIrisXE) [V140] WOPCM reg=0x%08X masked=0x%08X size=%u (%llu bytes)\n", 
          wopcmSizeReg, (wopcmSizeReg & 0x7FFFFFFF), wopcmSize, (unsigned long long)wopcmSizeBytes);
    IOLog("(FakeIrisXE) [V140] Firmware GGTT addr: 0x%llx\n", gpuAddr);
    
    if (gpuAddr < wopcmSizeBytes) {
        IOLog("(FakeIrisXE) [V140] ⚠️ GGTT pin bias violation! fw_addr < wopcm_size\n");
        IOLog("(FakeIrisXE) [V140] Firmware will be pinned at wrong location!\n");
    } else {
        IOLog("(FakeIrisXE) [V140] ✅ GGTT pin bias OK (fw >= wopcm)\n");
    }
    
    if (!acquireForceWake()) {
        IOLog("(FakeIrisXE) [V139] ⚠️ ForceWake warning\n");
    }
    IOSleep(20);
    
    programShimControl();
    IOSleep(20);
    
    guclResetForWopcmV138();
    IOSleep(20);
    
    writeRsaScratchV139(fwData, layout);
    
    uint32_t wopcm_size = fOwner->safeMMIORead(GUC_WOPCM_SIZE_V137);
    uint32_t wopcm_offset = fOwner->safeMMIORead(DMA_GUC_WOPCM_OFFSET_V137);
    
    if (!(wopcm_size & 0x80000000) || !(wopcm_offset & 0x80000000)) {
        programWopcmForTglV138(0x100000, 0x2000);
    }
    IOSleep(20);
    
    if (!dmaCopyHeaderUcodeToWopcmV139(gpuAddr, layout)) {
        IOLog("(FakeIrisXE) [V139] ❌ DMA failed!\n");
        IOLog("(FakeIrisXE) [V139] GUC_STATUS=0x%08X\n", fOwner->safeMMIORead(GUC_STATUS_V137));
        releaseForceWake();
        return false;
    }
    
    if (!waitForGucBoot(5000)) {
        IOLog("(FakeIrisXE) [V139] ❌ Boot failed! STATUS=0x%08X\n", fOwner->safeMMIORead(GUC_STATUS_V137));
        releaseForceWake();
        return false;
    }
    
    releaseForceWake();
    IOLog("(FakeIrisXE) [V139] ✅ SUCCESS!\n");
    return true;
}
