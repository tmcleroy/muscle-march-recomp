#include "wp/deterministic.h"
#include "wp/gx.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "wp/gx_lighting.h"
#include "wp/gx_render.h"
#include "wp/gx_state.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/profile.h"
#include "wp/settings.h"
#include "wp/video.h"

namespace wp::gx {

namespace {

constexpr size_t kFlushThreshold = 1u << 20;
constexpr uint8_t kCommandNop = 0x00;
constexpr uint8_t kCommandLoadCp = 0x08;
constexpr uint8_t kCommandLoadXf = 0x10;
constexpr uint8_t kCommandCallList = 0x40;
constexpr uint8_t kCommandMetrics = 0x44;
constexpr uint8_t kCommandInvalidate = 0x48;
constexpr uint8_t kCommandLoadBp = 0x61;
constexpr uint8_t kCommandDrawMask = 0x80;
constexpr uint32_t kBpMask = 0xFE;
constexpr uint32_t kBpCopyExecute = 0x52;
constexpr uint32_t kBpLoadTlut = 0x65;
constexpr uint32_t kBpInvalidateTexture = 0x66;
constexpr uint32_t kCpFifoBaseLow = 0xCC000020;
constexpr uint32_t kCpFifoBaseHigh = 0xCC000022;
constexpr uint32_t kPiFifoBase = 0xCC00300C;
constexpr uint32_t kPiFifoWritePointer = 0xCC003014;
constexpr uint32_t kFifoPhysicalMask = 0x03FFFFFF;
constexpr uint32_t kCopyToFramebuffer = 1u << 14;
constexpr uint32_t kBpDrawDone = 0x45;
constexpr uint32_t kCopyClear = 1u << 11;
constexpr uint32_t kPixelFormatRgba6Z24 = 1;
constexpr uint32_t kPixelFormatZ24 = 3;
constexpr uint32_t kXfSize = 0x1100;
constexpr uint32_t kArrayCount = 16;
constexpr uint32_t kRamBase = 0x80000000;
constexpr uint32_t kArrayAddressMask = 0x1FFFFFFF;
constexpr float kViewportOffset = 342.0f;
constexpr float kEfbWidth = 640.0f;
constexpr float kEfbHeight = 528.0f;
constexpr float kDepthRange = 16777215.0f;
constexpr int kMaxLoggedCopies = 60;
constexpr uint32_t kTexCoordCount = 8;
constexpr size_t kArrayReadMargin = 256;
constexpr double kSlowFrameMs = 40.0;

enum Attribute : uint32_t { kNone = 0, kDirect = 1, kIndex8 = 2, kIndex16 = 3 };

struct Statistics {
    uint32_t draws = 0;
    uint32_t vertices = 0;
    uint32_t bp_writes = 0;
    uint32_t cp_writes = 0;
    uint32_t xf_words = 0;
    uint32_t lists = 0;
    uint32_t unknown = 0;
};

struct Element {
    uint32_t mode = kNone;
    uint32_t components = 0;
    uint32_t format = 0;
    uint32_t shift = 0;
    uint32_t direct_size = 0;
};

struct Layout {
    bool position_matrix = false;
    uint32_t texture_matrices = 0;
    Element position;
    Element normal;
    Element color[2];
    Element texture[kTexCoordCount];
    uint32_t size = 0;
};

struct Vertex {
    float position[3] = {0, 0, 0};
    float normal[3] = {0, 0, 1};
    float color[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}};
    float texture[kTexCoordCount][2] = {};
    uint32_t position_matrix = 0;
    uint32_t texture_matrix[kTexCoordCount] = {};
    bool has_color[2] = {false, false};
};

std::vector<uint8_t> g_fifo;
uint32_t g_vcd_low = 0;
uint32_t g_vcd_high = 0;
uint32_t g_vat[3][8] = {};
uint32_t g_array_base[kArrayCount] = {};
uint32_t g_array_stride[kArrayCount] = {};
uint32_t g_bp[256] = {};
uint32_t g_konst[8] = {};
uint32_t g_bp_mask = 0xFFFFFF;
uint32_t g_xf[kXfSize] = {};
Statistics g_stats;
int g_logged_copies = 0;
int g_copy_total = 0;
std::atomic<uint64_t> g_frames_drawn{0};
int g_log_from = std::getenv("WP_LOG_FROM") ? std::atoi(std::getenv("WP_LOG_FROM")) : 0;
const char* g_log_level = std::getenv("WP_LOG_GX");
bool g_log = g_log_level != nullptr;
bool g_log_draws = g_log_level != nullptr && g_log_level[0] == '2';
int g_logged_draws = 0;
int g_logged_prepared = 0;
bool g_render_enabled = std::getenv("WP_NO_RENDER") == nullptr;
bool g_log_fps = std::getenv("WP_LOG_FPS") != nullptr;
int g_frames = 0;
std::chrono::steady_clock::time_point g_fps_start = std::chrono::steady_clock::now();
std::atomic<bool> g_capture_requested{false};
FILE* g_capture = nullptr;
int g_capture_index = 0;
int g_capture_draws = 0;
bool g_capture_first = true;
uint32_t g_captured_bp[256] = {};
uint32_t g_captured_konst[8] = {};
uint32_t g_captured_xf[kXfSize] = {};
char g_capture_name[64] = {};

uint32_t be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint32_t be16(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 8) | data[1];
}

float bits_to_float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

float xf_float(uint32_t index) {
    return bits_to_float(g_xf[index]);
}

uint32_t component_size(uint32_t format) {
    switch (format) {
    case 0:
    case 1:
        return 1;
    case 2:
    case 3:
        return 2;
    default:
        return 4;
    }
}

uint32_t color_size(uint32_t format) {
    static const uint32_t kSizes[] = {2, 3, 4, 2, 3, 4, 4, 4};
    return kSizes[format & 7];
}

uint32_t element_size(const Element& element) {
    switch (element.mode) {
    case kDirect:
        return element.direct_size;
    case kIndex8:
        return 1;
    case kIndex16:
        return 2;
    default:
        return 0;
    }
}

Layout build_layout(uint32_t vat) {
    Layout layout;
    uint32_t a = g_vat[0][vat];
    uint32_t b = g_vat[1][vat];
    uint32_t c = g_vat[2][vat];
    layout.position_matrix = (g_vcd_low & 1) != 0;
    layout.texture_matrices = (g_vcd_low >> 1) & 0xFF;
    layout.position.mode = (g_vcd_low >> 9) & 3;
    layout.position.components = (a & 1) ? 3 : 2;
    layout.position.format = (a >> 1) & 7;
    layout.position.shift = (a >> 4) & 31;
    layout.position.direct_size = layout.position.components * component_size(layout.position.format);
    layout.normal.mode = (g_vcd_low >> 11) & 3;
    layout.normal.components = (a & 0x200) ? 9 : 3;
    layout.normal.format = (a >> 10) & 7;
    layout.normal.direct_size = layout.normal.components * component_size(layout.normal.format);
    layout.color[0].mode = (g_vcd_low >> 13) & 3;
    layout.color[0].components = ((a >> 13) & 1) ? 4 : 3;
    layout.color[0].format = (a >> 14) & 7;
    layout.color[0].direct_size = color_size(layout.color[0].format);
    layout.color[1].mode = (g_vcd_low >> 15) & 3;
    layout.color[1].components = ((a >> 17) & 1) ? 4 : 3;
    layout.color[1].format = (a >> 18) & 7;
    layout.color[1].direct_size = color_size(layout.color[1].format);
    uint32_t counts[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    uint32_t formats[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    uint32_t shifts[8] = {(a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31};
    for (uint32_t i = 0; i < kTexCoordCount; i++) {
        Element& element = layout.texture[i];
        element.mode = (g_vcd_high >> (2 * i)) & 3;
        element.components = counts[i] + 1;
        element.format = formats[i];
        element.shift = shifts[i];
        element.direct_size = element.components * component_size(element.format);
    }
    uint32_t size = layout.position_matrix ? 1 : 0;
    size += __builtin_popcount(layout.texture_matrices);
    size += element_size(layout.position);
    size += element_size(layout.normal);
    size += element_size(layout.color[0]);
    size += element_size(layout.color[1]);
    for (const Element& element : layout.texture) {
        size += element_size(element);
    }
    layout.size = size;
    return layout;
}

const std::array<float, 32> kShiftScale = [] {
    std::array<float, 32> scale{};
    for (int i = 0; i < 32; i++) {
        scale[i] = std::ldexp(1.0f, -i);
    }
    return scale;
}();

struct CachedLayout {
    uint32_t key[5] = {};
    bool valid = false;
    Layout layout;
};

const Layout& make_layout(uint32_t vat) {
    static CachedLayout cache[8];
    CachedLayout& entry = cache[vat];
    uint32_t key[5] = {g_vat[0][vat], g_vat[1][vat], g_vat[2][vat], g_vcd_low, g_vcd_high};
    if (!entry.valid || std::memcmp(entry.key, key, sizeof key) != 0) {
        std::memcpy(entry.key, key, sizeof key);
        entry.layout = build_layout(vat);
        entry.valid = true;
    }
    return entry.layout;
}

float read_number(const uint8_t* p, uint32_t format, uint32_t shift) {
    float value;
    switch (format) {
    case 0:
        value = static_cast<float>(p[0]);
        break;
    case 1:
        value = static_cast<float>(static_cast<int8_t>(p[0]));
        break;
    case 2:
        value = static_cast<float>(be16(p));
        break;
    case 3:
        value = static_cast<float>(static_cast<int16_t>(be16(p)));
        break;
    default:
        return bits_to_float(be32(p));
    }
    return value * kShiftScale[shift];
}

float read_normal_component(const uint8_t* p, uint32_t format) {
    switch (format) {
    case 0:
        return static_cast<float>(p[0]) * kShiftScale[7];
    case 1:
        return static_cast<float>(static_cast<int8_t>(p[0])) * kShiftScale[6];
    case 2:
        return static_cast<float>(be16(p)) * kShiftScale[15];
    case 3:
        return static_cast<float>(static_cast<int16_t>(be16(p))) * kShiftScale[14];
    default:
        return bits_to_float(be32(p));
    }
}

void read_color(const uint8_t* p, uint32_t format, float* out) {
    float r = 1, g = 1, b = 1, a = 1;
    switch (format) {
    case 0: {
        uint32_t v = be16(p);
        r = ((v >> 11) & 31) / 31.0f;
        g = ((v >> 5) & 63) / 63.0f;
        b = (v & 31) / 31.0f;
        break;
    }
    case 1:
    case 2:
        r = p[0] / 255.0f;
        g = p[1] / 255.0f;
        b = p[2] / 255.0f;
        break;
    case 3: {
        uint32_t v = be16(p);
        r = ((v >> 12) & 15) / 15.0f;
        g = ((v >> 8) & 15) / 15.0f;
        b = ((v >> 4) & 15) / 15.0f;
        a = (v & 15) / 15.0f;
        break;
    }
    case 4: {
        uint32_t v = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
        r = ((v >> 18) & 63) / 63.0f;
        g = ((v >> 12) & 63) / 63.0f;
        b = ((v >> 6) & 63) / 63.0f;
        a = (v & 63) / 63.0f;
        break;
    }
    default:
        r = p[0] / 255.0f;
        g = p[1] / 255.0f;
        b = p[2] / 255.0f;
        a = p[3] / 255.0f;
        break;
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
}

const uint8_t* array_element(uint32_t array, uint32_t index) {
    uint32_t address = kRamBase | (g_array_base[array] & kArrayAddressMask);
    size_t offset = static_cast<size_t>(index) * g_array_stride[array];
    if (!render::guest_range_valid(address + offset, kArrayReadMargin)) {
        static const uint8_t zeros[kArrayReadMargin] = {};
        return zeros;
    }
    return host(address) + offset;
}

const uint8_t* fetch(const Element& element, uint32_t array, const uint8_t*& stream) {
    if (element.mode == kDirect) {
        const uint8_t* p = stream;
        stream += element.direct_size;
        return p;
    }
    uint32_t index;
    if (element.mode == kIndex8) {
        index = stream[0];
        stream += 1;
    } else {
        index = be16(stream);
        stream += 2;
    }
    return array_element(array, index);
}

Vertex decode_vertex(const Layout& layout, const uint8_t*& stream) {
    Vertex vertex;
    vertex.position_matrix = g_xf[0x1018] & 0x3F;
    if (layout.position_matrix) {
        vertex.position_matrix = stream[0] & 0x3F;
        stream++;
    }
    for (uint32_t i = 0; i < kTexCoordCount; i++) {
        vertex.texture_matrix[i] = i < 4 ? (g_xf[0x1018] >> (6 + 6 * i)) & 0x3F : (g_xf[0x1019] >> (6 * (i - 4))) & 0x3F;
        if (layout.texture_matrices & (1u << i)) {
            vertex.texture_matrix[i] = stream[0] & 0x3F;
            stream++;
        }
    }
    if (layout.position.mode != kNone) {
        const uint8_t* p = fetch(layout.position, 0, stream);
        uint32_t step = component_size(layout.position.format);
        for (uint32_t i = 0; i < layout.position.components; i++) {
            vertex.position[i] = read_number(p + i * step, layout.position.format, layout.position.shift);
        }
    }
    if (layout.normal.mode != kNone) {
        const uint8_t* p = fetch(layout.normal, 1, stream);
        uint32_t step = component_size(layout.normal.format);
        vertex.normal[0] = read_normal_component(p, layout.normal.format);
        vertex.normal[1] = read_normal_component(p + step, layout.normal.format);
        vertex.normal[2] = read_normal_component(p + 2 * step, layout.normal.format);
    }
    for (uint32_t k = 0; k < 2; k++) {
        if (layout.color[k].mode != kNone) {
            const uint8_t* p = fetch(layout.color[k], 2 + k, stream);
            read_color(p, layout.color[k].format, vertex.color[k]);
            vertex.has_color[k] = true;
        }
    }
    for (uint32_t k = 0; k < kTexCoordCount; k++) {
        const Element& element = layout.texture[k];
        if (element.mode != kNone) {
            const uint8_t* p = fetch(element, 4 + k, stream);
            uint32_t step = component_size(element.format);
            for (uint32_t i = 0; i < element.components && i < 2; i++) {
                vertex.texture[k][i] = read_number(p + i * step, element.format, element.shift);
            }
        }
    }
    return vertex;
}

uint8_t color_byte(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void rasterize_colors(const Vertex& vertex, const float* eye, float out[2][4]) {
    uint8_t vertex_color[2][4];
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t material = g_xf[0x100C + k];
        for (uint32_t i = 0; i < 4; i++) {
            vertex_color[k][i] = vertex.has_color[k] ? color_byte(vertex.color[k][i]) : static_cast<uint8_t>(material >> (24 - 8 * i));
        }
    }
    float normal[3] = {0, 0, 1};
    if ((g_xf[0x100E] | g_xf[0x100F] | g_xf[0x1010] | g_xf[0x1011]) & 2) {
        lighting::transform_normal(g_xf, vertex.position_matrix, vertex.normal, normal);
    }
    uint8_t result[2][4];
    lighting::light_channels(g_xf, eye, normal, vertex_color, result);
    for (uint32_t k = 0; k < 2; k++) {
        for (uint32_t i = 0; i < 4; i++) {
            out[k][i] = static_cast<float>(result[k][i]) / 255.0f;
        }
    }
}

void transform_position(const Vertex& vertex, float* eye) {
    uint32_t base = vertex.position_matrix * 4;
    const float x = vertex.position[0];
    const float y = vertex.position[1];
    const float z = vertex.position[2];
    for (uint32_t row = 0; row < 3; row++) {
        eye[row] = xf_float(base + row * 4) * x + xf_float(base + row * 4 + 1) * y + xf_float(base + row * 4 + 2) * z +
                   xf_float(base + row * 4 + 3);
    }
}

void generate_texture_coordinates(const Vertex& vertex, float out[8][3]) {
    uint32_t count = g_bp[0x00] & 15;
    bool dual = (g_xf[0x1012] & 1) != 0;
    for (uint32_t i = 0; i < count && i < kTexCoordCount; i++) {
        uint32_t info = g_xf[0x1040 + i];
        uint32_t source = (info >> 7) & 31;
        bool projected = ((info >> 1) & 1) != 0;
        bool abc1 = ((info >> 2) & 1) != 0;
        float input[3] = {0, 0, 1};
        if (source == 0) {
            input[0] = vertex.position[0];
            input[1] = vertex.position[1];
            input[2] = vertex.position[2];
        } else if (source == 1) {
            input[0] = vertex.normal[0];
            input[1] = vertex.normal[1];
            input[2] = vertex.normal[2];
        } else if (source >= 5 && source < 13) {
            input[0] = vertex.texture[source - 5][0];
            input[1] = vertex.texture[source - 5][1];
        }
        if (!abc1) {
            input[2] = 1.0f;
        }
        uint32_t base = vertex.texture_matrix[i] * 4;
        float coord[3];
        for (uint32_t row = 0; row < 3; row++) {
            uint32_t r = base + row * 4;
            coord[row] = xf_float(r) * input[0] + xf_float(r + 1) * input[1] + xf_float(r + 2) * input[2] + xf_float(r + 3);
        }
        if (!projected) {
            coord[2] = 1.0f;
        }
        if (dual) {
            uint32_t post = g_xf[0x1050 + i];
            if ((post >> 8) & 1) {
                float length = std::sqrt(coord[0] * coord[0] + coord[1] * coord[1] + coord[2] * coord[2]);
                if (length > 0.0f) {
                    coord[0] /= length;
                    coord[1] /= length;
                    coord[2] /= length;
                }
            }
            uint32_t post_base = 0x500 + (post & 0x3F) * 4;
            float result[3];
            for (uint32_t row = 0; row < 3; row++) {
                uint32_t r = post_base + row * 4;
                result[row] = xf_float(r) * coord[0] + xf_float(r + 1) * coord[1] + xf_float(r + 2) * coord[2] + xf_float(r + 3);
            }
            coord[0] = result[0];
            coord[1] = result[1];
            coord[2] = result[2];
        }
        if (coord[2] == 0.0f) {
            coord[0] = std::clamp(coord[0] / 2.0f, -1.0f, 1.0f);
            coord[1] = std::clamp(coord[1] / 2.0f, -1.0f, 1.0f);
            coord[2] = 1.0f;
        }
        out[i][0] = coord[0];
        out[i][1] = coord[1];
        out[i][2] = coord[2];
    }
}

bool project(const float* eye, float* screen) {
    float a = xf_float(0x1020), b = xf_float(0x1021), c = xf_float(0x1022), d = xf_float(0x1023), e = xf_float(0x1024), f = xf_float(0x1025);
    bool orthographic = g_xf[0x1026] != 0;
    float x, y, z, w;
    if (orthographic) {
        x = a * eye[0] + b;
        y = c * eye[1] + d;
        z = e * eye[2] + f;
        w = 1.0f;
    } else {
        x = a * eye[0] + b * eye[2];
        y = c * eye[1] + d * eye[2];
        z = e * eye[2] + f;
        w = -eye[2];
    }
    screen[0] = x * (2.0f * xf_float(0x101A) / kEfbWidth) + w * (2.0f * (xf_float(0x101D) - kViewportOffset) / kEfbWidth - 1.0f);
    screen[1] = y * (-2.0f * xf_float(0x101B) / kEfbHeight) + w * (1.0f - 2.0f * (xf_float(0x101E) - kViewportOffset) / kEfbHeight);
    screen[2] = (z * xf_float(0x101C) + w * xf_float(0x101F)) / kDepthRange;
    screen[3] = w;
    return std::isfinite(screen[0]) && std::isfinite(screen[1]) && std::isfinite(screen[2]) && std::isfinite(w);
}

struct Prepared {
    ScreenVertex vertex;
    bool valid;
};

Prepared prepare(const Vertex& vertex) {
    Prepared prepared;
    std::memset(&prepared.vertex, 0, sizeof prepared.vertex);
    float eye[3];
    transform_position(vertex, eye);
    float screen[4] = {0, 0, 0, 1};
    prepared.valid = project(eye, screen);
    prepared.vertex.x = screen[0];
    prepared.vertex.y = screen[1];
    prepared.vertex.z = screen[2];
    prepared.vertex.w = screen[3];
    rasterize_colors(vertex, eye, prepared.vertex.color);
    generate_texture_coordinates(vertex, prepared.vertex.uv);
    return prepared;
}

bool culled(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) {
    uint32_t mode = (g_bp[0x00] >> 14) & 3;
    if (mode == 0) {
        return false;
    }
    if (mode == 3) {
        return true;
    }
    float area = a.x * (b.y * c.w - c.y * b.w) - a.y * (b.x * c.w - c.x * b.w) + a.w * (b.x * c.y - c.x * b.y);
    bool front = area < 0.0f;
    return mode == 1 ? !front : front;
}

void emit_triangle(std::vector<ScreenVertex>& out, const Prepared& a, const Prepared& b, const Prepared& c) {
    if (!a.valid || !b.valid || !c.valid || culled(a.vertex, b.vertex, c.vertex)) {
        return;
    }
    out.push_back(a.vertex);
    out.push_back(b.vertex);
    out.push_back(c.vertex);
}

uint32_t g_indirect_draws = 0;
uint32_t g_coordinate_draws = 0;
std::atomic<uint32_t> g_skipped_presents{0};
#ifdef _WIN32
using ThreadClock = HANDLE;

ThreadClock current_thread_clock() {
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    return thread;
}

double thread_seconds(ThreadClock thread) {
    FILETIME created, exited, kernel, user;
    if (!thread || !GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    auto ticks = [](const FILETIME& time) { return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;
}
#elif defined(__APPLE__)
using ThreadClock = mach_port_t;

ThreadClock current_thread_clock() {
    return pthread_mach_thread_np(pthread_self());
}

double thread_seconds(ThreadClock thread) {
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (!thread || thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0.0;
    }
    return info.user_time.seconds + info.system_time.seconds + (info.user_time.microseconds + info.system_time.microseconds) * 1e-6;
}
#else
using ThreadClock = clockid_t;

ThreadClock current_thread_clock() {
    clockid_t clock = CLOCK_THREAD_CPUTIME_ID;
    pthread_getcpuclockid(pthread_self(), &clock);
    return clock;
}

double thread_seconds(ThreadClock clock) {
    timespec time{};
    if (clock_gettime(clock, &time) != 0) {
        return 0.0;
    }
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}
#endif

std::atomic<bool> g_cpu_clock_known{false};
ThreadClock g_cpu_clock{};

void count_indirect() {
    uint32_t stages = ((g_bp[0x00] >> 10) & 15) + 1;
    bool indirect = false;
    bool coordinates = false;
    for (uint32_t i = 0; i < stages; i++) {
        uint32_t stage = g_bp[0x10 + i];
        indirect |= (stage & (3u << 7)) != 0 || (stage & (3u << 9)) != 0;
        coordinates |= (stage & ((63u << 13) | (1u << 20))) != 0;
    }
    g_indirect_draws += indirect;
    g_coordinate_draws += coordinates && !indirect;
}

void emit_unculled(std::vector<ScreenVertex>& out, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) {
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
}

ScreenVertex shifted(const ScreenVertex& vertex, float dx, float dy) {
    ScreenVertex result = vertex;
    result.x += dx * vertex.w;
    result.y += dy * vertex.w;
    return result;
}

void emit_line(std::vector<ScreenVertex>& out, const Prepared& a, const Prepared& b) {
    if (!a.valid || !b.valid || a.vertex.w <= 0.0f || b.vertex.w <= 0.0f) {
        return;
    }
    float width = static_cast<float>(g_bp[0x22] & 0xFF) / 6.0f;
    float sx = b.vertex.x / b.vertex.w - a.vertex.x / a.vertex.w;
    float sy = b.vertex.y / b.vertex.w - a.vertex.y / a.vertex.w;
    float dx = std::fabs(sx) * kEfbWidth;
    float dy = std::fabs(sy) * kEfbHeight;
    float ox = 0.0f;
    float oy = 0.0f;
    if (dy > dx) {
        ox = width / kEfbWidth;
    } else {
        oy = width / kEfbHeight;
    }
    ScreenVertex a0 = shifted(a.vertex, -ox, -oy);
    ScreenVertex a1 = shifted(a.vertex, ox, oy);
    ScreenVertex b0 = shifted(b.vertex, -ox, -oy);
    ScreenVertex b1 = shifted(b.vertex, ox, oy);
    LinePointOffsets offsets = line_point_offsets(g_bp);
    if (offsets.line_coordinates != 0) {
        bool negative = line_offset_negative_side(sx, sy, dy > dx);
        offset_texture_coordinates(negative ? a0 : a1, offsets.line_coordinates, offsets.line, 0.0f);
        offset_texture_coordinates(negative ? b0 : b1, offsets.line_coordinates, offsets.line, 0.0f);
    }
    emit_unculled(out, a0, a1, b1);
    emit_unculled(out, a0, b1, b0);
}

void emit_point(std::vector<ScreenVertex>& out, const Prepared& a) {
    if (!a.valid || a.vertex.w <= 0.0f) {
        return;
    }
    float size = static_cast<float>((g_bp[0x22] >> 8) & 0xFF) / 6.0f;
    float ox = size / kEfbWidth;
    float oy = size / kEfbHeight;
    ScreenVertex v0 = shifted(a.vertex, -ox, -oy);
    ScreenVertex v1 = shifted(a.vertex, ox, -oy);
    ScreenVertex v2 = shifted(a.vertex, ox, oy);
    ScreenVertex v3 = shifted(a.vertex, -ox, oy);
    LinePointOffsets offsets = line_point_offsets(g_bp);
    if (offsets.point_coordinates != 0) {
        offset_texture_coordinates(v0, offsets.point_coordinates, 0.0f, offsets.point);
        offset_texture_coordinates(v1, offsets.point_coordinates, offsets.point, offsets.point);
        offset_texture_coordinates(v2, offsets.point_coordinates, offsets.point, 0.0f);
    }
    emit_unculled(out, v0, v1, v2);
    emit_unculled(out, v0, v2, v3);
}

void capture_registers() {
    int written = 0;
    auto field = [&](const char* format, uint32_t index, uint32_t value) {
        std::fprintf(g_capture, format, index, value);
        if (++written % 8 == 0) {
            std::fputc(10, g_capture);
        }
    };
    for (uint32_t i = 0; i < 256; i++) {
        if (g_capture_first || g_bp[i] != g_captured_bp[i]) {
            field(" bp%02x=%06x", i, g_bp[i]);
            g_captured_bp[i] = g_bp[i];
        }
    }
    for (uint32_t i = 0; i < 8; i++) {
        if (g_capture_first || g_konst[i] != g_captured_konst[i]) {
            field(" konst%u=%06x", i, g_konst[i]);
            g_captured_konst[i] = g_konst[i];
        }
    }
    for (uint32_t i = 0; i < kXfSize; i++) {
        if ((g_capture_first && g_xf[i] != 0) || g_xf[i] != g_captured_xf[i]) {
            field(" xf%04x=%08x", i, g_xf[i]);
            g_captured_xf[i] = g_xf[i];
        }
    }
    g_capture_first = false;
    std::fputc(10, g_capture);
}

void capture_draw(uint8_t command, uint32_t count, size_t triangles, const Layout& layout, const Vertex* raw, const std::vector<Prepared>& vertices) {
    uint32_t vat = command & 7;
    std::fprintf(g_capture, "draw %d primitive=%u vat=%u vertices=%u triangles=%zu vcd=%08x %08x vat=%08x %08x %08x size=%u\n", g_capture_draws++,
                 (command >> 3) & 7, vat, count, triangles, g_vcd_low, g_vcd_high, g_vat[0][vat], g_vat[1][vat], g_vat[2][vat], layout.size);
    std::fprintf(g_capture, " arrays");
    for (uint32_t i = 0; i < kArrayCount; i++) {
        if (g_array_base[i] != 0) {
            std::fprintf(g_capture, " %u=%08x/%u", i, g_array_base[i], g_array_stride[i]);
        }
    }
    std::fputc(10, g_capture);
    capture_registers();
    uint32_t texgens = g_bp[0x00] & 15;
    for (uint32_t i = 0; i < count && i < 3; i++) {
        const Vertex& v = raw[i];
        const ScreenVertex& s = vertices[i].vertex;
        std::fprintf(g_capture, "  v%u pos=(%g %g %g) normal=(%g %g %g) pnmtx=%u screen=(%g %g %g %g)%s", i, v.position[0], v.position[1], v.position[2], v.normal[0],
                     v.normal[1], v.normal[2], v.position_matrix, s.x, s.y, s.z, s.w, vertices[i].valid ? "" : " invalid");
        for (uint32_t t = 0; t < kTexCoordCount; t++) {
            if (layout.texture[t].mode != 0) {
                std::fprintf(g_capture, " tex%u=(%g %g)", t, v.texture[t][0], v.texture[t][1]);
            }
        }
        for (uint32_t t = 0; t < texgens && t < kTexCoordCount; t++) {
            std::fprintf(g_capture, " uv%u=(%g %g %g) mtx%u=%u", t, s.uv[t][0], s.uv[t][1], s.uv[t][2], t, v.texture_matrix[t]);
        }
        std::fputc(10, g_capture);
    }
}

void draw_primitive(uint8_t command, const uint8_t* data, uint32_t count) {
    if (!g_render_enabled) {
        return;
    }
    count_indirect();
    const Layout& layout = make_layout(command & 7);
    static std::vector<Prepared> vertices;
    vertices.clear();
    vertices.reserve(count);
    const uint8_t* stream = data;
    Vertex raw[3];
    for (uint32_t i = 0; i < count; i++) {
        Vertex vertex = decode_vertex(layout, stream);
        if (i < 3) {
            raw[i] = vertex;
        }
        vertices.push_back(prepare(vertex));
    }
    static std::vector<ScreenVertex> triangles;
    triangles.clear();
    uint32_t primitive = (command >> 3) & 7;
    switch (primitive) {
    case 0:
    case 1:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            emit_triangle(triangles, vertices[i], vertices[i + 1], vertices[i + 2]);
            emit_triangle(triangles, vertices[i], vertices[i + 2], vertices[i + 3]);
        }
        break;
    case 2:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            emit_triangle(triangles, vertices[i], vertices[i + 1], vertices[i + 2]);
        }
        break;
    case 3:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) {
                emit_triangle(triangles, vertices[i + 1], vertices[i], vertices[i + 2]);
            } else {
                emit_triangle(triangles, vertices[i], vertices[i + 1], vertices[i + 2]);
            }
        }
        break;
    case 4:
        for (uint32_t i = 1; i + 1 < count; i++) {
            emit_triangle(triangles, vertices[0], vertices[i], vertices[i + 1]);
        }
        break;
    case 5:
        for (uint32_t i = 0; i + 1 < count; i += 2) {
            emit_line(triangles, vertices[i], vertices[i + 1]);
        }
        break;
    case 6:
        for (uint32_t i = 0; i + 1 < count; i++) {
            emit_line(triangles, vertices[i], vertices[i + 1]);
        }
        break;
    default:
        for (uint32_t i = 0; i < count; i++) {
            emit_point(triangles, vertices[i]);
        }
        break;
    }
    if (g_log_draws && g_copy_total >= g_log_from && g_logged_prepared < 4000) {
        g_logged_prepared++;
        uint32_t image0 = g_bp[0x88];
        const ScreenVertex& v0 = vertices[0].vertex;
        std::fprintf(stderr, "P copy=%d cmd=%02x n=%u tri=%zu stg=%u tg=%u cull=%u blend=%06x z=%06x map0=%u/%ux%u v0=(%.2f %.2f %.2f)%s tev0=%06x/%06x\n",
                     g_copy_total, command, count, triangles.size() / 3, ((g_bp[0x00] >> 10) & 15) + 1, g_bp[0x00] & 15, (g_bp[0x00] >> 14) & 3,
                     g_bp[0x41], g_bp[0x40], (image0 >> 20) & 15, (image0 & 0x3FF) + 1, ((image0 >> 10) & 0x3FF) + 1, v0.x, v0.y, v0.z,
                     vertices[0].valid ? "" : " INVALID", g_bp[0xC0], g_bp[0xC1]);
        std::fprintf(stderr, "  regs=%06x %06x %06x %06x %06x %06x %06x %06x konst=%06x %06x %06x %06x order=%06x ksel=%06x c0=(%.2f %.2f %.2f %.2f) mat=%08x ctl=%08x\n",
                     g_bp[0xE0], g_bp[0xE1], g_bp[0xE2], g_bp[0xE3], g_bp[0xE4], g_bp[0xE5], g_bp[0xE6], g_bp[0xE7], g_konst[0], g_konst[1], g_konst[2],
                     g_konst[3], g_bp[0x28], g_bp[0xF6], vertices[0].vertex.color[0][0], vertices[0].vertex.color[0][1], vertices[0].vertex.color[0][2],
                     vertices[0].vertex.color[0][3], g_xf[0x100C], g_xf[0x100E]);
    }
    if (g_capture) {
        capture_draw(command, count, triangles.size() / 3, layout, raw, vertices);
    }
    render::draw(triangles.data(), static_cast<uint32_t>(triangles.size()));
}

void report_copy() {
    if (!g_log || g_logged_copies >= kMaxLoggedCopies) {
        return;
    }
    g_logged_copies++;
    std::fprintf(stderr, "GX copy: draws=%u vertices=%u bp=%u cp=%u xf=%u lists=%u unknown=%u\n", g_stats.draws,
                 g_stats.vertices, g_stats.bp_writes, g_stats.cp_writes, g_stats.xf_words, g_stats.lists,
                 g_stats.unknown);
    g_stats = Statistics{};
}

void capture_frame_boundary() {
    if (g_capture) {
        std::fprintf(g_capture, "end of frame, %d draws\n", g_capture_draws);
        std::fclose(g_capture);
        g_capture = nullptr;
        render::dump_copies(nullptr);
        char image[80];
        std::snprintf(image, sizeof image, "%s.png", g_capture_name);
        video::save_next_frame(image);
        std::fprintf(stderr, "GX capture saved to %s.txt and %s.png", g_capture_name, g_capture_name);
        std::fputc(10, stderr);
        log::write("gx", "capture saved to %s.txt and %s.png", g_capture_name, g_capture_name);
        return;
    }
    static const char* scheduled = std::getenv("WP_CAPTURE_AT");
    static const auto start = std::chrono::steady_clock::now();
    bool due = false;
    if (scheduled) {
        static const char* next = scheduled;
        static const bool by_frames = std::getenv("WP_SCRIPT_CLOCK") && std::strcmp(std::getenv("WP_SCRIPT_CLOCK"), "frames") == 0;
        double elapsed = by_frames ? frames_drawn() / 60.0 : std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (*next && elapsed >= std::atof(next)) {
            due = true;
            const char* comma = std::strchr(next, ',');
            next = comma ? comma + 1 : next + std::strlen(next);
        }
    }
    if (!g_capture_requested.exchange(false) && !due) {
        return;
    }
    std::error_code created;
    std::filesystem::create_directories("captures", created);
    char path[80];
    while (true) {
        std::snprintf(g_capture_name, sizeof g_capture_name, "captures/gx_capture_%03d", ++g_capture_index);
        std::snprintf(path, sizeof path, "%s.txt", g_capture_name);
        FILE* existing = std::fopen(path, "rb");
        if (!existing) {
            break;
        }
        std::fclose(existing);
    }
    g_capture = std::fopen(path, "w");
    if (std::getenv("WP_CAPTURE_RAM")) {
        // Raw MEM1 and MEM2 next to the capture, for RAM searches (tools/ramsearch.py).
        for (const auto& [suffix, start, size] : {std::tuple<const char*, uint32_t, uint32_t>{"mem1", 0x80000000u, 0x01800000u},
                                                  std::tuple<const char*, uint32_t, uint32_t>{"mem2", 0x90000000u, 0x04000000u}}) {
            char ram[96];
            std::snprintf(ram, sizeof ram, "%s.%s", g_capture_name, suffix);
            if (std::FILE* out = std::fopen(ram, "wb")) {
                std::fwrite(host(start), 1, size, out);
                std::fclose(out);
            }
        }
    }
    render::dump_copies(g_capture_name);
    g_capture_draws = 0;
    g_capture_first = true;
}

render::CopyFilter copy_filter(uint32_t control) {
    static constexpr float kGamma[4] = {1.0f, 1.7f, 2.2f, 2.2f};
    uint64_t bits = g_bp[0x53] | (static_cast<uint64_t>(g_bp[0x54]) << 24);
    uint32_t weights[7];
    for (int i = 0; i < 7; i++) {
        weights[i] = static_cast<uint32_t>((bits >> (6 * i)) & 0x3F);
    }
    render::CopyFilter filter;
    filter.coefficients[0] = weights[0] + weights[1];
    filter.coefficients[1] = weights[2] + weights[3] + weights[4];
    filter.coefficients[2] = weights[5] + weights[6];
    static const settings::LiveFlag filter_enabled("video.copy_filter", "WP_COPY_FILTER");
    if (!filter_enabled()) {
        filter.coefficients[1] += filter.coefficients[0] + filter.coefficients[2];
        filter.coefficients[0] = 0;
        filter.coefficients[2] = 0;
    }
    filter.gamma = kGamma[(control >> 7) & 3];
    filter.clamp_top = (control & 1) != 0;
    filter.clamp_bottom = (control & 2) != 0;
    return filter;
}

void execute_copy(uint32_t value) {
    g_copy_total++;
    report_copy();
    if (!g_render_enabled) {
        return;
    }
    if (g_capture && !(value & kCopyToFramebuffer)) {
        std::fprintf(g_capture, "copy to texture at %06x, source %06x size %06x, control %06x, pixel format %06x\n", g_bp[0x4B] << 5, g_bp[0x49], g_bp[0x4A], value, g_bp[0x43]);
    }
    if (value & kCopyToFramebuffer) {
        capture_frame_boundary();
        uint32_t source = g_bp[0x49];
        uint32_t size = g_bp[0x4A];
        int x = static_cast<int>(source & 0x3FF);
        int y = static_cast<int>((source >> 10) & 0x3FF);
        int width = static_cast<int>(size & 0x3FF) + 1;
        int height = static_cast<int>((size >> 10) & 0x3FF) + 1;
        {
            g_frames++;
            g_frames_drawn++;
            auto now = std::chrono::steady_clock::now();
            static auto previous_frame = now;
            double frame_ms = std::chrono::duration<double>(now - previous_frame).count() * 1000.0;
            previous_frame = now;
            if (frame_ms > kSlowFrameMs) {
                log::write("frame", "slow frame: %.0f ms", frame_ms);
            }
            double seconds = std::chrono::duration<double>(now - g_fps_start).count();
            if (seconds >= 1.0) {
                uint32_t batches = 0;
                uint32_t vertices = 0;
                double batch_seconds = 0.0;
                render::take_statistics(batches, vertices, batch_seconds);
                if (g_log_fps) {
                    std::fprintf(stderr, "fps %.1f batches %u vertices %u cpu-side draw time %.0f ms", g_frames / seconds, batches, vertices, batch_seconds * 1000.0);
                    std::fputc(10, stderr);
                }
                log::write("fps", "%.1f fps, %u draw batches, %u vertices, %.0f ms drawing, %u draws with indirect texturing, %u with wrapped coordinates, %u presents skipped", g_frames / seconds, batches,
                           vertices, batch_seconds * 1000.0, g_indirect_draws, g_coordinate_draws, g_skipped_presents.exchange(0));
                static double previous_cpu = 0.0;
                static double previous_gpu = 0.0;
                double cpu = g_cpu_clock_known.load() ? thread_seconds(g_cpu_clock) : 0.0;
                static const ThreadClock gpu_clock = current_thread_clock();
                double gpu = thread_seconds(gpu_clock);
                log::write("threads", "CPU thread busy %.0f%% (guest idle %.0f%%), GPU thread busy %.0f%%", (cpu - previous_cpu) * 100.0 / seconds, idle_share() * 100.0, (gpu - previous_gpu) * 100.0 / seconds);
                previous_cpu = cpu;
                previous_gpu = gpu;
                g_indirect_draws = 0;
                g_coordinate_draws = 0;
                video::update_statistics(g_frames / seconds);
                g_frames = 0;
                g_fps_start = now;
            }
        }
        render::copy_to_framebuffer(x, y, width, height, (g_bp[0x43] & 7) == kPixelFormatZ24, copy_filter(value));
    } else {
        uint32_t source = g_bp[0x49];
        uint32_t size = g_bp[0x4A];
        int x = static_cast<int>(source & 0x3FF);
        int y = static_cast<int>((source >> 10) & 0x3FF);
        int width = static_cast<int>(size & 0x3FF) + 1;
        int height = static_cast<int>((size >> 10) & 0x3FF) + 1;
        uint32_t address = kRamBase | ((g_bp[0x4B] & 0xFFFFFF) << 5);
        uint32_t coded = (value >> 3) & 15;
        uint32_t format = coded / 2 + (coded & 1) * 8;
        render::copy_to_texture(address, g_bp[0x4D] & 0x3FF, x, y, width, height, (value & (1u << 9)) != 0, format, (value & (1u << 15)) != 0, (g_bp[0x43] & 7) == kPixelFormatZ24,
                                (g_bp[0x43] & 7) == kPixelFormatRgba6Z24, copy_filter(value));
    }
    if (value & kCopyClear) {
        uint32_t source = g_bp[0x49];
        uint32_t size = g_bp[0x4A];
        render::clear(static_cast<int>(source & 0x3FF), static_cast<int>((source >> 10) & 0x3FF), static_cast<int>(size & 0x3FF) + 1,
                      static_cast<int>((size >> 10) & 0x3FF) + 1);
    }
}

std::atomic<bool> g_finish_pending{false};

void load_bp(uint32_t word) {
    uint32_t reg = word >> 24;
    uint32_t value = word & 0xFFFFFF;
    g_stats.bp_writes++;
    if (reg == kBpMask) {
        g_bp_mask = value;
        return;
    }
    uint32_t mask = g_bp_mask;
    g_bp_mask = 0xFFFFFF;
    if (reg >= 0xE0 && reg <= 0xE7 && (value & (1u << 23))) {
        g_konst[reg - 0xE0] = (g_konst[reg - 0xE0] & ~mask) | (value & mask);
        return;
    }
    g_bp[reg] = (g_bp[reg] & ~mask) | (value & mask);
    if (reg == kBpDrawDone && (value & 2)) {
        g_finish_pending = true;
    } else if (reg == kBpCopyExecute) {
        execute_copy(g_bp[reg]);
    } else if (reg == kBpInvalidateTexture) {
        render::invalidate_textures();
    } else if (reg == kBpLoadTlut) {
        uint32_t address = kRamBase | ((g_bp[0x64] & 0xFFFFFF) << 5);
        render::load_tlut(address, (g_bp[reg] & 0x3FF) << 9, ((g_bp[reg] >> 10) & 0x7FF) << 5);
    }
}

void load_cp(uint8_t reg, uint32_t value) {
    g_stats.cp_writes++;
    if (reg == 0x50) {
        g_vcd_low = value;
    } else if (reg == 0x60) {
        g_vcd_high = value;
    } else if (reg >= 0x70 && reg < 0x98) {
        g_vat[(reg >> 4) - 7][reg & 7] = value;
    } else if (reg >= 0xA0 && reg < 0xB0) {
        g_array_base[reg - 0xA0] = value;
    } else if (reg >= 0xB0 && reg < 0xC0) {
        g_array_stride[reg - 0xB0] = value & 0xFF;
    }
}

void write_xf(uint32_t address, uint32_t value) {
    if (address < kXfSize) {
        g_xf[address] = value;
    }
}

void indexed_xf_load(uint8_t command, uint32_t word) {
    uint32_t array = 12 + ((command >> 3) & 3);
    uint32_t index = word >> 16;
    uint32_t count = ((word >> 12) & 15) + 1;
    uint32_t address = word & 0xFFF;
    const uint8_t* source = array_element(array, index);
    for (uint32_t i = 0; i < count; i++) {
        write_xf(address + i, be32(source + 4 * i));
    }
}

size_t parse(const uint8_t* data, size_t size, bool list);

size_t parse_one(const uint8_t* data, size_t size, bool list) {
    uint8_t command = data[0];
    if (command == kCommandNop || command == kCommandMetrics || command == kCommandInvalidate) {
        return 1;
    }
    if (command == kCommandLoadCp) {
        if (size < 6) {
            return 0;
        }
        load_cp(data[1], be32(data + 2));
        return 6;
    }
    if (command == kCommandLoadXf) {
        if (size < 5) {
            return 0;
        }
        uint32_t header = be32(data + 1);
        uint32_t count = (header >> 16) + 1;
        if (size < 5 + 4 * static_cast<size_t>(count)) {
            return 0;
        }
        for (uint32_t i = 0; i < count; i++) {
            write_xf((header & 0xFFFF) + i, be32(data + 5 + 4 * i));
        }
        g_stats.xf_words += count;
        return 5 + 4 * static_cast<size_t>(count);
    }
    if ((command & 0xE7) == 0x20) {
        if (size < 5) {
            return 0;
        }
        indexed_xf_load(command, be32(data + 1));
        g_stats.xf_words++;
        return 5;
    }
    if (command == kCommandCallList) {
        if (size < 9) {
            return 0;
        }
        uint32_t address = be32(data + 1);
        uint32_t length = be32(data + 5);
        g_stats.lists++;
        if (static_cast<uint64_t>(address & 0x1FFFFFFF) + length > kPhysicalSize) {
            std::fprintf(stderr, "GX display list out of range: address=%08x length=%08x list=%d\n", address, length, list);
            g_stats.unknown++;
            return 9;
        }
        if (!list) {
            parse(host(address), length, true);
        }
        return 9;
    }
    if (command == kCommandLoadBp) {
        if (size < 5) {
            return 0;
        }
        load_bp(be32(data + 1));
        return 5;
    }
    if (command & kCommandDrawMask) {
        if (size < 3) {
            return 0;
        }
        uint32_t count = be16(data + 1);
        const Layout& layout = make_layout(command & 7);
        size_t total = 3 + static_cast<size_t>(count) * layout.size;
        if (size < total) {
            return 0;
        }
        if (g_log_draws && g_copy_total >= g_log_from && g_logged_draws < 60) {
            g_logged_draws++;
            std::fprintf(stderr, "GX draw %02x count=%u size=%u vcd=%08x/%08x\n", command, count, layout.size, g_vcd_low, g_vcd_high);
        }
        draw_primitive(command, data + 3, count);
        g_stats.draws++;
        g_stats.vertices += count;
        return total;
    }
    g_stats.unknown++;
    return 1;
}

size_t parse(const uint8_t* data, size_t size, bool list) {
    size_t offset = 0;
    while (offset < size) {
        size_t used = parse_one(data + offset, size - offset, list);
        if (used == 0) {
            break;
        }
        offset += used;
    }
    return offset;
}

}

const uint32_t* bp_registers() {
    return g_bp;
}

const uint32_t* konst_registers() {
    return g_konst;
}

const uint32_t* xf_registers() {
    return g_xf;
}

bool record_display_list(uint64_t value, unsigned bytes) {
    uint32_t pi_base = rd32(kPiFifoBase) & kFifoPhysicalMask;
    uint32_t cp_base = (rd16(kCpFifoBaseLow) | (static_cast<uint32_t>(rd16(kCpFifoBaseHigh)) << 16)) & kFifoPhysicalMask;
    if (cp_base == 0 || pi_base == cp_base) {
        return false;
    }
    uint32_t pointer = rd32(kPiFifoWritePointer);
    uint32_t address = pointer & kFifoPhysicalMask;
    for (unsigned i = 0; i < bytes; i++) {
        *host(kRamBase | (address + i)) = static_cast<uint8_t>(value >> (8 * (bytes - 1 - i)));
    }
    wr32(kPiFifoWritePointer, (pointer & ~kFifoPhysicalMask) | (address + bytes));
    return true;
}

std::vector<uint8_t>* g_fifo_capture = nullptr;

void capture_fifo(std::vector<uint8_t>* out) {
    g_fifo_capture = out;
}

void push(uint64_t value, unsigned bytes) {
    if (g_fifo_capture) {
        for (unsigned i = 0; i < bytes; i++) {
            g_fifo_capture->push_back(static_cast<uint8_t>(value >> (8 * (bytes - 1 - i))));
        }
        return;
    }
    if (record_display_list(value, bytes)) {
        return;
    }
    for (unsigned i = 0; i < bytes; i++) {
        g_fifo.push_back(static_cast<uint8_t>(value >> (8 * (bytes - 1 - i))));
    }
    if (g_fifo.size() >= kFlushThreshold) {
        process();
    }
}

bool take_finish_interrupt() {
    return g_finish_pending.exchange(false);
}

uint64_t frames_drawn() {
    return g_frames_drawn.load();
}

void request_capture() {
    g_capture_requested = true;
}

namespace {

constexpr int kMaxFramesAhead = 2;

struct GpuItem {
    std::vector<uint8_t> bytes;
    std::function<void()> task;
    bool frame = false;
};

struct GpuQueue {
    std::mutex lock;
    std::condition_variable work;
    std::deque<GpuItem> items;
    int frames = 0;
    bool started = false;
};

GpuQueue g_gpu;
const bool g_threaded = [] {
    if (det::enabled()) {
        return false;  // GPU results (copies to RAM, draw-done) must land at fixed points
    }
    const char* setting = std::getenv("WP_GPU_THREAD");
    return !setting || std::atoi(setting) != 0;
}();


void gpu_thread() {
    std::vector<uint8_t> buffer;
    while (true) {
        std::unique_lock<std::mutex> hold(g_gpu.lock);
        g_gpu.work.wait(hold, [] { return !g_gpu.items.empty(); });
        GpuItem item = std::move(g_gpu.items.front());
        g_gpu.items.pop_front();
        hold.unlock();
        if (!item.bytes.empty()) {
            buffer.insert(buffer.end(), item.bytes.begin(), item.bytes.end());
            size_t used = parse(buffer.data(), buffer.size(), false);
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(used));
        }
        if (item.task) {
            item.task();
        }
        if (item.frame) {
            std::lock_guard<std::mutex> hold(g_gpu.lock);
            g_gpu.frames--;
        }
    }
}

void enqueue(GpuItem item) {
    std::unique_lock<std::mutex> hold(g_gpu.lock);
    if (!g_gpu.started) {
        g_gpu.started = true;
        std::thread(gpu_thread).detach();
    }
    if (item.frame) {
        if (g_gpu.frames >= kMaxFramesAhead) {
            hold.unlock();
            g_skipped_presents++;
            return;
        }
        g_gpu.frames++;
    }
    g_gpu.items.push_back(std::move(item));
    hold.unlock();
    g_gpu.work.notify_one();
}

}

void process() {
    if (!g_cpu_clock_known.load()) {
        g_cpu_clock = current_thread_clock();
        g_cpu_clock_known = true;
    }
    if (g_fifo.empty()) {
        return;
    }
    if (!g_threaded) {
        size_t used = parse(g_fifo.data(), g_fifo.size(), false);
        g_fifo.erase(g_fifo.begin(), g_fifo.begin() + static_cast<std::ptrdiff_t>(used));
        return;
    }
    GpuItem item;
    item.bytes.swap(g_fifo);
    g_fifo.reserve(item.bytes.capacity());
    enqueue(std::move(item));
}

void run_frame_task(std::function<void()> task) {
    process();
    if (!g_threaded) {
        task();
        return;
    }
    GpuItem item;
    item.task = std::move(task);
    item.frame = true;
    enqueue(std::move(item));
}

}
