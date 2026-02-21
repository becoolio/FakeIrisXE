#pragma once

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>

extern volatile int32_t gFakeIrisXEGlobalPhase;

#define FXE_LOG(fmt, ...) IOLog("FakeIrisXE: " fmt "\n", ##__VA_ARGS__)

#define FXE_PHASE(mod, n, fmt, ...) do { \
    const int32_t _p = OSIncrementAtomic((volatile SInt32*)&gFakeIrisXEGlobalPhase); \
    FXE_LOG("[PHASE][%s][%03d][p=%d] " fmt, mod, (n), (int)_p, ##__VA_ARGS__); \
} while (0)

#define FXE_SUMMARY(fmt, ...) FXE_LOG("[SUMMARY] " fmt, ##__VA_ARGS__)
