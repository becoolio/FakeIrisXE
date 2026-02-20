#pragma once
#include <stdint.h>
#include <stddef.h>

struct GucFwLayout {
    uint32_t header_len_dw = 0;
    uint32_t ucode_size_dw = 0;
    uint32_t key_size_dw   = 0;

    uint32_t header_bytes  = 0;
    uint32_t ucode_bytes   = 0;
    uint32_t dma_bytes     = 0;  // header + ucode

    uint32_t rsa_offset    = 0;  // bytes
    uint32_t rsa_bytes     = 0;  // usually 256
};

static inline uint32_t read_le32_unaligned(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// Conservative parser: validate bounds, never invent tiny RSA offsets.
static inline bool parse_guc_layout(const uint8_t* fw, size_t fwLen, GucFwLayout& out) {
    if (!fw || fwLen < 0x40) return false;

    // dword indices:
    // [1] header_len_dw, [6] ucode_size_dw, [7] key_size_dw (common GuC layout)
    out.header_len_dw = read_le32_unaligned(fw + 0x04);
    out.ucode_size_dw = read_le32_unaligned(fw + 0x18);
    out.key_size_dw   = read_le32_unaligned(fw + 0x1C);

    if (out.header_len_dw == 0 || out.ucode_size_dw == 0 || out.key_size_dw == 0) return false;

    out.header_bytes = out.header_len_dw * 4;
    out.ucode_bytes  = out.ucode_size_dw * 4;
    out.dma_bytes    = out.header_bytes + out.ucode_bytes;

    out.rsa_offset   = out.dma_bytes;
    out.rsa_bytes    = out.key_size_dw * 4;

    if (out.header_bytes > fwLen) return false;
    if (out.dma_bytes > fwLen) return false;
    if (out.rsa_offset + out.rsa_bytes > fwLen) return false;

    // For GuC, rsa is typically 256 bytes; accept others but require >= 256.
    if (out.rsa_bytes < 256) return false;

    return true;
}
