#pragma once

#include <stdint.h>
#include <libkern/OSAtomic.h>

extern volatile int32_t gFakeIrisXEGlobalPhase;

enum FXE_Result : uint32_t {
    FXE_OK = 0,
    FXE_EINVAL = 0xE001,
    FXE_ENOTREADY = 0xE002,
    FXE_ENOSUPPORT = 0xE003,
    FXE_EINTERNAL = 0xE004,
    FXE_ETIMEOUT = 0xE005,
    FXE_ENOENT = 0xE006,
};

#define FXE_LOG(fmt, ...) IOLog("FakeIrisXE: " fmt "\n", ##__VA_ARGS__)

#define FXE_PHASE(mod, n, fmt, ...) do { \
    const int32_t _p = OSIncrementAtomic((volatile SInt32*)&gFakeIrisXEGlobalPhase); \
    const uint64_t _t = mach_absolute_time(); \
    FXE_LOG("[PHASE][%s][%03d][g=%d][t=%llu] " fmt, \
            mod, (n), (int)_p, (unsigned long long)_t, ##__VA_ARGS__); \
} while (0)
