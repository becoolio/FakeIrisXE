#pragma once

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>

#include "FakeIrisXEIosurfaceCompat.hpp"

struct FXE_SurfaceEntry {
    uint32_t surfaceId;
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
    uint32_t stride;
    uint32_t flags;
};

class FXE_SurfaceStore {
public:
    bool init() {
        mLock = IOLockAlloc();
        if (!mLock) {
            return false;
        }
        mMap = OSDictionary::withCapacity(32);
        if (!mMap) {
            IOLockFree(mLock);
            mLock = nullptr;
            return false;
        }
        mSalt = 1;
        return true;
    }

    void free() {
        clearAll();
        if (mMap) {
            mMap->release();
            mMap = nullptr;
        }
        if (mLock) {
            IOLockFree(mLock);
            mLock = nullptr;
        }
    }

    bool isReady() const {
        return mMap != nullptr && mLock != nullptr;
    }

    static uint64_t makeHandle(uint32_t surfaceId, uint32_t salt) {
        return (uint64_t(salt) << 32) | uint64_t(surfaceId);
    }

    bool bind(uint32_t surfaceId, uint32_t flags, uint64_t* outHandle, FXE_SurfaceEntry* outMeta) {
        if (!isReady() || !IOSurfaceKextAvailable() || !outHandle || !outMeta || surfaceId == 0) {
            return false;
        }

        IOSurfaceRef surf = IOSurfaceLookup(surfaceId);
        if (!surf) {
            return false;
        }

        FXE_SurfaceEntry entry = {};
        entry.surfaceId = surfaceId;
        entry.width = (uint32_t)IOSurfaceGetWidth(surf);
        entry.height = (uint32_t)IOSurfaceGetHeight(surf);
        entry.pixelFormat = 0;
        entry.stride = (uint32_t)IOSurfaceGetBytesPerRow(surf);
        entry.flags = flags;
        IOSurfaceRelease(surf);

        if (!entry.width || !entry.height || !entry.stride) {
            return false;
        }

        const uint32_t salt = (uint32_t)OSIncrementAtomic((volatile SInt32*)&mSalt);
        const uint64_t handle = makeHandle(surfaceId, salt);

        OSData* data = OSData::withBytes(&entry, sizeof(entry));
        if (!data) {
            return false;
        }

        char key[32];
        snprintf(key, sizeof(key), "%llu", (unsigned long long)handle);

        IOLockLock(mLock);
        mMap->setObject(key, data);
        IOLockUnlock(mLock);
        data->release();

        *outHandle = handle;
        *outMeta = entry;
        return true;
    }

    bool lookup(uint64_t handle, FXE_SurfaceEntry* outMeta) {
        if (!isReady() || !outMeta) {
            return false;
        }

        char key[32];
        snprintf(key, sizeof(key), "%llu", (unsigned long long)handle);

        IOLockLock(mLock);
        OSData* data = OSDynamicCast(OSData, mMap->getObject(key));
        bool ok = false;
        if (data && data->getLength() == sizeof(FXE_SurfaceEntry)) {
            const FXE_SurfaceEntry* entry = (const FXE_SurfaceEntry*)data->getBytesNoCopy();
            if (entry) {
                *outMeta = *entry;
                ok = true;
            }
        }
        IOLockUnlock(mLock);
        return ok;
    }

    void clearAll() {
        if (!isReady()) {
            return;
        }
        IOLockLock(mLock);
        mMap->flushCollection();
        IOLockUnlock(mLock);
    }

private:
    IOLock* mLock = nullptr;
    OSDictionary* mMap = nullptr;
    uint32_t mSalt = 1;
};
