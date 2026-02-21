#ifndef FAKEIRISXE_USER_SHARED_H
#define FAKEIRISXE_USER_SHARED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../FakeIrisXE/FXE_ABI.hpp"

enum {
    kFakeIris_Method_GetCaps                = FXE_SEL_GET_VERSION,
    kFakeIris_Method_CreateContext          = FXE_SEL_CREATE_CTX,
    kFakeIris_Method_DestroyContext         = FXE_SEL_ATTACH_SHARED,
    kFakeIris_Method_BindSurfaceUserMapped  = FXE_SEL_BIND_SURFACE,
    kFakeIris_Method_PresentContext         = FXE_SEL_PRESENT,
    kFakeIris_Method_SubmitExeclistFenceTest = FXE_SEL_FENCE_TEST,
};

enum {
    kFakeIrisXE_ABI_Major = FXE_ABI_MAJOR,
    kFakeIrisXE_ABI_Minor = FXE_ABI_MINOR,
    kFakeIrisXE_KextVersion_u32 = FXE_KEXT_VERSION_PACKED,
};

typedef struct {
    uint32_t version;
    uint32_t metalSupported;
    uint32_t reserved0;
    uint32_t reserved1;
} XEAccelCaps;

typedef struct {
    uint32_t flags;
    uint32_t pad;
    uint64_t sharedGPUPtr;
} XECreateCtxIn;

typedef struct {
    uint32_t ctxId;
    uint32_t pad;
} XECreateCtxOut;

typedef struct {
    uint32_t ctxId;
    uint32_t ioSurfaceID;
    uint32_t width;
    uint32_t height;
    uint32_t bytesPerRow;
    uint32_t surfaceID;
    void* cpuPtr;
    uint64_t gpuAddr;
    uint32_t pixelFormat;
    bool valid;
} XEBindSurfaceIn;

typedef struct {
    uint64_t gpuAddr;
    uint32_t status;
    uint32_t reserved;
} XEBindSurfaceOut;

enum {
    XE_CMD_NOP     = 0,
    XE_CMD_CLEAR   = 1,
    XE_CMD_RECT    = 2,
    XE_CMD_COPY    = 3,
    XE_CMD_FLUSH   = 4,
    XE_CMD_PRESENT = 5,
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t reserved[3];
} XEHdr;

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    uint32_t bytes;
    uint32_t ctxId;
    uint32_t reserved;
} XECmd;

enum {
    XE_MAGIC   = 0x53524558u,
    XE_VERSION = 1,
    XE_PAGE    = 4096,
};

static inline uint32_t xe_align_u32(uint32_t v) {
    return (v + 3u) & ~3u;
}

static inline void xe_ring_copy_wrap(uint8_t* ringBase,
                                     uint32_t cap,
                                     uint32_t offset,
                                     const void* src,
                                     uint32_t bytes) {
    if (offset + bytes <= cap) {
        memcpy(ringBase + offset, src, bytes);
        return;
    }

    uint32_t first = cap - offset;
    memcpy(ringBase + offset, src, first);
    memcpy(ringBase, ((const uint8_t*)src) + first, bytes - first);
}

static inline bool xe_ring_submit_cmd(volatile XEHdr* hdr,
                                      uint8_t* ringBase,
                                      uint32_t opcode,
                                      uint32_t ctxId,
                                      const void* payload,
                                      uint32_t payloadBytes) {
    if (!hdr || !ringBase) return false;

    uint32_t cap = hdr->capacity;
    if (cap == 0 || payloadBytes > cap || sizeof(XECmd) > cap) return false;

    uint32_t head = hdr->head;
    uint32_t tail = hdr->tail;
    uint32_t total = xe_align_u32((uint32_t)sizeof(XECmd) + payloadBytes);
    if (total > cap) return false;

    uint32_t nextHead = (head + total) % cap;
    if (nextHead == tail) return false;

    XECmd cmd = {
        .opcode = opcode,
        .bytes = payloadBytes,
        .ctxId = ctxId,
        .reserved = 0,
    };

    xe_ring_copy_wrap(ringBase, cap, head, &cmd, (uint32_t)sizeof(cmd));

    if (payloadBytes && payload) {
        uint32_t payloadOffset = (head + (uint32_t)sizeof(XECmd)) % cap;
        xe_ring_copy_wrap(ringBase, cap, payloadOffset, payload, payloadBytes);
    }

    __sync_synchronize();
    hdr->head = nextHead;
    __sync_synchronize();

    return true;
}

#endif
