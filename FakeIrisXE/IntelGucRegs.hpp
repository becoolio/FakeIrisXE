#pragma once
#include <stdint.h>

// From Linux i915 intel_guc_reg.h (keep these exact offsets/masks)
// Using REG_ prefix and different names to avoid macro conflicts
namespace i915_guc {

// Status register
static const uint32_t REG_STATUS             = 0xC000;

// Status decode
static const uint32_t GS_RESET_SHIFT         = 0;
static const uint32_t GS_MIA_IN_RESET        = (0x01u << GS_RESET_SHIFT);

static const uint32_t GS_BOOTROM_SHIFT       = 1;
static const uint32_t GS_BOOTROM_MASK        = (0x7Fu << GS_BOOTROM_SHIFT);
static const uint32_t GS_BOOTROM_RSA_FAILED  = (0x50u << GS_BOOTROM_SHIFT);
static const uint32_t GS_BOOTROM_JUMP_PASSED = (0x76u << GS_BOOTROM_SHIFT);

static const uint32_t GS_UKERNEL_SHIFT       = 8;
static const uint32_t GS_UKERNEL_MASK        = (0xFFu << GS_UKERNEL_SHIFT);
static const uint32_t GS_UKERNEL_READY       = (0xF0u << GS_UKERNEL_SHIFT);
static const uint32_t GS_UKERNEL_DPC_ERROR   = (0x60u << GS_UKERNEL_SHIFT);

static const uint32_t GS_MIA_SHIFT           = 16;
static const uint32_t GS_MIA_MASK            = (0x07u << GS_MIA_SHIFT);

static const uint32_t GS_AUTH_STATUS_SHIFT   = 30;
static const uint32_t GS_AUTH_STATUS_MASK    = (0x03u << GS_AUTH_STATUS_SHIFT);
static const uint32_t GS_AUTH_STATUS_BAD     = (0x01u << GS_AUTH_STATUS_SHIFT);
static const uint32_t GS_AUTH_STATUS_GOOD    = (0x02u << GS_AUTH_STATUS_SHIFT);

// Debug scratch
static const uint32_t SOFT_SCRATCH_BASE      = 0xC180;
inline uint32_t SOFT_SCRATCH(uint32_t n) { return SOFT_SCRATCH_BASE + n * 4; } // n=0..15

// RSA scratch
static const uint32_t RSA_SCRATCH_BASE       = 0xC200;
inline uint32_t RSA_SCRATCH(uint32_t i) { return RSA_SCRATCH_BASE + i * 4; } // i=0..63

// DMA registers (correct Tiger Lake offsets)
static const uint32_t DMA_SRC_LOW            = 0xC300;
static const uint32_t DMA_SRC_HIGH           = 0xC304;
static const uint32_t DMA_DST_LOW            = 0xC308;
static const uint32_t DMA_DST_HIGH           = 0xC30C;
static const uint32_t DMA_SIZE               = 0xC310;
static const uint32_t DMA_CONTROL            = 0xC314;

static const uint32_t DMA_SPACE_WOPCM        = (7u << 16);
static const uint32_t DMA_SPACE_GTT          = (8u << 16);

static const uint32_t DMA_FLAG_UOS_MOVE      = (1u << 4);
static const uint32_t DMA_FLAG_START         = (1u << 0);

// WOPCM
static const uint32_t WOPCM_OFFSET_REG       = 0xC340;
static const uint32_t WOPCM_OFFSET_VALID     = (1u << 0);
static const uint32_t WOPCM_OFFSET_SHIFT     = 14;
static const uint32_t WOPCM_OFFSET_MASK      = (0x3ffffu << WOPCM_OFFSET_SHIFT);

static const uint32_t WOPCM_SIZE_REG         = 0xC050;
static const uint32_t WOPCM_SIZE_LOCKED      = (1u << 0);
static const uint32_t WOPCM_SIZE_SHIFT       = 12;
static const uint32_t WOPCM_SIZE_MASK        = (0xfffffu << WOPCM_SIZE_SHIFT);

// SHIM control
static const uint32_t SHIM_REG               = 0xC064;
static const uint32_t SHIM_DISABLE_SRAM_INIT = (1u << 0);
static const uint32_t SHIM_ENABLE_CACHE      = (1u << 1);
static const uint32_t SHIM_ENABLE_MIA_CACHE  = (1u << 2);
static const uint32_t SHIM_ENABLE_SRAM_CACHE = (1u << 9);
static const uint32_t SHIM_ENABLE_WOPCM_CACHE= (1u << 10);
static const uint32_t SHIM_ENABLE_MIA_CLK    = (1u << 15);

// PM config
static const uint32_t PM_CONFIG_REG          = 0x13816C;
static const uint32_t PM_DOORBELL_ENABLE     = (1u << 0);

} // namespace i915_guc
