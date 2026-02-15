//
//  FakeIrisXEContext.hpp
//  FakeIrisXEFramebuffer
//
//  V58: Context Allocation and 3D Pipeline Setup
//

#ifndef FakeIrisXEContext_hpp
#define FakeIrisXEContext_hpp

#include <IOKit/IOLib.h>
#include "FakeIrisXEGEM.hpp"

// Forward declaration
class FakeIrisXEFramebuffer;

// V58: Context Image Format (based on Intel PRM Vol 7)
// Tiger Lake Gen12 context image structure
struct LRCContextImage {
    // Offset 0x0000 - 0x0FFF: Per-Process HW Status Page (ppHWSP)
    // - Seqno tracking
    // - Context status
    // - CSB ring buffer
    
    // Offset 0x1000+: LRC State (Register context)
    // - Ring registers
    // - PPGTT pointers
    // - Workaround batches
};

// V58: Context descriptor format (64-bit)
struct ContextDescriptor {
    union {
        uint64_t raw;
        struct {
            uint32_t flags:12;        // bits 0-11
            uint32_t lrca_lo:20;      // bits 12-31 (LRC address bits 31:12)
            uint32_t lrca_hi:21;      // bits 32-52 (LRC address bits 53:33)
            uint32_t reserved:2;      // bits 53-54 (MBZ)
            uint32_t group_id:9;      // bits 55-63
        } gen8;
        struct {
            uint32_t flags:12;        // bits 0-11
            uint32_t lrca_lo:20;      // bits 12-31
            uint32_t lrca_hi:5;       // bits 32-36
            uint32_t sw_ctx_id:11;    // bits 37-47
            uint32_t engine_instance:6; // bits 48-53
            uint32_t reserved:1;      // bit 54 (MBZ)
            uint32_t sw_counter:6;    // bits 55-60
            uint32_t engine_class:3;  // bits 61-63
        } gen11;
    };
};

// V58: Context flags
#define CTX_VALID                (1 << 0)
#define CTX_FORCE_PD_RESTORE     (1 << 1)
#define CTX_FORCE_RESTORE        (1 << 2)
#define CTX_ADDRESSING_MODE_SHIFT 3
#define CTX_L3LLC_COHERENT       (1 << 5)
#define CTX_PRIVILEGE            (1 << 8)

// V58: Engine classes
#define ENGINE_CLASS_RENDER      0
#define ENGINE_CLASS_VIDEO       1
#define ENGINE_CLASS_VIDEO_ENHANCE 2
#define ENGINE_CLASS_COPY        3

// V58: Context state structure
struct XEContextState {
    uint64_t ring_start;
    uint32_t ring_head;
    uint32_t ring_tail;
    uint32_t ring_ctl;
    uint32_t ring_buffer_size;
    
    uint64_t pp_hwsp_gtt;
    uint64_t lrc_gtt;
    
    uint32_t context_id;
    uint32_t priority;
    bool valid;
    bool active;
    
    // V58: 3D pipeline state
    uint32_t prim_mode;
    uint32_t blend_state;
    uint32_t depth_state;
    uint32_t stencil_state;
    uint32_t viewport_state;
    uint32_t scissor_state;
};

class FakeIrisXEContext : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEContext)

public:
    static FakeIrisXEContext* withOwner(FakeIrisXEFramebuffer* owner);
    void free() override;

    // V58: Context lifecycle
    bool allocateContext(uint32_t ctx_id, uint32_t priority);
    void releaseContext();
    bool validateContext();
    
    // V58: Context programming
    bool programLRCState();
    bool updateRingPointers(uint32_t head, uint32_t tail);
    uint64_t getContextDescriptor();
    
    // V58: 3D Pipeline setup
    bool setup3DPipeline();
    bool setRenderTarget(uint64_t fb_gtt, uint32_t width, uint32_t height, uint32_t format);
    bool setViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    bool setScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    bool enableDepthTest(bool enable);
    bool enableBlend(bool enable);
    
    // V58: State management
    void dumpContextState(const char* label);
    bool isValid() const { return fState.valid; }
    uint32_t getContextId() const { return fState.context_id; }
    XEContextState* getState() { return &fState; }

private:
    FakeIrisXEFramebuffer* fOwner;
    
    // V58: Context resources
    FakeIrisXEGEM* fLrcGem;
    FakeIrisXEGEM* fRingGem;
    FakeIrisXEGEM* fPpHwspGem;
    FakeIrisXEGEM* fRingBufferGem;
    
    uint64_t fLrcGGTT;
    uint64_t fRingGGTT;
    uint64_t fPpHwspGGTT;
    uint64_t fRingBufferGGTT;
    
    XEContextState fState;
    
    // V58: Constants
    static const size_t kLrcSize = 56 * 4096;      // 56 pages for Gen11 RCS
    static const size_t kRingSize = 32 * 4096;     // 32KB ring buffer
    static const size_t kPpHwspSize = 4096;        // 4KB status page
    static const size_t kRingBufferSize = 128 * 4096; // 128KB command ring
    
    // V58: LRC register offsets
    static const uint32_t LRC_RING_START = 0x20;
    static const uint32_t LRC_RING_HEAD = 0x24;
    static const uint32_t LRC_RING_TAIL = 0x28;
    static const uint32_t LRC_RING_CTL = 0x2C;
    static const uint32_t LRC_PPHWSP = 0x40;
    
    // V58: Helpers
    static inline void write_le32(uint8_t* p, uint32_t v) { *(uint32_t*)p = v; }
    static inline void write_le64(uint8_t* p, uint64_t v) { *(uint64_t*)p = v; }
    uint32_t mmioRead32(uint32_t off);
    void mmioWrite32(uint32_t off, uint32_t val);
    bool initializeLRC();
    bool initializeRingBuffer();
    void writeLRCRegister(uint32_t offset, uint32_t value);
    void writeLRCRegister64(uint32_t offset, uint64_t value);
};

#endif /* FakeIrisXEContext_hpp */
