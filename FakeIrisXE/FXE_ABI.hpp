#pragma once

#include <stdint.h>

#define FXE_ABI_MAJOR 1u
#define FXE_ABI_MINOR 0u
#define FXE_KEXT_VERSION_PACKED 0x0001009Au

enum {
    FXE_SEL_GET_VERSION   = 0,
    FXE_SEL_CREATE_CTX    = 1,
    FXE_SEL_ATTACH_SHARED = 2,
    FXE_SEL_BIND_SURFACE  = 3,
    FXE_SEL_PRESENT       = 4,
    FXE_SEL_FENCE_TEST    = 7,
};

enum FXE_Result {
    FXE_OK         = 0,
    FXE_EINVAL     = 0xE001,
    FXE_ENOTREADY  = 0xE002,
    FXE_ENOSUPPORT = 0xE003,
    FXE_EINTERNAL  = 0xE004,
    FXE_ETIMEOUT   = 0xE005,
    FXE_ENOENT     = 0xE006,
};

#pragma pack(push, 1)

typedef struct FXE_VersionInfo {
    uint32_t abiMajor;
    uint32_t abiMinor;
    uint32_t kextVersionPacked;
    uint32_t features;
} FXE_VersionInfo;

typedef struct FXE_CreateCtx_In {
    uint32_t flags;
} FXE_CreateCtx_In;

typedef struct FXE_CreateCtx_Out {
    uint32_t ctxId;
    uint32_t rc;
} FXE_CreateCtx_Out;

typedef struct FXE_AttachShared_In {
    uint32_t flags;
} FXE_AttachShared_In;

typedef struct FXE_AttachShared_Out {
    uint32_t rc;
    uint32_t magic;
    uint32_t cap;
} FXE_AttachShared_Out;

typedef struct FXE_BindSurface_In {
    uint32_t surfaceId;
    uint32_t flags;
} FXE_BindSurface_In;

typedef struct FXE_BindSurface_Out {
    uint64_t surfaceHandle;
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
    uint32_t stride;
    uint32_t rc;
} FXE_BindSurface_Out;

typedef struct FXE_Present_In {
    uint64_t surfaceHandle;
    uint64_t frameId;
    uint32_t flags;
    uint32_t reserved;
} FXE_Present_In;

typedef struct FXE_Present_Out {
    uint64_t completionValue;
    uint32_t rc;
    uint32_t reserved;
} FXE_Present_Out;

typedef struct FXE_FenceTest_In {
    uint32_t engine;
    uint32_t timeoutMs;
} FXE_FenceTest_In;

typedef struct FXE_FenceTest_Out {
    uint32_t pass;
    uint32_t reason;
    uint32_t rc;
} FXE_FenceTest_Out;

#pragma pack(pop)
