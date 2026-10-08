#include "wp/gx_render.h"

#include "gx_render_common.h"
#include "../platform/gl_window.h"

#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "wp/custom_textures.h"
#include "wp/game.h"
#include "wp/gx_state.h"
#include "wp/hud.h"
#include "wp/memory.h"
#include "wp/options.h"
#include "wp/settings.h"
#include "wp/video.h"

namespace wp::gx::render {

namespace {

#define WP_GL_FUNCTIONS(X)                                                                                                                                  \
    X(ActiveTexture) X(AttachShader) X(BindBuffer) X(BindBufferBase) X(BindFramebuffer) X(BindSampler) X(BindTexture) X(BindVertexArray)              \
    X(BlendEquationSeparate) X(BlendFuncSeparate) X(BufferData) X(BufferSubData) X(CheckFramebufferStatus) X(Clear) X(ClearColor) X(ClearDepth)      \
    X(ColorMask) X(CompileShader) X(CreateProgram) X(CreateShader) X(DeleteFramebuffers) X(DeleteShader) X(DeleteTextures) X(DepthFunc)              \
    X(DepthMask) X(Disable) X(DrawArrays) X(Enable) X(EnableVertexAttribArray) X(FramebufferTexture2D) X(GenBuffers) X(GenFramebuffers)             \
    X(GenSamplers) X(GenTextures) X(GenVertexArrays) X(GenerateMipmap) X(GetIntegerv) X(GetProgramInfoLog) X(GetProgramiv) X(GetShaderInfoLog)      \
    X(GetShaderiv) X(GetString) X(GetStringi) X(GetUniformBlockIndex) X(GetUniformLocation) X(LinkProgram) X(PixelStorei) X(ReadPixels)             \
    X(SamplerParameterf) X(SamplerParameteri) X(Scissor) X(ShaderSource) X(TexImage2D) X(TexParameteri) X(TexSubImage2D) X(Uniform1i)              \
    X(Uniform1f) X(Uniform4f) X(UniformBlockBinding) X(UseProgram) X(VertexAttribPointer) X(Viewport)

struct Gl {
#define WP_GL_MEMBER(name) decltype(&::gl##name) name = nullptr;
    WP_GL_FUNCTIONS(WP_GL_MEMBER)
#undef WP_GL_MEMBER
    decltype(&::glClipControl) ClipControl = nullptr;
};

Gl gl;

bool load_functions() {
    bool ok = true;
#define WP_GL_LOAD(name)                                                                                   \
    gl.name = reinterpret_cast<decltype(gl.name)>(SDL_GL_GetProcAddress("gl" #name));                     \
    if (!gl.name) {                                                                                        \
        std::fprintf(stderr, "OpenGL function gl%s is missing\n", #name);                                  \
        ok = false;                                                                                        \
    }
    WP_GL_FUNCTIONS(WP_GL_LOAD)
#undef WP_GL_LOAD
    return ok;
}

constexpr uint32_t kVertexCapacity = 1 << 16;

const char* kFullscreenVertex = R"GLSL(#version 410 core
uniform float flip;
out vec2 uv;
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = corner;
    gl_Position = vec4(corner.x * 2.0 - 1.0, (1.0 - corner.y * 2.0) * flip, 0.0, 1.0);
}
)GLSL";

const char* kPresentFragment = R"GLSL(#version 410 core
uniform sampler2D frame;
in vec2 uv;
out vec4 color;
void main() {
    color = vec4(texture(frame, uv).rgb, 1.0);
}
)GLSL";

const char* kOverlayFragment = R"GLSL(#version 410 core
uniform sampler2D overlay;
in vec2 uv;
out vec4 color;
void main() {
    color = texture(overlay, uv);
}
)GLSL";

const char* kClearFragment = R"GLSL(#version 410 core
uniform vec4 clear_color;
uniform float clear_depth;
in vec2 uv;
out vec4 color;
void main() {
    color = clear_color;
    gl_FragDepth = clear_depth;
}
)GLSL";

const char* kCopyFragment = R"GLSL(#version 410 core
uniform sampler2D color_source;
uniform sampler2D depth_source;
layout(std140) uniform CopyConstants {
    uvec4 params;
    vec4 region;
    uvec4 filter_taps;
    vec4 rows;
    vec4 extent;
};
in vec2 uv;
out vec4 out_color;

vec4 convert(uvec4 raw) {
    if (params.z == 0u) {
        raw.a = 255u;
    }
    if (params.y != 0u) {
        const vec4 y_const = vec4(66.0, 129.0, 25.0, 16.0);
        const vec4 u_const = vec4(-38.0, -74.0, 112.0, 128.0);
        const vec4 v_const = vec4(112.0, -94.0, -18.0, 128.0);
        vec4 source = vec4(vec3(raw.rgb), 256.0);
        uvec3 yuv = uvec3(dot(y_const, source), dot(u_const, source), dot(v_const, source));
        raw.rgb = min((yuv >> 8u) + ((yuv >> 7u) & 1u), uvec3(255u));
    }
    vec4 value = vec4(raw) / 255.0;
    switch (params.x) {
    case 0u: {
        float red = float(raw.r & 0xF0u) / 240.0;
        return vec4(red);
    }
    case 1u:
    case 8u: return value.rrrr;
    case 2u: {
        vec2 red_alpha = vec2(uvec2(raw.r, raw.a) & 0xF0u) / 240.0;
        return red_alpha.rrrg;
    }
    case 3u: return value.rrra;
    case 4u: {
        vec2 red_blue = vec2(uvec2(raw.r, raw.b) & 0xF8u) / 248.0;
        float green = float(raw.g & 0xFCu) / 252.0;
        return vec4(red_blue.r, green, red_blue.g, 1.0);
    }
    case 5u: return vec4(vec3(raw.rgb & 0xF8u) / 248.0, float(raw.a & 0xE0u) / 224.0);
    case 7u: return value.aaaa;
    case 9u: return value.gggg;
    case 10u: return value.bbbb;
    case 11u: return value.rrrg;
    case 12u: return value.gggb;
    case 13u: return vec4(value.rgb, 1.0);
    default: return value;
    }
}

uvec4 sample_efb(vec2 position, float offset) {
    float y = clamp(position.y + offset, rows.y, rows.z);
    if (params.w != 0u) {
        uint depth = min(uint(clamp(texelFetch(depth_source, ivec2(int(position.x), int(y)), 0).r, 0.0, 1.0) * 16777216.0), 0xFFFFFFu);
        return uvec4((depth >> 16u) & 255u, (depth >> 8u) & 255u, depth & 255u, 255u);
    }
    return uvec4(roundEven(clamp(textureLod(color_source, vec2(position.x, y) / extent.xy, 0.0), 0.0, 1.0) * 255.0));
}

void main() {
    vec2 source = region.xy + uv * region.zw;
    uvec4 current = sample_efb(source, 0.0);
    uvec3 combined = current.rgb * filter_taps.y;
    if (filter_taps.x != 0u || filter_taps.z != 0u) {
        combined += sample_efb(source, -1.0).rgb * filter_taps.x + sample_efb(source, 1.0).rgb * filter_taps.z;
    }
    uvec4 raw = uvec4(combined >> 6u, current.a);
    if (filter_taps.w != 0u) {
        raw &= 0x1FFu;
    }
    raw = min(raw, uvec4(255u));
    if (rows.x != 1.0) {
        raw.rgb = uvec3(roundEven(pow(vec3(raw.rgb) / 255.0, vec3(rows.x)) * 255.0));
    }
    out_color = convert(raw);
}
)GLSL";

const char* kTevVertex = R"GLSL(#version 410 core
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_c0;
layout(location = 2) in vec4 a_c1;
layout(location = 3) in vec3 a_uv[8];
uniform int depth_zero_to_one;
out vec4 v_c0;
out vec4 v_c1;
out vec3 v_uv[8];
void main() {
    float z = depth_zero_to_one != 0 ? a_position.z : 2.0 * a_position.z - a_position.w;
    gl_Position = vec4(a_position.x, -a_position.y, z, a_position.w);
    v_c0 = a_c0;
    v_c1 = a_c1;
    for (int i = 0; i < 8; i++) {
        v_uv[i] = a_uv[i];
    }
}
)GLSL";

const char* kTevFragment = R"GLSL(
layout(std140) uniform Constants {
    vec4 initial[4];
    vec4 konst[4];
    uvec4 stage[16];
    uvec4 header;
    uvec4 swaps;
    vec4 texdims[8];
    ivec4 indmtx[6];
    uvec4 indirect;
    uvec4 tevind[16];
    vec4 fog;
    ivec4 fog_integer;
    vec4 fog_range[3];
};

uniform sampler2D t0;
uniform sampler2D t1;
uniform sampler2D t2;
uniform sampler2D t3;
uniform sampler2D t4;
uniform sampler2D t5;
uniform sampler2D t6;
uniform sampler2D t7;

in vec4 v_c0;
in vec4 v_c1;
in vec3 v_uv[8];

layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;

vec2 projected(vec3 uv) {
    return uv.z == 0.0 ? clamp(uv.xy / 2.0, -1.0, 1.0) : uv.xy / uv.z;
}

vec2 select_uv(uint i) {
    switch (i) {
    case 0u: return projected(v_uv[0]);
    case 1u: return projected(v_uv[1]);
    case 2u: return projected(v_uv[2]);
    case 3u: return projected(v_uv[3]);
    case 4u: return projected(v_uv[4]);
    case 5u: return projected(v_uv[5]);
    case 6u: return projected(v_uv[6]);
    default: return projected(v_uv[7]);
    }
}

vec4 sample_map(uint m, vec2 uv, vec2 dx, vec2 dy) {
    switch (m) {
    case 0u: return textureGrad(t0, uv, dx, dy);
    case 1u: return textureGrad(t1, uv, dx, dy);
    case 2u: return textureGrad(t2, uv, dx, dy);
    case 3u: return textureGrad(t3, uv, dx, dy);
    case 4u: return textureGrad(t4, uv, dx, dy);
    case 5u: return textureGrad(t5, uv, dx, dy);
    case 6u: return textureGrad(t6, uv, dx, dy);
    default: return textureGrad(t7, uv, dx, dy);
    }
}

const int kFractions[8] = int[8](255, 223, 191, 159, 128, 96, 64, 32);

ivec4 quantized(vec4 value) {
    return ivec4(roundEven(value * 255.0));
}

ivec3 konst_color(uint sel, ivec4 k[4]) {
    if (sel < 8u) {
        return ivec3(kFractions[sel]);
    }
    if (sel >= 12u && sel < 16u) {
        return k[sel - 12u].rgb;
    }
    if (sel >= 16u) {
        uint index = (sel - 16u) & 3u;
        uint component = (sel - 16u) >> 2u;
        return ivec3(k[index][component]);
    }
    return ivec3(0);
}

int konst_alpha(uint sel, ivec4 k[4]) {
    if (sel < 8u) {
        return kFractions[sel];
    }
    if (sel >= 16u) {
        uint index = (sel - 16u) & 3u;
        uint component = (sel - 16u) >> 2u;
        return k[index][component];
    }
    return 0;
}

ivec3 color_input(uint sel, ivec4 r[4], ivec4 tex, ivec4 ras, ivec3 kc) {
    switch (sel) {
    case 0u: return r[0].rgb;
    case 1u: return r[0].aaa;
    case 2u: return r[1].rgb;
    case 3u: return r[1].aaa;
    case 4u: return r[2].rgb;
    case 5u: return r[2].aaa;
    case 6u: return r[3].rgb;
    case 7u: return r[3].aaa;
    case 8u: return tex.rgb;
    case 9u: return tex.aaa;
    case 10u: return ras.rgb;
    case 11u: return ras.aaa;
    case 12u: return ivec3(255);
    case 13u: return ivec3(128);
    case 14u: return kc;
    default: return ivec3(0);
    }
}

int alpha_input(uint sel, ivec4 r[4], ivec4 tex, ivec4 ras, int ka) {
    switch (sel) {
    case 0u: return r[0].a;
    case 1u: return r[1].a;
    case 2u: return r[2].a;
    case 3u: return r[3].a;
    case 4u: return tex.a;
    case 5u: return ras.a;
    case 6u: return ka;
    default: return 0;
    }
}

bool compare_alpha(uint mode, uint value, uint reference) {
    switch (mode) {
    case 0u: return false;
    case 1u: return value < reference;
    case 2u: return value == reference;
    case 3u: return value <= reference;
    case 4u: return value > reference;
    case 5u: return value != reference;
    case 6u: return value >= reference;
    default: return true;
    }
}

bool compare_inputs(uint mode, bool eq, ivec3 a, ivec3 b) {
    int left = a.r;
    int right = b.r;
    if (mode == 1u) {
        left = a.r + a.g * 256;
        right = b.r + b.g * 256;
    } else if (mode == 2u) {
        left = a.r + a.g * 256 + a.b * 65536;
        right = b.r + b.g * 256 + b.b * 65536;
    }
    return eq ? left == right : left > right;
}

ivec3 compare_color(uint mode, bool eq, ivec3 a, ivec3 b, ivec3 c) {
    if (mode == 3u) {
        bvec3 passed = eq ? equal(a, b) : greaterThan(a, b);
        return ivec3(passed.r ? c.r : 0, passed.g ? c.g : 0, passed.b ? c.b : 0);
    }
    return compare_inputs(mode, eq, a, b) ? c : ivec3(0);
}

ivec4 apply_swap(ivec4 value, uint table) {
    uint order_bits = swaps[table];
    int components[4] = int[4](value.r, value.g, value.b, value.a);
    return ivec4(components[order_bits & 3u], components[(order_bits >> 2u) & 3u], components[(order_bits >> 4u) & 3u], components[(order_bits >> 6u) & 3u]);
}

ivec3 combine(ivec3 a, ivec3 b, ivec3 c, ivec3 d, uint bias_code, bool subtract, uint scale_code) {
    int bias = bias_code == 1u ? 128 : (bias_code == 2u ? -128 : 0);
    int shift = scale_code == 1u ? 1 : (scale_code == 2u ? 2 : 0);
    int rounding = scale_code == 3u ? 0 : (subtract ? 127 : 128);
    ivec3 blended = ((((a << 8) + (b - a) * (c + (c >> 7))) << shift) + rounding) >> 8;
    ivec3 base = (d + bias) << shift;
    ivec3 value = subtract ? base - blended : base + blended;
    return scale_code == 3u ? value >> 1 : value;
}

int idot(ivec3 a, ivec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

const int kWraps[5] = int[5](256 << 7, 128 << 7, 64 << 7, 32 << 7, 16 << 7);
const int kFormatShifts[4] = int[4](0, 3, 4, 5);
const int kBumpShifts[4] = int[4](0, 5, 4, 3);

ivec2 fixpoint(uint coord) {
    return ivec2(select_uv(coord) * texdims[coord].zw * 128.0);
}

ivec4 sample_fixed(uint map, ivec2 coords, vec2 dx, vec2 dy, uint coord) {
    vec2 size = texdims[map].xy;
    vec2 ratio = texdims[coord].zw / size;
    return quantized(sample_map(map, vec2(coords) / (size * 128.0), dx * ratio, dy * ratio));
}

void main() {
    vec2 gradient_x[8];
    vec2 gradient_y[8];
    for (int g = 0; g < 8; g++) {
        vec2 value = projected(v_uv[g]);
        gradient_x[g] = dFdx(value);
        gradient_y[g] = dFdy(value);
    }
    ivec4 k[4] = ivec4[4](quantized(konst[0]), quantized(konst[1]), quantized(konst[2]), quantized(konst[3]));
    ivec4 r[4] = ivec4[4](quantized(initial[0]), quantized(initial[1]), quantized(initial[2]), quantized(initial[3]));
    ivec4 c0 = quantized(v_c0);
    ivec4 c1 = quantized(v_c1);
    uint count = header.x;
    if (count == 0u) {
        r[0] = c0;
    }
    uint indirect_stages = indirect.x & 7u;
    uint texgens = (indirect.x >> 8u) & 15u;
    ivec3 indirect_texel[4] = ivec3[4](ivec3(0), ivec3(0), ivec3(0), ivec3(0));
    for (uint j = 0u; j < indirect_stages && j < 4u; j++) {
        uint reference = indirect.y >> (6u * j);
        uint map = reference & 7u;
        uint coord = (reference >> 3u) & 7u;
        if (coord >= texgens) {
            coord = 0u;
        }
        uint scales = j < 2u ? indirect.z : indirect.w;
        uint shift_s = (scales >> ((j & 1u) * 8u)) & 15u;
        uint shift_t = (scales >> ((j & 1u) * 8u + 4u)) & 15u;
        ivec2 base = fixpoint(coord);
        indirect_texel[j] = sample_fixed(map, ivec2(base.x >> shift_s, base.y >> shift_t), gradient_x[coord], gradient_y[coord], coord).abg;
    }
    ivec2 tevcoord = ivec2(0);
    int alphabump = 0;
    for (uint i = 0u; i < count; i++) {
        uint ce = stage[i].x;
        uint ae = stage[i].y;
        uint order = stage[i].z;
        uint ksel = stage[i].w;
        uint coord = (order >> 3u) & 7u;
        if (coord >= texgens) {
            coord = 0u;
        }
        uint ind = tevind[i].x;
        uint bt = ind & 3u;
        uint format = (ind >> 2u) & 3u;
        uint bias = (ind >> 4u) & 7u;
        uint bump = (ind >> 7u) & 3u;
        uint matrix_index = (ind >> 9u) & 3u;
        uint matrix_id = (ind >> 11u) & 3u;
        uint wrap_s = (ind >> 13u) & 7u;
        uint wrap_t = (ind >> 16u) & 7u;
        bool has_stage = bt < indirect_stages;
        ivec2 base = fixpoint(coord);
        ivec2 offset = ivec2(0);
        if (has_stage && bump != 0u) {
            alphabump = (indirect_texel[bt][bump - 1u] << kBumpShifts[format]) & 248;
        }
        if (has_stage && matrix_index != 0u) {
            ivec3 crd = indirect_texel[bt] >> kFormatShifts[format];
            int add = format == 0u ? -128 : 1;
            if ((bias & 1u) != 0u) crd.x += add;
            if ((bias & 2u) != 0u) crd.y += add;
            if ((bias & 4u) != 0u) crd.z += add;
            uint m = 2u * (matrix_index - 1u);
            if (matrix_id == 0u) {
                offset = ivec2(idot(indmtx[m].xyz, crd), idot(indmtx[m + 1u].xyz, crd)) >> 3;
            } else if (matrix_id == 1u) {
                offset = (base * crd.xx) >> 8;
            } else if (matrix_id == 2u) {
                offset = (base * crd.yy) >> 8;
            }
            int shift = indmtx[m].w;
            offset = shift >= 0 ? (offset >> shift) : (offset << (-shift));
        }
        ivec2 wrapped;
        wrapped.x = wrap_s == 0u ? base.x : (wrap_s >= 6u ? 0 : (base.x & (kWraps[wrap_s - 1u] - 1)));
        wrapped.y = wrap_t == 0u ? base.y : (wrap_t >= 6u ? 0 : (base.y & (kWraps[wrap_t - 1u] - 1)));
        tevcoord = ((ind >> 20u) & 1u) != 0u ? tevcoord + wrapped + offset : wrapped + offset;
        tevcoord = (tevcoord << 8) >> 8;
        ivec4 tex = ivec4(255);
        if ((order & 0x40u) != 0u) {
            tex = texgens == 0u ? ivec4(0) : apply_swap(sample_fixed(order & 7u, tevcoord, gradient_x[coord], gradient_y[coord], coord), (ae >> 2u) & 3u);
        }
        ivec4 ras = ivec4(0);
        uint chan = (order >> 7u) & 7u;
        if (chan == 0u) {
            ras = c0;
        } else if (chan == 1u) {
            ras = c1;
        } else if (chan == 5u) {
            ras = ivec4(alphabump);
        } else if (chan == 6u) {
            ras = ivec4(alphabump | (alphabump >> 5));
        }
        ras = apply_swap(ras, ae & 3u);
        ivec3 kc = konst_color(ksel & 31u, k);
        int ka = konst_alpha((ksel >> 5u) & 31u, k);
        ivec3 a = color_input((ce >> 12u) & 15u, r, tex, ras, kc) & 255;
        ivec3 b = color_input((ce >> 8u) & 15u, r, tex, ras, kc) & 255;
        ivec3 c = color_input((ce >> 4u) & 15u, r, tex, ras, kc) & 255;
        ivec3 d = color_input(ce & 15u, r, tex, ras, kc);
        uint bias_code = (ce >> 16u) & 3u;
        uint scale_code = (ce >> 20u) & 3u;
        ivec3 color;
        if (bias_code == 3u) {
            color = d + compare_color(scale_code, ((ce >> 18u) & 1u) != 0u, a, b, c);
        } else {
            color = combine(a, b, c, d, bias_code, ((ce >> 18u) & 1u) != 0u, scale_code);
        }
        color = ((ce >> 19u) & 1u) != 0u ? clamp(color, 0, 255) : clamp(color, -1024, 1023);
        int aa = alpha_input((ae >> 13u) & 7u, r, tex, ras, ka) & 255;
        int ab = alpha_input((ae >> 10u) & 7u, r, tex, ras, ka) & 255;
        int ac = alpha_input((ae >> 7u) & 7u, r, tex, ras, ka) & 255;
        int ad = alpha_input((ae >> 4u) & 7u, r, tex, ras, ka);
        uint abias_code = (ae >> 16u) & 3u;
        uint ascale_code = (ae >> 20u) & 3u;
        int alpha;
        if (abias_code == 3u) {
            bool aequal = ((ae >> 18u) & 1u) != 0u;
            bool apass = ascale_code == 3u ? (aequal ? aa == ab : aa > ab) : compare_inputs(ascale_code, aequal, a, b);
            alpha = ad + (apass ? ac : 0);
        } else {
            alpha = combine(ivec3(aa, 0, 0), ivec3(ab, 0, 0), ivec3(ac, 0, 0), ivec3(ad, 0, 0), abias_code, ((ae >> 18u) & 1u) != 0u, ascale_code).x;
        }
        alpha = ((ae >> 19u) & 1u) != 0u ? clamp(alpha, 0, 255) : clamp(alpha, -1024, 1023);
        uint color_dest = (ce >> 22u) & 3u;
        uint alpha_dest = (ae >> 22u) & 3u;
        r[color_dest].rgb = color;
        r[alpha_dest].a = alpha;
    }
    ivec4 result = r[0];
    if (count > 0u) {
        uint last_color = (stage[count - 1u].x >> 22u) & 3u;
        uint last_alpha = (stage[count - 1u].y >> 22u) & 3u;
        result = ivec4(r[last_color].rgb, r[last_alpha].a);
    }
    result &= 255;
    if ((header.w & 2u) != 0u) {
        ivec2 dither = ivec2(gl_FragCoord.xy) & 1;
        result.rgb = (result.rgb - (result.rgb >> 6)) + (dither.x ^ dither.y) * 2 + dither.y;
    }
    uint fog_select = uint(fog_integer.x) & 15u;
    uint fog_type = fog_select >> 1u;
    if (fog_type != 0u) {
        int z = clamp(int(gl_FragCoord.z * 16777216.0), 0, 0xFFFFFF);
        float ze = (fog_select & 1u) == 0u ? (fog.x * 16777216.0) / float(fog_integer.y - (z >> fog_integer.w)) : fog.x * float(z) / 16777216.0;
        if ((fog_integer.x & 16) != 0) {
            float offset = (2.0 * (gl_FragCoord.x / fog.w)) - 1.0 - fog.z;
            float index = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
            uint lower = uint(index);
            uint upper = min(lower + 1u, 9u);
            float kr = mix(fog_range[lower >> 2u][lower & 3u], fog_range[upper >> 2u][upper & 3u], fract(index));
            ze *= sqrt(offset * offset + kr * kr) / kr;
        }
        float amount = clamp(ze - fog.y, 0.0, 1.0);
        if (fog_type == 4u) {
            amount = 1.0 - exp2(-8.0 * amount);
        } else if (fog_type == 5u) {
            amount = 1.0 - exp2(-8.0 * amount * amount);
        } else if (fog_type == 6u) {
            amount = exp2(-8.0 * (1.0 - amount));
        } else if (fog_type == 7u) {
            amount = exp2(-8.0 * (1.0 - amount) * (1.0 - amount));
        }
        int weight = int(roundEven(amount * 256.0));
        ivec3 fog_color = ivec3((fog_integer.z >> 16) & 255, (fog_integer.z >> 8) & 255, fog_integer.z & 255);
        result.rgb = (result.rgb * (256 - weight) + fog_color * weight) >> 8;
    }
    uint compare = header.y;
    uint value = uint(result.a);
    bool first = compare_alpha((compare >> 16u) & 7u, value, compare & 255u);
    bool second = compare_alpha((compare >> 19u) & 7u, value, (compare >> 8u) & 255u);
    uint logic = (compare >> 22u) & 3u;
    bool accepted = logic == 0u ? (first && second) : (logic == 1u ? (first || second) : (logic == 2u ? (first != second) : (first == second)));
    if (!accepted) {
        discard;
    }
    out_blend = vec4(result) / 255.0;
    uint source = (header.w >> 2u) & 3u;
    ivec4 written = source == 0u ? result : (source == 1u ? ivec4(0) : (source == 2u ? ivec4(255) : 255 - result));
    out_color = vec4(written) / 255.0;
    if ((header.z & 0x100u) != 0u) {
        out_color.a = float(header.z & 255u) / 255.0;
    }
    if ((header.w & 1u) != 0u) {
        out_color.rgb = vec3(written.rgb >> 2) / 63.0;
        out_color.a = float(((header.z & 0x100u) != 0u ? int(header.z & 255u) : written.a) >> 2) / 63.0;
    }
}
)GLSL";

struct Target {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct CachedTexture {
    GLuint texture = 0;
    bool owned = true;
    uint64_t hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t levels = 0;
    uint64_t verified_epoch = ~0ull;
    bool mipmaps = false;
    bool custom = false;
};

struct TexturePack {
    bool dump = false;
    bool load = false;
    std::filesystem::path dump_directory;
    custom_textures::Index index;
    std::unordered_set<std::string> dumped;
    std::unordered_map<std::string, GLuint> loaded;
};

struct CopiedTexture {
    Target converted;
    GLuint view = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t bytes = 0;
    uint64_t guest_hash = 0;
};

struct Program {
    GLuint id = 0;
    GLint flip = -1;
};

struct Device {
    Program tev;
    Program early_tev;
    Program present;
    Program overlay;
    Program copy;
    Program clear;
    GLint clear_color = -1;
    GLint clear_depth = -1;
    GLuint tev_layout = 0;
    GLuint empty_layout = 0;
    GLuint vertex_buffer = 0;
    GLuint constants = 0;
    GLuint copy_constants = 0;
    GLuint linear_sampler = 0;
    GLuint point_sampler = 0;
    GLuint efb_color = 0;
    GLuint efb_depth = 0;
    GLuint efb_framebuffer = 0;
    Target frame;
    Target write_back;
    GLuint overlay_texture = 0;
    std::vector<hud::Sprite> hud;     // placement (pixels dropped once uploaded)
    std::vector<GLuint> hud_textures;
    uint64_t hud_version = 0;
    uint32_t overlay_width = 0;
    uint32_t overlay_height = 0;
    uint64_t overlay_version = 0;
    bool overlay_visible = false;
    bool clip_control = false;
    std::map<uint64_t, GLuint> samplers;
    std::map<uint64_t, CachedTexture> textures;
    std::map<uint32_t, CopiedTexture> copies;
    bool failed = false;
    bool ready = false;
};

struct PendingBatch {
    std::vector<ScreenVertex> vertices;
    Constants constants;
    GLuint views[kTextureMaps];
    GLuint samplers[kTextureMaps];
    uint32_t blend = 0;
    uint32_t depth = 0;
    bool early_depth = false;
    uint32_t top_left = 0;
    uint32_t bottom_right = 0;
};

Device g_device;
PendingBatch g_pending;
TexturePack g_pack;
uint32_t g_custom_maps = 0;

void flush_pending();
bool g_log_gx = std::getenv("WP_LOG_GX") != nullptr;
bool g_logged_palette = false;
bool g_logged_copy_format = false;
uint64_t g_frame = 0;
uint64_t g_texture_epoch = 0;
int g_scale = 1;
uint32_t g_vertex_cursor = kVertexCapacity;
uint32_t g_batches = 0;
uint32_t g_batch_vertices = 0;
double g_batch_seconds = 0.0;

int scaled(int value) {
    return value * g_scale;
}

GLuint compile_stage(GLenum type, const std::string& source) {
    GLuint shader = gl.CreateShader(type);
    const char* text = source.c_str();
    gl.ShaderSource(shader, 1, &text, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        gl.GetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader compile error: %s\n", log);
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

bool link(Program& program, const std::string& vertex, const std::string& fragment) {
    GLuint vs = compile_stage(GL_VERTEX_SHADER, vertex);
    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        return false;
    }
    GLuint id = gl.CreateProgram();
    gl.AttachShader(id, vs);
    gl.AttachShader(id, fs);
    gl.LinkProgram(id);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        gl.GetProgramInfoLog(id, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader link error: %s\n", log);
        return false;
    }
    program.id = id;
    program.flip = gl.GetUniformLocation(id, "flip");
    return true;
}

void bind_samplers(GLuint program, std::initializer_list<const char*> names) {
    gl.UseProgram(program);
    GLint unit = 0;
    for (const char* name : names) {
        gl.Uniform1i(gl.GetUniformLocation(program, name), unit++);
    }
}

bool has_extension(const char* name) {
    GLint count = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; i++) {
        const char* extension = reinterpret_cast<const char*>(gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (extension && std::strcmp(extension, name) == 0) {
            return true;
        }
    }
    return false;
}

void set_texture_levels(GLuint texture, uint32_t levels) {
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels) - 1);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

bool create_render_texture(Target& target, uint32_t width, uint32_t height) {
    gl.GenTextures(1, &target.texture);
    set_texture_levels(target.texture, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &target.framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    target.width = width;
    target.height = height;
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void release_target(Target& target) {
    if (target.framebuffer) {
        gl.DeleteFramebuffers(1, &target.framebuffer);
    }
    if (target.texture) {
        gl.DeleteTextures(1, &target.texture);
    }
    target = Target{};
}

bool create_efb() {
    gl.GenTextures(1, &g_device.efb_color);
    set_texture_levels(g_device.efb_color, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, scaled(kEfbWidth), scaled(kEfbHeight), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenTextures(1, &g_device.efb_depth);
    gl.BindTexture(GL_TEXTURE_2D, g_device.efb_depth);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, scaled(kEfbWidth), scaled(kEfbHeight), 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    gl.GenFramebuffers(1, &g_device.efb_framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_device.efb_framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_device.efb_color, 0);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_device.efb_depth, 0);
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void release_efb() {
    if (g_device.efb_framebuffer) {
        gl.DeleteFramebuffers(1, &g_device.efb_framebuffer);
    }
    GLuint textures[2] = {g_device.efb_color, g_device.efb_depth};
    gl.DeleteTextures(2, textures);
    g_device.efb_framebuffer = 0;
    g_device.efb_color = 0;
    g_device.efb_depth = 0;
}

GLuint make_sampler(GLint min_filter, GLint mag_filter, GLint wrap) {
    GLuint sampler = 0;
    gl.GenSamplers(1, &sampler);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, mag_filter);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wrap);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wrap);
    return sampler;
}

bool create_pipeline() {
    GLint major = 0;
    GLint minor = 0;
    gl.GetIntegerv(GL_MAJOR_VERSION, &major);
    gl.GetIntegerv(GL_MINOR_VERSION, &minor);
    int version = major * 10 + minor;
    if (version >= 45 || has_extension("GL_ARB_clip_control")) {
        gl.ClipControl = reinterpret_cast<decltype(gl.ClipControl)>(SDL_GL_GetProcAddress("glClipControl"));
    }
    if (gl.ClipControl) {
        gl.ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        g_device.clip_control = true;
    }
    std::string fragment = std::string("#version 410 core\n") + kTevFragment;
    if (!link(g_device.tev, kTevVertex, fragment)) {
        return false;
    }
    if (has_extension("GL_ARB_shader_image_load_store")) {
        std::string early = std::string("#version 410 core\n#extension GL_ARB_shader_image_load_store : require\nlayout(early_fragment_tests) in;\n") + kTevFragment;
        if (!link(g_device.early_tev, kTevVertex, early)) {
            return false;
        }
    } else {
        g_device.early_tev = g_device.tev;
    }
    for (Program* program : {&g_device.tev, &g_device.early_tev}) {
        bind_samplers(program->id, {"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7"});
        gl.Uniform1i(gl.GetUniformLocation(program->id, "depth_zero_to_one"), g_device.clip_control ? 1 : 0);
        gl.UniformBlockBinding(program->id, gl.GetUniformBlockIndex(program->id, "Constants"), 0);
    }
    if (!link(g_device.present, kFullscreenVertex, kPresentFragment) || !link(g_device.overlay, kFullscreenVertex, kOverlayFragment) ||
        !link(g_device.copy, kFullscreenVertex, kCopyFragment) || !link(g_device.clear, kFullscreenVertex, kClearFragment)) {
        return false;
    }
    bind_samplers(g_device.present.id, {"frame"});
    bind_samplers(g_device.overlay.id, {"overlay"});
    bind_samplers(g_device.copy.id, {"color_source", "depth_source"});
    gl.UniformBlockBinding(g_device.copy.id, gl.GetUniformBlockIndex(g_device.copy.id, "CopyConstants"), 1);
    g_device.clear_color = gl.GetUniformLocation(g_device.clear.id, "clear_color");
    g_device.clear_depth = gl.GetUniformLocation(g_device.clear.id, "clear_depth");
    gl.GenVertexArrays(1, &g_device.empty_layout);
    gl.GenVertexArrays(1, &g_device.tev_layout);
    gl.GenBuffers(1, &g_device.vertex_buffer);
    gl.BindVertexArray(g_device.tev_layout);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_device.vertex_buffer);
    gl.BufferData(GL_ARRAY_BUFFER, kVertexCapacity * sizeof(ScreenVertex), nullptr, GL_STREAM_DRAW);
    auto attribute = [](GLuint index, GLint size, size_t offset) {
        gl.EnableVertexAttribArray(index);
        gl.VertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, sizeof(ScreenVertex), reinterpret_cast<const void*>(offset));
    };
    attribute(0, 4, 0);
    attribute(1, 4, 16);
    attribute(2, 4, 32);
    for (GLuint i = 0; i < 8; i++) {
        attribute(3 + i, 3, 48 + 12 * i);
    }
    gl.GenBuffers(1, &g_device.constants);
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_device.constants);
    gl.BufferData(GL_UNIFORM_BUFFER, sizeof(Constants), nullptr, GL_STREAM_DRAW);
    gl.GenBuffers(1, &g_device.copy_constants);
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_device.copy_constants);
    gl.BufferData(GL_UNIFORM_BUFFER, 80, nullptr, GL_STREAM_DRAW);
    g_device.linear_sampler = make_sampler(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE);
    g_device.point_sampler = make_sampler(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.Enable(GL_DEPTH_CLAMP);
    return true;
}

void prepare_texture_pack() {
    const char* directory = game::description().textures_directory;
    if (!directory || !*directory) {
        return;
    }
    std::filesystem::path base = std::filesystem::path(directory);
    g_pack.dump = settings::flag("video.dump_textures", "WP_DUMP_TEXTURES");
    g_pack.dump_directory = base / "dump";
    if (settings::flag("video.custom_textures", "WP_CUSTOM_TEXTURES")) {
        size_t count = g_pack.index.scan(base / "load");
        g_pack.load = count > 0;
        if (count > 0) {
            std::fprintf(stderr, "custom textures: %zu files in %s/load\n", count, directory);
        }
    }
    if (g_pack.dump) {
        std::fprintf(stderr, "dumping textures to %s/dump\n", directory);
    }
}

int requested_scale() {
    static const settings::LiveNumber scale("video.scale", "WP_SCALE");
    return std::clamp(scale(), 1, 6);
}

bool initialize() {
    if (g_device.ready) {
        return true;
    }
    if (g_device.failed) {
        return false;
    }
    g_scale = requested_scale();
    if (!video::make_gl_current() || !load_functions() || !create_pipeline() || !create_efb()) {
        std::fputs("cannot initialise the OpenGL renderer\n", stderr);
        g_device.failed = true;
        return false;
    }
    std::fprintf(stderr, "OpenGL renderer: %s, %s\n", reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)),
                 reinterpret_cast<const char*>(gl.GetString(GL_VERSION)));
    prepare_texture_pack();
    g_device.ready = true;
    return true;
}

std::string g_copy_dump_prefix;
int g_copy_dump_index = 0;
FILE* g_texture_notes = nullptr;
std::map<uint64_t, bool> g_noted_textures;

void note_texture(uint32_t map, uint32_t address, uint32_t width, uint32_t height, uint32_t format, const char* source) {
    if (!g_texture_notes) {
        return;
    }
    uint64_t key = (static_cast<uint64_t>(address) << 24) ^ (static_cast<uint64_t>(width) << 12) ^ height ^ (static_cast<uint64_t>(format) << 56) ^
                   (static_cast<uint64_t>(map) << 60);
    if (g_noted_textures[key]) {
        return;
    }
    g_noted_textures[key] = true;
    std::fprintf(g_texture_notes, "map %u address %08x %ux%u format %u: %s\n", map, address, width, height, format, source);
}

GLuint upload_levels(const std::vector<std::vector<uint32_t>>& images, uint32_t width, uint32_t height, bool generate) {
    GLuint texture = 0;
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    for (size_t level = 0; level < images.size(); level++) {
        gl.TexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA8, static_cast<GLsizei>(std::max(width >> level, 1u)),
                      static_cast<GLsizei>(std::max(height >> level, 1u)), 0, GL_RGBA, GL_UNSIGNED_BYTE, images[level].data());
    }
    if (generate) {
        uint32_t levels = 1;
        while ((width >> levels) != 0 || (height >> levels) != 0) {
            levels++;
        }
        set_texture_levels(texture, levels);
        gl.GenerateMipmap(GL_TEXTURE_2D);
    } else {
        set_texture_levels(texture, static_cast<uint32_t>(images.size()));
    }
    return texture;
}

GLuint custom_texture(const std::string& key, bool mipmaps) {
    auto [found, inserted] = g_pack.loaded.try_emplace(key, 0);
    if (!inserted) {
        return found->second;
    }
    const std::filesystem::path* path = g_pack.index.find(key);
    std::vector<std::vector<uint32_t>> images(1);
    uint32_t width = 0;
    uint32_t height = 0;
    if (!path || !custom_textures::decode_png(*path, images[0], width, height)) {
        std::fprintf(stderr, "custom texture %s cannot be read, using the original\n", key.c_str());
        return 0;
    }
    if (mipmaps) {
        for (uint32_t level = 1; (width >> level) != 0 || (height >> level) != 0; level++) {
            const std::filesystem::path* level_path = g_pack.index.find(custom_textures::level_name(key, level));
            std::vector<uint32_t> pixels;
            uint32_t level_width = 0;
            uint32_t level_height = 0;
            if (!level_path || !custom_textures::decode_png(*level_path, pixels, level_width, level_height) ||
                level_width != std::max(width >> level, 1u) || level_height != std::max(height >> level, 1u)) {
                break;
            }
            images.push_back(std::move(pixels));
        }
    }
    bool generate = mipmaps && images.size() == 1 && (width > 1 || height > 1);
    found->second = upload_levels(images, width, height, generate);
    return found->second;
}

void dump_texture(const custom_textures::TextureName& name, const std::vector<std::vector<uint32_t>>& pixels, uint32_t width, uint32_t height) {
    std::string full = name.full();
    if (!g_pack.dumped.insert(full).second) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(g_pack.dump_directory, error);
    for (uint32_t level = 0; level < pixels.size(); level++) {
        std::filesystem::path path = g_pack.dump_directory / (custom_textures::level_name(full, level) + ".png");
        if (!std::filesystem::exists(path, error)) {
            video::save_png_rgba(path.string().c_str(), pixels[level], std::max(width >> level, 1u), std::max(height >> level, 1u));
        }
    }
}

void release_cached(CachedTexture& entry) {
    if (entry.texture && entry.owned) {
        gl.DeleteTextures(1, &entry.texture);
    }
    entry.texture = 0;
}

GLuint texture_for(uint32_t map) {
    g_custom_maps &= ~(1u << map);
    TextureRequest request = describe_texture(map);
    if (!request.supported && !g_logged_palette) {
        g_logged_palette = true;
        std::fprintf(stderr, "unsupported texture format %u\n", request.format);
    }
    if (!request.valid) {
        return 0;
    }
    uint32_t address = request.address;
    uint32_t width = request.width;
    uint32_t height = request.height;
    uint32_t format = request.format;
    uint32_t levels = request.levels;
    const uint8_t* source = request.source;
    auto copied = g_device.copies.find(address);
    if (copied != g_device.copies.end() && copied->second.logical_width == width && copied->second.logical_height == height && format <= 6 &&
        copied->second.guest_hash == sample_hash(source, copied->second.bytes)) {
        note_texture(map, address, width, height, format, "EFB copy");
        return copied->second.view;
    }
    if (copied != g_device.copies.end()) {
        char reason[160];
        std::snprintf(reason, sizeof reason, "EFB copy rejected (copy %ux%u, hash %s), read from RAM", copied->second.logical_width, copied->second.logical_height,
                      copied->second.guest_hash == sample_hash(source, copied->second.bytes) ? "same" : "changed");
        note_texture(map, address, width, height, format, reason);
    } else {
        note_texture(map, address, width, height, format, "RAM");
    }
    bool mipmaps = request.mipmaps;
    CachedTexture& entry = g_device.textures[request.key];
    auto use = [&](CachedTexture& texture) {
        if (texture.custom) {
            g_custom_maps |= 1u << map;
        }
        return texture.texture;
    };
    auto matches = [&]() {
        return entry.texture && entry.width == width && entry.height == height && entry.format == format && entry.levels == levels && entry.mipmaps == mipmaps;
    };
    if (matches() && entry.verified_epoch == g_texture_epoch) {
        return use(entry);
    }
    uint64_t hash = texture_hash(request);
    if (matches() && entry.hash == hash) {
        entry.verified_epoch = g_texture_epoch;
        return use(entry);
    }
    flush_pending();
    release_cached(entry);
    entry.custom = false;
    entry.owned = true;
    auto finish = [&]() {
        entry.hash = hash;
        entry.width = width;
        entry.height = height;
        entry.format = format;
        entry.levels = levels;
        entry.mipmaps = mipmaps;
        entry.verified_epoch = g_texture_epoch;
        return use(entry);
    };
    bool named = request.supported && copied == g_device.copies.end() && (g_pack.dump || g_pack.load);
    custom_textures::TextureName name;
    GLuint custom = 0;
    if (named) {
        name = custom_textures::name_texture(source, request.size, width, height, format, mipmaps, request.tlut);
        if (g_pack.load) {
            std::string resolved = g_pack.index.resolve(name);
            if (!resolved.empty()) {
                custom = custom_texture(resolved, mipmaps);
            }
        }
    }
    if (custom && !g_pack.dump) {
        entry.texture = custom;
        entry.owned = false;
        entry.custom = true;
        return finish();
    }
    std::vector<std::vector<uint32_t>> pixels = decode_levels(request);
    if (named && g_pack.dump) {
        dump_texture(name, pixels, width, height);
    }
    if (custom) {
        entry.texture = custom;
        entry.owned = false;
        entry.custom = true;
        return finish();
    }
    pixels.resize(levels);
    entry.texture = upload_levels(pixels, width, height, false);
    return finish();
}

GLint gl_wrap(uint32_t wrap) {
    return wrap == 1 ? GL_REPEAT : wrap == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

GLuint sampler_for(uint32_t map) {
    SamplerParams params = sampler_params(map, (g_custom_maps >> map) & 1);
    auto found = g_device.samplers.find(params.key);
    if (found != g_device.samplers.end()) {
        return found->second;
    }
    GLint min_filter = params.mip_mode == 2 ? (params.min_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                                            : (params.min_linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
    GLuint sampler = 0;
    gl.GenSamplers(1, &sampler);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, params.mag_linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, gl_wrap(params.wrap_s));
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, gl_wrap(params.wrap_t));
    gl.SamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, params.min_lod);
    gl.SamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, params.unlimited_lod ? 1000.0f : params.max_lod);
    gl.SamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, params.lod_bias);
    g_device.samplers[params.key] = sampler;
    return sampler;
}

GLenum gl_factor(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero:
        return GL_ZERO;
    case BlendFactor::One:
        return GL_ONE;
    case BlendFactor::SourceColor:
        return GL_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SourceAlpha:
        return GL_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return GL_DST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::DestinationAlpha:
        return GL_DST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return GL_ONE_MINUS_DST_ALPHA;
    case BlendFactor::TevColor:
        return GL_SRC1_COLOR;
    case BlendFactor::TevAlpha:
        return GL_SRC1_ALPHA;
    case BlendFactor::InverseTevAlpha:
        return GL_ONE_MINUS_SRC1_ALPHA;
    }
    return GL_ONE;
}

GLenum gl_operation(BlendOperation operation) {
    switch (operation) {
    case BlendOperation::Subtract:
        return GL_FUNC_SUBTRACT;
    case BlendOperation::ReverseSubtract:
        return GL_FUNC_REVERSE_SUBTRACT;
    default:
        return GL_FUNC_ADD;
    }
}

void apply_blend(uint32_t word) {
    static std::map<uint32_t, BlendState> cache;
    auto found = cache.find(word);
    if (found == cache.end()) {
        BlendState state = blend_state_for(word);
        if (state.logic_op && g_log_gx) {
            std::fprintf(stderr, "GX logic op %s (%u), %s, blend mode %06x\n", logic_op_name(state.logic_mode), state.logic_mode,
                         logic_op_exact(state.logic_mode) ? "exact" : "approximated with blending", word);
        }
        found = cache.emplace(word, state).first;
    }
    const BlendState& blend = found->second;
    if (blend.enable) {
        gl.Enable(GL_BLEND);
        gl.BlendFuncSeparate(gl_factor(blend.source), gl_factor(blend.destination), gl_factor(blend.source_alpha), gl_factor(blend.destination_alpha));
        gl.BlendEquationSeparate(gl_operation(blend.operation), gl_operation(blend.alpha_operation));
    } else {
        gl.Disable(GL_BLEND);
    }
    gl.ColorMask(blend.color_update, blend.color_update, blend.color_update, blend.alpha_update);
}

void apply_depth(uint32_t word) {
    if (word & 1) {
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(GL_NEVER + ((word >> 1) & 7));
        gl.DepthMask((word & (1u << 4)) ? GL_TRUE : GL_FALSE);
    } else {
        gl.Disable(GL_DEPTH_TEST);
    }
}

void bind_efb() {
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_device.efb_framebuffer);
    gl.Viewport(0, 0, scaled(kEfbWidth), scaled(kEfbHeight));
}

void flush_pending() {
    PendingBatch& pending = g_pending;
    if (pending.vertices.empty()) {
        return;
    }
    auto started = std::chrono::steady_clock::now();
    g_batches++;
    g_batch_vertices += static_cast<uint32_t>(pending.vertices.size());
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_device.constants);
    gl.BufferData(GL_UNIFORM_BUFFER, sizeof(Constants), &pending.constants, GL_STREAM_DRAW);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 0, g_device.constants);
    bind_efb();
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        gl.ActiveTexture(GL_TEXTURE0 + map);
        gl.BindTexture(GL_TEXTURE_2D, pending.views[map]);
        gl.BindSampler(map, pending.samplers[map]);
    }
    int left = static_cast<int>(((pending.top_left >> 12) & 0x7FF) - kScissorOffset);
    int top = static_cast<int>((pending.top_left & 0x7FF) - kScissorOffset);
    int right = static_cast<int>(((pending.bottom_right >> 12) & 0x7FF) - kScissorOffset + 1);
    int bottom = static_cast<int>((pending.bottom_right & 0x7FF) - kScissorOffset + 1);
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, kEfbWidth);
    bottom = std::min(bottom, kEfbHeight);
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(scaled(left), scaled(top), std::max(0, scaled(right - left)), std::max(0, scaled(bottom - top)));
    apply_blend(pending.blend);
    apply_depth(pending.depth);
    gl.UseProgram(pending.early_depth ? g_device.early_tev.id : g_device.tev.id);
    gl.BindVertexArray(g_device.tev_layout);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_device.vertex_buffer);
    size_t total = pending.vertices.size();
    size_t offset = 0;
    while (offset < total) {
        uint32_t batch = static_cast<uint32_t>(std::min<size_t>(total - offset, kVertexCapacity));
        if (g_vertex_cursor + batch > kVertexCapacity) {
            gl.BufferData(GL_ARRAY_BUFFER, kVertexCapacity * sizeof(ScreenVertex), nullptr, GL_STREAM_DRAW);
            g_vertex_cursor = 0;
        }
        gl.BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(g_vertex_cursor) * sizeof(ScreenVertex), batch * sizeof(ScreenVertex),
                         pending.vertices.data() + offset);
        gl.DrawArrays(GL_TRIANGLES, static_cast<GLint>(g_vertex_cursor), static_cast<GLsizei>(batch));
        g_vertex_cursor += batch;
        offset += batch;
    }
    pending.vertices.clear();
    g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void plain_state() {
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.BindVertexArray(g_device.empty_layout);
}

void use_fullscreen(const Program& program, float flip) {
    gl.UseProgram(program.id);
    gl.Uniform1f(program.flip, flip);
}

}

const char* api_name() {
    return "OpenGL";
}

void draw(const ScreenVertex* vertices, uint32_t count) {
    if (count == 0 || !initialize()) {
        return;
    }
    const uint32_t* bp = bp_registers();
    Constants constants;
    fill_constants(constants, g_scale);
    GLuint views[kTextureMaps] = {};
    GLuint samplers[kTextureMaps] = {};
    uint32_t used_maps = 0;
    for (uint32_t i = 0; i < constants.header[0] && i < kMaxStages; i++) {
        if (constants.stage[i][2] & 0x40) {
            used_maps |= 1u << (constants.stage[i][2] & 7);
        }
    }
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        if (used_maps & (1u << map)) {
            views[map] = texture_for(map);
            samplers[map] = sampler_for(map);
        }
    }
    uint32_t pixel_format = bp[0x43] & 7;
    AlphaTestResult alpha_test = alpha_test_result(bp[0xF3]);
    uint32_t blend = (bp[0x41] & 0xFFFF) | (static_cast<uint32_t>(pixel_format == 1) << 16) | (((bp[0x42] >> 8) & 1) << 17) |
                     (static_cast<uint32_t>(!alpha_test.can_pass) << 18);
    uint32_t depth = bp[0x40] & 0x1F;
    bool early_depth = (depth & 1) != 0 && (bp[0x43] & (1u << 6)) != 0 && alpha_test.can_pass && alpha_test.can_fail;
    PendingBatch& pending = g_pending;
    bool same = !pending.vertices.empty() && pending.blend == blend && pending.depth == depth && pending.early_depth == early_depth && pending.top_left == bp[0x20] &&
                pending.bottom_right == bp[0x21] && std::memcmp(&pending.constants, &constants, sizeof constants) == 0 &&
                std::memcmp(pending.views, views, sizeof views) == 0 && std::memcmp(pending.samplers, samplers, sizeof samplers) == 0;
    if (!same) {
        flush_pending();
        pending.constants = constants;
        std::memcpy(pending.views, views, sizeof views);
        std::memcpy(pending.samplers, samplers, sizeof samplers);
        pending.blend = blend;
        pending.depth = depth;
        pending.early_depth = early_depth;
        pending.top_left = bp[0x20];
        pending.bottom_right = bp[0x21];
    }
    pending.vertices.insert(pending.vertices.end(), vertices, vertices + count);
}

void take_statistics(uint32_t& batches, uint32_t& vertices, double& seconds) {
    batches = g_batches;
    vertices = g_batch_vertices;
    seconds = g_batch_seconds;
    g_batches = 0;
    g_batch_vertices = 0;
    g_batch_seconds = 0.0;
}

bool guest_range_valid(uint32_t address, size_t size) {
    return static_cast<size_t>(address & kAddressMask) + size <= kPhysicalSize;
}

void invalidate_textures() {
    g_texture_epoch++;
}

void release_copy(CopiedTexture& entry) {
    release_target(entry.converted);
    entry.view = 0;
}

void dump_copy(const CopiedTexture& entry);

bool run_copy(const Target& target, uint32_t target_width, uint32_t target_height, int x, int y, int width, int height, uint32_t format, bool intensity,
              bool alpha, bool depth, const CopyFilter& filter) {
    struct CopyConstants {
        uint32_t params[4];
        float region[4];
        uint32_t filter[4];
        float rows[4];
        float extent[4];
    } constants{};
    uint32_t sum = filter.coefficients[0] + filter.coefficients[1] + filter.coefficients[2];
    int efb_height = scaled(kEfbHeight);
    int top = filter.clamp_top ? scaled(y) : 0;
    int bottom = (filter.clamp_bottom ? scaled(y + height) : efb_height) - 1;
    constants.params[0] = format;
    constants.params[1] = intensity ? 1u : 0u;
    constants.params[2] = alpha ? 1u : 0u;
    constants.params[3] = depth ? 1u : 0u;
    constants.region[0] = static_cast<float>(scaled(x));
    constants.region[1] = static_cast<float>(scaled(y));
    constants.region[2] = static_cast<float>(scaled(x + width) - scaled(x));
    constants.region[3] = static_cast<float>(scaled(y + height) - scaled(y));
    constants.filter[0] = filter.coefficients[0];
    constants.filter[1] = filter.coefficients[1];
    constants.filter[2] = filter.coefficients[2];
    constants.filter[3] = sum >= 128 ? 1u : 0u;
    constants.rows[0] = 1.0f / filter.gamma;
    constants.rows[1] = static_cast<float>(top) + 0.5f;
    constants.rows[2] = static_cast<float>(bottom) + 0.5f;
    constants.extent[0] = static_cast<float>(scaled(kEfbWidth));
    constants.extent[1] = static_cast<float>(efb_height);
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_device.copy_constants);
    gl.BufferData(GL_UNIFORM_BUFFER, sizeof constants, &constants, GL_STREAM_DRAW);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 1, g_device.copy_constants);
    gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    gl.Viewport(0, 0, static_cast<GLsizei>(target_width), static_cast<GLsizei>(target_height));
    plain_state();
    use_fullscreen(g_device.copy, -1.0f);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_device.efb_color);
    gl.BindSampler(0, g_device.linear_sampler);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, g_device.efb_depth);
    gl.BindSampler(1, g_device.point_sampler);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    return true;
}

bool write_copy_to_ram(uint32_t address, uint32_t stride, int x, int y, int width, int height, uint32_t logical_width, uint32_t logical_height, uint32_t format,
                       bool intensity, bool alpha, bool depth, const CopyFilter& filter) {
    if (!g_device.write_back.framebuffer && !create_render_texture(g_device.write_back, kEfbWidth, kEfbHeight)) {
        return false;
    }
    uint32_t texture_format = copy_texture_format(format);
    Layout layout = layout_for(texture_format);
    uint32_t blocks_x = (logical_width + layout.block_width - 1) / layout.block_width;
    uint32_t blocks_y = (logical_height + layout.block_height - 1) / layout.block_height;
    uint32_t row_bytes = stride * 32;
    uint32_t bytes = block_bytes(texture_format);
    if (blocks_x * bytes > row_bytes || !guest_range_valid(address, static_cast<size_t>(blocks_y - 1) * row_bytes + blocks_x * bytes)) {
        return false;
    }
    if (!run_copy(g_device.write_back, logical_width, logical_height, x, y, width, height, format, intensity, alpha, depth, filter)) {
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(logical_width) * logical_height * 4);
    gl.ReadPixels(0, 0, static_cast<GLsizei>(logical_width), static_cast<GLsizei>(logical_height), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    size_t pitch = static_cast<size_t>(logical_width) * 4;
    uint8_t block[64];
    for (uint32_t by = 0; by < blocks_y; by++) {
        uint8_t* row = host(address + by * row_bytes);
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            std::memset(block, 0, sizeof block);
            for (uint32_t ty = 0; ty < layout.block_height; ty++) {
                uint32_t py = std::min(by * layout.block_height + ty, logical_height - 1);
                for (uint32_t tx = 0; tx < layout.block_width; tx++) {
                    uint32_t px = std::min(bx * layout.block_width + tx, logical_width - 1);
                    encode_texel(block, texture_format, ty * layout.block_width + tx, pixels.data() + py * pitch + px * 4);
                }
            }
            std::memcpy(row + bx * bytes, block, bytes);
        }
    }
    return true;
}

void copy_to_texture(uint32_t address, uint32_t stride, int x, int y, int width, int height, bool half, uint32_t format, bool intensity, bool depth, bool alpha,
                     const CopyFilter& filter) {
    if (!initialize()) {
        return;
    }
    if (format > 12) {
        if (!g_logged_copy_format) {
            g_logged_copy_format = true;
            std::fprintf(stderr, "unsupported EFB copy format %u\n", format);
        }
        auto stale = g_device.copies.find(address);
        if (stale != g_device.copies.end()) {
            release_copy(stale->second);
            g_device.copies.erase(stale);
        }
        return;
    }
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    flush_pending();
    CopiedTexture& entry = g_device.copies[address];
    uint32_t w = static_cast<uint32_t>(width);
    uint32_t h = static_cast<uint32_t>(height);
    entry.logical_width = half ? std::max(1u, w / 2) : w;
    entry.logical_height = half ? std::max(1u, h / 2) : h;
    uint32_t target_width = std::max(1u, entry.logical_width * g_scale);
    uint32_t target_height = std::max(1u, entry.logical_height * g_scale);
    if (!entry.converted.framebuffer || entry.converted.width != target_width || entry.converted.height != target_height) {
        release_copy(entry);
        if (!create_render_texture(entry.converted, target_width, target_height)) {
            release_copy(entry);
            g_device.copies.erase(address);
            return;
        }
    }
    run_copy(entry.converted, target_width, target_height, x, y, width, height, format, intensity, alpha || depth, depth, filter);
    entry.view = entry.converted.texture;
    write_copy_to_ram(address, stride, x, y, width, height, entry.logical_width, entry.logical_height, format, intensity, alpha || depth, depth, filter);
    entry.bytes = entry.logical_width * entry.logical_height * copy_bits(format) / 8;
    entry.guest_hash = guest_range_valid(address, entry.bytes) ? sample_hash(host(address), entry.bytes) : 0;
    dump_copy(entry);
}

void copy_to_framebuffer(int x, int y, int width, int height, bool depth, const CopyFilter& filter) {
    if (!initialize()) {
        return;
    }
    flush_pending();
    g_frame++;
    g_texture_epoch++;
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    uint32_t physical_width = static_cast<uint32_t>(scaled(width));
    uint32_t physical_height = static_cast<uint32_t>(scaled(height));
    if (!g_device.frame.framebuffer || g_device.frame.width != physical_width || g_device.frame.height != physical_height) {
        release_target(g_device.frame);
        if (!create_render_texture(g_device.frame, physical_width, physical_height)) {
            release_target(g_device.frame);
            return;
        }
    }
    run_copy(g_device.frame, physical_width, physical_height, x, y, width, height, kXfbFormat, false, false, depth, filter);
}

void apply_scale() {
    int scale = requested_scale();
    if (scale == g_scale) {
        return;
    }
    flush_pending();
    int previous = g_scale;
    release_efb();
    g_scale = scale;
    if (!create_efb()) {
        release_efb();
        g_scale = previous;
        if (!create_efb()) {
            std::fputs("cannot recreate the EFB after a scale change\n", stderr);
            g_device.ready = false;
            g_device.failed = true;
            return;
        }
    }
    bind_efb();
    plain_state();
    gl.DepthMask(GL_TRUE);
    gl.ClearColor(0, 0, 0, 0);
    gl.ClearDepth(1.0);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void upload_overlay(const options::Image& image) {
    if (image.pixels.size() != static_cast<size_t>(image.width) * image.height || image.width == 0 || image.height == 0) {
        return;
    }
    if (!g_device.overlay_texture) {
        gl.GenTextures(1, &g_device.overlay_texture);
        set_texture_levels(g_device.overlay_texture, 1);
    }
    gl.BindTexture(GL_TEXTURE_2D, g_device.overlay_texture);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0, GL_BGRA, GL_UNSIGNED_BYTE,
                  image.pixels.data());
    g_device.overlay_width = image.width;
    g_device.overlay_height = image.height;
}

// HUD sprites over the game picture (placed in its viewport, GL's y up).
void draw_hud(int x, int y, int width, int height) {
    std::vector<hud::Sprite> sprites;
    if (hud::take(g_device.hud_version, sprites)) {
        if (!g_device.hud_textures.empty()) {
            gl.DeleteTextures(static_cast<GLsizei>(g_device.hud_textures.size()), g_device.hud_textures.data());
        }
        g_device.hud_textures.clear();
        g_device.hud.clear();
        for (hud::Sprite& sprite : sprites) {
            const options::Image& image = sprite.image;
            if (image.width == 0 || image.height == 0 || image.pixels.size() != static_cast<size_t>(image.width) * image.height) {
                continue;
            }
            GLuint texture = 0;
            gl.GenTextures(1, &texture);
            set_texture_levels(texture, 1);
            gl.BindTexture(GL_TEXTURE_2D, texture);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0, GL_BGRA,
                          GL_UNSIGNED_BYTE, image.pixels.data());
            sprite.image.pixels.clear();
            g_device.hud_textures.push_back(texture);
            g_device.hud.push_back(std::move(sprite));
        }
    }
    hud::set_picture_size(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    if (g_device.hud.empty()) {
        return;
    }
    gl.Enable(GL_BLEND);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    use_fullscreen(g_device.overlay, 1.0f);
    for (size_t i = 0; i < g_device.hud.size(); i++) {
        const hud::Sprite& sprite = g_device.hud[i];
        int w = static_cast<int>(sprite.width * static_cast<float>(width));
        int h = static_cast<int>(sprite.height * static_cast<float>(height));
        int left = x + static_cast<int>(sprite.x * static_cast<float>(width));
        int top = static_cast<int>(sprite.y * static_cast<float>(height));
        gl.Viewport(left, y + height - top - h, w, h);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, g_device.hud_textures[i]);
        gl.BindSampler(0, g_device.linear_sampler);
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
    }
    gl.Disable(GL_BLEND);
}

void draw_overlay(int client_width, int client_height) {
    options::Image image;
    if (options::take_overlay(g_device.overlay_version, image, g_device.overlay_visible) && !image.pixels.empty()) {
        upload_overlay(image);
    }
    if (!g_device.overlay_visible || !g_device.overlay_texture) {
        return;
    }
    options::Rect placed = options::place(client_width, client_height, static_cast<int>(g_device.overlay_width), static_cast<int>(g_device.overlay_height));
    if (placed.width <= 0 || placed.height <= 0) {
        return;
    }
    gl.Viewport(placed.x, client_height - placed.y - placed.height, placed.width, placed.height);
    gl.Enable(GL_BLEND);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    use_fullscreen(g_device.overlay, 1.0f);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_device.overlay_texture);
    gl.BindSampler(0, g_device.linear_sampler);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.Disable(GL_BLEND);
}

bool present_frame(void* window_handle, double aspect) {
    if (!g_device.ready || !g_device.frame.texture || !window_handle) {
        return false;
    }
    SDL_Window* window = static_cast<SDL_Window*>(window_handle);
    int client_width = 0;
    int client_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &client_width, &client_height) || client_width <= 0 || client_height <= 0) {
        return false;
    }
    flush_pending();
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    plain_state();
    gl.Viewport(0, 0, client_width, client_height);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    double width = client_width;
    double height = width / aspect;
    if (height > client_height) {
        height = client_height;
        width = height * aspect;
    }
    gl.Viewport(static_cast<GLint>((client_width - width) / 2), static_cast<GLint>((client_height - height) / 2), static_cast<GLsizei>(width),
                static_cast<GLsizei>(height));
    use_fullscreen(g_device.present, 1.0f);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_device.frame.texture);
    gl.BindSampler(0, g_device.linear_sampler);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    draw_hud(static_cast<int>((client_width - width) / 2), static_cast<int>((client_height - height) / 2), static_cast<int>(width), static_cast<int>(height));
    draw_overlay(client_width, client_height);
    SDL_GL_SwapWindow(window);
    apply_scale();
    return true;
}

bool read_target(GLuint framebuffer, uint32_t width, uint32_t height, std::vector<uint32_t>& pixels) {
    std::vector<uint8_t> bytes(static_cast<size_t>(width) * height * 4);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.ReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
    pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = (static_cast<uint32_t>(bytes[i * 4]) << 16) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8) | bytes[i * 4 + 2];
    }
    return true;
}

void dump_target(GLuint framebuffer, uint32_t width, uint32_t height, const char* suffix) {
    std::vector<uint32_t> pixels;
    if (framebuffer && read_target(framebuffer, width, height, pixels)) {
        std::string path = g_copy_dump_prefix + "_copy" + std::to_string(g_copy_dump_index) + suffix + ".png";
        video::save_png(path.c_str(), pixels, width, height);
    }
}

void dump_copy(const CopiedTexture& entry) {
    if (g_copy_dump_prefix.empty()) {
        return;
    }
    g_copy_dump_index++;
    dump_target(g_device.efb_framebuffer, static_cast<uint32_t>(scaled(kEfbWidth)), static_cast<uint32_t>(scaled(kEfbHeight)), "_efb");
    dump_target(entry.converted.framebuffer, entry.converted.width, entry.converted.height, "");
}

void dump_copies(const char* prefix) {
    g_copy_dump_prefix = prefix ? prefix : "";
    g_copy_dump_index = 0;
    if (g_texture_notes) {
        std::fclose(g_texture_notes);
        g_texture_notes = nullptr;
    }
    g_noted_textures.clear();
    if (prefix) {
        g_texture_notes = std::fopen((g_copy_dump_prefix + "_textures.txt").c_str(), "w");
    }
}

bool read_frame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
    if (!g_device.ready || !g_device.frame.framebuffer) {
        return false;
    }
    flush_pending();
    width = g_device.frame.width;
    height = g_device.frame.height;
    return read_target(g_device.frame.framebuffer, width, height, pixels);
}

void clear(int x, int y, int width, int height) {
    if (!initialize()) {
        return;
    }
    flush_pending();
    const uint32_t* bp = bp_registers();
    uint32_t pixel_format = bp[0x43] & 7;
    bool color_enable = (bp[0x41] & (1u << 3)) != 0;
    bool has_alpha = pixel_format != 0 && pixel_format != 2 && pixel_format != 3;
    bool alpha_enable = (bp[0x41] & (1u << 4)) != 0 && has_alpha;
    bool depth_enable = (bp[0x40] & (1u << 4)) != 0;
    if (!color_enable && !alpha_enable && !depth_enable) {
        return;
    }
    uint32_t red = bp[0x4F] & 0xFF;
    uint32_t alpha = (bp[0x4F] >> 8) & 0xFF;
    uint32_t green = (bp[0x50] >> 8) & 0xFF;
    uint32_t blue = bp[0x50] & 0xFF;
    uint32_t depth = bp[0x51] & 0xFFFFFF;
    if (pixel_format == 1) {
        red = quantize(red, 6);
        green = quantize(green, 6);
        blue = quantize(blue, 6);
        alpha = quantize(alpha, 6);
    } else if (pixel_format == 2) {
        red = quantize(red, 5);
        green = quantize(green, 6);
        blue = quantize(blue, 5);
        depth = (depth & 0xFFFF00) | (depth >> 16);
    }
    if (!has_alpha) {
        alpha_enable = true;
        alpha = 0;
    }
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    bind_efb();
    plain_state();
    gl.ColorMask(color_enable, color_enable, color_enable, alpha_enable);
    if (depth_enable) {
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(GL_ALWAYS);
        gl.DepthMask(GL_TRUE);
    }
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(scaled(x), scaled(y), scaled(x + width) - scaled(x), scaled(y + height) - scaled(y));
    use_fullscreen(g_device.clear, -1.0f);
    gl.Uniform4f(g_device.clear_color, red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f);
    gl.Uniform1f(g_device.clear_depth, static_cast<float>(depth) / 16777215.0f);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

}
