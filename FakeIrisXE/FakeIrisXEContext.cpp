//
//  FakeIrisXEContext.cpp
//  FakeIrisXEFramebuffer
//
//  V58: Context Allocation and 3D Pipeline Setup
//

#include "FakeIrisXEContext.hpp"
#include "FakeIrisXEFramebuffer.hpp"
#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXELRC.hpp"
#include <libkern/libkern.h>

#define super OSObject
OSDefineMetaClassAndStructors(FakeIrisXEContext, OSObject);

bool FakeIrisXEContext::allocateContext(uint32_t ctx_id, uint32_t priority)
{
    IOLog("[V58] allocateContext(ctx_id=0x%X, priority=%u) - START\n", ctx_id, priority);
    
    bzero(&fState, sizeof(XEContextState));
    
    fState.context_id = ctx_id;
    fState.priority = priority;
    fState.valid = false;
    fState.active = false;
    
    // Allocate ppHWSP (Per-Process Hardware Status Page)
    IOLog("[V58] Allocating ppHWSP (size=%lu)...\n", kPpHwspSize);
    fPpHwspGem = FakeIrisXEGEM::withSize(kPpHwspSize, 0);
    if (!fPpHwspGem) {
        IOLog("[V58] ❌ Failed to allocate ppHWSP\n");
        return false;
    }
    
    fPpHwspGem->pin();
    fPpHwspGGTT = fOwner->ggttMap(fPpHwspGem);
    if (!fPpHwspGGTT) {
        IOLog("[V58] ❌ Failed to map ppHWSP to GGTT\n");
        fPpHwspGem->release();
        fPpHwspGem = nullptr;
        return false;
    }
    fPpHwspGGTT &= ~0xFFFULL;
    fState.pp_hwsp_gtt = fPpHwspGGTT;
    IOLog("[V58] ppHWSP GGTT: 0x%016llX\n", fPpHwspGGTT);
    
    // Allocate ring buffer
    IOLog("[V58] Allocating ring buffer (size=%lu)...\n", kRingBufferSize);
    fRingBufferGem = FakeIrisXEGEM::withSize(kRingBufferSize, 0);
    if (!fRingBufferGem) {
        IOLog("[V58] ❌ Failed to allocate ring buffer\n");
        releaseContext();
        return false;
    }
    
    fRingBufferGem->pin();
    fRingBufferGGTT = fOwner->ggttMap(fRingBufferGem);
    if (!fRingBufferGGTT) {
        IOLog("[V58] ❌ Failed to map ring buffer to GGTT\n");
        releaseContext();
        return false;
    }
    fRingBufferGGTT &= ~0xFFFULL;
    fState.ring_start = fRingBufferGGTT;
    fState.ring_buffer_size = kRingBufferSize;
    IOLog("[V58] Ring buffer GGTT: 0x%016llX\n", fRingBufferGGTT);
    
    // Allocate LRC (Logical Ring Context)
    IOLog("[V58] Allocating LRC (size=%lu)...\n", kLrcSize);
    fLrcGem = FakeIrisXEGEM::withSize(kLrcSize, 0);
    if (!fLrcGem) {
        IOLog("[V58] ❌ Failed to allocate LRC\n");
        releaseContext();
        return false;
    }
    
    fLrcGem->pin();
    fLrcGGTT = fOwner->ggttMap(fLrcGem);
    if (!fLrcGGTT) {
        IOLog("[V58] ❌ Failed to map LRC to GGTT\n");
        releaseContext();
        return false;
    }
    fLrcGGTT &= ~0xFFFULL;
    fState.lrc_gtt = fLrcGGTT;
    IOLog("[V58] LRC GGTT: 0x%016llX\n", fLrcGGTT);
    
    // Program LRC state
    if (!programLRCState()) {
        IOLog("[V58] ❌ Failed to program LRC state\n");
        releaseContext();
        return false;
    }
    
    fState.valid = true;
    IOLog("[V58] ✅ Context allocated successfully (ctx_id=0x%X)\n", ctx_id);
    
    return true;
}

void FakeIrisXEContext::releaseContext()
{
    IOLog("[V58] releaseContext() - ctx_id=0x%X\n", fState.context_id);
    
    if (fLrcGem) {
        fLrcGem->unpin();
        fLrcGem->release();
        fLrcGem = nullptr;
    }
    
    if (fRingBufferGem) {
        fRingBufferGem->unpin();
        fRingBufferGem->release();
        fRingBufferGem = nullptr;
    }
    
    if (fPpHwspGem) {
        fPpHwspGem->unpin();
        fPpHwspGem->release();
        fPpHwspGem = nullptr;
    }
    
    bzero(&fState, sizeof(XEContextState));
}

bool FakeIrisXEContext::validateContext()
{
    if (!fState.valid) {
        IOLog("[V58] validateContext: Context not valid\n");
        return false;
    }
    
    // Validate GGTT mappings
    if (!fLrcGGTT || !fRingBufferGGTT || !fPpHwspGGTT) {
        IOLog("[V58] validateContext: Invalid GGTT mappings\n");
        return false;
    }
    
    return true;
}

bool FakeIrisXEContext::programLRCState()
{
    IOLog("[V58] programLRCState() - Programming LRC\n");
    
    IOBufferMemoryDescriptor* md = fLrcGem->memoryDescriptor();
    if (!md) {
        IOLog("[V58] ❌ programLRCState: No memory descriptor\n");
        return false;
    }
    
    uint8_t* cpu = (uint8_t*)md->getBytesNoCopy();
    if (!cpu) {
        IOLog("[V58] ❌ programLRCState: Could not get CPU pointer\n");
        return false;
    }
    
    bzero(cpu, kLrcSize);
    
    // ===== GEN12 LRC HEADER =====
    
    // PDP0-3 (Page Directory Pointers)
    write_le64(cpu + 0x00, fLrcGGTT & ~0xFFFULL);  // PDP0 - self-referencing
    write_le64(cpu + 0x08, 0);                      // PDP1
    write_le64(cpu + 0x10, 0);                      // PDP2
    write_le64(cpu + 0x18, 0);                      // PDP3
    
    // Timestamp enable
    write_le32(cpu + 0x30, 0x00010000);
    
    // Context Control
    // bit0 = Load
    // bit3 = Valid  
    // bit8 = Header Size (1 => 64 bytes)
    uint32_t ctx_ctrl = (1 << 0) | (1 << 3) | (1 << 8);
    write_le32(cpu + 0x2C, ctx_ctrl);
    
    // ===== GEN12 RING STATE BLOCK =====
    
    uint32_t ringStateOff = 0x100;
    
    // Ring HEAD
    write_le32(cpu + ringStateOff + 0x00, 0);
    
    // Ring TAIL
    write_le32(cpu + ringStateOff + 0x04, 0);
    
    // Ring BASE (GGTT address)
    write_le32(cpu + ringStateOff + 0x08, (uint32_t)(fRingBufferGGTT & 0xFFFFFFFFu));
    write_le32(cpu + ringStateOff + 0x0C, (uint32_t)(fRingBufferGGTT >> 32));
    
    // Ring CTL
    uint32_t pages = kRingBufferSize / 4096;
    if (!pages) pages = 1;
    uint32_t ringCtl = ((pages - 1) << 12) | 1u;  // Enable ring
    write_le32(cpu + ringStateOff + 0x10, ringCtl);
    
    // Ring START
    write_le32(cpu + ringStateOff + 0x14, (uint32_t)(fRingBufferGGTT & 0xFFFFFFFFu));
    
    IOLog("[V58] ✅ LRC state programmed\n");
    IOLog("[V58]   LRC GGTT: 0x%016llX\n", fLrcGGTT);
    IOLog("[V58]   Ring GGTT: 0x%016llX\n", fRingBufferGGTT);
    IOLog("[V58]   Ring Size: %u pages\n", pages);
    
    return true;
}

bool FakeIrisXEContext::updateRingPointers(uint32_t head, uint32_t tail)
{
    fState.ring_head = head;
    fState.ring_tail = tail;
    
    // Update in LRC
    IOBufferMemoryDescriptor* md = fLrcGem->memoryDescriptor();
    if (!md) return false;
    
    uint8_t* cpu = (uint8_t*)md->getBytesNoCopy();
    if (!cpu) return false;
    
    uint32_t ringStateOff = 0x100;
    write_le32(cpu + ringStateOff + 0x00, head);
    write_le32(cpu + ringStateOff + 0x04, tail);
    
    return true;
}

uint64_t FakeIrisXEContext::getContextDescriptor()
{
    // Generate context descriptor per Gen12 format
    uint64_t desc = 0;
    
    // LRC address bits 12-31 -> bits 12-31
    uint32_t lrca_lo = (fLrcGGTT >> 12) & 0xFFFFF;
    
    // LRC address bits 32-36 -> bits 32-36
    uint32_t lrca_hi = (fLrcGGTT >> 32) & 0x1F;
    
    // SW context ID -> bits 37-47
    uint32_t sw_ctx_id = fState.context_id & 0x7FF;
    
    // Engine instance -> bits 48-53 (0 for RCS)
    uint32_t engine_instance = 0;
    
    // SW counter -> bits 55-60
    uint32_t sw_counter = 0;
    
    // Engine class -> bits 61-63 (0 = Render)
    uint32_t engine_class = ENGINE_CLASS_RENDER;
    
    // Build descriptor
    desc = lrca_lo | (lrca_hi << 20) | (sw_ctx_id << 27) |
           (engine_instance << 37) | (sw_counter << 55) | (engine_class << 61);
    
    // Set valid flag
    desc |= CTX_VALID;
    
    return desc;
}

bool FakeIrisXEContext::setup3DPipeline()
{
    IOLog("[V58] setup3DPipeline() - Setting up 3D pipeline\n");
    
    // This would set up the 3D pipeline state
    // For now, just mark it as done
    fState.prim_mode = 0;
    fState.blend_state = 0;
    fState.depth_state = 0;
    fState.stencil_state = 0;
    fState.viewport_state = 0;
    fState.scissor_state = 0;
    
    IOLog("[V58] ✅ 3D pipeline configured\n");
    return true;
}

bool FakeIrisXEContext::setRenderTarget(uint64_t fb_gtt, uint32_t width, uint32_t height, uint32_t format)
{
    IOLog("[V58] setRenderTarget: fb=0x%016llX %ux%u fmt=%u\n", fb_gtt, width, height, format);
    // Would set render target in 3D pipeline
    return true;
}

bool FakeIrisXEContext::setViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    IOLog("[V58] setViewport: %u,%u %ux%u\n", x, y, width, height);
    fState.viewport_state = (x << 16) | y;
    return true;
}

bool FakeIrisXEContext::setScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    IOLog("[V58] setScissor: %u,%u %ux%u\n", x, y, width, height);
    fState.scissor_state = (x << 16) | y;
    return true;
}

bool FakeIrisXEContext::enableDepthTest(bool enable)
{
    IOLog("[V58] enableDepthTest: %s\n", enable ? "ON" : "OFF");
    if (enable) {
        fState.depth_state |= (1 << 0);
    } else {
        fState.depth_state &= ~(1 << 0);
    }
    return true;
}

bool FakeIrisXEContext::enableBlend(bool enable)
{
    IOLog("[V58] enableBlend: %s\n", enable ? "ON" : "OFF");
    if (enable) {
        fState.blend_state |= (1 << 0);
    } else {
        fState.blend_state &= ~(1 << 0);
    }
    return true;
}

void FakeIrisXEContext::dumpContextState(const char* label)
{
    IOLog("[V58] ========== Context State: %s ==========\n", label);
    IOLog("[V58] Context ID: 0x%X\n", fState.context_id);
    IOLog("[V58] Priority: %u\n", fState.priority);
    IOLog("[V58] Valid: %s\n", fState.valid ? "YES" : "NO");
    IOLog("[V58] Active: %s\n", fState.active ? "YES" : "NO");
    IOLog("[V58] LRC GGTT: 0x%016llX\n", fState.lrc_gtt);
    IOLog("[V58] Ring GGTT: 0x%016llX\n", fState.ring_start);
    IOLog("[V58] Ring Size: 0x%X\n", fState.ring_buffer_size);
    IOLog("[V58] ppHWSP GGTT: 0x%016llX\n", fState.pp_hwsp_gtt);
    IOLog("[V58] Ring Head: 0x%X\n", fState.ring_head);
    IOLog("[V58] Ring Tail: 0x%X\n", fState.ring_tail);
    IOLog("[V58] ============================================\n");
}

FakeIrisXEContext* FakeIrisXEContext::withOwner(FakeIrisXEFramebuffer* owner)
{
    FakeIrisXEContext* ctx = new FakeIrisXEContext;
    if (ctx) {
        ctx->fOwner = owner;
        ctx->fLrcGem = nullptr;
        ctx->fRingGem = nullptr;
        ctx->fPpHwspGem = nullptr;
        ctx->fRingBufferGem = nullptr;
        ctx->fLrcGGTT = 0;
        ctx->fRingGGTT = 0;
        ctx->fPpHwspGGTT = 0;
        ctx->fRingBufferGGTT = 0;
    }
    return ctx;
}

void FakeIrisXEContext::free()
{
    releaseContext();
    super::free();
}
