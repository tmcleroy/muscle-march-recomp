// Native GX: the SDK's graphics library (gx.a) as hand-written C++.
//
// Each GX entry point the game calls is a host function. It keeps the SDK's
// own state block (__GXData) in guest memory, so translated and native
// functions agree while the port is in progress, and it emits the same
// command stream the SDK would, straight into the renderer's queue (no FIFO
// MMIO). gx.native=1 (default) / WP_NATIVE_GX=0 runs the translated SDK code.
//
// WP_VERIFY=1 (or WP_GX_VERIFY=1) checks the port against the original with the
// lift toolkit's differential verifier (wp/lift.h); WP_GX_SELFTEST=1 also fuzzes
// the functions the game rarely calls.

#include "wp/gx_api.h"

// Float code below must round like the original single-precision instruction sequence.
#pragma clang fp contract(off)

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "wp/cpu.h"
#include "wp/gx.h"
#include "wp/hle.h"
#include "wp/lift.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/settings.h"
#include "wp/threads.h"

namespace wp::gx_api {

namespace {

using namespace lift;

// --- guest state ---------------------------------------------------------------

uint32_t g_data_pointer = 0;  // address of the __GXData pointer

uint32_t gx() {
    if (!g_data_pointer) {
        g_data_pointer = symbol_address("__GXData");
    }
    return rd32(g_data_pointer);
}

uint32_t ld32(uint32_t offset) {
    return rd32(gx() + offset);
}
void st32(uint32_t offset, uint32_t value) {
    wr32(gx() + offset, value);
}
uint16_t ld16(uint32_t offset) {
    return rd16(gx() + offset);
}
void st16(uint32_t offset, uint16_t value) {
    wr16(gx() + offset, value);
}
uint8_t ld8(uint32_t offset) {
    return rd8(gx() + offset);
}
void st8(uint32_t offset, uint8_t value) {
    wr8(gx() + offset, value);
}

// rlwimi-style field insert: value's low `bits` bits at bit `shift` (LSB = 0).
constexpr uint32_t field(uint32_t word, uint32_t value, int shift, int bits) {
    uint32_t mask = (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1)) << shift;
    return (word & ~mask) | ((value << shift) & mask);
}

// --- command stream --------------------------------------------------------------

void fifo8(uint8_t value) {
    gx::push(value, 1);
}
void fifo16(uint16_t value) {
    gx::push(value, 2);
}
void fifo32(uint32_t value) {
    gx::push(value, 4);
}
void fifo_float(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    gx::push(bits, 4);
}
void bp(uint32_t value) {
    fifo8(0x61);
    fifo32(value);
}
void cp(uint8_t reg, uint32_t value) {
    fifo8(0x08);
    fifo8(reg);
    fifo32(value);
}
void xf(uint16_t address, uint32_t value) {
    fifo8(0x10);
    fifo32(address);
    fifo32(value);
}

// Dirty-state bits in __GXData+0x5FC (flushed by __GXSetDirtyState).
constexpr uint32_t kDirtyState = 0x5FC;
void dirty(uint32_t bits) {
    st32(kDirtyState, ld32(kDirtyState) | bits);
}

// Hardware register blocks, as the SDK addresses them through __cpReg/__peReg/__piReg.
uint32_t cp_reg() {
    return rd32(0x80422954);
}
uint32_t pe_reg() {
    return rd32(0x80422958);
}
uint32_t pi_reg() {
    return rd32(0x80422950);
}

// --- lifted-function plumbing (wp/lift.h) ------------------------------------------------

const lift::Group kGroup{"gx.native", "WP_NATIVE_GX"};

#define GX_HANDLER(fn, name, ret) WP_LIFT(kGroup, fn, name, ret)

// Guest functions outside gx.a (this game's SDK build).
constexpr uint32_t kOSRegisterVersion = 0x801C0D90;
constexpr uint32_t kOSGetTime = 0x801CA620;
constexpr uint32_t kOSDisableInterrupts = 0x801C62E0;
constexpr uint32_t kOSRestoreInterrupts = 0x801C6320;
constexpr uint32_t kOSSetInterruptHandler = 0x801C6350;
constexpr uint32_t kOSUnmaskInterrupts = 0x801C6730;
constexpr uint32_t kOSRegisterShutdownFunction = 0x801C7A00;
constexpr uint32_t kOSSetCurrentContext = 0x801C26B0;
constexpr uint32_t kOSClearContext = 0x801C28F0;
constexpr uint32_t kOSGetCurrentThread = 0x801C92B0;
constexpr uint32_t kOSResumeThread = 0x801C9ED0;
constexpr uint32_t kOSSuspendThread = 0x801CA170;
constexpr uint32_t kVIGetTvFormat = 0x801D6A30;
constexpr uint32_t kPPCMfhid2 = 0x8016D7B0;
constexpr uint32_t kPPCMthid2 = 0x8016D7C0;
constexpr uint32_t kPPCMtwpar = 0x8016D7D0;
constexpr uint32_t kPPCSync = 0x8016D6D0;
constexpr uint32_t kMemset = 0x80004350;

// gx.a functions called from native code (whichever version is installed runs).
constexpr uint32_t kGXSetMisc = 0x801AC6B0;
constexpr uint32_t kGXFifoInit = 0x801AB730;
constexpr uint32_t kGXInitFifoBase = 0x801AB0D0;
constexpr uint32_t kGXSetCPUFifo = 0x801AB260;
constexpr uint32_t kGXSetGPFifo = 0x801AB440;
constexpr uint32_t kCPGPLinkCheck = 0x801AB160;
constexpr uint32_t kGXPEInit = 0x801ACF00;
constexpr uint32_t kGXFlushTextureState = 0x801AFE00;
constexpr uint32_t kGXInitRevisionBits = 0x801AA070;
constexpr uint32_t kGXInitTexCacheRegion = 0x801AF1C0;
constexpr uint32_t kGXInitTlutRegion = 0x801AF270;
constexpr uint32_t kGXSetIndirectMask = 0x801AFDD0;
constexpr uint32_t kGXSetTmemConfig = 0x801AF680;
constexpr uint32_t kGXInitGX = 0x801AA6B0;
constexpr uint32_t kGXGetTexObjFmt = 0x801AEEE0;
constexpr uint32_t kGXGetTexObjMipMap = 0x801AEF10;
constexpr uint32_t kGXSetBreakPtCallback = 0x801AB6E0;
constexpr uint32_t kGXSetDrawSyncCallback = 0x801ACD50;
constexpr uint32_t kGXSetDrawDoneCallback = 0x801ACE30;
constexpr uint32_t kGXAbort = 0x801AC730;
constexpr uint32_t kGXDefaultTexRegionCallback = 0x801A9DE0;
constexpr uint32_t kGXDefaultTlutRegionCallback = 0x801A9ED0;
constexpr uint32_t kGXCPInterruptHandler = 0x801AAF70;

// SDK globals (.sdata/.sbss/.bss/.data of this build).
constexpr uint32_t kGXVersion = 0x80421BC8;
constexpr uint32_t kMemRegPointer = 0x8042295C;
constexpr uint32_t kShutdownCounter = 0x80422960;   // last memory-interface request count
constexpr uint32_t kShutdownTimeHigh = 0x80422968;
constexpr uint32_t kShutdownTimeLow = 0x8042296C;
constexpr uint32_t kShutdownStarted = 0x80422970;
constexpr uint32_t kShutdownRegistered = 0x80422974;
constexpr uint32_t kCPUFifoReady = 0x80422978;      // u8
constexpr uint32_t kGPFifoReady = 0x80422979;       // u8
constexpr uint32_t kOverflowCount = 0x8042297C;
constexpr uint32_t kBreakPointCallback = 0x80422984;
constexpr uint32_t kOverflowSuspended = 0x80422988;
constexpr uint32_t kFifoThread = 0x8042298C;
constexpr uint32_t kFifoLinked = 0x80422990;        // u8: CPU and GP FIFO are the same
constexpr uint32_t kGPFifo = 0x80372C60;            // GXFifoObj copy (0x24 bytes)
constexpr uint32_t kCPUFifo = 0x80372C84;
constexpr uint32_t kFifoObj = 0x803725E0;
constexpr uint32_t kShutdownFuncInfo = 0x8029B5B0;
constexpr uint32_t kTexRegionAddrTable = 0x8029B4F0;
constexpr uint32_t kGXDefaults = 0x8029B380;        // identity matrices, vertex formats, default texture
constexpr uint32_t kRenderModes[] = {0x8029B7A0, 0x8029B854, 0x8029B818, 0x8029B890};  // NTSC, PAL, MPAL, EURGB60

// =====================================================================================
// GXInit
// =====================================================================================

// __GXInvokeCallback: tail call to an optional SDK hook (arguments pass through).
GX_HANDLER(invoke_callback, "__GXInvokeCallback", Ret::Blocking) {
    uint32_t callback = rd32(0x8042294C);
    if (callback) {
        call(c, callback);
    }
}

// __GXDefaultTexRegionCallback(GXTexObj*, GXTexMapID): TMEM region for a texture.
GX_HANDLER(default_tex_region_callback, "__GXDefaultTexRegionCallback", Ret::Int) {
    uint32_t object = c.r[3];
    uint32_t map = c.r[4];
    uint32_t format = gcall(c, kGXGetTexObjFmt, {object});
    uint8_t mipmap = static_cast<uint8_t>(gcall(c, kGXGetTexObjMipMap, {object}));
    uint32_t index = static_cast<uint32_t>(static_cast<int32_t>(map) % 8);
    uint32_t region;
    if (format - 8 <= 2) {  // color-indexed
        region = 0x258;
    } else if (format == 6) {  // RGBA8
        region = mipmap ? 0x358 : 0x2D8;
    } else {
        region = mipmap ? 0x2D8 : 0x258;
    }
    c.r[3] = gx() + region + 16 * index;
}

// __GXDefaultTlutRegionCallback(u32 tlut)
GX_HANDLER(default_tlut_region_callback, "__GXDefaultTlutRegionCallback", Ret::Int) {
    uint32_t tlut = c.r[3];
    c.r[3] = tlut >= 0x14 ? 0 : gx() + 0x3D8 + 16 * tlut;
}

uint32_t memory_request_count() {
    uint32_t mem = rd32(kMemRegPointer);
    uint32_t high = rd16(mem + 0x4E);
    uint32_t previous;
    uint32_t low;
    do {
        previous = high;
        low = rd16(mem + 0x50);
        high = rd16(mem + 0x4E);
    } while (high != previous);
    return (high << 16) | low;
}

// __GXShutdown(BOOL final): OS shutdown hook. Not final: report "idle" once the memory
// interface stops seeing GPU requests; final: drop callbacks and stop the command processor.
GX_HANDLER(shutdown, "__GXShutdown", Ret::Int) {
    if (c.r[3] == 0) {
        if (rd32(kShutdownStarted) == 0) {
            uint32_t count = memory_request_count();
            wr32(kShutdownCounter, count);
            gcall(c, kOSGetTime);
            wr32(kShutdownTimeHigh, c.r[3]);
            wr32(kShutdownTimeLow, c.r[4]);
            wr32(kShutdownStarted, 1);
            c.r[3] = 0;
            return;
        }
        gcall(c, kOSGetTime);
        uint64_t now = (static_cast<uint64_t>(c.r[3]) << 32) | c.r[4];
        uint32_t count = memory_request_count();
        uint64_t then = (static_cast<uint64_t>(rd32(kShutdownTimeHigh)) << 32) | rd32(kShutdownTimeLow);
        if (static_cast<int64_t>(now - then) < 10) {
            c.r[3] = 0;
            return;
        }
        if (count != rd32(kShutdownCounter)) {
            wr32(kShutdownTimeHigh, static_cast<uint32_t>(now >> 32));
            wr32(kShutdownCounter, count);
            wr32(kShutdownTimeLow, static_cast<uint32_t>(now));
            c.r[3] = 0;
            return;
        }
        c.r[3] = 1;
        return;
    }
    gcall(c, kGXSetBreakPtCallback, {0});
    gcall(c, kGXSetDrawSyncCallback, {0});
    gcall(c, kGXSetDrawDoneCallback, {0});
    for (int i = 0; i < 8; i++) {
        fifo32(0);
    }
    gcall(c, kPPCSync);
    wr16(cp_reg() + 0x2, 0);
    wr16(cp_reg() + 0x4, 3);
    st8(0x5FA, 1);
    gcall(c, kGXAbort);
    c.r[3] = 1;
}

// __GXInitRevisionBits: mark the vertex formats "normal index 3" and "byte dequant", load the
// XF/BP revision defaults.
GX_HANDLER(init_revision_bits, "__GXInitRevisionBits", Ret::None) {
    uint32_t g = gx();
    for (uint32_t format = 0; format < 8; format++) {
        uint32_t a = g + 0x1C + 4 * format;
        uint32_t b = g + 0x3C + 4 * format;
        wr32(a, rd32(a) | 0x40000000);
        wr32(b, rd32(b) | 0x80000000);
        cp(static_cast<uint8_t>(0x80 | format), rd32(b));
    }
    xf(0x1000, 0x3F);
    xf(0x1012, 1);
    bp(0x5800000F);
}

GX_HANDLER(init, "GXInit", Ret::Int) {
    uint32_t base = c.r[3];
    uint32_t size = c.r[4];
    gcall(c, kOSRegisterVersion, {rd32(kGXVersion)});
    uint32_t g = gx();
    wr8(g + 0x5F8, 0);
    wr8(g + 0x5F9, 1);
    wr8(g + 0x5FA, 1);
    wr32(g + 0x5E4, 0);
    wr32(g + 0x5E8, 0);
    gcall(c, kGXSetMisc, {1, 0});
    wr32(0x80422954, 0xCC000000);  // __cpReg
    wr32(0x80422950, 0xCC003000);  // __piReg
    wr32(0x80422958, 0xCC001000);  // __peReg
    wr32(kMemRegPointer, 0xCC004000);
    gcall(c, kGXFifoInit);
    gcall(c, kGXInitFifoBase, {kFifoObj, base, size});
    gcall(c, kGXSetCPUFifo, {kFifoObj});
    gcall(c, kGXSetGPFifo, {kFifoObj});
    if (rd32(kShutdownRegistered) == 0) {
        gcall(c, kOSRegisterShutdownFunction, {kShutdownFuncInfo});
        wr32(kShutdownRegistered, 1);
    }
    gcall(c, kGXPEInit);
    uint32_t hid2 = gcall(c, kPPCMfhid2);
    gcall(c, kPPCMtwpar, {0x0C008000});
    gcall(c, kPPCMthid2, {hid2 | 0x40000000});

    // Shadow registers.
    wr32(g + 0x254, 0);           // genMode
    wr32(g + 0x174, 0x0F0000FF);  // BP mask
    wr32(g + 0x7C, 0x22000000);
    for (uint32_t k = 0; k < 16; k++) {
        wr32(g + 0x180 + 4 * k, (0xC0 + 2 * k) << 24);  // TEV color stage
        wr32(g + 0x1C0 + 4 * k, (0xC1 + 2 * k) << 24);  // TEV alpha stage
        wr32(g + 0x5A4 + 4 * k, 0xFF);
    }
    for (uint32_t j = 0; j < 8; j++) {
        wr32(g + 0x150 + 4 * j, (0x28 + j) << 24);                          // TEV order
        wr32(g + 0x200 + 4 * j, rlwimi(rd32(g + 0x200 + 4 * j), 0xF6 + j, 24, 0, 7));  // konst select
    }
    wr32(g + 0x170, 0x27000000);
    for (uint32_t m = 0; m < 8; m++) {
        wr32(g + 0x108 + 4 * m, (0x30 + 2 * m) << 24);  // texture coordinate scale
        wr32(g + 0x128 + 4 * m, (0x31 + 2 * m) << 24);
    }
    wr32(g + 0x148, rlwimi(rd32(g + 0x148), 0x20, 24, 0, 7));
    wr32(g + 0x14C, rlwimi(rd32(g + 0x14C), 0x21, 24, 0, 7));
    wr32(g + 0x220, rlwimi(rd32(g + 0x220), 0x41, 24, 0, 7));
    wr32(g + 0x224, rlwimi(rd32(g + 0x224), 0x42, 24, 0, 7));
    wr32(g + 0x228, rlwimi(rd32(g + 0x228), 0x40, 24, 0, 7));
    wr32(g + 0x22C, rlwimi(rd32(g + 0x22C), 0x43, 24, 0, 7));
    wr32(g + 0x24C, rlwinm(rd32(g + 0x24C), 0, 25, 22));
    stf(g + 0x560, 16777216.0f);
    stf(g + 0x55C, 0.0f);
    wr32(g + 0x5FC, 0);
    wr8(g + 0x5FB, 0);

    // Texture cache and memory timing from the bus clock.
    uint32_t ticks = static_cast<uint32_t>((static_cast<uint64_t>(0x10624DD3) * rd32(0x800000F8)) >> 32) >> 5;
    gcall(c, kGXFlushTextureState);
    bp(0x69000400 | (ticks >> 11));
    gcall(c, kGXFlushTextureState);
    bp(0x46000200 | (static_cast<uint32_t>((static_cast<uint64_t>(0x3E0F83E1) * ticks) >> 32) >> 10));
    gcall(c, kGXInitRevisionBits);

    for (uint32_t i = 0; i < 8; i++) {
        uint32_t t = kTexRegionAddrTable + 4 * i;
        gcall(c, kGXInitTexCacheRegion, {g + 0x258 + 16 * i, 0, rd32(t), 0, rd32(t + 0x20), 0});
        gcall(c, kGXInitTexCacheRegion, {g + 0x2D8 + 16 * i, 0, rd32(t + 0x40), 0, rd32(t + 0x60), 0});
        gcall(c, kGXInitTexCacheRegion, {g + 0x358 + 16 * i, 1, rd32(t + 0x80), 0, rd32(t + 0xA0), 0});
    }
    for (uint32_t k = 0; k < 16; k++) {
        gcall(c, kGXInitTlutRegion, {g + 0x3D8 + 16 * k, 0xC0000 + 0x2000 * k, 0x10});
    }
    for (uint32_t k = 0; k < 4; k++) {
        gcall(c, kGXInitTlutRegion, {g + 0x3D8 + 16 * (16 + k), 0xE0000 + 0x8000 * k, 0x40});
    }
    wr16(cp_reg() + 0x6, 0);
    wr32(g + 0x5F4, rlwinm(rd32(g + 0x5F4), 0, 28, 23));
    cp(0x20, rd32(g + 0x5F4));
    xf(0x1006, 0);
    bp(0x23000000);
    bp(0x24000000);
    bp(0x67000000);
    gcall(c, kGXSetIndirectMask, {0});
    gcall(c, kGXSetTmemConfig, {2});
    gcall(c, kGXInitGX);
    c.r[3] = kFifoObj;
}

// __GXInitGX: the SDK's default state (render mode from the TV format).
GX_HANDLER(init_gx, "__GXInitGX", Ret::None) {
    uint32_t mode;
    switch (gcall(c, kVIGetTvFormat)) {
    case 1:
        mode = kRenderModes[1];
        break;
    case 2:
        mode = kRenderModes[2];
        break;
    case 5:
        mode = kRenderModes[3];
        break;
    default:
        mode = kRenderModes[0];
        break;
    }
    const uint32_t defaults = kGXDefaults;
    const uint32_t gray = 0x404040FF;
    const uint32_t black = 0;
    const uint32_t white = 0xFFFFFFFF;
    GuestFrame frame(c, 0x60);
    uint32_t color = frame.base();         // GXColor
    uint32_t texture = frame.base() + 4;   // GXTexObj (0x20)
    uint32_t matrix = frame.base() + 0x24; // Mtx

    wr32(color, gray);
    gcall(c, 0x801ADC80, {color, 0x00FFFFFF});  // GXSetCopyClear
    for (uint32_t i = 0; i < 8; i++) {
        gcall(c, 0x801AC430, {i, 1, 4 + i, 0x3C, 0, 0x7D});  // GXSetTexCoordGen2
    }
    gcall(c, 0x801AC680, {1});  // GXSetNumTexGens
    gcall(c, 0x801ABFB0);       // GXClearVtxDesc
    gcall(c, 0x801AC420);       // GXInvalidateVtxCache
    for (uint32_t attr = 9; attr <= 0x18; attr++) {
        gcall(c, 0x801AC3E0, {attr, gx(), 0});  // GXSetArray
    }
    for (uint32_t format = 0; format < 8; format++) {
        gcall(c, 0x801AC190, {format, defaults + 0x80});  // GXSetVtxAttrFmtv
    }
    gcall(c, 0x801AD420, {6, 0});  // GXSetLineWidth
    gcall(c, 0x801AD460, {6, 0});  // GXSetPointSize
    for (uint32_t i = 0; i < 8; i++) {
        gcall(c, 0x801AD4A0, {i, 0, 0});  // GXEnableTexOffsets
    }
    for (uint32_t i = 0; i < 12; i++) {
        stf(matrix + 4 * i, (i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f);
    }
    gcall(c, 0x801B0CB0, {matrix, 0});        // GXLoadPosMtxImm
    gcall(c, 0x801B0D00, {matrix, 0});        // GXLoadNrmMtxImm
    gcall(c, 0x801B0D60, {0});                // GXSetCurrentMtx
    gcall(c, 0x801B0D80, {matrix, 0x3C, 0});  // GXLoadTexMtxImm
    gcall(c, 0x801B0D80, {matrix, 0x7D, 0});
    double width = static_cast<float>(rd16(mode + 0x4));
    double height = static_cast<float>(rd16(mode + 0x8));
    gcall(c, 0x801B0F10, {}, {0.0, 0.0, width, height, 0.0, 1.0});  // GXSetViewport
    gcall(c, 0x801B0C60, {defaults + 0x150});                       // GXSetProjectionv
    gcall(c, 0x801AD550);                                            // GXSetCoPlanar(FALSE)?
    gcall(c, 0x801AD510, {0});
    gcall(c, 0x801AD4E0, {2});  // GXSetCullMode(GX_CULL_BACK)
    gcall(c, 0x801B0FF0, {0});  // GXSetClipMode
    gcall(c, 0x801B0F40, {0, 0, rd16(mode + 0x4), rd16(mode + 0x6)});  // GXSetScissor
    gcall(c, 0x801B0FB0, {0, 0});                                      // GXSetScissorBoxOffset
    gcall(c, 0x801AE810, {0});                                         // GXSetNumChans
    for (uint32_t channel : {4u, 5u}) {
        gcall(c, 0x801AE840, {channel, 0, 0, 1, 0, 0, 2});  // GXSetChanCtrl
        wr32(color, black);
        gcall(c, 0x801AE650, {channel, color});  // GXSetChanAmbColor
        wr32(color, white);
        gcall(c, 0x801AE730, {channel, color});  // GXSetChanMatColor
    }
    gcall(c, 0x801AF290);                                 // GXInvalidateTexAll
    gcall(c, 0x801AF2E0, {kGXDefaultTexRegionCallback});  // GXSetTexRegionCallback
    gcall(c, 0x801AF300, {kGXDefaultTlutRegionCallback}); // GXSetTlutRegionCallback
    gcall(c, 0x801AEAE0, {texture, defaults + 0x60, 4, 4, 3, 0, 0, 0});  // GXInitTexObj
    for (uint32_t map = 0; map < 8; map++) {
        gcall(c, 0x801AF090, {texture, map});  // GXLoadTexObj
    }
    for (uint32_t stage = 0; stage < 8; stage++) {
        gcall(c, 0x801B0370, {stage, stage, stage, 4});  // GXSetTevOrder
    }
    for (uint32_t stage = 8; stage < 16; stage++) {
        gcall(c, 0x801B0370, {stage, 0xFF, 0xFF, 0xFF});
    }
    gcall(c, 0x801B04D0, {1});              // GXSetNumTevStages
    gcall(c, 0x801AFE30, {0, 3});           // GXSetTevOp
    gcall(c, 0x801B02A0, {7, 0, 0, 7, 0});  // GXSetAlphaCompare
    gcall(c, 0x801B02E0, {0, 0x11, 0});     // GXSetZTexture
    for (uint32_t stage = 0; stage < 16; stage++) {
        gcall(c, 0x801B0140, {stage, 6});     // GXSetTevKColorSel
        gcall(c, 0x801B0190, {stage, 0});     // GXSetTevKAlphaSel
        gcall(c, 0x801B01E0, {stage, 0, 0});  // GXSetTevSwapMode
    }
    gcall(c, 0x801B0220, {0, 0, 1, 2, 3});  // GXSetTevSwapModeTable
    gcall(c, 0x801B0220, {1, 0, 0, 0, 3});
    gcall(c, 0x801B0220, {2, 1, 1, 1, 3});
    gcall(c, 0x801B0220, {3, 2, 2, 2, 3});
    for (uint32_t stage = 0; stage < 16; stage++) {
        gcall(c, 0x801AFD70, {stage});  // GXSetTevDirect
    }
    gcall(c, 0x801AFD50, {0});  // GXSetNumIndStages
    for (uint32_t stage = 0; stage < 4; stage++) {
        gcall(c, 0x801AFB80, {stage, 0, 0});  // GXSetIndTexCoordScale
    }
    wr32(color, black);
    gcall(c, 0x801B0500, {0, color}, {0.0, 1.0, single(0.1), 1.0});  // GXSetFog
    gcall(c, 0x801B0730, {0, 0, 0});        // GXSetFogRangeAdj
    gcall(c, 0x801B0840, {0, 4, 5, 0});     // GXSetBlendMode
    gcall(c, 0x801B0890, {1});              // GXSetColorUpdate
    gcall(c, 0x801B08C0, {1});              // GXSetAlphaUpdate
    gcall(c, 0x801B08F0, {1, 3, 1});        // GXSetZMode
    gcall(c, 0x801B0930, {1});              // GXSetZCompLoc
    gcall(c, 0x801B0A10, {1});              // GXSetDither
    gcall(c, 0x801B0A40, {0, 0});           // GXSetDstAlpha
    gcall(c, 0x801B0960, {0, 0});           // GXSetPixelFmt
    gcall(c, 0x801B0A70, {1, 1});           // GXSetFieldMask
    uint32_t half = rd16(mode + 0x10) == 2u * rd16(mode + 0x8) ? 1 : 0;
    gcall(c, 0x801B0AB0, {rd8(mode + 0x18), half});  // GXSetFieldMode
    gcall(c, 0x801AD6E0, {0, 0, rd16(mode + 0x4), rd16(mode + 0x6)});  // GXSetDispCopySrc
    gcall(c, 0x801AD760, {rd16(mode + 0x4), rd16(mode + 0x6)});        // GXSetDispCopyDst
    float scale = static_cast<float>(rd16(mode + 0x8)) / static_cast<float>(rd16(mode + 0x6));
    gcall(c, 0x801ADBB0, {}, {scale});                                 // GXSetDispCopyYScale
    gcall(c, 0x801AD8C0, {3});                                         // GXSetCopyClamp
    gcall(c, 0x801ADD00, {rd8(mode + 0x19), mode + 0x1A, 1, mode + 0x32});  // GXSetCopyFilter
    gcall(c, 0x801ADEF0, {0});            // GXSetDispCopyGamma
    gcall(c, 0x801AD8A0, {0});            // GXSetDispCopyFrame2Field
    gcall(c, 0x801AE1B0);                 // GXClearBoundingBox
    gcall(c, 0x801ACCD0, {1});            // GXPokeColorUpdate
    gcall(c, 0x801ACC50, {1});            // GXPokeAlphaUpdate
    gcall(c, 0x801ACD10, {0});            // GXPokeDither
    gcall(c, 0x801ACC70, {0, 0, 1, 0xF}); // GXPokeBlendMode
    gcall(c, 0x801ACC20, {7, 0});         // GXPokeAlphaMode
    gcall(c, 0x801ACC30, {1});            // GXPokeAlphaRead
    gcall(c, 0x801ACCF0, {0, 0});         // GXPokeDstAlpha
    gcall(c, 0x801ACD30, {1, 7, 1});      // GXPokeZMode
    gcall(c, 0x801B10B0, {0x23, 0x16});   // GXSetGPMetric
    gcall(c, 0x801B18D0);                 // GXClearGPMetric
}

// __GXCPInterruptHandler: FIFO high/low watermark and breakpoint interrupts.
GX_HANDLER(cp_interrupt_handler, "__GXCPInterruptHandler", Ret::None) {
    uint32_t context = c.r[4];
    uint32_t g = gx();
    wr32(g + 0xC, rd16(cp_reg()));  // cpStatus
    if ((rd32(g + 0x8) & 0x8) && (rd32(g + 0xC) & 0x2)) {  // underflow while suspended: resume
        gcall(c, kOSResumeThread, {rd32(kFifoThread)});
        wr32(kOverflowSuspended, 0);
        uint32_t clear = rd32(gx() + 0x10) | 0x3;
        wr32(gx() + 0x10, clear);
        wr16(cp_reg() + 0x4, static_cast<uint16_t>(clear));
        uint32_t enable = rlwinm(rd32(g + 0x8) | 0x4, 0, 29, 27);
        wr32(g + 0x8, enable);
        wr16(cp_reg() + 0x2, static_cast<uint16_t>(enable));
    }
    if ((rd32(g + 0x8) & 0x4) && (rd32(g + 0xC) & 0x1)) {  // overflow: suspend the writer
        wr32(kOverflowCount, rd32(kOverflowCount) + 1);
        uint32_t enable = rlwinm(rd32(g + 0x8), 0, 30, 28) | 0x8;
        wr32(g + 0x8, enable);
        wr16(cp_reg() + 0x2, static_cast<uint16_t>(enable));
        uint32_t clear = rlwinm(rd32(gx() + 0x10) | 0x1, 0, 31, 29);
        wr32(gx() + 0x10, clear);
        wr16(cp_reg() + 0x4, static_cast<uint16_t>(clear));
        wr32(kOverflowSuspended, 1);
        gcall(c, kOSSuspendThread, {rd32(kFifoThread)});
    }
    uint32_t enable = rd32(g + 0x8);
    if ((enable & 0x20) && (rd32(g + 0xC) & 0x10)) {  // breakpoint
        wr32(g + 0x8, rlwinm(enable, 0, 27, 25));
        wr16(cp_reg() + 0x2, static_cast<uint16_t>(rd32(g + 0x8)));
        if (rd32(kBreakPointCallback)) {
            GuestFrame frame(c, 0x2D0);
            gcall(c, kOSClearContext, {frame.base()});
            gcall(c, kOSSetCurrentContext, {frame.base()});
            call(c, rd32(kBreakPointCallback));
            gcall(c, kOSClearContext, {frame.base()});
            gcall(c, kOSSetCurrentContext, {context});
        }
    }
}

// =====================================================================================
// GXFifo
// =====================================================================================

constexpr uint32_t kGXIsGPFifoReady = 0x801AB6D0;
constexpr uint32_t kGXCleanGPFifo = 0x801AB7B0;
constexpr uint32_t kGXSetDirtyState = 0x801ACF70;

void cp_enable(uint32_t g, uint32_t value) {
    wr32(g + 0x8, value);
    wr16(cp_reg() + 0x2, static_cast<uint16_t>(value));
}
void cp_clear(uint32_t g, uint32_t value) {
    wr32(g + 0x10, value);
    wr16(cp_reg() + 0x4, static_cast<uint16_t>(value));
}
void copy_fifo(uint32_t to, uint32_t from) {
    for (uint32_t i = 0; i < 0x24; i += 4) {
        wr32(to + i, rd32(from + i));
    }
}

// GXInitFifoBase(GXFifoObj*, void* base, u32 size)
GX_HANDLER(init_fifo_base, "GXInitFifoBase", Ret::None) {
    uint32_t fifo = c.r[3];
    uint32_t base = c.r[4];
    uint32_t size = c.r[5];
    wr32(fifo + 0x0, base);
    wr32(fifo + 0x4, base + size - 4);  // top
    wr32(fifo + 0x8, size);
    wr32(fifo + 0x1C, 0);
    wr32(fifo + 0xC, size - 0x4000);                  // high watermark
    wr32(fifo + 0x10, rlwinm(size, 31, 1, 26));       // low watermark
    uint32_t level = gcall(c, kOSDisableInterrupts);
    wr32(fifo + 0x14, base);  // read pointer
    wr32(fifo + 0x18, base);  // write pointer
    wr32(fifo + 0x1C, 0);
    gcall(c, kOSRestoreInterrupts, {level});
}

// CPGPLinkCheck: TRUE when the CPU and GP FIFO are the same buffer.
GX_HANDLER(cpgp_link_check, "CPGPLinkCheck", Ret::Int) {
    if (rd8(kCPUFifoReady) == 0 || rd8(kGPFifoReady) == 0) {
        c.r[3] = 0;
        return;
    }
    uint32_t gp_base = rd32(kGPFifo);
    uint32_t cpu_base = rd32(kCPUFifo);
    uint32_t gp_top = rd32(kGPFifo + 4);
    uint32_t cpu_top = rd32(kCPUFifo + 4);
    if (cpu_base == gp_base && cpu_top == gp_top) {
        c.r[3] = 1;
        return;
    }
    int32_t a = static_cast<int32_t>(cpu_top - gp_base);
    int32_t b = static_cast<int32_t>(gp_top - cpu_base);
    if ((a > 0 && b > 0) || (a < 0 && b < 0)) {
        gcall(c, symbol_address("OSReport"), {0x8029B5C0});
        gcall(c, symbol_address("OSReport"), {0x8029B5D8, rd32(kGPFifo), rd32(kGPFifo + 4)});
    }
    c.r[3] = 0;
}

void write_pi_fifo(uint32_t fifo) {
    wr32(pi_reg() + 0xC, rd32(fifo) & 0x3FFFFFFF);
    wr32(pi_reg() + 0x10, rd32(fifo + 0x4) & 0x3FFFFFFF);
    wr32(pi_reg() + 0x14, rd32(fifo + 0x18) & 0x1FFFFFE0);
}

GX_HANDLER(set_cpu_fifo, "GXSetCPUFifo", Ret::None) {
    uint32_t fifo = c.r[3];
    uint32_t level = gcall(c, kOSDisableInterrupts);
    if (fifo == 0) {
        wr8(kCPUFifoReady, 0);
        wr8(kFifoLinked, 0);
        wr8(kCPUFifo + 0x22, 0);
        wr8(kCPUFifo + 0x21, 0);
        gcall(c, kOSRestoreInterrupts, {level});
        return;
    }
    copy_fifo(kCPUFifo, fifo);
    wr8(kCPUFifoReady, 1);
    wr8(kCPUFifo + 0x21, 1);
    uint32_t g = gx();
    if (static_cast<uint8_t>(gcall(c, kCPGPLinkCheck))) {
        wr8(kFifoLinked, 1);
        wr8(kCPUFifo + 0x22, 1);
        write_pi_fifo(kCPUFifo);
        cp_clear(g, rd32(g + 0x10) | 0x3);
        cp_enable(g, rlwinm(rd32(g + 0x8) | 0x4, 0, 29, 27));
        cp_enable(g, rd32(g + 0x8) | 0x10);
    } else {
        wr8(kCPUFifo + 0x22, 0);
        if (rd8(kFifoLinked)) {
            cp_enable(g, rlwinm(rd32(g + 0x8), 0, 28, 26));
            wr8(kFifoLinked, 0);
        }
        cp_enable(g, rlwinm(rd32(g + 0x8), 0, 30, 27));
        write_pi_fifo(kCPUFifo);
    }
    gcall(c, kPPCSync);
    gcall(c, kOSRestoreInterrupts, {level});
}

void write_cp_fifo_pointers(uint32_t fifo) {
    uint32_t cp = cp_reg();
    wr16(cp + 0x30, static_cast<uint16_t>(rd32(fifo + 0x1C)));
    wr16(cp + 0x34, static_cast<uint16_t>(rd32(fifo + 0x18)));
    wr16(cp + 0x38, static_cast<uint16_t>(rd32(fifo + 0x14)));
}

GX_HANDLER(set_gp_fifo, "GXSetGPFifo", Ret::None) {
    uint32_t fifo = c.r[3];
    uint32_t level = gcall(c, kOSDisableInterrupts);
    uint32_t g = gx();
    cp_enable(g, rd32(g + 0x8) & ~1u);
    cp_enable(g, rlwinm(rd32(g + 0x8), 0, 30, 27));
    if (fifo == 0) {
        wr8(kGPFifoReady, 0);
        wr8(kFifoLinked, 0);
        cp_enable(g, rlwinm(rd32(g + 0x8), 0, 28, 26));
        wr8(kGPFifo + 0x21, 0);
        wr8(kGPFifo + 0x22, 0);
        gcall(c, kOSRestoreInterrupts, {level});
        return;
    }
    copy_fifo(kGPFifo, fifo);
    wr8(kGPFifoReady, 1);
    wr8(kGPFifo + 0x22, 1);
    uint32_t cp = cp_reg();
    wr16(cp + 0x20, static_cast<uint16_t>(rd32(kGPFifo)));
    wr16(cp + 0x24, static_cast<uint16_t>(rd32(kGPFifo + 0x4)));
    write_cp_fifo_pointers(kGPFifo);
    wr16(cp + 0x28, static_cast<uint16_t>(rd32(kGPFifo + 0xC)));
    wr16(cp + 0x2C, static_cast<uint16_t>(rd32(kGPFifo + 0x10)));
    wr16(cp + 0x22, static_cast<uint16_t>((rd32(kGPFifo) >> 16) & 0x3FFF));
    wr16(cp + 0x26, static_cast<uint16_t>((rd32(kGPFifo + 0x4) >> 16) & 0x3FFF));
    wr16(cp + 0x32, static_cast<uint16_t>(static_cast<int32_t>(rd32(kGPFifo + 0x1C)) >> 16));
    wr16(cp + 0x36, static_cast<uint16_t>((rd32(kGPFifo + 0x18) >> 16) & 0x3FFF));
    wr16(cp + 0x3A, static_cast<uint16_t>((rd32(kGPFifo + 0x14) >> 16) & 0x3FFF));
    wr16(cp + 0x2A, static_cast<uint16_t>(rd32(kGPFifo + 0xC) >> 16));
    wr16(cp + 0x2E, static_cast<uint16_t>(rd32(kGPFifo + 0x10) >> 16));
    gcall(c, kPPCSync);
    if (static_cast<uint8_t>(gcall(c, kCPGPLinkCheck))) {
        wr8(kFifoLinked, 1);
        wr8(kGPFifo + 0x21, 1);
        cp_enable(g, rlwinm(rd32(g + 0x8) | 0x4, 0, 29, 27));
        cp_enable(g, rd32(g + 0x8) | 0x10);
    } else {
        wr8(kFifoLinked, 0);
        wr8(kGPFifo + 0x21, 0);
        cp_enable(g, rlwinm(rd32(g + 0x8), 0, 30, 27));
        cp_enable(g, rlwinm(rd32(g + 0x8), 0, 28, 26));
    }
    wr16(cp_reg() + 0x2, static_cast<uint16_t>(rlwinm(rlwinm(rd32(g + 0x8), 0, 31, 29), 0, 27, 25)));
    wr16(cp_reg() + 0x2, static_cast<uint16_t>(rd32(g + 0x8)));
    cp_clear(g, rd32(gx() + 0x10) | 0x3);
    cp_enable(g, rd32(g + 0x8) | 0x1);
    gcall(c, kOSRestoreInterrupts, {level});
}

GX_HANDLER(is_gp_fifo_ready, "__GXIsGPFifoReady", Ret::Int) {
    c.r[3] = rd8(kGPFifoReady);
}

GX_HANDLER(set_break_pt_callback, "GXSetBreakPtCallback", Ret::Int) {
    uint32_t callback = c.r[3];
    uint32_t previous = rd32(kBreakPointCallback);
    uint32_t level = gcall(c, kOSDisableInterrupts);
    wr32(kBreakPointCallback, callback);
    gcall(c, kOSRestoreInterrupts, {level});
    c.r[3] = previous;
}

GX_HANDLER(fifo_init, "__GXFifoInit", Ret::None) {
    gcall(c, kOSSetInterruptHandler, {0x11, kGXCPInterruptHandler});
    gcall(c, kOSUnmaskInterrupts, {0x4000});
    wr32(kFifoThread, gcall(c, kOSGetCurrentThread));
    wr32(kOverflowSuspended, 0);
    gcall(c, kMemset, {kCPUFifo, 0, 0x24});
    gcall(c, kMemset, {kGPFifo, 0, 0x24});
    wr8(kCPUFifoReady, 0);
    wr8(kGPFifoReady, 0);
}

// __GXCleanGPFifo: empty the GP FIFO (read pointer = write pointer) and restart it.
GX_HANDLER(clean_gp_fifo, "__GXCleanGPFifo", Ret::None) {
    if (rd8(kGPFifoReady) == 0) {
        return;
    }
    uint32_t level = gcall(c, kOSDisableInterrupts);
    uint32_t g = gx();
    cp_enable(g, rd32(g + 0x8) & ~1u);
    cp_enable(g, rlwinm(rd32(g + 0x8), 0, 30, 27));
    wr32(kGPFifo + 0x14, rd32(kGPFifo + 0x18));
    wr32(kGPFifo + 0x1C, 0);
    uint32_t cp = cp_reg();
    wr16(cp + 0x30, 0);
    wr16(cp + 0x34, static_cast<uint16_t>(rd32(kGPFifo + 0x18)));
    wr16(cp + 0x38, static_cast<uint16_t>(rd32(kGPFifo + 0x14)));
    wr16(cp + 0x32, static_cast<uint16_t>(static_cast<int32_t>(rd32(kGPFifo + 0x1C)) >> 16));
    wr16(cp + 0x36, static_cast<uint16_t>((rd32(kGPFifo + 0x18) >> 16) & 0x3FFF));
    wr16(cp + 0x3A, static_cast<uint16_t>((rd32(kGPFifo + 0x14) >> 16) & 0x3FFF));
    gcall(c, kPPCSync);
    if (rd8(kFifoLinked)) {
        wr32(kCPUFifo + 0x14, rd32(kGPFifo + 0x14));
        wr32(kCPUFifo + 0x18, rd32(kGPFifo + 0x18));
        wr32(kCPUFifo + 0x1C, rd32(kGPFifo + 0x1C));
        wr32(pi_reg() + 0x14, rd32(kGPFifo + 0x18) & 0x1FFFFFE0);
        cp_enable(g, rlwinm(rd32(g + 0x8) | 0x4, 0, 29, 27));
        cp_enable(g, rd32(g + 0x8) | 0x10);
    }
    cp_enable(g, rlwinm(rlwinm(rd32(g + 0x8), 0, 31, 29), 0, 27, 25));
    wr32(0x80422980, 0);
    cp_clear(g, rd32(gx() + 0x10) | 0x3);
    cp_enable(g, rd32(g + 0x8) | 0x1);
    gcall(c, kOSRestoreInterrupts, {level});
}

// =====================================================================================
// GXAttr
// =====================================================================================

// One attribute of GXSetVtxDesc/GXSetVtxDescv into vcdLo (+0x14) / vcdHi (+0x18).
void set_vertex_desc(uint32_t g, uint32_t attr, uint32_t type) {
    if (attr <= 8) {  // position/normal and texture matrix indices: one bit each
        wr32(g + 0x14, rlwimi(rd32(g + 0x14), type, attr, 31 - attr, 31 - attr));
    } else if (attr == 9) {  // position
        wr32(g + 0x14, rlwimi(rd32(g + 0x14), type, 9, 21, 22));
    } else if (attr == 10 || attr == 25) {  // normal / normal-binormal-tangent
        uint32_t mine = attr == 10 ? 0x524 : 0x525;
        uint32_t other = attr == 10 ? 0x525 : 0x524;
        if (type != 0) {
            wr8(g + mine, 1);
            wr8(g + other, 0);
            wr32(g + 0x520, type);
        } else {
            wr8(g + mine, 0);
        }
    } else if (attr == 11) {  // color 0
        wr32(g + 0x14, rlwimi(rd32(g + 0x14), type, 13, 17, 18));
    } else if (attr == 12) {  // color 1
        wr32(g + 0x14, rlwimi(rd32(g + 0x14), type, 15, 15, 16));
    } else if (attr >= 13 && attr <= 20) {  // texture coordinates 0-7
        uint32_t shift = 2 * (attr - 13);
        wr32(g + 0x18, rlwimi(rd32(g + 0x18), type, shift, 30 - shift, 31 - shift));
    }
}

void finish_vertex_desc(uint32_t g) {
    if (rd8(g + 0x524) || rd8(g + 0x525)) {
        wr32(g + 0x14, rlwimi(rd32(g + 0x14), rd32(g + 0x520), 11, 19, 20));
    } else {
        wr32(g + 0x14, rlwinm(rd32(g + 0x14), 0, 21, 18));
    }
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x8);
}

GX_HANDLER(set_vtx_desc, "GXSetVtxDesc", Ret::None) {
    uint32_t g = gx();
    if (c.r[3] <= 0x19) {
        set_vertex_desc(g, c.r[3], c.r[4]);
    }
    finish_vertex_desc(g);
}

GX_HANDLER(set_vtx_descv, "GXSetVtxDescv", Ret::None) {
    uint32_t g = gx();
    for (uint32_t entry = c.r[3]; rd32(entry) != 0xFF; entry += 8) {
        if (rd32(entry) <= 0x19) {
            set_vertex_desc(g, rd32(entry), rd32(entry + 4));
        }
    }
    finish_vertex_desc(g);
}

uint32_t significant_bits(uint32_t value) {
    return 33 - static_cast<uint32_t>(value ? __builtin_clz(value) : 32);
}

// __GXSetVCD: send the vertex descriptor and the XF input counts.
GX_HANDLER(set_vcd, "__GXSetVCD", Ret::None) {
    uint32_t g = gx();
    cp(0x50, rd32(g + 0x14));
    cp(0x60, rd32(g + 0x18));
    uint32_t normals = rd8(g + 0x525) ? 2 : (rd8(g + 0x524) != 0 ? 1 : 0);
    uint32_t colors = significant_bits((rd32(g + 0x14) >> 13) & 0xF) >> 1;
    uint32_t textures = rlwinm(significant_bits(rd32(g + 0x18) & 0xFFFF), 3, 0, 27);
    xf(0x1008, textures | colors | (normals << 2));
    wr16(g + 0x2, 1);
}

// __GXCalculateVLim: vertex size in bytes for format 0 (used to size immediate-mode batches).
GX_HANDLER(calculate_vlim, "__GXCalculateVLim", Ret::None) {
    static const uint8_t kColorSize[4] = {0, 4, 1, 2};
    static const uint8_t kTextureSize[4] = {0, 8, 1, 2};
    static const uint8_t kPositionSize[4] = {0, 12, 1, 2};
    uint32_t g = gx();
    if (rd16(g + 0x4) == 0) {
        return;
    }
    uint32_t lo = rd32(g + 0x14);
    uint32_t hi = rd32(g + 0x18);
    uint32_t vat = rd32(g + 0x1C);
    uint32_t size = 0;
    for (int bit = 0; bit <= 8; bit++) {
        size += (lo >> bit) & 1;
    }
    uint32_t normal_count = ((vat >> 9) & 1) == 1 ? 3 : 1;
    size += kPositionSize[(lo >> 11) & 3] * normal_count;
    size += kPositionSize[(lo >> 9) & 3];
    size += kColorSize[(lo >> 13) & 3];
    size += kColorSize[(lo >> 15) & 3];
    for (int t = 0; t < 8; t++) {
        size += kTextureSize[(hi >> (2 * t)) & 3];
    }
    wr16(g + 0x6, static_cast<uint16_t>(size));
}

GX_HANDLER(clear_vtx_desc, "GXClearVtxDesc", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x14, 0x200);  // position direct
    wr32(g + 0x18, 0);
    wr8(g + 0x524, 0);
    wr8(g + 0x525, 0);
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x8);
}

// One attribute of GXSetVtxAttrFmt(v) into vatA/B/C of the format (base = g + 4 * format).
void set_vertex_format(uint32_t base, uint32_t attr, uint32_t count, uint32_t type, uint32_t frac) {
    uint32_t a = base + 0x1C;
    uint32_t b = base + 0x3C;
    uint32_t c3 = base + 0x5C;
    switch (attr) {
    case 9:  // position
        wr32(a, rlwimi(rlwimi(rlwimi(rd32(a), count, 0, 31, 31), type, 1, 28, 30), frac, 4, 23, 27));
        break;
    case 10:
    case 25:  // normal
        wr32(a, rlwimi(rd32(a), type, 10, 19, 21));
        if (count == 2) {
            wr32(a, rd32(a) | 0x200 | 0x80000000);
        } else {
            wr32(a, rlwimi(rd32(a), count, 9, 22, 22) & 0x7FFFFFFF);
        }
        break;
    case 11:
        wr32(a, rlwimi(rlwimi(rd32(a), count, 13, 18, 18), type, 14, 15, 17));
        break;
    case 12:
        wr32(a, rlwimi(rlwimi(rd32(a), count, 17, 14, 14), type, 18, 11, 13));
        break;
    case 13:
        wr32(a, rlwimi(rlwimi(rlwimi(rd32(a), count, 21, 10, 10), type, 22, 7, 9), frac, 25, 2, 6));
        break;
    case 14:
        wr32(b, rlwimi(rlwimi(rlwimi(rd32(b), count, 0, 31, 31), type, 1, 28, 30), frac, 4, 23, 27));
        break;
    case 15:
        wr32(b, rlwimi(rlwimi(rlwimi(rd32(b), count, 9, 22, 22), type, 10, 19, 21), frac, 13, 14, 18));
        break;
    case 16:
        wr32(b, rlwimi(rlwimi(rlwimi(rd32(b), count, 18, 13, 13), type, 19, 10, 12), frac, 22, 5, 9));
        break;
    case 17:
        wr32(b, rlwimi(rlwimi(rd32(b), count, 27, 4, 4), type, 28, 1, 3));
        wr32(c3, rlwimi(rd32(c3), frac, 0, 27, 31));
        break;
    case 18:
        wr32(c3, rlwimi(rlwimi(rlwimi(rd32(c3), count, 5, 26, 26), type, 6, 23, 25), frac, 9, 18, 22));
        break;
    case 19:
        wr32(c3, rlwimi(rlwimi(rlwimi(rd32(c3), count, 14, 17, 17), type, 15, 14, 16), frac, 18, 9, 13));
        break;
    case 20:
        wr32(c3, rlwimi(rlwimi(rlwimi(rd32(c3), count, 23, 8, 8), type, 24, 5, 7), frac, 27, 0, 4));
        break;
    default:
        break;
    }
}

void finish_vertex_format(uint32_t format) {
    uint32_t g = gx();
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x10);
    wr8(g + 0x5FB, rd8(g + 0x5FB) | static_cast<uint8_t>(1u << (format & 0xFF)));
}

GX_HANDLER(set_vtx_attr_fmt, "GXSetVtxAttrFmt", Ret::None) {
    uint32_t format = c.r[3];
    set_vertex_format(gx() + 4 * format, c.r[4], c.r[5], c.r[6], c.r[7]);
    finish_vertex_format(format);
}

GX_HANDLER(set_vtx_attr_fmtv, "GXSetVtxAttrFmtv", Ret::None) {
    uint32_t format = c.r[3];
    uint32_t base = gx() + 4 * format;
    for (uint32_t entry = c.r[4]; rd32(entry) != 0xFF; entry += 0x10) {
        set_vertex_format(base, rd32(entry), rd32(entry + 4), rd32(entry + 8), rd8(entry + 0xC));
    }
    finish_vertex_format(format);
}

// __GXSetVAT: send the vertex attribute tables of the formats marked dirty.
GX_HANDLER(set_vat, "__GXSetVAT", Ret::None) {
    uint32_t g = gx();
    uint8_t dirty_formats = rd8(g + 0x5FB);
    for (uint32_t format = 0; dirty_formats; format++, dirty_formats >>= 1) {
        if (dirty_formats & 1) {
            cp(static_cast<uint8_t>(0x70 | format), rd32(g + 0x1C + 4 * format));
            cp(static_cast<uint8_t>(0x80 | format), rd32(g + 0x3C + 4 * format));
            cp(static_cast<uint8_t>(0x90 | format), rd32(g + 0x5C + 4 * format));
        }
    }
    fifo8(0);
    wr8(g + 0x5FB, 0);
}

GX_HANDLER(set_array, "GXSetArray", Ret::None) {
    uint32_t attr = c.r[3] == 0x19 ? 0xA : c.r[3];
    uint32_t index = attr - 9;
    cp(static_cast<uint8_t>(0xA0 | index), c.r[4] & 0x3FFFFFFF);
    cp(static_cast<uint8_t>(0xB0 | index), c.r[5]);
}

GX_HANDLER(invalidate_vtx_cache, "GXInvalidateVtxCache", Ret::None) {
    fifo8(0x48);
}

// GXSetTexCoordGen2(dst, func, src, mtx, normalize, postMtx)
GX_HANDLER(set_tex_coord_gen2, "GXSetTexCoordGen2", Ret::None) {
    uint32_t dst = c.r[3];
    uint32_t func = c.r[4];
    uint32_t src = c.r[5];
    uint32_t mtx = c.r[6];
    uint32_t normalize = c.r[7];
    uint32_t post = c.r[8];
    uint32_t row = 5;   // source row: 0 position, 1 normal, 2 color, 3 binormal, 4 tangent, 5+ texcoord
    uint32_t form = 0;  // 1: (x, y, z) input, 0: (s, t)
    switch (src) {
    case 0: row = 0; form = 1; break;
    case 1: row = 1; form = 1; break;
    case 2: row = 3; form = 1; break;
    case 3: row = 4; form = 1; break;
    case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: row = 5 + (src - 4); break;
    case 19: case 20: row = 2; break;
    default: break;
    }
    uint32_t reg = 0;
    if (func - 2 <= 7) {  // bump mapping
        reg = rlwimi(reg, form, 2, 29, 29);
        reg = rlwimi(reg, 1, 4, 25, 27);
        reg = rlwimi(reg, row, 7, 20, 24);
        reg = rlwimi(reg, src - 12, 12, 17, 19);
        reg = rlwimi(reg, func - 2, 15, 14, 16);
    } else if (func == 1) {  // 2x4 matrix
        reg = rlwimi(rlwimi(0, form, 2, 29, 29), row, 7, 20, 24);
    } else if (func == 0) {  // 3x4 matrix
        reg = rlwimi(rlwimi(2, form, 2, 29, 29), row, 7, 20, 24);
    } else if (func == 10) {  // SRTG
        reg = rlwimi(0, form, 2, 29, 29);
        reg = rlwimi(reg, src == 0x13 ? 2 : 3, 4, 25, 27);
        reg = rlwimi(reg, 2, 7, 20, 24);
    }
    uint32_t g = gx();
    wr32(g + 0xC8 + 4 * dst, reg);
    uint32_t post_reg = rlwimi(rlwimi(0, post - 0x40, 0, 26, 31), normalize, 8, 23, 23);
    wr32(g + kDirtyState, rd32(g + kDirtyState) | (0x10000u << dst));
    wr32(g + 0xE8 + 4 * dst, post_reg);
    switch (dst) {
    case 0: wr32(g + 0x80, rlwimi(rd32(g + 0x80), mtx, 6, 20, 25)); break;
    case 1: wr32(g + 0x80, rlwimi(rd32(g + 0x80), mtx, 12, 14, 19)); break;
    case 2: wr32(g + 0x80, rlwimi(rd32(g + 0x80), mtx, 18, 8, 13)); break;
    case 3: wr32(g + 0x80, rlwimi(rd32(g + 0x80), mtx, 24, 2, 7)); break;
    case 4: wr32(g + 0x84, rlwimi(rd32(g + 0x84), mtx, 0, 26, 31)); break;
    case 5: wr32(g + 0x84, rlwimi(rd32(g + 0x84), mtx, 6, 20, 25)); break;
    case 6: wr32(g + 0x84, rlwimi(rd32(g + 0x84), mtx, 12, 14, 19)); break;
    default: wr32(g + 0x84, rlwimi(rd32(g + 0x84), mtx, 18, 8, 13)); break;
    }
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x04000000);
}

GX_HANDLER(set_num_tex_gens, "GXSetNumTexGens", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x254, rlwimi(rd32(g + 0x254), c.r[3], 0, 28, 31));
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x02000004);
}

// =====================================================================================
// GXMisc
// =====================================================================================

GX_HANDLER(set_misc, "GXSetMisc", Ret::None) {
    uint32_t token = c.r[3];
    uint32_t value = c.r[4];
    uint32_t g = gx();
    if (token == 1) {  // GX_MT_XF_FLUSH
        wr16(g + 0x4, static_cast<uint16_t>(value));
        wr16(g + 0x0, (value & 0xFFFF) == 0 ? 1 : 0);
        wr16(g + 0x2, 1);
        if (value & 0xFFFF) {
            wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x8);
        }
    } else if (token == 2) {  // GX_MT_DL_SAVE_CONTEXT
        wr8(g + 0x5F9, value != 0);
    } else if (token == 3) {  // GX_MT_ABORT_WAIT_COPYOUT
        wr8(g + 0x5FA, value != 0);
    }
}

// Wait until `ticks` time-base ticks have passed (the SDK's spin waits around GPU resets).
void spin_ticks(Cpu& c, int64_t ticks) {
    gcall(c, kOSGetTime);
    uint64_t start = (static_cast<uint64_t>(c.r[3]) << 32) | c.r[4];
    while (true) {
        gcall(c, kOSGetTime);
        uint64_t now = (static_cast<uint64_t>(c.r[3]) << 32) | c.r[4];
        if (static_cast<int64_t>(now - start) > ticks) {
            return;
        }
    }
}

// Reset the GPU's FIFO reader: wait for the memory interface to go quiet, then pulse the
// PI FIFO reset.
void abort_gpu(Cpu& c) {
    if (rd8(gx() + 0x5FA) && static_cast<uint8_t>(gcall(c, kGXIsGPFifoReady))) {
        uint32_t count = memory_request_count();
        while (true) {
            spin_ticks(c, 8);
            uint32_t now = memory_request_count();
            bool changed = now != count;
            count = now;
            if (!changed) {
                break;
            }
        }
    }
    wr32(0xCC003018, 1);
    spin_ticks(c, 0x32);
    wr32(0xCC003018, 0);
    spin_ticks(c, 5);
}

GX_HANDLER(abort, "__GXAbort", Ret::Blocking) {
    abort_gpu(c);
}

GX_HANDLER(abort_frame, "GXAbortFrame", Ret::Blocking) {
    uint32_t g = gx();
    abort_gpu(c);
    if (static_cast<uint8_t>(gcall(c, kGXIsGPFifoReady))) {
        gcall(c, kGXCleanGPFifo);
        gcall(c, kGXInitRevisionBits);
        wr32(g + kDirtyState, 0);
        for (int i = 0; i < 8; i++) {
            fifo32(0);
        }
        gcall(c, kPPCSync);
    }
}

void flush_padding(Cpu& c) {
    for (int i = 0; i < 8; i++) {
        fifo32(0);
    }
    gcall(c, kPPCSync);
}

GX_HANDLER(set_draw_sync, "GXSetDrawSync", Ret::None) {
    uint32_t token = c.r[3] & 0xFFFF;
    uint32_t level = gcall(c, kOSDisableInterrupts);
    uint32_t g = gx();
    bp(0x48000000 | token);
    bp(0x47000000 | token);
    if (rd32(g + kDirtyState) != 0) {
        gcall(c, kGXSetDirtyState);
    }
    flush_padding(c);
    gcall(c, kOSRestoreInterrupts, {level});
    wr16(g + 0x2, 0);
}

constexpr uint32_t kDrawDone = 0x804229A0;       // u8
constexpr uint32_t kDrawDoneQueue = 0x80422998;  // OSThreadQueue

GX_HANDLER(draw_done, "GXDrawDone", Ret::Blocking) {
    uint32_t level = gcall(c, kOSDisableInterrupts);
    uint32_t g = gx();
    bp(0x45000002);
    if (rd32(g + kDirtyState) != 0) {
        gcall(c, kGXSetDirtyState);
    }
    flush_padding(c);
    wr8(kDrawDone, 0);
    gcall(c, kOSRestoreInterrupts, {level});
    wr16(g + 0x2, 0);
    level = gcall(c, kOSDisableInterrupts);
    while (rd8(kDrawDone) == 0) {
        gcall(c, symbol_address("OSSleepThread"), {kDrawDoneQueue});
    }
    gcall(c, kOSRestoreInterrupts, {level});
}

GX_HANDLER(pix_mode_sync, "GXPixModeSync", Ret::None) {
    uint32_t g = gx();
    bp(rd32(g + 0x22C));
    wr16(g + 0x2, 0);
}

GX_HANDLER(poke_alpha_mode, "GXPokeAlphaMode", Ret::None) {
    wr16(pe_reg() + 0x6, static_cast<uint16_t>(rlwimi(c.r[4], c.r[3], 8, 0, 23)));
}

GX_HANDLER(poke_alpha_read, "GXPokeAlphaRead", Ret::None) {
    wr16(pe_reg() + 0x8, static_cast<uint16_t>((c.r[3] & 3) | 4));
}

GX_HANDLER(poke_alpha_update, "GXPokeAlphaUpdate", Ret::None) {
    uint32_t pe = pe_reg();
    wr16(pe + 0x2, static_cast<uint16_t>(rlwimi(rd16(pe + 0x2), c.r[3], 4, 27, 27)));
}

GX_HANDLER(poke_blend_mode, "GXPokeBlendMode", Ret::None) {
    uint32_t type = c.r[3];
    uint32_t pe = pe_reg();
    uint32_t reg = rd16(pe + 0x2);
    reg = rlwimi(reg, (type == 1 || type == 3) ? 1 : 0, 0, 31, 31);
    reg = rlwimi(reg, type == 3 ? 32 : 0, 6, 20, 20);
    reg = rlwimi(reg, type == 2 ? 32 : 0, 28, 30, 30);
    reg = rlwimi(reg, c.r[6], 12, 16, 19);
    reg = rlwimi(reg, c.r[4], 8, 21, 23);
    reg = rlwimi(reg, c.r[5], 5, 24, 26);
    wr16(pe + 0x2, static_cast<uint16_t>(reg));
}

GX_HANDLER(poke_color_update, "GXPokeColorUpdate", Ret::None) {
    uint32_t pe = pe_reg();
    wr16(pe + 0x2, static_cast<uint16_t>(rlwimi(rd16(pe + 0x2), c.r[3], 3, 28, 28)));
}

GX_HANDLER(poke_dst_alpha, "GXPokeDstAlpha", Ret::None) {
    wr16(pe_reg() + 0x4, static_cast<uint16_t>(rlwimi(c.r[4] & 0xFF, c.r[3], 8, 23, 23)));
}

GX_HANDLER(poke_dither, "GXPokeDither", Ret::None) {
    uint32_t pe = pe_reg();
    wr16(pe + 0x2, static_cast<uint16_t>(rlwimi(rd16(pe + 0x2), c.r[3], 2, 29, 29)));
}

GX_HANDLER(poke_z_mode, "GXPokeZMode", Ret::None) {
    uint32_t reg = rlwimi(rlwimi(c.r[3] & 1, c.r[4], 1, 28, 30), c.r[5], 4, 27, 27);
    wr16(pe_reg(), static_cast<uint16_t>(reg));
}

constexpr uint32_t kDrawSyncCallback = 0x804229A8;
constexpr uint32_t kDrawDoneCallback = 0x804229A4;
constexpr uint32_t kOSWakeupThread = 0x801CA3F0;

uint32_t swap_callback(Cpu& c, uint32_t variable, uint32_t callback) {
    uint32_t previous = rd32(variable);
    uint32_t level = gcall(c, kOSDisableInterrupts);
    wr32(variable, callback);
    gcall(c, kOSRestoreInterrupts, {level});
    return previous;
}

GX_HANDLER(set_draw_sync_callback, "GXSetDrawSyncCallback", Ret::Int) {
    c.r[3] = swap_callback(c, kDrawSyncCallback, c.r[3]);
}

GX_HANDLER(set_draw_done_callback, "GXSetDrawDoneCallback", Ret::Int) {
    c.r[3] = swap_callback(c, kDrawDoneCallback, c.r[3]);
}

// Run an interrupt-time callback on a scratch OSContext, like the SDK handlers do.
void run_on_scratch_context(Cpu& c, uint32_t callback, uint32_t context, std::initializer_list<uint32_t> args) {
    GuestFrame frame(c, 0x2D0);
    gcall(c, kOSClearContext, {frame.base()});
    gcall(c, kOSSetCurrentContext, {frame.base()});
    gcall(c, callback, args);
    gcall(c, kOSClearContext, {frame.base()});
    gcall(c, kOSSetCurrentContext, {context});
}

GX_HANDLER(token_interrupt_handler, "GXTokenInterruptHandler", Ret::Blocking) {
    uint32_t context = c.r[4];
    uint32_t token = rd16(pe_reg() + 0xE);
    if (rd32(kDrawSyncCallback)) {
        run_on_scratch_context(c, rd32(kDrawSyncCallback), context, {token});
    }
    wr16(pe_reg() + 0xA, static_cast<uint16_t>(rd16(pe_reg() + 0xA) | 0x4));
}

GX_HANDLER(finish_interrupt_handler, "GXFinishInterruptHandler", Ret::Blocking) {
    uint32_t context = c.r[4];
    wr16(pe_reg() + 0xA, static_cast<uint16_t>(rd16(pe_reg() + 0xA) | 0x8));
    wr8(kDrawDone, 1);
    if (rd32(kDrawDoneCallback)) {
        run_on_scratch_context(c, rd32(kDrawDoneCallback), context, {});
    }
    gcall(c, kOSWakeupThread, {kDrawDoneQueue});
}

GX_HANDLER(pe_init, "__GXPEInit", Ret::None) {
    gcall(c, kOSSetInterruptHandler, {0x12, 0x801ACDA0});  // GXTokenInterruptHandler
    gcall(c, kOSSetInterruptHandler, {0x13, 0x801ACE80});  // GXFinishInterruptHandler
    gcall(c, symbol_address("OSInitThreadQueue"), {kDrawDoneQueue});
    gcall(c, kOSUnmaskInterrupts, {0x2000});
    gcall(c, kOSUnmaskInterrupts, {0x1000});
    wr16(pe_reg() + 0xA, static_cast<uint16_t>(rd16(pe_reg() + 0xA) | 0xF));
}

// __GXSetDirtyState: send the shadow state GX functions changed since the last draw.
GX_HANDLER(set_dirty_state, "__GXSetDirtyState", Ret::None) {
    uint32_t g = gx();
    uint32_t state = rd32(g + kDirtyState);
    if (state & 0x1) {
        gcall(c, 0x801AF510);  // __GXSetSUTexRegs
    }
    if (state & 0x2) {
        gcall(c, 0x801AFDC0);  // __GXUpdateBPMask
    }
    if (state & 0x4) {
        bp(rd32(g + 0x254));
        wr16(g + 0x2, 0);
    }
    if (state & 0x8) {
        gcall(c, 0x801ABDD0);  // __GXSetVCD
    }
    if (state & 0x10) {
        gcall(c, 0x801AC360);  // __GXSetVAT
    }
    if (state & 0x18) {
        gcall(c, 0x801ABE80);  // __GXCalculateVLim
    }
    state &= ~0xFFu;
    if (state) {
        uint32_t colors = state & 0xF00;  // ambient/material colors
        for (uint32_t i = 0; i < 4; i++) {
            if (colors & (0x100u << i)) {
                xf(static_cast<uint16_t>(0x100A + i), rd32(g + 0xA8 + 4 * i));
            }
        }
        uint32_t channels = (state & 0x01000000) | (state & 0xF000);
        if (channels) {
            if (channels & 0x01000000) {
                xf(0x1009, (rd32(g + 0x254) >> 4) & 7);  // number of color channels
            }
            uint32_t bits = (channels >> 12) & 0xF;
            for (uint32_t i = 0; bits; i++, bits >>= 1) {
                if (bits & 1) {
                    xf(static_cast<uint16_t>(0x100E + i), rd32(g + 0xB8 + 4 * i));
                }
            }
        }
        uint32_t texgens = state & 0x02FF0000;
        if (texgens) {
            if (texgens & 0x02000000) {
                xf(0x103F, rd32(g + 0x254) & 0xF);  // number of texture coordinates
            }
            uint32_t bits = (texgens >> 16) & 0xFF;
            for (uint32_t i = 0; bits; i++, bits >>= 1) {
                if (bits & 1) {
                    xf(static_cast<uint16_t>(0x1040 + i), rd32(g + 0xC8 + 4 * i));
                    xf(static_cast<uint16_t>(0x1050 + i), rd32(g + 0xE8 + 4 * i));
                }
            }
        }
        if (state & 0x04000000) {
            gcall(c, 0x801B1020, {0});  // __GXSetMatrixIndex
            gcall(c, 0x801B1020, {5});
        }
        if (state & 0x10000000) {
            gcall(c, 0x801B0E40);  // __GXSetViewport
        }
        if (state & 0x08000000) {
            gcall(c, 0x801B0BA0);  // __GXSetProjection
        }
        wr16(g + 0x2, 1);
    }
    wr32(g + kDirtyState, 0);
}

// The flush primitive the SDK sends before a draw when the XF needs flushing: an empty
// point list of vNum vertices padded with vNum * vLim zero bytes.
void send_flush_prim(uint32_t g) {
    uint32_t bytes = rd16(g + 0x4) * rd16(g + 0x6);
    fifo8(0x98);
    fifo16(rd16(g + 0x4));
    for (uint32_t i = 0; i < (bytes + 3) / 4; i++) {
        fifo32(0);
    }
    wr16(g + 0x2, 1);
}

void run_self_test(Cpu& c);

GX_HANDLER(begin, "GXBegin", Ret::None) {
    static int calls = 0;
    if (++calls == 2000 && std::getenv("WP_GX_SELFTEST")) {
        run_self_test(c);
    }
    uint32_t primitive = c.r[3];
    uint32_t format = c.r[4];
    uint32_t vertices = c.r[5];
    uint32_t g = gx();
    if (rd32(g + kDirtyState) != 0) {
        gcall(c, kGXSetDirtyState);
    }
    if (rd32(g + 0x0) == 0) {  // vNumNot and bpSentNot both clear
        send_flush_prim(g);
    }
    fifo8(static_cast<uint8_t>(primitive | format));
    fifo16(static_cast<uint16_t>(vertices));
}

GX_HANDLER(send_flush_prim_handler, "__GXSendFlushPrim", Ret::None) {
    send_flush_prim(gx());
}

GX_HANDLER(set_line_width, "GXSetLineWidth", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x7C, rlwimi(rlwimi(rd32(g + 0x7C), c.r[3], 0, 24, 31), c.r[4], 16, 13, 15));
    bp(rd32(g + 0x7C));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_point_size, "GXSetPointSize", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x7C, rlwimi(rlwimi(rd32(g + 0x7C), c.r[3], 8, 16, 23), c.r[4], 19, 10, 12));
    bp(rd32(g + 0x7C));
    wr16(g + 0x2, 0);
}

GX_HANDLER(enable_tex_offsets, "GXEnableTexOffsets", Ret::None) {
    uint32_t g = gx();
    uint32_t reg = g + 0x108 + 4 * c.r[3];
    wr32(reg, rlwimi(rlwimi(rd32(reg), c.r[4], 18, 13, 13), c.r[5], 19, 12, 12));
    bp(rd32(reg));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_coplanar, "GXSetCoPlanar", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x254, rlwimi(rd32(g + 0x254), c.r[3], 19, 12, 12));
    bp(0xFE080000);
    bp(rd32(g + 0x254));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_gen_mode, "__GXSetGenMode", Ret::None) {
    uint32_t g = gx();
    bp(rd32(g + 0x254));
    wr16(g + 0x2, 0);
}

// =====================================================================================
// GXFrameBuf
// =====================================================================================

// GXAdjustForOverscan(const GXRenderModeObj* in, GXRenderModeObj* out, u16 hor, u16 ver)
GX_HANDLER(adjust_for_overscan, "GXAdjustForOverscan", Ret::None) {
    uint32_t in = c.r[3];
    uint32_t out = c.r[4];
    uint32_t hor = c.r[5];
    uint32_t ver = c.r[6];
    uint32_t hor2 = (hor & 0x7FFF) << 1;
    uint32_t ver2 = (ver & 0x7FFF) << 1;
    if (in != out) {
        for (uint32_t i = 0; i < 0x3C; i += 4) {
            wr32(out + i, rd32(in + i));
        }
    }
    uint32_t efb_height = rd16(in + 0x6);
    uint32_t xfb_mode = rd32(in + 0x14);
    uint32_t tv_mode = rd32(in + 0x0) & 3;
    wr16(out + 0x4, static_cast<uint16_t>(rd16(in + 0x4) - hor2));
    wr16(out + 0x6, static_cast<uint16_t>(efb_height - (ver2 * efb_height) / rd16(in + 0x8)));
    if (xfb_mode == 0 && tv_mode == 0) {
        wr16(out + 0x8, static_cast<uint16_t>(rd16(in + 0x8) - ((ver2 >> 1) & 0x7FFF)));
    } else {
        wr16(out + 0x8, static_cast<uint16_t>(rd16(in + 0x8) - ver2));
    }
    wr16(out + 0xE, static_cast<uint16_t>(rd16(in + 0xE) - hor2));
    if (tv_mode == 1) {
        wr16(out + 0x10, static_cast<uint16_t>(rd16(in + 0x10) - ((ver2 & 0xFFFF) << 1)));
    } else {
        wr16(out + 0x10, static_cast<uint16_t>(rd16(in + 0x10) - ver2));
    }
    wr16(out + 0xC, static_cast<uint16_t>(rd16(in + 0xC) + ver));
    wr16(out + 0xA, static_cast<uint16_t>(rd16(in + 0xA) + hor));
}

uint32_t copy_source(uint32_t left, uint32_t top, uint32_t reg) {
    return rlwimi(rlwimi(rlwimi(0, left, 0, 22, 31), top, 10, 12, 21), reg, 24, 0, 7);
}

GX_HANDLER(set_disp_copy_src, "GXSetDispCopySrc", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x230, copy_source(c.r[3], c.r[4], 0x49));
    wr32(g + 0x234, copy_source(c.r[5] - 1, c.r[6] - 1, 0x4A));
}

GX_HANDLER(set_tex_copy_src, "GXSetTexCopySrc", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x240, copy_source(c.r[3], c.r[4], 0x49));
    wr32(g + 0x244, copy_source(c.r[5] - 1, c.r[6] - 1, 0x4A));
}

GX_HANDLER(set_disp_copy_dst, "GXSetDispCopyDst", Ret::None) {
    int32_t stride = static_cast<int32_t>((c.r[3] & 0x7FFF) << 1) >> 5;
    wr32(gx() + 0x238, rlwimi(rlwimi(0, static_cast<uint32_t>(stride), 0, 22, 31), 0x4D, 24, 0, 7));
}

GX_HANDLER(set_tex_copy_dst, "GXSetTexCopyDst", Ret::None) {
    uint32_t width = c.r[3];
    uint32_t height = c.r[4];
    uint32_t format = c.r[5];
    uint32_t mipmap = c.r[6];
    uint32_t g = gx();
    uint32_t hw_format = format == 0x13 ? 0xB : (format & 0xF);
    wr8(g + 0x250, 0);
    uint32_t copy = g + 0x24C;
    wr32(copy, rlwimi(rd32(copy), (format <= 3 || format == 0x26) ? 3 : 2, 15, 15, 16));
    wr8(g + 0x250, (format >> 4) & 1);
    wr32(copy, rlwimi(rd32(copy), hw_format, 0, 28, 28));
    GuestFrame frame(c, 0x10);
    uint32_t tiles = frame.base();
    gcall(c, 0x801AEA20, {format, width, height, tiles + 8, tiles + 4, tiles});  // __GetImageTileCount
    uint32_t g2 = gx();
    wr32(g2 + 0x248, 0);
    wr32(g2 + 0x248, rlwimi(rlwimi(0, rd32(tiles + 8) * rd32(tiles), 0, 22, 31), 0x4D, 24, 0, 7));
    wr32(copy, rlwimi(rlwimi(rd32(copy), mipmap, 9, 22, 22), hw_format, 4, 25, 27));
}

GX_HANDLER(set_disp_copy_frame2field, "GXSetDispCopyFrame2Field", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x23C, rlwimi(rd32(g + 0x23C), c.r[3], 12, 18, 19));
    wr32(g + 0x24C, rlwinm(rd32(g + 0x24C), 0, 20, 17));
}

GX_HANDLER(set_copy_clamp, "GXSetCopyClamp", Ret::None) {
    uint32_t g = gx();
    uint32_t clamp = c.r[3];
    for (uint32_t reg : {0x23Cu, 0x24Cu}) {
        wr32(g + reg, rlwimi(rlwimi(rd32(g + reg), clamp, 0, 31, 31), clamp, 0, 30, 30));
    }
}

uint32_t fp_to_unsigned(Cpu& c, float value) {
    return gcall(c, 0x8014FF08, {}, {value});  // __cvt_fp2unsigned
}

// Number of XFB lines a copy of efb_height lines produces with vertical scale register iscale.
uint32_t xfb_lines(uint32_t efb_height, uint32_t iscale) {
    uint32_t lines = ((efb_height - 1) << 8) / iscale + 1;
    if (iscale - 0x81 <= 0x7E) {
        while ((iscale & 1) == 0) {
            iscale >>= 1;
        }
        if (efb_height % iscale == 0) {
            lines++;
        }
    }
    return lines > 0x400 ? 0x400 : lines;
}

GX_HANDLER(get_num_xfb_lines, "GXGetNumXfbLines", Ret::Int) {
    uint32_t efb_height = c.r[3];
    float scale = static_cast<float>(c.f[1]);
    uint32_t iscale = fp_to_unsigned(c, 256.0f / scale) & 0x1FF;
    c.r[3] = xfb_lines(efb_height, iscale) & 0xFFFF;
}

GX_HANDLER(get_y_scale_factor, "GXGetYScaleFactor", Ret::Float) {
    uint32_t efb_height = c.r[3];
    uint32_t xfb_height = c.r[4];
    auto lines_for = [&](float scale) { return xfb_lines(efb_height, fp_to_unsigned(c, 256.0f / scale) & 0x1FF); };
    float scale = static_cast<float>(xfb_height) / static_cast<float>(efb_height);
    uint32_t lines = lines_for(scale);
    uint32_t target = xfb_height;
    while (lines > xfb_height) {
        target--;
        scale = static_cast<float>(target) / static_cast<float>(efb_height);
        lines = lines_for(scale);
    }
    float result = scale;
    while (lines < xfb_height) {
        target++;
        result = scale;
        scale = static_cast<float>(target) / static_cast<float>(efb_height);
        lines = lines_for(scale);
    }
    c.f[1] = result;
}

GX_HANDLER(set_disp_copy_y_scale, "GXSetDispCopyYScale", Ret::Int) {
    float scale = static_cast<float>(c.f[1]);
    uint32_t value = fp_to_unsigned(c, 256.0f / scale);
    uint32_t iscale = value & 0x1FF;
    uint32_t g = gx();
    bp(rlwimi(rlwimi(0, value, 0, 23, 31), 0x4E, 24, 0, 7));
    wr16(g + 0x2, 0);
    wr32(g + 0x23C, rlwimi(rd32(g + 0x23C), iscale != 0x100 ? 0x80000000u : 0, 11, 21, 21));
    uint32_t height = ((rd32(g + 0x234) >> 10) & 0x3FF) + 1;
    c.r[3] = xfb_lines(height, iscale);
}

GX_HANDLER(set_copy_clear, "GXSetCopyClear", Ret::None) {
    uint32_t color = c.r[3];
    uint32_t z = c.r[4];
    bp(0x4F000000 | (rd8(color + 3) << 8) | rd8(color + 0));
    bp(0x50000000 | (rd8(color + 1) << 8) | rd8(color + 2));
    bp(0x51000000 | (z & 0xFFFFFF));
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_copy_filter, "GXSetCopyFilter", Ret::None) {
    uint32_t aa = c.r[3];
    uint32_t pattern = c.r[4];
    uint32_t use_vfilter = c.r[5];
    uint32_t vfilter = c.r[6];
    uint32_t sample[4];
    for (uint32_t k = 0; k < 4; k++) {
        if (aa) {
            uint32_t reg = (k + 1) << 24;
            for (uint32_t i = 0; i < 6; i++) {
                reg |= (rd8(pattern + 6 * k + i) & 0xF) << (4 * i);
            }
            sample[k] = reg;
        } else {
            sample[k] = ((k + 1) << 24) | 0x666666;
        }
    }
    for (uint32_t reg : sample) {
        bp(reg);
    }
    uint32_t v0 = 0x53000000;
    uint32_t v1 = 0x54000000;
    if (use_vfilter) {
        v0 = rlwimi(v0, rd8(vfilter + 0), 0, 26, 31);
        v0 = rlwimi(v0, rd8(vfilter + 1), 6, 20, 25);
        v0 = rlwimi(v0, rd8(vfilter + 2), 12, 14, 19);
        v0 = rlwimi(v0, rd8(vfilter + 3), 18, 8, 13);
        v1 = rlwimi(v1, rd8(vfilter + 4), 0, 26, 31);
        v1 = rlwimi(v1, rd8(vfilter + 5), 6, 20, 25);
        v1 = rlwimi(v1, rd8(vfilter + 6), 12, 14, 19);
    } else {
        v0 = rlwimi(rlwimi(v0 & ~0xFFFu, 0x15, 12, 14, 19), 0x16, 18, 8, 13);
        v1 = rlwinm(rlwimi(v1, 0x15, 0, 26, 31), 0, 26, 13);
    }
    bp(v0);
    bp(v1);
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_disp_copy_gamma, "GXSetDispCopyGamma", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x23C, rlwimi(rd32(g + 0x23C), c.r[3], 7, 23, 24));
}

uint32_t copy_address(uint32_t dest) {
    return rlwimi(rlwimi(0, dest, 27, 8, 31), 0x4B, 24, 0, 7);
}

// Clearing during a copy needs Z writes on and color/alpha updates off for the clear pass.
void copy_clear_setup(uint32_t g) {
    bp(rd32(g + 0x228) | 0xF);
    bp(rd32(g + 0x220) & ~3u);
}

GX_HANDLER(copy_disp, "GXCopyDisp", Ret::None) {
    uint32_t dest = c.r[3];
    uint32_t clear = c.r[4];
    uint32_t g = gx();
    if (clear) {
        copy_clear_setup(g);
    }
    bool restore_pe = false;
    uint32_t pe = rd32(g + 0x22C);
    if ((clear || (pe & 7) == 3) && ((pe >> 6) & 1)) {
        bp(rlwinm(pe, 0, 26, 24));
        restore_pe = true;
    }
    bp(rd32(g + 0x230));
    bp(rd32(g + 0x234));
    bp(rd32(g + 0x238));
    bp(copy_address(dest));
    wr32(g + 0x23C, rlwimi(rlwimi(rd32(g + 0x23C), clear, 11, 20, 20) | 0x4000, 0x52, 24, 0, 7));
    bp(rd32(g + 0x23C));
    if (clear) {
        bp(rd32(g + 0x228));
        bp(rd32(g + 0x220));
    }
    if (restore_pe) {
        bp(rd32(g + 0x22C));
    }
    wr16(g + 0x2, 0);
}

GX_HANDLER(copy_tex, "GXCopyTex", Ret::None) {
    uint32_t dest = c.r[3];
    uint32_t clear = c.r[4];
    uint32_t g = gx();
    if (clear) {
        copy_clear_setup(g);
    }
    bool changed = false;
    uint32_t pe = rd32(g + 0x22C);
    if (rd8(g + 0x250) && (pe & 7) != 3) {  // Z copies need the Z24 pixel format
        changed = true;
        pe = rlwimi(pe, 3, 0, 29, 31);
    }
    if ((clear || (pe & 7) == 3) && ((pe >> 6) & 1)) {
        changed = true;
        pe = rlwinm(pe, 0, 26, 24);
    }
    if (changed) {
        bp(pe);
    }
    bp(rd32(g + 0x240));
    bp(rd32(g + 0x244));
    bp(rd32(g + 0x248));
    bp(copy_address(dest));
    wr32(g + 0x24C, rlwimi(rlwinm(rlwimi(rd32(g + 0x24C), clear, 11, 20, 20), 0, 18, 16), 0x52, 24, 0, 7));
    bp(rd32(g + 0x24C));
    if (clear) {
        bp(rd32(g + 0x228));
        bp(rd32(g + 0x220));
    }
    if (changed) {
        bp(rd32(g + 0x22C));
    }
    wr16(g + 0x2, 0);
}

// =====================================================================================
// GXLight
// =====================================================================================

GX_HANDLER(clear_bounding_box, "GXClearBoundingBox", Ret::None) {
    bp(0x550003FF);
    bp(0x560003FF);
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(init_light_attn, "GXInitLightAttn", Ret::None) {
    uint32_t light = c.r[3];
    for (int i = 0; i < 6; i++) {
        stf(light + 0x10 + 4 * i, static_cast<float>(c.f[1 + i]));
    }
}

GX_HANDLER(init_light_spot, "GXInitLightSpot", Ret::None) {
    uint32_t light = c.r[3];
    float cutoff = static_cast<float>(c.f[1]);
    uint32_t function = c.r[4];
    if (cutoff <= 0.0f || !(cutoff <= 90.0f)) {
        function = 0;
    }
    float radians = 3.1415927f * cutoff;
    radians = radians / 180.0f;
    gcall(c, 0x8015FA2C, {}, {radians});  // cos
    float cr = static_cast<float>(c.f[1]);
    float a0;
    float a1;
    float a2;
    switch (function) {
    case 1:  // GX_SP_FLAT
        a0 = -1000.0f * cr;
        a1 = 1000.0f;
        a2 = 0.0f;
        break;
    case 2: {  // GX_SP_COS
        float d = 1.0f - cr;
        d = 1.0f / d;
        a0 = -cr * d;
        a1 = d;
        a2 = 0.0f;
        break;
    }
    case 3: {  // GX_SP_COS2
        float d = 1.0f - cr;
        d = 1.0f / d;
        a0 = 0.0f;
        a1 = -cr * d;
        a2 = d;
        break;
    }
    case 4: {  // GX_SP_SHARP
        float d = 1.0f - cr;
        float e = cr - 2.0f;
        d = d * d;
        e = cr * e;
        d = 1.0f / d;
        a0 = d * e;
        a1 = 2.0f * d;
        a2 = -d;
        break;
    }
    case 5: {  // GX_SP_RING1
        float d = 1.0f - cr;
        float e = 1.0f + cr;
        d = d * d;
        e = 4.0f * e;
        d = 1.0f / d;
        a2 = -4.0f * d;
        a1 = e * d;
        a0 = a2 * cr;
        break;
    }
    case 6: {  // GX_SP_RING2
        float d = 1.0f - cr;
        float e = 2.0f * cr;
        float f = 4.0f * cr;
        d = d * d;
        e = e * cr;
        d = 1.0f / d;
        e = d * e;
        a1 = f * d;
        a2 = -2.0f * d;
        a0 = 1.0f - e;
        break;
    }
    default:
        a0 = 1.0f;
        a1 = 0.0f;
        a2 = 0.0f;
        break;
    }
    stf(light + 0x10, a0);
    stf(light + 0x14, a1);
    stf(light + 0x18, a2);
}

GX_HANDLER(init_light_dist_attn, "GXInitLightDistAttn", Ret::None) {
    uint32_t light = c.r[3];
    float distance = static_cast<float>(c.f[1]);
    float brightness = static_cast<float>(c.f[2]);
    uint32_t function = c.r[4];
    if (!(distance >= 0.0f)) {
        function = 0;
    }
    if (brightness <= 0.0f || brightness >= 1.0f) {
        function = 0;
    }
    float k0 = 1.0f;
    float k1 = 0.0f;
    float k2 = 0.0f;
    switch (function) {
    case 1: {  // GX_DA_GENTLE
        float product = brightness * distance;
        float rest = 1.0f - brightness;
        k1 = rest / product;
        break;
    }
    case 2: {  // GX_DA_MEDIUM
        float product = brightness * distance;
        float rest = 1.0f - brightness;
        float square = distance * product;
        float half = 0.5f * rest;
        k1 = half / product;
        k2 = half / square;
        break;
    }
    case 3: {  // GX_DA_STEEP
        float product = brightness * distance;
        float rest = 1.0f - brightness;
        product = distance * product;
        k2 = rest / product;
        break;
    }
    default:
        break;
    }
    stf(light + 0x1C, k0);
    stf(light + 0x20, k1);
    stf(light + 0x24, k2);
}

GX_HANDLER(init_light_pos, "GXInitLightPos", Ret::None) {
    uint32_t light = c.r[3];
    stf(light + 0x28, static_cast<float>(c.f[1]));
    stf(light + 0x2C, static_cast<float>(c.f[2]));
    stf(light + 0x30, static_cast<float>(c.f[3]));
}

GX_HANDLER(init_light_dir, "GXInitLightDir", Ret::None) {
    uint32_t light = c.r[3];
    stf(light + 0x34, -static_cast<float>(c.f[1]));
    stf(light + 0x38, -static_cast<float>(c.f[2]));
    stf(light + 0x3C, -static_cast<float>(c.f[3]));
}

GX_HANDLER(init_specular_dir, "GXInitSpecularDir", Ret::None) {
    uint32_t light = c.r[3];
    float nx = static_cast<float>(c.f[1]);
    float ny = static_cast<float>(c.f[2]);
    float nz = static_cast<float>(c.f[3]);
    float hx = -nx;
    float hy = -ny;
    float hz = 1.0f + -nz;
    float sx = hx * hx;
    float sy = hy * hy;
    float sz = hz * hz;
    float sum = sx + sy;
    sum = sz + sum;
    float scale = sum;
    if (0.0f != sum) {
        gcall(c, 0x801601E8, {}, {sum});  // sqrt
        float root = static_cast<float>(c.f[1]);
        scale = 1.0f / root;
    }
    stf(light + 0x34, hx * scale);
    stf(light + 0x38, hy * scale);
    stf(light + 0x3C, hz * scale);
    const float far = -9.999999843067494e+17f;
    stf(light + 0x28, far * nx);
    stf(light + 0x2C, far * ny);
    stf(light + 0x30, far * nz);
}

GX_HANDLER(init_light_color, "GXInitLightColor", Ret::None) {
    wr32(c.r[3] + 0xC, rd32(c.r[4]));
}

GX_HANDLER(load_light_obj_imm, "GXLoadLightObjImm", Ret::None) {
    uint32_t light = c.r[3];
    uint32_t index = 31 - static_cast<uint32_t>(__builtin_clz(c.r[4] ? c.r[4] : 1));
    if (c.r[4] == 0) {
        index = static_cast<uint32_t>(-1);
    }
    fifo8(0x10);
    fifo32(0x000F0000 | (0x600 + ((index & 7) << 4)));
    fifo32(0);
    fifo32(0);
    fifo32(0);
    fifo32(rd32(light + 0xC));
    for (uint32_t i = 0; i < 12; i++) {
        fifo32(rd32(light + 0x10 + 4 * i));
    }
    wr16(gx() + 0x2, 1);
}

// GXSetChanAmbColor / GXSetChanMatColor share the channel decoding.
void set_channel_color(uint32_t channel, uint32_t color, uint32_t base, uint32_t dirty_bit) {
    uint32_t g = gx();
    uint32_t index;
    uint32_t value;
    switch (channel) {
    case 0: index = 0; value = rlwimi(rd32(g + base), rd32(color), 0, 0, 23); break;
    case 1: index = 1; value = rlwimi(rd32(g + base + 4), rd32(color), 0, 0, 23); break;
    case 2: index = 0; value = rlwimi(rd32(g + base), rd8(color + 3), 0, 24, 31); break;
    case 3: index = 1; value = rlwimi(rd32(g + base + 4), rd8(color + 3), 0, 24, 31); break;
    case 4: index = 0; value = rd32(color); break;
    case 5: index = 1; value = rd32(color); break;
    default: return;
    }
    wr32(g + kDirtyState, rd32(g + kDirtyState) | (dirty_bit << index));
    wr32(g + base + 4 * index, value);
}

GX_HANDLER(set_chan_amb_color, "GXSetChanAmbColor", Ret::None) {
    set_channel_color(c.r[3], c.r[4], 0xA8, 0x100);
}

GX_HANDLER(set_chan_mat_color, "GXSetChanMatColor", Ret::None) {
    set_channel_color(c.r[3], c.r[4], 0xB0, 0x400);
}

GX_HANDLER(set_num_chans, "GXSetNumChans", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x254, rlwimi(rd32(g + 0x254), c.r[3], 4, 25, 27));
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x01000004);
}

// GXSetChanCtrl(chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn)
GX_HANDLER(set_chan_ctrl, "GXSetChanCtrl", Ret::None) {
    uint32_t channel = c.r[3];
    uint32_t enable = c.r[4];
    uint32_t amb_src = c.r[5];
    uint32_t mat_src = c.r[6];
    uint32_t mask = c.r[7];
    uint32_t diffuse = c.r[8];
    uint32_t attenuation = c.r[9];
    if (attenuation == 0) {  // GX_AF_SPEC
        diffuse = 0;
    }
    uint32_t reg = rlwimi(0, enable, 1, 30, 30);
    reg = rlwimi(reg, mat_src, 0, 31, 31);
    reg = rlwimi(reg, amb_src, 6, 25, 25);
    reg = rlwimi(reg, diffuse, 7, 23, 24);
    reg = rlwimi(reg, attenuation != 2 ? 0x80000000u : 0, 10, 22, 22);
    reg = rlwimi(reg, attenuation != 0 ? 0x80000000u : 0, 11, 21, 21);
    reg = rlwimi(reg, mask, 2, 26, 29);
    reg = rlwimi(reg, mask, 7, 17, 20);
    uint32_t index = channel & 3;
    uint32_t g = gx();
    wr32(g + 0xB8 + 4 * index, reg);
    wr32(g + kDirtyState, rd32(g + kDirtyState) | (0x1000u << index));
    if (channel == 4) {
        wr32(g + 0xC0, reg);
        wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x5000);
    } else if (channel == 5) {
        wr32(g + 0xC4, reg);
        wr32(g + kDirtyState, rd32(g + kDirtyState) | 0xA000);
    }
}

// =====================================================================================
// GXTexture
// =====================================================================================

// Tile size (log2 width, log2 height) of a texture or copy format.
void tile_shifts(uint32_t format, uint32_t& sx, uint32_t& sy) {
    switch (format) {
    case 0: case 8: case 14: case 32: case 48:
        sx = 3; sy = 3; break;
    case 1: case 2: case 9: case 17: case 34: case 39: case 40: case 41: case 42: case 57: case 58:
        sx = 3; sy = 2; break;
    case 3: case 4: case 5: case 6: case 10: case 19: case 22: case 35: case 43: case 44: case 60:
        sx = 2; sy = 2; break;
    default:
        sx = 0; sy = 0; break;
    }
}

int32_t tiles(int32_t size, uint32_t shift) {
    return (size + (1 << shift) - 1) >> shift;
}

GX_HANDLER(get_tex_buffer_size, "GXGetTexBufferSize", Ret::Int) {
    uint32_t width = c.r[3];
    uint32_t height = c.r[4];
    uint32_t format = c.r[5];
    uint32_t mipmap = c.r[6];
    uint32_t levels = c.r[7];
    uint32_t sx;
    uint32_t sy;
    tile_shifts(format, sx, sy);
    int32_t tile_bytes = (format == 6 || format == 0x16) ? 64 : 32;
    uint32_t total = 0;
    if (mipmap == 1) {
        for (uint32_t level = 0; level < levels; level++) {
            total += tile_bytes * tiles(static_cast<int32_t>(width), sx) * tiles(static_cast<int32_t>(height), sy);
            if (width == 1 && height == 1) {
                break;
            }
            width = width > 1 ? (width >> 1) & 0x7FFF : 1;
            height = height > 1 ? (height >> 1) & 0x7FFF : 1;
        }
    } else {
        total = tile_bytes * tiles(static_cast<int32_t>(width), sx) * tiles(static_cast<int32_t>(height), sy);
    }
    c.r[3] = total;
}

GX_HANDLER(get_image_tile_count, "__GetImageTileCount", Ret::None) {
    uint32_t format = c.r[3];
    int32_t width = static_cast<int32_t>(c.r[4]);
    int32_t height = static_cast<int32_t>(c.r[5]);
    uint32_t sx;
    uint32_t sy;
    tile_shifts(format, sx, sy);
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
    wr32(c.r[6], static_cast<uint32_t>(tiles(width, sx)));
    wr32(c.r[7], static_cast<uint32_t>(tiles(height, sy)));
    wr32(c.r[8], (format == 6 || format == 0x16) ? 2 : 1);
}

int32_t fctiwz(double value) {
    if (std::isnan(value)) {
        return static_cast<int32_t>(0x80000000);
    }
    if (value >= 2147483647.0) {
        return 0x7FFFFFFF;
    }
    if (value <= -2147483648.0) {
        return static_cast<int32_t>(0x80000000);
    }
    return static_cast<int32_t>(value);
}

// GXInitTexObj(obj, image, width, height, format, wrapS, wrapT, mipmap)
GX_HANDLER(init_tex_obj, "GXInitTexObj", Ret::None) {
    uint32_t obj = c.r[3];
    uint32_t image = c.r[4];
    uint32_t width = c.r[5];
    uint32_t height = c.r[6];
    uint32_t format = c.r[7];
    uint32_t wrap_s = c.r[8];
    uint32_t wrap_t = c.r[9];
    uint32_t mipmap = c.r[10];
    gcall(c, kMemset, {obj, 0, 0x20});
    uint32_t mode0 = rlwimi(rlwimi(rd32(obj), wrap_s, 0, 30, 31), wrap_t, 2, 28, 29) | 0x10;
    wr32(obj, mode0);
    if (mipmap) {
        wr8(obj + 0x1F, rd8(obj + 0x1F) | 1);
        mode0 = rlwimi(mode0, format - 8 <= 2 ? 5 : 6, 5, 24, 26);
        wr32(obj, mode0);
        uint32_t dimension = width > height ? width : height;
        uint32_t max_level = 31 - static_cast<uint32_t>(__builtin_clz(dimension ? dimension : 1));
        if (dimension == 0) {
            max_level = static_cast<uint32_t>(-1);
        }
        float lod = static_cast<float>(max_level);
        lod = 16.0f * lod;
        wr32(obj + 4, rlwimi(rd32(obj + 4), static_cast<uint32_t>(fctiwz(lod)), 8, 16, 23));
    } else {
        wr32(obj, rlwimi(mode0, 4, 5, 24, 26));
    }
    uint32_t image0 = rlwimi(rlwimi(rlwimi(rd32(obj + 8), width - 1, 0, 22, 31), height - 1, 10, 12, 21), format, 20, 8, 11);
    uint32_t image3 = rlwimi(rd32(obj + 0xC), image, 27, 8, 31);
    wr32(obj + 0x14, format);
    wr32(obj + 0x8, image0);
    wr32(obj + 0xC, image3);
    uint32_t load_format;
    uint32_t sx;
    uint32_t sy;
    switch (format & 0xF) {
    case 0: case 8: load_format = 1; sx = 3; sy = 3; break;
    case 1: case 2: case 9: load_format = 2; sx = 3; sy = 2; break;
    case 3: case 4: case 5: case 10: load_format = 2; sx = 2; sy = 2; break;
    case 6: load_format = 3; sx = 2; sy = 2; break;
    case 14: load_format = 0; sx = 3; sy = 3; break;
    default: load_format = 2; sx = 2; sy = 2; break;
    }
    wr8(obj + 0x1E, static_cast<uint8_t>(load_format));
    wr8(obj + 0x1F, rd8(obj + 0x1F) | 2);
    int32_t count = tiles(static_cast<int32_t>(width), sx) * tiles(static_cast<int32_t>(height), sy);
    wr16(obj + 0x1C, static_cast<uint16_t>(count & 0x7FFF));
}

GX_HANDLER(init_tex_obj_ci, "GXInitTexObjCI", Ret::None) {
    uint32_t obj = c.r[3];
    uint32_t tlut = rd32(c.r[1] + 8);
    gcall(c, 0x801AEAE0, {c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.r[8], c.r[9], c.r[10]});  // GXInitTexObj
    wr32(obj + 0x18, tlut);
    wr8(obj + 0x1F, rd8(obj + 0x1F) & ~2u);
}

// GXInitTexObjLOD(obj, minFilt, magFilt, minLod, maxLod, lodBias, biasClamp, doEdgeLod, maxAniso)
GX_HANDLER(init_tex_obj_lod, "GXInitTexObjLOD", Ret::None) {
    static const uint8_t kMinFilter[8] = {0, 4, 1, 5, 2, 6, 0, 0};
    uint32_t obj = c.r[3];
    uint32_t min_filter = c.r[4];
    uint32_t mag_filter = c.r[5];
    float min_lod = static_cast<float>(c.f[1]);
    float max_lod = static_cast<float>(c.f[2]);
    float bias = static_cast<float>(c.f[3]);
    uint32_t bias_clamp = c.r[6];
    uint32_t edge_lod = c.r[7];
    uint32_t aniso = c.r[8];
    if (!(bias >= -4.0f)) {
        bias = -4.0f;
    } else if (bias >= 4.0f) {
        bias = 3.99f;
    }
    float scaled_bias = 32.0f * bias;
    uint32_t mode0 = rd32(obj);
    mode0 = rlwimi(mode0, static_cast<uint32_t>(fctiwz(scaled_bias)), 9, 15, 22);
    mode0 = rlwimi(mode0, mag_filter == 1 ? 32 : 0, 31, 27, 27);
    wr32(obj, mode0);
    mode0 = rlwimi(mode0, kMinFilter[min_filter & 7], 5, 24, 26);
    mode0 = rlwimi(mode0, edge_lod == 0 ? 32 : 0, 3, 23, 23);
    mode0 = rlwinm(mode0, 0, 15, 12);
    mode0 = rlwimi(mode0, aniso, 19, 11, 12);
    mode0 = rlwimi(mode0, bias_clamp, 21, 10, 10);
    wr32(obj, mode0);
    if (!(min_lod >= 0.0f)) {
        min_lod = 0.0f;
    } else if (min_lod > 10.0f) {
        min_lod = 10.0f;
    }
    float scaled_min = 16.0f * min_lod;
    if (!(max_lod >= 0.0f)) {
        max_lod = 0.0f;
    } else if (max_lod > 10.0f) {
        max_lod = 10.0f;
    }
    float scaled_max = 16.0f * max_lod;
    uint32_t mode1 = rlwimi(rd32(obj + 4), static_cast<uint32_t>(fctiwz(scaled_min)), 0, 24, 31);
    mode1 = rlwimi(mode1, static_cast<uint32_t>(fctiwz(scaled_max)), 8, 16, 23);
    wr32(obj + 4, mode1);
}

GX_HANDLER(init_tex_obj_wrap_mode, "GXInitTexObjWrapMode", Ret::None) {
    uint32_t obj = c.r[3];
    wr32(obj, rlwimi(rlwimi(rd32(obj), c.r[4], 0, 30, 31), c.r[5], 2, 28, 29));
}

GX_HANDLER(init_tex_obj_tlut, "GXInitTexObjTlut", Ret::None) {
    wr32(c.r[3] + 0x18, c.r[4]);
}

GX_HANDLER(init_tex_obj_user_data, "GXInitTexObjUserData", Ret::None) {
    wr32(c.r[3] + 0x10, c.r[4]);
}

GX_HANDLER(get_tex_obj_user_data, "GXGetTexObjUserData", Ret::Int) {
    c.r[3] = rd32(c.r[3] + 0x10);
}

GX_HANDLER(get_tex_obj_width, "GXGetTexObjWidth", Ret::Int) {
    c.r[3] = ((rd32(c.r[3] + 8) & 0x3FF) + 1) & 0xFFFF;
}

GX_HANDLER(get_tex_obj_height, "GXGetTexObjHeight", Ret::Int) {
    c.r[3] = (((rd32(c.r[3] + 8) >> 10) & 0x3FF) + 1) & 0xFFFF;
}

GX_HANDLER(get_tex_obj_fmt, "GXGetTexObjFmt", Ret::Int) {
    c.r[3] = rd32(c.r[3] + 0x14);
}

GX_HANDLER(get_tex_obj_wrap_s, "GXGetTexObjWrapS", Ret::Int) {
    c.r[3] = rd32(c.r[3]) & 3;
}

GX_HANDLER(get_tex_obj_wrap_t, "GXGetTexObjWrapT", Ret::Int) {
    c.r[3] = (rd32(c.r[3]) >> 2) & 3;
}

GX_HANDLER(get_tex_obj_mipmap, "GXGetTexObjMipMap", Ret::Int) {
    c.r[3] = rd8(c.r[3] + 0x1F) & 1;
}

// Texture map register numbers (BP) per map: mode0, mode1, image0, image1, image2, image3, tlut.
constexpr uint8_t kTexMapRegs[7][8] = {
    {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3},
    {0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7},
    {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB},
    {0x8C, 0x8D, 0x8E, 0x8F, 0xAC, 0xAD, 0xAE, 0xAF},
    {0x90, 0x91, 0x92, 0x93, 0xB0, 0xB1, 0xB2, 0xB3},
    {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7},
    {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB},
};

GX_HANDLER(load_tex_obj_preloaded, "GXLoadTexObjPreLoaded", Ret::None) {
    uint32_t obj = c.r[3];
    uint32_t region = c.r[4];
    uint32_t map = c.r[5];
    uint32_t mode0 = rlwimi(rd32(obj + 0x0), kTexMapRegs[0][map], 24, 0, 7);
    uint32_t mode1 = rlwimi(rd32(obj + 0x4), kTexMapRegs[1][map], 24, 0, 7);
    uint32_t image0 = rlwimi(rd32(obj + 0x8), kTexMapRegs[2][map], 24, 0, 7);
    uint32_t image1 = rlwimi(rd32(region + 0x0), kTexMapRegs[3][map], 24, 0, 7);
    uint32_t image2 = rlwimi(rd32(region + 0x4), kTexMapRegs[4][map], 24, 0, 7);
    uint32_t image3 = rlwimi(rd32(obj + 0xC), kTexMapRegs[5][map], 24, 0, 7);
    bp(mode0);
    bp(mode1);
    bp(image0);
    bp(image1);
    bp(image2);
    wr32(obj + 0x0, mode0);
    wr32(obj + 0x4, mode1);
    wr32(obj + 0x8, image0);
    wr32(region + 0x0, image1);
    wr32(region + 0x4, image2);
    wr32(obj + 0xC, image3);
    bp(image3);
    if ((rd8(obj + 0x1F) & 2) == 0) {  // color-indexed: load the palette region
        uint32_t tlut_region = gcall(c, rd32(gx() + 0x51C), {rd32(obj + 0x18)});
        uint32_t reg = rlwimi(rd32(tlut_region + 4), kTexMapRegs[6][map], 24, 0, 7);
        wr32(tlut_region + 4, reg);
        bp(rd32(tlut_region + 4));
    }
    uint32_t g = gx();
    wr32(g + 0x564 + 4 * map, rd32(obj + 0x8));
    wr32(g + 0x584 + 4 * map, rd32(obj + 0x0));
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 1);
    wr16(g + 0x2, 0);
}

GX_HANDLER(load_tex_obj, "GXLoadTexObj", Ret::None) {
    uint32_t obj = c.r[3];
    uint32_t map = c.r[4];
    uint32_t region = gcall(c, rd32(gx() + 0x518), {obj, map});
    gcall(c, 0x801AEF20, {obj, region, map});  // GXLoadTexObjPreLoaded
}

GX_HANDLER(init_tlut_obj, "GXInitTlutObj", Ret::None) {
    uint32_t tlut = c.r[3];
    uint32_t reg = rlwimi(rlwimi(rd32(tlut + 4), c.r[4], 27, 8, 31), 0x64, 24, 0, 7);
    wr16(tlut + 0x8, static_cast<uint16_t>(c.r[6]));
    wr32(tlut + 0x0, rlwimi(0, c.r[5], 10, 20, 21));
    wr32(tlut + 0x4, reg);
}

GX_HANDLER(load_tlut, "GXLoadTlut", Ret::None) {
    uint32_t tlut = c.r[3];
    uint32_t region = gcall(c, rd32(gx() + 0x51C), {c.r[4]});
    gcall(c, kGXFlushTextureState);
    bp(rd32(tlut + 4));
    bp(rd32(region + 0));
    gcall(c, kGXFlushTextureState);
    uint32_t value = rlwimi(rd32(tlut + 0), rd32(region + 0), 0, 22, 31);
    wr32(region + 0x4, value);
    wr32(region + 0x8, rd32(tlut + 4));
    wr32(region + 0xC, rd32(tlut + 8));
    wr32(tlut + 0x0, value);
}

uint32_t cache_size_code(uint32_t size, bool odd) {
    switch (size) {
    case 0: return 3;
    case 1: return 4;
    case 2: return 5;
    case 3: return odd ? 0 : 3;
    default: return size;
    }
}

GX_HANDLER(init_tex_cache_region, "GXInitTexCacheRegion", Ret::None) {
    uint32_t region = c.r[3];
    uint32_t is32 = c.r[4];
    uint32_t even = cache_size_code(c.r[6], false);
    if (c.r[6] > 2) {
        even = c.r[6];
    }
    uint32_t odd = cache_size_code(c.r[8], true);
    if (c.r[8] > 3) {
        odd = c.r[8];
    }
    uint32_t image1 = rlwimi(rlwimi(rlwimi(0, c.r[5], 27, 17, 31), even, 15, 14, 16), even, 18, 11, 13);
    wr32(region + 0x0, image1);
    uint32_t image2 = rlwimi(rlwimi(rlwimi(0, c.r[7], 27, 17, 31), odd, 15, 14, 16), odd, 18, 11, 13);
    wr8(region + 0xC, static_cast<uint8_t>(is32));
    wr8(region + 0xD, 1);
    wr32(region + 0x4, image2);
}

GX_HANDLER(init_tlut_region, "GXInitTlutRegion", Ret::None) {
    uint32_t reg = rlwimi(rlwimi(rlwimi(0, c.r[4] - 0x80000, 23, 22, 31), c.r[5], 10, 11, 21), 0x65, 24, 0, 7);
    wr32(c.r[3], reg);
}

GX_HANDLER(invalidate_tex_all, "GXInvalidateTexAll", Ret::None) {
    gcall(c, kGXFlushTextureState);
    bp(0x66001000);
    bp(0x66001100);
    gcall(c, kGXFlushTextureState);
}

GX_HANDLER(set_tex_region_callback, "GXSetTexRegionCallback", Ret::Int) {
    uint32_t g = gx();
    uint32_t previous = rd32(g + 0x518);
    wr32(g + 0x518, c.r[3]);
    c.r[3] = previous;
}

GX_HANDLER(set_tlut_region_callback, "GXSetTlutRegionCallback", Ret::Int) {
    uint32_t g = gx();
    uint32_t previous = rd32(g + 0x51C);
    wr32(g + 0x51C, c.r[3]);
    c.r[3] = previous;
}

void send_su_scale(uint32_t g, uint32_t coord) {
    bp(rd32(g + 0x108 + 4 * coord));
    bp(rd32(g + 0x128 + 4 * coord));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_tex_coord_scale_manually, "GXSetTexCoordScaleManually", Ret::None) {
    uint32_t coord = c.r[3];
    uint32_t enable = c.r[4];
    uint32_t g = gx();
    wr32(g + 0x5E4, (rd32(g + 0x5E4) & ~(1u << coord)) | (enable << coord));
    if (!enable) {
        return;
    }
    wr32(g + 0x108 + 4 * coord, rlwimi(rd32(g + 0x108 + 4 * coord), c.r[5] - 1, 0, 16, 31));
    wr32(g + 0x128 + 4 * coord, rlwimi(rd32(g + 0x128 + 4 * coord), c.r[6] - 1, 0, 16, 31));
    send_su_scale(g, coord);
}

void set_su_bits(uint32_t coord, uint32_t s, uint32_t t, int shift, int bit) {
    uint32_t g = gx();
    wr32(g + 0x108 + 4 * coord, rlwimi(rd32(g + 0x108 + 4 * coord), s, shift, bit, bit));
    wr32(g + 0x128 + 4 * coord, rlwimi(rd32(g + 0x128 + 4 * coord), t, shift, bit, bit));
    if (rd32(g + 0x5E4) & (1u << coord)) {
        send_su_scale(g, coord);
    }
}

GX_HANDLER(set_tex_coord_cyl_wrap, "GXSetTexCoordCylWrap", Ret::None) {
    set_su_bits(c.r[3], c.r[4], c.r[5], 17, 14);
}

GX_HANDLER(set_tex_coord_bias, "GXSetTexCoordBias", Ret::None) {
    set_su_bits(c.r[3], c.r[4], c.r[5], 16, 15);
}

// __SetSURegs(texmap, texcoord): coordinate scale from the loaded texture's size and wrap.
GX_HANDLER(set_su_regs, "__SetSURegs", Ret::None) {
    uint32_t map = c.r[3];
    uint32_t coord = c.r[4];
    uint32_t g = gx();
    uint32_t image0 = rd32(g + 0x564 + 4 * map);
    uint32_t s = rlwimi(rd32(g + 0x108 + 4 * coord), image0 & 0x3FF, 0, 16, 31);
    wr32(g + 0x108 + 4 * coord, s);
    uint32_t t = rlwimi(rd32(g + 0x128 + 4 * coord), (image0 >> 10) & 0x3FF, 0, 16, 31);
    wr32(g + 0x128 + 4 * coord, t);
    uint32_t mode0 = rd32(g + 0x584 + 4 * map);
    s = rlwimi(s, (mode0 & 3) == 1 ? 32 : 0, 11, 15, 15);
    wr32(g + 0x108 + 4 * coord, s);
    t = rlwimi(t, ((mode0 >> 2) & 3) == 1 ? 32 : 0, 11, 15, 15);
    wr32(g + 0x128 + 4 * coord, t);
    send_su_scale(g, coord);
}

// __GXSetSUTexRegs: refresh the coordinate scales of every texture in use (indirect and TEV stages).
GX_HANDLER(set_su_tex_regs, "__GXSetSUTexRegs", Ret::None) {
    uint32_t g = gx();
    if (rd32(g + 0x5E4) == 0xFF) {
        return;
    }
    uint32_t gen_mode = rd32(g + 0x254);
    uint32_t indirect_stages = (gen_mode >> 16) & 7;
    uint32_t tev_stages = ((gen_mode >> 10) & 0xF) + 1;
    uint32_t map = 0;
    uint32_t coord = 0;
    for (uint32_t i = 0; i < indirect_stages; i++) {
        if (i < 4) {
            uint32_t iref = rd32(g + 0x170);
            map = (iref >> (6 * i)) & 7;
            coord = (iref >> (6 * i + 3)) & 7;
        }
        if ((rd32(g + 0x5E4) & (1u << coord)) == 0) {
            gcall(c, 0x801AF480, {map, coord});
        }
    }
    for (uint32_t i = 0; i < tev_stages; i++) {
        uint32_t order = rd32(g + 0x150 + 4 * (i >> 1));
        uint32_t stage_map = rd32(g + 0x5A4 + 4 * i) & ~0x100u;
        uint32_t stage_coord = (i & 1) ? (order >> 15) & 7 : (order >> 3) & 7;
        if (stage_map != 0xFF && (rd32(g + 0x5E4) & (1u << stage_coord)) == 0 && (rd32(g + 0x5E8) & (1u << i))) {
            gcall(c, 0x801AF480, {stage_map, stage_coord});
        }
    }
}

constexpr uint32_t kTmemConfig[3][16] = {
    {0x8C0D8000, 0x900DC000, 0x8D0D8400, 0x910DC400, 0x8E0D8800, 0x920DC800, 0x8F0D8C00, 0x930DCC00, 0xAC0D9000, 0xB00DD000, 0xAD0D9400, 0xB10DD400, 0xAE0D9800, 0xB20DD800, 0xAF0D9C00, 0xB30DDC00},
    {0x8C0D8000, 0x900DC000, 0x8D0D8800, 0x910DC800, 0x8E0D9000, 0x920DD000, 0x8F0D9800, 0x930DD800, 0xAC0DA000, 0xB00DE000, 0xAD0DA800, 0xB10DE800, 0xAE0DB000, 0xB20DF000, 0xAF0DB800, 0xB30DF800},
    {0x8C0D8000, 0x900DC000, 0x8D0D8800, 0x910DC800, 0x8E0D9000, 0x920DD000, 0x8F0D9800, 0x930DD800, 0xAC0DA000, 0xB00DC400, 0xAD0DA800, 0xB10DCC00, 0xAE0DB000, 0xB20DD400, 0xAF0DB800, 0xB30DDC00},
};

GX_HANDLER(set_tmem_config, "__GXSetTmemConfig", Ret::None) {
    uint32_t config = c.r[3];
    const uint32_t* regs = kTmemConfig[config == 2 ? 2 : config == 1 ? 1 : 0];
    for (int i = 0; i < 16; i++) {
        bp(regs[i]);
    }
}

// =====================================================================================
// GXBump
// =====================================================================================

GX_HANDLER(set_tev_indirect, "GXSetTevIndirect", Ret::None) {
    uint32_t stage = c.r[3];
    uint32_t utc_lod = rd8(c.r[1] + 0xB);
    uint32_t alpha_sel = rd32(c.r[1] + 0xC);
    uint32_t reg = rlwimi(0, c.r[4], 0, 30, 31);
    reg = rlwimi(reg, c.r[5], 2, 28, 29);
    reg = rlwimi(reg, c.r[6], 4, 25, 27);
    reg = rlwimi(reg, alpha_sel, 7, 23, 24);
    reg = rlwimi(reg, c.r[7], 9, 19, 22);
    reg = rlwimi(reg, c.r[8], 13, 16, 18);
    reg = rlwimi(reg, c.r[9], 16, 13, 15);
    reg = rlwimi(reg, utc_lod, 19, 12, 12);
    reg = rlwimi(reg, c.r[10], 20, 11, 11);
    reg = rlwimi(reg, stage + 0x10, 24, 0, 7);
    bp(reg);
    wr16(gx() + 0x2, 0);
}

// GXSetIndTexMtx(mtxId, const f32 offset[2][3], s8 scaleExp)
GX_HANDLER(set_ind_tex_mtx, "GXSetIndTexMtx", Ret::None) {
    uint32_t id = c.r[3];
    uint32_t m = c.r[4];
    uint32_t scale = c.r[5] + 0x11;
    uint32_t index = 0;
    if (id - 1 <= 2) {
        index = id - 1;
    } else if (id - 5 <= 2) {
        index = id - 5;
    } else if (id - 9 <= 2) {
        index = id - 9;
    }
    uint32_t v[6];
    for (int i = 0; i < 6; i++) {
        float value = ldf(m + 4 * i);
        value = 1024.0f * value;
        v[i] = static_cast<uint32_t>(fctiwz(value));
    }
    uint32_t base = 3 * index;
    // Row-major [2][3]: column k is (v[k], v[3 + k]).
    bp(rlwimi(rlwimi(rlwimi(rlwimi(0, v[0], 0, 21, 31), v[3], 11, 10, 20), scale, 22, 8, 9), base + 6, 24, 0, 7));
    bp(rlwimi(rlwimi(rlwimi(rlwimi(0, v[1], 0, 21, 31), v[4], 11, 10, 20), scale, 20, 8, 9), base + 7, 24, 0, 7));
    bp(rlwimi(rlwimi(rlwimi(rlwimi(0, v[2], 0, 21, 31), v[5], 11, 10, 20), scale, 18, 8, 9), base + 8, 24, 0, 7));
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_ind_tex_coord_scale, "GXSetIndTexCoordScale", Ret::None) {
    uint32_t stage = c.r[3];
    uint32_t g = gx();
    if (stage <= 3) {
        uint32_t offset = stage < 2 ? 0x178 : 0x17C;
        uint32_t reg_id = stage < 2 ? 0x25 : 0x26;
        int shift = (stage & 1) ? 8 : 0;
        uint32_t reg = rd32(g + offset);
        reg = rlwimi(reg, c.r[4], shift, 28 - shift, 31 - shift);
        reg = rlwimi(reg, c.r[5], shift + 4, 24 - shift, 27 - shift);
        reg = rlwimi(reg, reg_id, 24, 0, 7);
        wr32(g + offset, reg);
        bp(rd32(g + offset));
    }
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_ind_tex_order, "GXSetIndTexOrder", Ret::None) {
    uint32_t stage = c.r[3];
    uint32_t coord = c.r[4] == 0xFF ? 0 : c.r[4];
    uint32_t map = c.r[5] == 0xFF ? 0 : c.r[5];
    uint32_t g = gx();
    if (stage <= 3) {
        int shift = 6 * static_cast<int>(stage);
        uint32_t iref = rd32(g + 0x170);
        iref = rlwimi(iref, map, shift, 29 - shift, 31 - shift);
        iref = rlwimi(iref, coord, shift + 3, 26 - shift, 28 - shift);
        wr32(g + 0x170, iref);
    }
    bp(rd32(g + 0x170));
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x3);
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_num_ind_stages, "GXSetNumIndStages", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x254, rlwimi(rd32(g + 0x254), c.r[3], 16, 13, 15));
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x6);
}

GX_HANDLER(set_tev_direct, "GXSetTevDirect", Ret::None) {
    bp(rlwimi(0, c.r[3] + 0x10, 24, 0, 7));
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(update_bp_mask, "__GXUpdateBPMask", Ret::None) {
}

GX_HANDLER(set_indirect_mask, "__GXSetIndirectMask", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x174, rlwimi(rd32(g + 0x174), c.r[3], 0, 24, 31));
    bp(rd32(g + 0x174));
    wr16(g + 0x2, 0);
}

GX_HANDLER(flush_texture_state, "__GXFlushTextureState", Ret::None) {
    uint32_t g = gx();
    bp(rd32(g + 0x174));
    wr16(g + 0x2, 0);
}

// =====================================================================================
// GXTev
// =====================================================================================

// GXSetTevOp presets: color then alpha, for stage 0 and the other stages.
constexpr uint32_t kTevOpColor[2][5] = {{0xC008F8AF, 0xC008A89F, 0xC008AC8F, 0xC008FFF8, 0xC008FFFA},
                                        {0xC008F80F, 0xC008089F, 0xC0080C8F, 0xC008FFF8, 0xC008FFF0}};
constexpr uint32_t kTevOpAlpha[2][5] = {{0xC108F2F0, 0xC108FFD0, 0xC108F2F0, 0xC108FFC0, 0xC108FFD0},
                                        {0xC108F070, 0xC108FF80, 0xC108F070, 0xC108FFC0, 0xC108FF80}};

GX_HANDLER(set_tev_op, "GXSetTevOp", Ret::None) {
    uint32_t stage = c.r[3];
    uint32_t mode = c.r[4];
    uint32_t which = stage == 0 ? 0 : 1;
    uint32_t g = gx();
    uint32_t color = rlwimi(rd32(g + 0x180 + 4 * stage) & 0xFF000000, kTevOpColor[which][mode], 0, 8, 31);
    bp(color);
    wr32(g + 0x180 + 4 * stage, color);
    uint32_t alpha = rlwimi(rlwinm(rd32(g + 0x1C0 + 4 * stage), 0, 28, 7), kTevOpAlpha[which][mode], 0, 8, 27);
    bp(alpha);
    wr32(g + 0x1C0 + 4 * stage, alpha);
    wr16(g + 0x2, 0);
}

void tev_stage_write(uint32_t offset, uint32_t value) {
    uint32_t g = gx();
    bp(value);
    wr32(g + offset, value);
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_tev_color_in, "GXSetTevColorIn", Ret::None) {
    uint32_t offset = 0x180 + 4 * c.r[3];
    uint32_t reg = rd32(gx() + offset);
    reg = rlwimi(reg, c.r[4], 12, 16, 19);
    reg = rlwimi(reg, c.r[5], 8, 20, 23);
    reg = rlwimi(reg, c.r[6], 4, 24, 27);
    reg = rlwimi(reg, c.r[7], 0, 28, 31);
    tev_stage_write(offset, reg);
}

GX_HANDLER(set_tev_alpha_in, "GXSetTevAlphaIn", Ret::None) {
    uint32_t offset = 0x1C0 + 4 * c.r[3];
    uint32_t reg = rd32(gx() + offset);
    reg = rlwimi(reg, c.r[4], 13, 16, 18);
    reg = rlwimi(reg, c.r[5], 10, 19, 21);
    reg = rlwimi(reg, c.r[6], 7, 22, 24);
    reg = rlwimi(reg, c.r[7], 4, 25, 27);
    tev_stage_write(offset, reg);
}

// GXSetTevColorOp / GXSetTevAlphaOp(stage, op, bias, scale, clamp, outReg)
uint32_t tev_op(uint32_t reg, Cpu& c) {
    uint32_t op = c.r[4];
    reg = rlwimi(reg, op, 18, 13, 13);
    if (static_cast<int32_t>(op) <= 1) {
        reg = rlwimi(reg, c.r[6], 20, 10, 11);
        reg = rlwimi(reg, c.r[5], 16, 14, 15);
    } else {  // compare ops: the scale field selects the comparison, bias is "compare"
        reg = rlwimi(reg, op, 19, 10, 11);
        reg |= 0x30000;
    }
    reg = rlwimi(reg, c.r[7], 19, 12, 12);
    reg = rlwimi(reg, c.r[8], 22, 8, 9);
    return reg;
}

GX_HANDLER(set_tev_color_op, "GXSetTevColorOp", Ret::None) {
    uint32_t offset = 0x180 + 4 * c.r[3];
    tev_stage_write(offset, tev_op(rd32(gx() + offset), c));
}

GX_HANDLER(set_tev_alpha_op, "GXSetTevAlphaOp", Ret::None) {
    uint32_t offset = 0x1C0 + 4 * c.r[3];
    tev_stage_write(offset, tev_op(rd32(gx() + offset), c));
}

GX_HANDLER(set_tev_color, "GXSetTevColor", Ret::None) {
    uint32_t id = c.r[3];
    uint32_t color = rd32(c.r[4]);
    uint32_t ra = rlwimi(rlwimi((0xE0 + 2 * id) << 24, color, 8, 24, 31), color, 12, 12, 19);
    uint32_t bg = rlwimi(rlwimi((0xE1 + 2 * id) << 24, color, 24, 24, 31), color, 28, 12, 19);
    bp(ra);
    bp(bg);
    bp(bg);
    bp(bg);
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_tev_color_s10, "GXSetTevColorS10", Ret::None) {
    uint32_t id = c.r[3];
    uint32_t rg = rd32(c.r[4]);
    uint32_t ba = rd32(c.r[4] + 4);
    uint32_t ra = rlwimi(rlwimi((0xE0 + 2 * id) << 24, rg, 16, 21, 31), ba, 12, 9, 19);
    uint32_t bg = rlwimi(rlwimi((0xE1 + 2 * id) << 24, ba, 16, 21, 31), rg, 12, 9, 19);
    bp(ra);
    bp(bg);
    bp(bg);
    bp(bg);
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_tev_k_color, "GXSetTevKColor", Ret::None) {
    uint32_t id = c.r[3];
    uint32_t color = rd32(c.r[4]);
    uint32_t ra = rlwimi(rlwimi(rlwimi((0xE0 + 2 * id) << 24, color, 8, 24, 31), color, 12, 12, 19), 8, 20, 8, 11);
    uint32_t bg = rlwimi(rlwimi(rlwimi((0xE1 + 2 * id) << 24, color, 24, 24, 31), color, 28, 12, 19), 8, 20, 8, 11);
    bp(ra);
    bp(bg);
    wr16(gx() + 0x2, 0);
}

void set_konst_select(uint32_t stage, uint32_t select, int odd_shift, int odd_mb, int odd_me, int even_shift, int even_mb, int even_me) {
    uint32_t g = gx();
    uint32_t reg = g + 0x200 + 4 * (stage >> 1);
    if (stage & 1) {
        wr32(reg, rlwimi(rd32(reg), select, odd_shift, odd_mb, odd_me));
    } else {
        wr32(reg, rlwimi(rd32(reg), select, even_shift, even_mb, even_me));
    }
    bp(rd32(reg));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_tev_k_color_sel, "GXSetTevKColorSel", Ret::None) {
    set_konst_select(c.r[3], c.r[4], 14, 13, 17, 4, 23, 27);
}

GX_HANDLER(set_tev_k_alpha_sel, "GXSetTevKAlphaSel", Ret::None) {
    set_konst_select(c.r[3], c.r[4], 19, 8, 12, 9, 18, 22);
}

GX_HANDLER(set_tev_swap_mode, "GXSetTevSwapMode", Ret::None) {
    uint32_t g = gx();
    uint32_t reg = g + 0x1C0 + 4 * c.r[3];
    wr32(reg, rlwimi(rlwimi(rd32(reg), c.r[4], 0, 30, 31), c.r[5], 2, 28, 29));
    bp(rd32(reg));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_tev_swap_mode_table, "GXSetTevSwapModeTable", Ret::None) {
    uint32_t table = c.r[3];
    uint32_t g = gx();
    uint32_t first = g + 0x200 + 8 * table;
    uint32_t second = g + 0x200 + 4 * (2 * table + 1);
    wr32(first, rlwimi(rlwimi(rd32(first), c.r[4], 0, 30, 31), c.r[5], 2, 28, 29));
    bp(rd32(first));
    wr32(second, rlwimi(rlwimi(rd32(second), c.r[6], 0, 30, 31), c.r[7], 2, 28, 29));
    bp(rd32(second));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_alpha_compare, "GXSetAlphaCompare", Ret::None) {
    uint32_t reg = 0xF3000000;
    reg = rlwimi(reg, c.r[4], 0, 24, 31);
    reg = rlwimi(reg, c.r[7], 8, 16, 23);
    reg = rlwimi(reg, c.r[3], 16, 13, 15);
    reg = rlwimi(reg, c.r[6], 19, 10, 12);
    reg = rlwimi(reg, c.r[5], 22, 8, 9);
    bp(reg);
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_z_texture, "GXSetZTexture", Ret::None) {
    uint32_t op = c.r[3];
    uint32_t format = c.r[4];
    uint32_t bias = c.r[5];
    uint32_t type = format == 0x11 ? 0 : format == 0x13 ? 1 : 2;
    bp(rlwimi(rlwimi(0, bias, 0, 8, 31), 0xF4, 24, 0, 7));
    bp(rlwimi(rlwimi(rlwimi(0, type, 0, 30, 31), op, 2, 28, 29), 0xF5, 24, 0, 7));
    wr16(gx() + 0x2, 0);
}

// GXSetTevOrder(stage, coord, map, color)
GX_HANDLER(set_tev_order, "GXSetTevOrder", Ret::None) {
    static const uint32_t kColorChannel[10] = {0, 1, 0, 1, 0, 1, 7, 5, 6, 0};
    uint32_t stage = c.r[3];
    uint32_t coord = c.r[4];
    uint32_t map = c.r[5];
    uint32_t color = c.r[6];
    uint32_t texmap = map & ~0x100u;
    if (texmap >= 8) {
        texmap = 0;
    }
    uint32_t g = gx();
    wr32(g + 0x5A4 + 4 * stage, map);
    uint32_t order = g + 0x150 + 4 * ((stage + (stage >> 31)) >> 1);
    if (static_cast<int32_t>(coord) >= 8) {
        wr32(g + 0x5E8, rd32(g + 0x5E8) & ~(1u << stage));
        coord = 0;
    } else {
        wr32(g + 0x5E8, rd32(g + 0x5E8) | (1u << stage));
    }
    uint32_t channel = color == 0xFF ? 7 : kColorChannel[color];
    uint32_t enable = (map != 0xFF && (map & 0x100) == 0) ? 1 : 0;
    uint32_t reg = rd32(order);
    if (stage & 1) {
        reg = rlwimi(rlwimi(reg, texmap, 12, 17, 19), coord, 15, 14, 16);
        wr32(order, reg);
        reg = rlwimi(reg, channel, 19, 10, 12);
        wr32(order, reg);
        reg = rlwimi(reg, enable, 18, 13, 13);
    } else {
        reg = rlwimi(rlwimi(reg, texmap, 0, 29, 31), coord, 3, 26, 28);
        wr32(order, reg);
        reg = rlwimi(reg, channel, 7, 22, 24);
        wr32(order, reg);
        reg = rlwimi(reg, enable, 6, 25, 25);
    }
    wr32(order, reg);
    bp(rd32(order));
    wr16(g + 0x2, 0);
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x1);
}

GX_HANDLER(set_num_tev_stages, "GXSetNumTevStages", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x254, rlwimi(rd32(g + 0x254), c.r[3] - 1, 10, 18, 21));
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x4);
}

uint32_t float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

// GXSetFog(type, startz, endz, nearz, farz, color)
GX_HANDLER(set_fog, "GXSetFog", Ret::None) {
    uint32_t type = c.r[3];
    uint32_t color = c.r[4];
    float start = static_cast<float>(c.f[1]);
    float end = static_cast<float>(c.f[2]);
    float near = static_cast<float>(c.f[3]);
    float far = static_cast<float>(c.f[4]);
    uint32_t function = type & 7;
    uint32_t ortho = (type >> 3) & 1;
    float a;
    float cc;
    if (ortho) {
        if (far == near || end == start) {
            a = 0.0f;
            cc = 0.0f;
        } else {
            float range = end - start;
            float depth = far - near;
            float offset = start - near;
            float inverse = 1.0f / range;
            a = inverse * depth;
            cc = inverse * offset;
        }
    } else {
        float b;
        if (far == near || end == start) {
            a = 0.0f;
            b = 0.5f;
            cc = 0.0f;
        } else {
            float depth = far - near;
            float range = end - start;
            float product = far * near;
            b = far / depth;
            depth = depth * range;
            cc = start / range;
            a = product / depth;
        }
        int32_t exponent = 0;
        while (static_cast<double>(b) > 1.0) {
            b = b * 0.5f;
            exponent++;
        }
        while (b > 0.0f && static_cast<double>(b) < 0.5) {
            b = b * 2.0f;
            exponent--;
        }
        float mantissa_scale = 8388638.0f * b;
        uint32_t shift = static_cast<uint32_t>(exponent + 1) & 0x3F;  // slw semantics
        float divisor = static_cast<float>(static_cast<int32_t>(shift < 32 ? 1u << shift : 0u));
        a = a / divisor;
        uint32_t mantissa = fp_to_unsigned(c, mantissa_scale);
        bp(rlwimi(rlwimi(0, mantissa, 0, 8, 31), 0xEF, 24, 0, 7));
        bp(rlwimi(rlwimi(0, static_cast<uint32_t>(exponent + 1), 0, 27, 31), 0xF0, 24, 0, 7));
    }
    uint32_t abits = float_bits(a);
    uint32_t cbits = float_bits(cc);
    uint32_t reg_a = rlwimi(rlwimi(rlwimi(0, abits, 20, 21, 31), abits, 20, 13, 20), abits, 20, 12, 12);
    reg_a = rlwimi(reg_a, 0xEE, 24, 0, 7);
    bp(reg_a);
    uint32_t reg_c = rlwimi(rlwimi(rlwimi(0, cbits, 20, 21, 31), cbits, 20, 13, 20), cbits, 20, 12, 12);
    reg_c = rlwimi(reg_c, ortho, 20, 11, 11);
    reg_c = rlwimi(reg_c, function, 21, 8, 10);
    reg_c = rlwimi(reg_c, 0xF1, 24, 0, 7);
    bp(reg_c);
    bp(rlwimi(rlwimi(0, rd32(color), 24, 8, 31), 0xF2, 24, 0, 7));
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_fog_range_adj, "GXSetFogRangeAdj", Ret::None) {
    uint32_t enable = c.r[3];
    uint32_t center = c.r[4];
    uint32_t table = c.r[5];
    if (enable) {
        for (uint32_t k = 0; k < 5; k++) {
            uint32_t reg = rlwimi(rlwimi(0, rd16(table + 4 * k), 0, 20, 31), rd16(table + 4 * k + 2), 12, 8, 19);
            bp(rlwimi(reg, 0xE9 + k, 24, 0, 7));
        }
    }
    bp(rlwimi(rlwimi(rlwimi(0, center + 0x156, 0, 22, 31), enable, 10, 21, 21), 0xE8, 24, 0, 7));
    wr16(gx() + 0x2, 0);
}

// =====================================================================================
// GXPixel
// =====================================================================================

GX_HANDLER(set_blend_mode, "GXSetBlendMode", Ret::None) {
    uint32_t type = c.r[3];
    uint32_t g = gx();
    uint32_t reg = rd32(g + 0x220);
    reg = rlwimi(reg, type == 3 ? 32 : 0, 6, 20, 20);
    reg = rlwimi(reg, type, 0, 31, 31);
    reg = rlwimi(reg, type == 2 ? 32 : 0, 28, 30, 30);
    reg = rlwimi(reg, c.r[6], 12, 16, 19);
    reg = rlwimi(reg, c.r[4], 8, 21, 23);
    reg = rlwimi(reg, c.r[5], 5, 24, 26);
    bp(reg);
    wr32(g + 0x220, reg);
    wr16(g + 0x2, 0);
}

void update_cmode0_bit(uint32_t value, int shift, int bit) {
    uint32_t g = gx();
    uint32_t reg = rlwimi(rd32(g + 0x220), value, shift, bit, bit);
    bp(reg);
    wr32(g + 0x220, reg);
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_color_update, "GXSetColorUpdate", Ret::None) {
    update_cmode0_bit(c.r[3], 3, 28);
}

GX_HANDLER(set_alpha_update, "GXSetAlphaUpdate", Ret::None) {
    update_cmode0_bit(c.r[3], 4, 27);
}

GX_HANDLER(set_dither, "GXSetDither", Ret::None) {
    update_cmode0_bit(c.r[3], 2, 29);
}

GX_HANDLER(set_z_mode, "GXSetZMode", Ret::None) {
    uint32_t g = gx();
    uint32_t reg = rd32(g + 0x228);
    reg = rlwimi(reg, c.r[3], 0, 31, 31);
    reg = rlwimi(reg, c.r[4], 1, 28, 30);
    reg = rlwimi(reg, c.r[5], 4, 27, 27);
    bp(reg);
    wr32(g + 0x228, reg);
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_z_comp_loc, "GXSetZCompLoc", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x22C, rlwimi(rd32(g + 0x22C), c.r[3], 6, 25, 25));
    bp(rd32(g + 0x22C));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_pixel_fmt, "GXSetPixelFmt", Ret::None) {
    static const uint32_t kPixelFormat[8] = {0, 1, 2, 3, 4, 4, 4, 5};
    uint32_t pixel = c.r[3];
    uint32_t g = gx();
    uint32_t old = rd32(g + 0x22C);
    uint32_t updated = rlwimi(rlwimi(old, kPixelFormat[pixel], 0, 29, 31), c.r[4], 3, 26, 28);
    wr32(g + 0x22C, updated);
    if (old != updated) {
        bp(rd32(g + 0x22C));
        wr32(g + 0x254, rlwimi(rd32(g + 0x254), pixel == 2 ? 32 : 0, 4, 22, 22));
        wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x4);
    }
    if (kPixelFormat[pixel] == 4) {  // Y8/U8/V8: select the channel in cmode1
        uint32_t reg = rlwimi(rlwimi(rd32(g + 0x224), pixel - 4, 9, 21, 22), 0x42, 24, 0, 7);
        wr32(g + 0x224, reg);
        bp(rd32(g + 0x224));
    }
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_dst_alpha, "GXSetDstAlpha", Ret::None) {
    uint32_t g = gx();
    uint32_t reg = rlwimi(rlwimi(rd32(g + 0x224), c.r[4], 0, 24, 31), c.r[3], 8, 23, 23);
    bp(reg);
    wr32(g + 0x224, reg);
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_field_mask, "GXSetFieldMask", Ret::None) {
    bp(rlwimi(rlwimi(rlwimi(0, c.r[4], 0, 31, 31), c.r[3], 1, 30, 30), 0x44, 24, 0, 7));
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_field_mode, "GXSetFieldMode", Ret::None) {
    uint32_t mode = c.r[3];
    uint32_t g = gx();
    wr32(g + 0x7C, rlwimi(rd32(g + 0x7C), c.r[4], 22, 9, 9));
    bp(rd32(g + 0x7C));
    gcall(c, kGXFlushTextureState);
    bp(mode | 0x68000000);
    gcall(c, kGXFlushTextureState);
}

// =====================================================================================
// GXDisplayList
// =====================================================================================

GX_HANDLER(call_display_list, "GXCallDisplayList", Ret::None) {
    uint32_t list = c.r[3];
    uint32_t bytes = c.r[4];
    uint32_t g = gx();
    if (rd32(g + kDirtyState) != 0) {
        gcall(c, kGXSetDirtyState);
    }
    if (rd32(g + 0x0) == 0) {
        gcall(c, 0x801AD340);  // __GXSendFlushPrim
    }
    fifo8(0x40);
    fifo32(list);
    fifo32(bytes);
}

// =====================================================================================
// GXTransform
// =====================================================================================

GX_HANDLER(set_projection_regs, "__GXSetProjection", Ret::None) {
    uint32_t g = gx();
    fifo8(0x10);
    fifo32(0x00061020);
    for (uint32_t i = 0; i < 6; i++) {
        fifo32(rd32(g + 0x52C + 4 * i));
    }
    fifo32(rd32(g + 0x528));
}

GX_HANDLER(set_projection, "GXSetProjection", Ret::None) {
    uint32_t m = c.r[3];
    uint32_t type = c.r[4];
    uint32_t g = gx();
    wr32(g + 0x528, type);
    wr32(g + 0x52C, rd32(m + 0x0));
    wr32(g + 0x534, rd32(m + 0x14));
    wr32(g + 0x53C, rd32(m + 0x28));
    wr32(g + 0x540, rd32(m + 0x2C));
    if (type == 1) {  // orthographic
        wr32(g + 0x530, rd32(m + 0xC));
        wr32(g + 0x538, rd32(m + 0x1C));
    } else {
        wr32(g + 0x530, rd32(m + 0x8));
        wr32(g + 0x538, rd32(m + 0x18));
    }
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x08000000);
}

GX_HANDLER(set_projectionv, "GXSetProjectionv", Ret::None) {
    uint32_t v = c.r[3];
    uint32_t g = gx();
    wr32(g + 0x528, 0.0f == ldf(v) ? 0 : 1);
    for (uint32_t i = 0; i < 6; i++) {
        wr32(g + 0x52C + 4 * i, rd32(v + 4 + 4 * i));
    }
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x08000000);
}

GX_HANDLER(load_pos_mtx_imm, "GXLoadPosMtxImm", Ret::None) {
    uint32_t m = c.r[3];
    fifo8(0x10);
    fifo32(0x000B0000 | (c.r[4] << 2));
    for (uint32_t i = 0; i < 12; i++) {
        fifo32(rd32(m + 4 * i));
    }
}

GX_HANDLER(load_nrm_mtx_imm, "GXLoadNrmMtxImm", Ret::None) {
    uint32_t m = c.r[3];
    fifo8(0x10);
    fifo32(0x00080000 | (3 * c.r[4] + 0x400));
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t col = 0; col < 3; col++) {
            fifo32(rd32(m + 0x10 * row + 4 * col));
        }
    }
}

GX_HANDLER(set_current_mtx, "GXSetCurrentMtx", Ret::None) {
    uint32_t g = gx();
    wr32(g + 0x80, rlwimi(rd32(g + 0x80), c.r[3], 0, 26, 31));
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x04000000);
}

GX_HANDLER(load_tex_mtx_imm, "GXLoadTexMtxImm", Ret::None) {
    uint32_t m = c.r[3];
    uint32_t id = c.r[4];
    uint32_t type = c.r[5];
    uint32_t address = id >= 0x40 ? 0x500 + ((id - 0x40) << 2) : id << 2;
    uint32_t count = type == 1 ? 8 : 12;
    fifo8(0x10);
    fifo32(address | ((count - 1) << 16));
    uint32_t words = type == 0 ? 12 : 8;
    for (uint32_t i = 0; i < words; i++) {
        fifo32(rd32(m + 4 * i));
    }
}

GX_HANDLER(set_viewport_regs, "__GXSetViewport", Ret::None) {
    uint32_t g = gx();
    float left = ldf(g + 0x544);
    float top = ldf(g + 0x548);
    float width = ldf(g + 0x54C);
    float height = ldf(g + 0x550);
    float near = ldf(g + 0x554);
    float far = ldf(g + 0x558);
    float zoffset = ldf(g + 0x55C);
    float zscale = ldf(g + 0x560);
    float sx = width * 0.5f;
    float half_height = height * 0.5f;
    float sy = -height * 0.5f;
    float near_z = near * zscale;
    float far_z = far * zscale;
    float oy = top + half_height;
    float ox = left + sx;
    float sz = far_z - near_z;
    ox = 342.0f + ox;
    oy = 342.0f + oy;
    float oz = far_z + zoffset;
    fifo8(0x10);
    fifo32(0x0005101A);
    fifo_float(sx);
    fifo_float(sy);
    fifo_float(sz);
    fifo_float(ox);
    fifo_float(oy);
    fifo_float(oz);
}

void store_viewport(Cpu& c, float top) {
    uint32_t g = gx();
    stf(g + 0x544, static_cast<float>(c.f[1]));
    stf(g + 0x548, top);
    stf(g + 0x54C, static_cast<float>(c.f[3]));
    stf(g + 0x550, static_cast<float>(c.f[4]));
    stf(g + 0x554, static_cast<float>(c.f[5]));
    stf(g + 0x558, static_cast<float>(c.f[6]));
    wr32(g + kDirtyState, rd32(g + kDirtyState) | 0x10000000);
}

GX_HANDLER(set_viewport_jitter, "GXSetViewportJitter", Ret::None) {
    float top = static_cast<float>(c.f[2]);
    if (c.r[3] == 0) {
        top = top - 0.5f;
    }
    store_viewport(c, top);
}

GX_HANDLER(set_viewport, "GXSetViewport", Ret::None) {
    store_viewport(c, static_cast<float>(c.f[2]));
}

GX_HANDLER(set_scissor, "GXSetScissor", Ret::None) {
    uint32_t left = c.r[3] + 0x156;
    uint32_t top = c.r[4] + 0x156;
    uint32_t g = gx();
    wr32(g + 0x148, rlwimi(rlwimi(rd32(g + 0x148), top, 0, 21, 31), left, 12, 9, 19));
    wr32(g + 0x14C, rlwimi(rlwimi(rd32(g + 0x14C), top + c.r[6] - 1, 0, 21, 31), left + c.r[5] - 1, 12, 9, 19));
    bp(rd32(g + 0x148));
    bp(rd32(g + 0x14C));
    wr16(g + 0x2, 0);
}

GX_HANDLER(set_scissor_box_offset, "GXSetScissorBoxOffset", Ret::None) {
    uint32_t x = c.r[3] + 0x156;
    uint32_t y = c.r[4] + 0x156;
    bp(rlwimi(rlwimi(rlwimi(0, x, 31, 22, 31), y, 9, 12, 21), 0x59, 24, 0, 7));
    wr16(gx() + 0x2, 0);
}

GX_HANDLER(set_clip_mode, "GXSetClipMode", Ret::None) {
    xf(0x1005, c.r[3]);
    wr16(gx() + 0x2, 1);
}

GX_HANDLER(set_matrix_index, "__GXSetMatrixIndex", Ret::None) {
    uint32_t g = gx();
    if (static_cast<int32_t>(c.r[3]) < 5) {
        cp(0x30, rd32(g + 0x80));
        xf(0x1018, rd32(g + 0x80));
    } else {
        cp(0x40, rd32(g + 0x84));
        xf(0x1019, rd32(g + 0x84));
    }
    wr16(g + 0x2, 1);
}

// =====================================================================================
// GXPerf
// =====================================================================================

GX_HANDLER(set_gp_metric, "GXSetGPMetric", Ret::None) {
    static const uint16_t kXfMetric[0x23] = {0x273, 0x14A, 0x16B, 0x84, 0xC6, 0x210, 0x252, 0x231, 0x1AD, 0x1CE, 0x153};
    static const uint32_t kBpMetric[0x23] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                             0x2300AE7F, 0x23008E7F, 0x23009E7F, 0x23001E7F, 0x2300AC3F, 0x2300AC7F, 0x2300ACBF,
                                             0x2300ACFF, 0x2300AD3F, 0x2300AD7F, 0x2300ADBF, 0x2300ADFF, 0x2300AE3F, 0x2300A27F,
                                             0x2300A67F, 0x2300AA7F, 0x2402C0C6, 0x2402C16B, 0x2402C0E7, 0x2402C108, 0x2402C129,
                                             0x2402C14A, 0x2402C1AD};
    static const uint32_t kPerf1Bp[9] = {0x67000042, 0x67000084, 0x67000063, 0x67000129, 0x6700014B,
                                         0x6700018D, 0x670001CF, 0x67000211, 0x67000252};
    static const uint32_t kPerf1Cp[8] = {2, 3, 4, 5, 6, 7, 9, 8};
    uint32_t perf0 = c.r[3];
    uint32_t perf1 = c.r[4];
    uint32_t g = gx();
    // Stop the previous counters.
    uint32_t previous0 = rd32(g + 0x5EC);
    if (previous0 - 0xB <= 0xF) {
        bp(0x23000000);
    } else if (previous0 <= 0xA || previous0 == 0x22) {
        xf(0x1006, 0);
    } else if (previous0 - 0x1B <= 6) {
        bp(0x24000000);
    }
    uint32_t previous1 = rd32(g + 0x5F0);
    if (previous1 <= 8 || previous1 == 0x15) {
        bp(0x67000000);
    } else if (previous1 - 9 <= 7) {
        wr32(g + 0x5F4, rlwinm(rd32(g + 0x5F4), 0, 28, 23));
        cp(0x20, rd32(g + 0x5F4));
    } else if (previous1 - 0x11 <= 3) {
        wr16(cp_reg() + 0x6, 0);
    }
    // Start the new ones.
    wr32(g + 0x5EC, perf0);
    if (perf0 <= 0xA) {
        xf(0x1006, kXfMetric[perf0]);
    } else if (perf0 == 0x22) {
        xf(0x1006, 0x21);
    } else if (perf0 <= 0x21) {
        bp(kBpMetric[perf0]);
    }
    wr32(g + 0x5F0, perf1);
    if (perf1 <= 8) {
        bp(kPerf1Bp[perf1]);
    } else if (perf1 <= 0x10) {
        wr32(g + 0x5F4, rlwimi(rd32(g + 0x5F4), kPerf1Cp[perf1 - 9], 4, 24, 27));
        cp(0x20, rd32(g + 0x5F4));
    } else if (perf1 <= 0x14) {
        wr16(cp_reg() + 0x6, static_cast<uint16_t>(perf1 - 0x11 + 2));
    } else if (perf1 == 0x15) {
        bp(0x67000021);
    }
    wr16(g + 0x2, 0);
}

GX_HANDLER(clear_gp_metric, "GXClearGPMetric", Ret::None) {
    wr16(cp_reg() + 0x4, 4);
}

// =====================================================================================
// GXGeometry
// =====================================================================================

// GXSetCullMode(GXCullMode mode): the hardware's front/back bits are swapped.
GX_HANDLER(set_cull_mode, "GXSetCullMode", Ret::None) {
    uint32_t mode = c.r[3];
    uint32_t hw = ((mode >> 1) & 1) | ((mode & 1) << 1);
    st32(0x254, field(ld32(0x254), hw, 14, 2));
    dirty(0x4);
}

// --- self-test ------------------------------------------------------------------------
// WP_GX_SELFTEST=1: once the game is drawing, call the GX functions the game rarely uses with
// random arguments and compare each with the original (memory is rolled back after every trial).

struct Random {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state >> 16);
    }
    uint32_t below(uint32_t n) {
        return next() % n;
    }
    float real() {
        static const float kSpecial[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 1000.0f, 1e-30f, 3.99f, 4.0f, 10.0f, 90.0f};
        if (below(4) == 0) {
            return kSpecial[below(sizeof kSpecial / sizeof kSpecial[0])];
        }
        return (static_cast<float>(next()) / 4294967296.0f - 0.5f) * (below(2) ? 4.0f : 400.0f);
    }
};

constexpr uint32_t kScratch = 0x93F00000;  // MEM2 scratch for pointer arguments (rolled back)

void fill_scratch(Random& rng, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 4) {
        wr32(kScratch + i, rng.next());
    }
}

void fill_floats(Random& rng, uint32_t address, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        stf(address + 4 * i, rng.real());
    }
}

struct SelfTest {
    const char* name;
    void (*native)(Cpu&);
    Ret ret;
    void (*setup)(Cpu&, Random&);
};

void args(Cpu& c, std::initializer_list<uint32_t> ints) {
    int i = 3;
    for (uint32_t value : ints) {
        c.r[i++] = value;
    }
}
void float_args(Cpu& c, Random& rng, int count) {
    for (int i = 1; i <= count; i++) {
        c.f[i] = rng.real();
    }
}

void run_self_test(Cpu& c) {
    static const uint32_t kTexFormats[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14, 0x11, 0x13, 0x16, 0x20, 0x22, 0x23, 0x24, 0x26, 0x27,
                                           0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x30, 0x39, 0x3A, 0x3C};
    const SelfTest tests[] = {
        {"GXSetVtxAttrFmt", set_vtx_attr_fmt, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(8), 9 + r.below(17), r.below(2), r.below(5), r.below(32)}); }},
        {"__GXSendFlushPrim", send_flush_prim_handler, Ret::None, [](Cpu&, Random& r) { wr16(gx() + 4, static_cast<uint16_t>(r.below(8))); wr16(gx() + 6, static_cast<uint16_t>(r.below(40))); }},
        {"GXSetTexCopySrc", set_tex_copy_src, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(640), r.below(528), 1 + r.below(640), 1 + r.below(528)}); }},
        {"GXSetTexCopyDst", set_tex_copy_dst, Ret::None, [](Cpu& c, Random& r) { args(c, {1 + r.below(640), 1 + r.below(528), kTexFormats[r.below(sizeof kTexFormats / 4)], r.below(2)}); }},
        {"GXCopyTex", copy_tex, Ret::None, [](Cpu& c, Random& r) {
             wr8(gx() + 0x250, static_cast<uint8_t>(r.below(2)));
             wr32(gx() + 0x22C, r.next());
             args(c, {0x80100000 + 32 * r.below(1000), r.below(2)});
         }},
        {"GXCopyDisp", copy_disp, Ret::None, [](Cpu& c, Random& r) { wr32(gx() + 0x22C, r.next()); args(c, {0x80100000 + 32 * r.below(1000), r.below(2)}); }},
        {"GXInitLightAttn", init_light_attn, Ret::None, [](Cpu& c, Random& r) { args(c, {kScratch}); float_args(c, r, 6); }},
        {"GXInitLightDir", init_light_dir, Ret::None, [](Cpu& c, Random& r) { args(c, {kScratch}); float_args(c, r, 3); }},
        {"GXInitSpecularDir", init_specular_dir, Ret::None, [](Cpu& c, Random& r) { args(c, {kScratch}); float_args(c, r, 3); }},
        {"GXInitLightSpot", init_light_spot, Ret::None, [](Cpu& c, Random& r) { args(c, {kScratch, r.below(8)}); float_args(c, r, 1); }},
        {"GXInitLightDistAttn", init_light_dist_attn, Ret::None, [](Cpu& c, Random& r) { args(c, {kScratch, r.below(5)}); float_args(c, r, 2); }},
        {"__GetImageTileCount", get_image_tile_count, Ret::None, [](Cpu& c, Random& r) {
             args(c, {kTexFormats[r.below(sizeof kTexFormats / 4)], r.below(1024), r.below(1024), kScratch, kScratch + 4, kScratch + 8});
         }},
        {"GXGetTexBufferSize", get_tex_buffer_size, Ret::Int, [](Cpu& c, Random& r) {
             args(c, {1 + r.below(1024), 1 + r.below(1024), kTexFormats[r.below(sizeof kTexFormats / 4)], r.below(2), r.below(12)});
         }},
        {"GXInitTexObjTlut", init_tex_obj_tlut, Ret::None, [](Cpu& c, Random& r) { args(c, {kScratch, r.next()}); }},
        {"GXInitTexObjUserData", init_tex_obj_user_data, Ret::None, [](Cpu& c, Random& r) { args(c, {kScratch, r.next()}); }},
        {"GXGetTexObjUserData", get_tex_obj_user_data, Ret::Int, [](Cpu& c, Random& r) { fill_scratch(r, 0x20); args(c, {kScratch}); }},
        {"GXGetTexObjWidth", get_tex_obj_width, Ret::Int, [](Cpu& c, Random& r) { fill_scratch(r, 0x20); args(c, {kScratch}); }},
        {"GXGetTexObjHeight", get_tex_obj_height, Ret::Int, [](Cpu& c, Random& r) { fill_scratch(r, 0x20); args(c, {kScratch}); }},
        {"GXGetTexObjWrapS", get_tex_obj_wrap_s, Ret::Int, [](Cpu& c, Random& r) { fill_scratch(r, 0x20); args(c, {kScratch}); }},
        {"GXGetTexObjWrapT", get_tex_obj_wrap_t, Ret::Int, [](Cpu& c, Random& r) { fill_scratch(r, 0x20); args(c, {kScratch}); }},
        {"GXInitTexObj", init_tex_obj, Ret::None, [](Cpu& c, Random& r) {
             args(c, {kScratch, 0x80100000 + 32 * r.below(1000), 1 + r.below(1024), 1 + r.below(1024), kTexFormats[r.below(sizeof kTexFormats / 4)], r.below(3), r.below(3), r.below(2)});
         }},
        {"GXInitTexObjLOD", init_tex_obj_lod, Ret::None, [](Cpu& c, Random& r) {
             fill_scratch(r, 0x20);
             args(c, {kScratch, r.below(6), r.below(2), r.below(2), r.below(2), r.below(3)});
             float_args(c, r, 3);
         }},
        {"GXSetTevIndirect", set_tev_indirect, Ret::None, [](Cpu& c, Random& r) {
             args(c, {r.below(16), r.below(4), r.below(4), r.below(8), r.below(16), r.below(7), r.below(7), r.below(2)});
             wr32(c.r[1] + 8, r.below(2));
             wr32(c.r[1] + 0xC, r.below(4));
         }},
        {"GXSetIndTexMtx", set_ind_tex_mtx, Ret::None, [](Cpu& c, Random& r) {
             fill_floats(r, kScratch, 6);
             args(c, {r.below(12), kScratch, static_cast<uint32_t>(static_cast<int32_t>(r.below(64)) - 17)});
         }},
        {"GXSetIndTexOrder", set_ind_tex_order, Ret::None, [](Cpu& c, Random& r) {
             args(c, {r.below(5), r.below(3) ? r.below(8) : 0xFF, r.below(3) ? r.below(8) : 0xFF});
         }},
        {"GXSetIndTexCoordScale", set_ind_tex_coord_scale, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(5), r.below(9), r.below(9)}); }},
        {"GXSetTevColor", set_tev_color, Ret::None, [](Cpu& c, Random& r) { fill_scratch(r, 8); args(c, {r.below(4), kScratch}); }},
        {"GXSetTevColorS10", set_tev_color_s10, Ret::None, [](Cpu& c, Random& r) { fill_scratch(r, 8); args(c, {r.below(4), kScratch}); }},
        {"GXSetTevKColor", set_tev_k_color, Ret::None, [](Cpu& c, Random& r) { fill_scratch(r, 8); args(c, {r.below(4), kScratch}); }},
        {"GXSetTevColorOp", set_tev_color_op, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(16), r.below(16), r.below(4), r.below(4), r.below(2), r.below(4)}); }},
        {"GXSetTevAlphaOp", set_tev_alpha_op, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(16), r.below(16), r.below(4), r.below(4), r.below(2), r.below(4)}); }},
        {"GXSetTevOrder", set_tev_order, Ret::None, [](Cpu& c, Random& r) {
             static const uint32_t kMaps[] = {0, 3, 7, 0xFF, 0x100, 0x103};
             args(c, {r.below(16), r.below(3) ? r.below(8) : 0xFF, kMaps[r.below(6)], r.below(3) ? r.below(10) : 0xFF});
         }},
        {"GXSetZTexture", set_z_texture, Ret::None, [](Cpu& c, Random& r) {
             static const uint32_t kFormats[] = {0x11, 0x13, 0x16, 0};
             args(c, {r.below(3), kFormats[r.below(4)], r.next()});
         }},
        {"GXSetFog", set_fog, Ret::None, [](Cpu& c, Random& r) {
             fill_scratch(r, 4);
             args(c, {r.below(16), kScratch});
             float_args(c, r, 4);
             if (r.below(3) == 0) {
                 c.f[4] = c.f[3];
             }
         }},
        {"GXSetFogRangeAdj", set_fog_range_adj, Ret::None, [](Cpu& c, Random& r) { fill_scratch(r, 0x20); args(c, {r.below(2), r.below(640), kScratch}); }},
        {"GXSetPixelFmt", set_pixel_fmt, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(8), r.below(8)}); }},
        {"GXSetViewportJitter", set_viewport_jitter, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(2)}); float_args(c, r, 6); }},
        {"__GXSetViewport", set_viewport_regs, Ret::None, [](Cpu&, Random& r) { fill_floats(r, gx() + 0x544, 6); }},
        {"GXSetProjection", set_projection, Ret::None, [](Cpu& c, Random& r) { fill_floats(r, kScratch, 16); args(c, {kScratch, r.below(2)}); }},
        {"GXSetProjectionv", set_projectionv, Ret::None, [](Cpu& c, Random& r) { fill_floats(r, kScratch, 7); args(c, {kScratch}); }},
        {"GXLoadTexMtxImm", load_tex_mtx_imm, Ret::None, [](Cpu& c, Random& r) {
             fill_floats(r, kScratch, 12);
             args(c, {kScratch, r.below(2) ? 0x40 + 3 * r.below(21) : 3 * r.below(10), r.below(2)});
         }},
        {"GXGetYScaleFactor", get_y_scale_factor, Ret::Float, [](Cpu& c, Random& r) {
             uint32_t efb = 100 + r.below(429);  // as callers use it: xfb between efb and 2 * efb
             args(c, {efb, std::min(efb + r.below(efb + 1), 1000u)});
         }},
        {"GXGetNumXfbLines", get_num_xfb_lines, Ret::Int, [](Cpu& c, Random& r) { args(c, {1 + r.below(528)}); c.f[1] = 0.25f + r.below(400) / 100.0f; }},
        {"GXAdjustForOverscan", adjust_for_overscan, Ret::None, [](Cpu& c, Random& r) { fill_scratch(r, 0x40); args(c, {kScratch, kScratch + 0x80, r.below(32), r.below(32)}); }},
        {"GXSetGPMetric", set_gp_metric, Ret::None, [](Cpu& c, Random& r) {
             wr32(gx() + 0x5EC, r.below(0x24));
             wr32(gx() + 0x5F0, r.below(0x17));
             args(c, {r.below(0x24), r.below(0x17)});
         }},
        {"GXPixModeSync", pix_mode_sync, Ret::None, [](Cpu&, Random&) {}},
        {"__GXIsGPFifoReady", is_gp_fifo_ready, Ret::Int, [](Cpu&, Random&) {}},
        {"GXSetBreakPtCallback", set_break_pt_callback, Ret::Int, [](Cpu& c, Random& r) { args(c, {r.next()}); }},
        {"GXSetDrawDoneCallback", set_draw_done_callback, Ret::Int, [](Cpu& c, Random& r) { args(c, {r.next()}); }},
        {"__GXCleanGPFifo", clean_gp_fifo, Ret::None, [](Cpu&, Random&) {}},
        {"__GXShutdown", shutdown, Ret::Int, [](Cpu& c, Random&) { args(c, {0}); }},
        {"__SetSURegs", set_su_regs, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(8), r.below(8)}); }},
        {"GXLoadLightObjImm", load_light_obj_imm, Ret::None, [](Cpu& c, Random& r) { fill_floats(r, kScratch, 16); args(c, {kScratch, 1u << r.below(8)}); }},
        {"GXSetChanCtrl", set_chan_ctrl, Ret::None, [](Cpu& c, Random& r) { args(c, {r.below(6), r.below(2), r.below(2), r.below(2), r.below(256), r.below(3), r.below(3)}); }},
    };
    Random rng;
    uint64_t before = lift::mismatches();
    lift::use_self_test_ranges(true, kScratch, 0x1000);
    for (const SelfTest& test : tests) {
        std::fprintf(stderr, "GX self-test: %s\n", test.name);
        for (int trial = 0; trial < 100; trial++) {
            if (std::getenv("WP_GX_SELFTEST_TRACE")) {
                std::fprintf(stderr, "  trial %d\n", trial);
            }
            GuestFrame frame(c, 0x40);
            Cpu saved = c;
            // The setup writes into the GX state and the scratch area; undo them afterwards.
            std::vector<uint8_t> state(0x600);
            std::vector<uint8_t> scratch(0x100);
            std::memcpy(state.data(), host(gx()), state.size());
            std::memcpy(scratch.data(), host(kScratch), scratch.size());
            test.setup(c, rng);
            verify(c, test.name, test.native, test.ret, false);
            std::memcpy(host(gx()), state.data(), state.size());
            std::memcpy(host(kScratch), scratch.data(), scratch.size());
            c = saved;
        }
    }
    lift::use_self_test_ranges(false, 0, 0);
    std::fprintf(stderr, "GX self-test: %zu functions x 100 trials, %llu mismatches\n", sizeof tests / sizeof tests[0],
                 static_cast<unsigned long long>(lift::mismatches() - before));
}

}  // namespace

bool enabled() {
    return kGroup.enabled();
}

void print_report() {
    lift::print_report();
}

const input_hle::Handler kHandlers[] = {
    {"__GXInvokeCallback", invoke_callback},
    {"__GXDefaultTexRegionCallback", default_tex_region_callback},
    {"__GXDefaultTlutRegionCallback", default_tlut_region_callback},
    {"__GXShutdown", shutdown},
    {"__GXInitRevisionBits", init_revision_bits},
    {"GXInit", init},
    {"__GXInitGX", init_gx},
    {"__GXCPInterruptHandler", cp_interrupt_handler},
    {"GXInitFifoBase", init_fifo_base},
    {"CPGPLinkCheck", cpgp_link_check},
    {"GXSetCPUFifo", set_cpu_fifo},
    {"GXSetGPFifo", set_gp_fifo},
    {"__GXIsGPFifoReady", is_gp_fifo_ready},
    {"GXSetBreakPtCallback", set_break_pt_callback},
    {"__GXFifoInit", fifo_init},
    {"__GXCleanGPFifo", clean_gp_fifo},
    {"GXSetVtxDesc", set_vtx_desc},
    {"GXSetVtxDescv", set_vtx_descv},
    {"__GXSetVCD", set_vcd},
    {"__GXCalculateVLim", calculate_vlim},
    {"GXClearVtxDesc", clear_vtx_desc},
    {"GXSetVtxAttrFmt", set_vtx_attr_fmt},
    {"GXSetVtxAttrFmtv", set_vtx_attr_fmtv},
    {"__GXSetVAT", set_vat},
    {"GXSetArray", set_array},
    {"GXInvalidateVtxCache", invalidate_vtx_cache},
    {"GXSetTexCoordGen2", set_tex_coord_gen2},
    {"GXSetNumTexGens", set_num_tex_gens},
    {"GXSetMisc", set_misc},
    {"__GXAbort", abort},
    {"GXAbortFrame", abort_frame},
    {"GXSetDrawSync", set_draw_sync},
    {"GXDrawDone", draw_done},
    {"GXPixModeSync", pix_mode_sync},
    {"GXPokeAlphaMode", poke_alpha_mode},
    {"GXPokeAlphaRead", poke_alpha_read},
    {"GXPokeAlphaUpdate", poke_alpha_update},
    {"GXPokeBlendMode", poke_blend_mode},
    {"GXPokeColorUpdate", poke_color_update},
    {"GXPokeDstAlpha", poke_dst_alpha},
    {"GXPokeDither", poke_dither},
    {"GXPokeZMode", poke_z_mode},
    {"GXSetDrawSyncCallback", set_draw_sync_callback},
    {"GXTokenInterruptHandler", token_interrupt_handler},
    {"GXSetDrawDoneCallback", set_draw_done_callback},
    {"GXFinishInterruptHandler", finish_interrupt_handler},
    {"__GXPEInit", pe_init},
    {"__GXSetDirtyState", set_dirty_state},
    {"GXBegin", begin},
    {"__GXSendFlushPrim", send_flush_prim_handler},
    {"GXSetLineWidth", set_line_width},
    {"GXSetPointSize", set_point_size},
    {"GXEnableTexOffsets", enable_tex_offsets},
    {"GXSetCoPlanar", set_coplanar},
    {"__GXSetGenMode", set_gen_mode},
    {"GXAdjustForOverscan", adjust_for_overscan},
    {"GXSetDispCopySrc", set_disp_copy_src},
    {"GXSetTexCopySrc", set_tex_copy_src},
    {"GXSetDispCopyDst", set_disp_copy_dst},
    {"GXSetTexCopyDst", set_tex_copy_dst},
    {"GXSetDispCopyFrame2Field", set_disp_copy_frame2field},
    {"GXSetCopyClamp", set_copy_clamp},
    {"GXGetNumXfbLines", get_num_xfb_lines},
    {"GXGetYScaleFactor", get_y_scale_factor},
    {"GXSetDispCopyYScale", set_disp_copy_y_scale},
    {"GXSetCopyClear", set_copy_clear},
    {"GXSetCopyFilter", set_copy_filter},
    {"GXSetDispCopyGamma", set_disp_copy_gamma},
    {"GXCopyDisp", copy_disp},
    {"GXCopyTex", copy_tex},
    {"GXClearBoundingBox", clear_bounding_box},
    {"GXInitLightAttn", init_light_attn},
    {"GXInitLightSpot", init_light_spot},
    {"GXInitLightDistAttn", init_light_dist_attn},
    {"GXInitLightPos", init_light_pos},
    {"GXInitLightDir", init_light_dir},
    {"GXInitSpecularDir", init_specular_dir},
    {"GXInitLightColor", init_light_color},
    {"GXLoadLightObjImm", load_light_obj_imm},
    {"GXSetChanAmbColor", set_chan_amb_color},
    {"GXSetChanMatColor", set_chan_mat_color},
    {"GXSetNumChans", set_num_chans},
    {"GXSetChanCtrl", set_chan_ctrl},
    {"GXGetTexBufferSize", get_tex_buffer_size},
    {"__GetImageTileCount", get_image_tile_count},
    {"GXInitTexObj", init_tex_obj},
    {"GXInitTexObjCI", init_tex_obj_ci},
    {"GXInitTexObjLOD", init_tex_obj_lod},
    {"GXInitTexObjWrapMode", init_tex_obj_wrap_mode},
    {"GXInitTexObjTlut", init_tex_obj_tlut},
    {"GXInitTexObjUserData", init_tex_obj_user_data},
    {"GXGetTexObjUserData", get_tex_obj_user_data},
    {"GXGetTexObjWidth", get_tex_obj_width},
    {"GXGetTexObjHeight", get_tex_obj_height},
    {"GXGetTexObjFmt", get_tex_obj_fmt},
    {"GXGetTexObjWrapS", get_tex_obj_wrap_s},
    {"GXGetTexObjWrapT", get_tex_obj_wrap_t},
    {"GXGetTexObjMipMap", get_tex_obj_mipmap},
    {"GXLoadTexObjPreLoaded", load_tex_obj_preloaded},
    {"GXLoadTexObj", load_tex_obj},
    {"GXInitTlutObj", init_tlut_obj},
    {"GXLoadTlut", load_tlut},
    {"GXInitTexCacheRegion", init_tex_cache_region},
    {"GXInitTlutRegion", init_tlut_region},
    {"GXInvalidateTexAll", invalidate_tex_all},
    {"GXSetTexRegionCallback", set_tex_region_callback},
    {"GXSetTlutRegionCallback", set_tlut_region_callback},
    {"GXSetTexCoordScaleManually", set_tex_coord_scale_manually},
    {"GXSetTexCoordCylWrap", set_tex_coord_cyl_wrap},
    {"GXSetTexCoordBias", set_tex_coord_bias},
    {"__SetSURegs", set_su_regs},
    {"__GXSetSUTexRegs", set_su_tex_regs},
    {"__GXSetTmemConfig", set_tmem_config},
    {"GXSetTevIndirect", set_tev_indirect},
    {"GXSetIndTexMtx", set_ind_tex_mtx},
    {"GXSetIndTexCoordScale", set_ind_tex_coord_scale},
    {"GXSetIndTexOrder", set_ind_tex_order},
    {"GXSetNumIndStages", set_num_ind_stages},
    {"GXSetTevDirect", set_tev_direct},
    {"__GXUpdateBPMask", update_bp_mask},
    {"__GXSetIndirectMask", set_indirect_mask},
    {"__GXFlushTextureState", flush_texture_state},
    {"GXSetTevOp", set_tev_op},
    {"GXSetTevColorIn", set_tev_color_in},
    {"GXSetTevAlphaIn", set_tev_alpha_in},
    {"GXSetTevColorOp", set_tev_color_op},
    {"GXSetTevAlphaOp", set_tev_alpha_op},
    {"GXSetTevColor", set_tev_color},
    {"GXSetTevColorS10", set_tev_color_s10},
    {"GXSetTevKColor", set_tev_k_color},
    {"GXSetTevKColorSel", set_tev_k_color_sel},
    {"GXSetTevKAlphaSel", set_tev_k_alpha_sel},
    {"GXSetTevSwapMode", set_tev_swap_mode},
    {"GXSetTevSwapModeTable", set_tev_swap_mode_table},
    {"GXSetAlphaCompare", set_alpha_compare},
    {"GXSetZTexture", set_z_texture},
    {"GXSetTevOrder", set_tev_order},
    {"GXSetNumTevStages", set_num_tev_stages},
    {"GXSetFog", set_fog},
    {"GXSetFogRangeAdj", set_fog_range_adj},
    {"GXSetBlendMode", set_blend_mode},
    {"GXSetColorUpdate", set_color_update},
    {"GXSetAlphaUpdate", set_alpha_update},
    {"GXSetZMode", set_z_mode},
    {"GXSetZCompLoc", set_z_comp_loc},
    {"GXSetPixelFmt", set_pixel_fmt},
    {"GXSetDither", set_dither},
    {"GXSetDstAlpha", set_dst_alpha},
    {"GXSetFieldMask", set_field_mask},
    {"GXSetFieldMode", set_field_mode},
    {"GXCallDisplayList", call_display_list},
    {"__GXSetProjection", set_projection_regs},
    {"GXSetProjection", set_projection},
    {"GXSetProjectionv", set_projectionv},
    {"GXLoadPosMtxImm", load_pos_mtx_imm},
    {"GXLoadNrmMtxImm", load_nrm_mtx_imm},
    {"GXSetCurrentMtx", set_current_mtx},
    {"GXLoadTexMtxImm", load_tex_mtx_imm},
    {"__GXSetViewport", set_viewport_regs},
    {"GXSetViewportJitter", set_viewport_jitter},
    {"GXSetViewport", set_viewport},
    {"GXSetScissor", set_scissor},
    {"GXSetScissorBoxOffset", set_scissor_box_offset},
    {"GXSetClipMode", set_clip_mode},
    {"__GXSetMatrixIndex", set_matrix_index},
    {"GXSetGPMetric", set_gp_metric},
    {"GXClearGPMetric", clear_gp_metric},
    {"GXSetCullMode", set_cull_mode},
    {nullptr, nullptr},
};

}  // namespace wp::gx_api
