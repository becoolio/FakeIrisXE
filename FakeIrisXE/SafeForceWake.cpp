#include "SafeForceWake.hpp"
#include "SafeRegisterAccess.hpp"

void SafeForceWakeManager::initialize(void) {}

uint32_t SafeForceWakeManager::determinePowerDomain(uint32_t offset) {
    return SafeRegisterAccess::determinePowerDomainForOffset(offset);
}

bool SafeForceWakeManager::acquireForceWakeDomain(uint32_t, void*) {
    return true;
}

void SafeForceWakeManager::releaseForceWakeDomain(uint32_t, void*) {}

uint32_t SafeForceWakeManager::safeReadRegister32(uint32_t offset, void* mmioBase, uint32_t mmioLength) {
    return SafeRegisterAccess::safeReadRegister32(offset, mmioBase, mmioLength);
}

void SafeForceWakeManager::safeWriteRegister32(uint32_t offset, uint32_t value, void* mmioBase, uint32_t mmioLength) {
    SafeRegisterAccess::safeWriteRegister32(offset, value, mmioBase, mmioLength);
}
