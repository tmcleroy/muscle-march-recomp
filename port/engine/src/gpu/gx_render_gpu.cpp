// SDL_GPU render backend: Metal on Apple platforms, Vulkan on Android/Linux,
// D3D12 on Windows. It mirrors gx_render_gl.cpp (same texture cache, EFB copy
// handling, custom textures and overlay) with SDL_GPU's command-buffer model:
//
// - The GX thread records into one command buffer per frame. Draw batches are
//   collected into a "segment" and recorded as one copy pass (vertices and
//   texture uploads) followed by one EFB render pass, so the EFB pass is not
//   broken for every batch (expensive on tile-based GPUs).
// - EFB copies, clears, readbacks and presentation end the segment first.
// - The window's swapchain may only be used on the thread that created the
//   window, so present_frame() publishes the finished frame and the window
//   thread draws it to the swapchain in present_to_window().
//
// Shaders come from tools/gen_gpu_shaders.py (generated from the GL backend's
// GLSL; see gpu_shaders.inc).

#include "wp/gx_render.h"

#include "gx_render_common.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "wp/custom_textures.h"
#include "wp/game.h"
#include "wp/gx_state.h"
#include "wp/hud.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/options.h"
#include "wp/settings.h"
#include "wp/video.h"

namespace wp::gx::render {

namespace {

#include "gpu_shaders.inc"

constexpr uint32_t kVertexCapacity = 1 << 16;
constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

struct Target {
    SDL_GPUTexture* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct CachedTexture {
    SDL_GPUTexture* texture = nullptr;
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
    std::unordered_map<std::string, SDL_GPUTexture*> loaded;
};

struct CopiedTexture {
    Target converted;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t bytes = 0;
    uint64_t guest_hash = 0;
};

struct Shaders {
    SDL_GPUShader* fullscreen = nullptr;
    SDL_GPUShader* present = nullptr;
    SDL_GPUShader* overlay = nullptr;
    SDL_GPUShader* clear = nullptr;
    SDL_GPUShader* copy = nullptr;
    SDL_GPUShader* tev_vertex = nullptr;
    SDL_GPUShader* tev = nullptr;
    SDL_GPUShader* tev_early = nullptr;
};

struct Device {
    SDL_GPUDevice* device = nullptr;
    SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;
    Shaders shaders;
    std::map<uint64_t, SDL_GPUGraphicsPipeline*> tev_pipelines;
    std::map<uint32_t, SDL_GPUGraphicsPipeline*> clear_pipelines;
    SDL_GPUGraphicsPipeline* copy_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* present_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* overlay_pipeline = nullptr;
    SDL_GPUTextureFormat swapchain_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUTransferBuffer* vertex_staging = nullptr;
    SDL_GPUSampler* linear_sampler = nullptr;
    SDL_GPUSampler* point_sampler = nullptr;
    SDL_GPUTexture* dummy = nullptr;
    SDL_GPUTexture* efb_color = nullptr;
    SDL_GPUTexture* efb_depth = nullptr;
    bool efb_fresh = true;
    Target frames[2];
    int frame_write = 0;
    int frame_published = -1;
    Target write_back;
    SDL_GPUTexture* overlay_texture = nullptr;
    uint32_t overlay_width = 0;
    uint32_t overlay_height = 0;
    uint64_t overlay_version = 0;
    bool overlay_visible = false;
    std::vector<hud::Sprite> hud;             // placement (pixels dropped once uploaded)
    std::vector<SDL_GPUTexture*> hud_textures;
    uint64_t hud_version = 0;
    std::map<uint64_t, SDL_GPUSampler*> samplers;
    std::map<uint64_t, CachedTexture> textures;
    std::map<uint32_t, CopiedTexture> copies;
    bool failed = false;
    bool ready = false;
};

// One draw batch, recorded into the EFB pass when the segment is flushed.
struct Batch {
    Constants constants;
    SDL_GPUTexture* views[kTextureMaps];
    SDL_GPUSampler* samplers[kTextureMaps];
    uint32_t blend = 0;
    uint32_t depth = 0;
    bool early_depth = false;
    uint32_t top_left = 0;
    uint32_t bottom_right = 0;
    uint32_t first = 0;
    uint32_t count = 0;
};

struct Upload {
    SDL_GPUTexture* texture;
    uint32_t level;
    uint32_t width;
    uint32_t height;
    std::vector<uint32_t> pixels;
    bool generate_mipmaps;
};

struct Segment {
    std::vector<ScreenVertex> vertices;
    std::vector<Batch> batches;
    std::vector<Upload> uploads;
    std::vector<SDL_GPUTexture*> graveyard;  // released after the segment is recorded
};

Device g_device;
Segment g_segment;
Batch g_open;  // the batch draw() is extending (not yet in g_segment.batches)
bool g_open_valid = false;
TexturePack g_pack;
uint32_t g_custom_maps = 0;
SDL_GPUCommandBuffer* g_command = nullptr;
std::mutex g_publish_lock;  // frame_published, shared with the window thread
bool g_log_gx = std::getenv("WP_LOG_GX") != nullptr;
bool g_logged_palette = false;
bool g_logged_copy_format = false;
uint64_t g_frame = 0;
uint64_t g_texture_epoch = 0;
int g_scale = 1;
uint32_t g_batches = 0;
uint32_t g_batch_vertices = 0;
double g_batch_seconds = 0.0;
std::string g_copy_dump_prefix;
int g_copy_dump_index = 0;
FILE* g_texture_notes = nullptr;
std::map<uint64_t, bool> g_noted_textures;

int scaled(int value) {
    return value * g_scale;
}

// --- device objects -----------------------------------------------------

SDL_GPUShader* make_shader(SDL_GPUShaderStage stage, const unsigned char* spirv, size_t spirv_size, const char* msl, uint32_t samplers, uint32_t uniforms) {
    SDL_GPUShaderCreateInfo info{};
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniforms;
    if (g_device.format & SDL_GPU_SHADERFORMAT_MSL) {
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.code = reinterpret_cast<const Uint8*>(msl);
        info.code_size = std::strlen(msl);
        info.entrypoint = "main0";
    } else {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = spirv;
        info.code_size = spirv_size;
        info.entrypoint = "main";
    }
    SDL_GPUShader* shader = SDL_CreateGPUShader(g_device.device, &info);
    if (!shader) {
        std::fprintf(stderr, "SDL_GPU shader: %s\n", SDL_GetError());
    }
    return shader;
}

#define WP_SHADER(stage, name, samplers, uniforms) make_shader(stage, k_##name##_spirv, sizeof k_##name##_spirv, k_##name##_msl, samplers, uniforms)

bool create_shaders() {
    Shaders& s = g_device.shaders;
    s.fullscreen = WP_SHADER(SDL_GPU_SHADERSTAGE_VERTEX, fullscreen_vertex, 0, 1);
    s.present = WP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, present_fragment, 1, 0);
    s.overlay = WP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, overlay_fragment, 1, 0);
    s.clear = WP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, clear_fragment, 0, 1);
    s.copy = WP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, copy_fragment, 2, 1);
    s.tev_vertex = WP_SHADER(SDL_GPU_SHADERSTAGE_VERTEX, tev_vertex, 0, 1);
    s.tev = WP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, tev_fragment, kTextureMaps, 2);
    s.tev_early = WP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, tev_early_fragment, kTextureMaps, 2);
    return s.fullscreen && s.present && s.overlay && s.clear && s.copy && s.tev_vertex && s.tev && s.tev_early;
}

SDL_GPUTexture* create_texture(uint32_t width, uint32_t height, uint32_t levels, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = std::max(width, 1u);
    info.height = std::max(height, 1u);
    info.layer_count_or_depth = 1;
    info.num_levels = std::max(levels, 1u);
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(g_device.device, &info);
    if (!texture) {
        std::fprintf(stderr, "SDL_GPU texture %ux%u: %s\n", width, height, SDL_GetError());
    }
    return texture;
}

bool create_render_texture(Target& target, uint32_t width, uint32_t height) {
    target.texture = create_texture(width, height, 1, kColorFormat, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    target.width = width;
    target.height = height;
    return target.texture != nullptr;
}

void release_texture_later(SDL_GPUTexture* texture) {
    if (texture) {
        g_segment.graveyard.push_back(texture);
    }
}

void release_target(Target& target) {
    release_texture_later(target.texture);
    target = Target{};
}

bool create_efb() {
    g_device.efb_color = create_texture(scaled(kEfbWidth), scaled(kEfbHeight), 1, kColorFormat,
                                        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    g_device.efb_depth = create_texture(scaled(kEfbWidth), scaled(kEfbHeight), 1, kDepthFormat,
                                        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    g_device.efb_fresh = true;
    return g_device.efb_color && g_device.efb_depth;
}

void release_efb() {
    release_texture_later(g_device.efb_color);
    release_texture_later(g_device.efb_depth);
    g_device.efb_color = nullptr;
    g_device.efb_depth = nullptr;
}

SDL_GPUSampler* make_sampler(SDL_GPUFilter filter, SDL_GPUSamplerMipmapMode mip, SDL_GPUSamplerAddressMode wrap_s, SDL_GPUSamplerAddressMode wrap_t,
                             float min_lod, float max_lod, float bias, SDL_GPUFilter min_filter) {
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = min_filter;
    info.mag_filter = filter;
    info.mipmap_mode = mip;
    info.address_mode_u = wrap_s;
    info.address_mode_v = wrap_t;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.mip_lod_bias = bias;
    info.min_lod = min_lod;
    info.max_lod = max_lod;
    return SDL_CreateGPUSampler(g_device.device, &info);
}

SDL_GPUColorTargetBlendState no_blend(SDL_GPUColorComponentFlags mask) {
    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend = false;
    blend.enable_color_write_mask = true;
    blend.color_write_mask = mask;
    return blend;
}

constexpr SDL_GPUColorComponentFlags kRgba = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

SDL_GPUGraphicsPipeline* fullscreen_pipeline(SDL_GPUShader* fragment, SDL_GPUTextureFormat format, SDL_GPUColorTargetBlendState blend, bool depth,
                                             bool depth_write) {
    SDL_GPUColorTargetDescription target{};
    target.format = format;
    target.blend_state = blend;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = g_device.shaders.fullscreen;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.enable_depth_clip = false;
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &target;
    if (depth) {
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = kDepthFormat;
        info.depth_stencil_state.enable_depth_test = depth_write;
        info.depth_stencil_state.enable_depth_write = depth_write;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    }
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g_device.device, &info);
    if (!pipeline) {
        std::fprintf(stderr, "SDL_GPU pipeline: %s\n", SDL_GetError());
    }
    return pipeline;
}

SDL_GPUBlendFactor gpu_factor(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero:
        return SDL_GPU_BLENDFACTOR_ZERO;
    case BlendFactor::One:
        return SDL_GPU_BLENDFACTOR_ONE;
    case BlendFactor::SourceColor:
    case BlendFactor::TevColor:
        return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SourceAlpha:
    case BlendFactor::TevAlpha:
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
    case BlendFactor::InverseTevAlpha:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::DestinationAlpha:
        return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    }
    return SDL_GPU_BLENDFACTOR_ONE;
}

SDL_GPUBlendOp gpu_operation(BlendOperation operation) {
    switch (operation) {
    case BlendOperation::Subtract:
        return SDL_GPU_BLENDOP_SUBTRACT;
    case BlendOperation::ReverseSubtract:
        return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    default:
        return SDL_GPU_BLENDOP_ADD;
    }
}

const BlendState& blend_for(uint32_t word) {
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
    return found->second;
}

bool uses_tev_factor(const BlendState& blend) {
    auto tev = [](BlendFactor f) { return f == BlendFactor::TevColor || f == BlendFactor::TevAlpha || f == BlendFactor::InverseTevAlpha; };
    return blend.enable && (tev(blend.source) || tev(blend.destination) || tev(blend.source_alpha) || tev(blend.destination_alpha));
}

// pass: 0 = normal, 1 = colour with the TEV alpha as blend source, 2 = alpha only.
SDL_GPUGraphicsPipeline* tev_pipeline(uint32_t blend_word, uint32_t depth, bool early, uint32_t pass) {
    uint64_t key = blend_word | (static_cast<uint64_t>(depth) << 32) | (static_cast<uint64_t>(early) << 40) | (static_cast<uint64_t>(pass) << 41);
    auto found = g_device.tev_pipelines.find(key);
    if (found != g_device.tev_pipelines.end()) {
        return found->second;
    }
    const BlendState& blend = blend_for(blend_word);
    SDL_GPUColorTargetDescription target{};
    target.format = kColorFormat;
    SDL_GPUColorTargetBlendState& state = target.blend_state;
    state.enable_blend = blend.enable;
    state.src_color_blendfactor = gpu_factor(blend.source);
    state.dst_color_blendfactor = gpu_factor(blend.destination);
    state.color_blend_op = gpu_operation(blend.operation);
    state.src_alpha_blendfactor = gpu_factor(blend.source_alpha);
    state.dst_alpha_blendfactor = gpu_factor(blend.destination_alpha);
    state.alpha_blend_op = gpu_operation(blend.alpha_operation);
    state.enable_color_write_mask = true;
    SDL_GPUColorComponentFlags mask = 0;
    if (blend.color_update && pass != 2) {
        mask |= SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
    }
    if (blend.alpha_update && pass != 1) {
        mask |= SDL_GPU_COLORCOMPONENT_A;
    }
    state.color_write_mask = mask;
    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(ScreenVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[11];
    attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0};
    attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16};
    attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32};
    for (uint32_t i = 0; i < 8; i++) {
        attributes[3 + i] = {3 + i, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 48 + 12 * i};
    }
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = g_device.shaders.tev_vertex;
    info.fragment_shader = early ? g_device.shaders.tev_early : g_device.shaders.tev;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_attributes = 11;
    info.vertex_input_state.vertex_attributes = attributes;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.enable_depth_clip = false;  // GL_DEPTH_CLAMP
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = kDepthFormat;
    if (depth & 1) {
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = (depth & (1u << 4)) != 0;
        info.depth_stencil_state.compare_op = static_cast<SDL_GPUCompareOp>(SDL_GPU_COMPAREOP_NEVER + ((depth >> 1) & 7));
    }
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g_device.device, &info);
    if (!pipeline) {
        std::fprintf(stderr, "SDL_GPU TEV pipeline %06x/%02x: %s\n", blend_word, depth, SDL_GetError());
    }
    g_device.tev_pipelines[key] = pipeline;
    return pipeline;
}

SDL_GPUGraphicsPipeline* clear_pipeline(bool color, bool alpha, bool depth) {
    uint32_t key = (color ? 1 : 0) | (alpha ? 2 : 0) | (depth ? 4 : 0);
    auto found = g_device.clear_pipelines.find(key);
    if (found != g_device.clear_pipelines.end()) {
        return found->second;
    }
    SDL_GPUColorComponentFlags mask = (color ? SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B : 0) |
                                      (alpha ? SDL_GPU_COLORCOMPONENT_A : 0);
    SDL_GPUGraphicsPipeline* pipeline = fullscreen_pipeline(g_device.shaders.clear, kColorFormat, no_blend(mask), true, depth);
    g_device.clear_pipelines[key] = pipeline;
    return pipeline;
}

bool create_pipelines() {
    g_device.copy_pipeline = fullscreen_pipeline(g_device.shaders.copy, kColorFormat, no_blend(kRgba), false, false);
    SDL_GPUBufferCreateInfo buffer{};
    buffer.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer.size = kVertexCapacity * sizeof(ScreenVertex);
    g_device.vertex_buffer = SDL_CreateGPUBuffer(g_device.device, &buffer);
    SDL_GPUTransferBufferCreateInfo staging{};
    staging.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    staging.size = kVertexCapacity * sizeof(ScreenVertex);
    g_device.vertex_staging = SDL_CreateGPUTransferBuffer(g_device.device, &staging);
    g_device.linear_sampler = make_sampler(SDL_GPU_FILTER_LINEAR, SDL_GPU_SAMPLERMIPMAPMODE_NEAREST, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                           SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, 0, 0, 0, SDL_GPU_FILTER_LINEAR);
    g_device.point_sampler = make_sampler(SDL_GPU_FILTER_NEAREST, SDL_GPU_SAMPLERMIPMAPMODE_NEAREST, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                          SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, 0, 0, 0, SDL_GPU_FILTER_NEAREST);
    g_device.dummy = create_texture(1, 1, 1, kColorFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (g_device.dummy) {
        g_segment.uploads.push_back(Upload{g_device.dummy, 0, 1, 1, {0}, false});
    }
    return g_device.copy_pipeline && g_device.vertex_buffer && g_device.vertex_staging && g_device.linear_sampler && g_device.point_sampler && g_device.dummy;
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

// Above 3x the menus, which draw many full-screen layers through the TEV shader, fall below full
// speed even on fast graphics cards.
constexpr int kMaxScale = 3;

int requested_scale() {
    static const settings::LiveNumber scale("video.scale", "WP_SCALE");
    return std::clamp(scale(), 1, kMaxScale);
}

bool initialize() {
    if (g_device.ready) {
        return true;
    }
    if (g_device.failed) {
        return false;
    }
    g_scale = requested_scale();
    g_device.device = static_cast<SDL_GPUDevice*>(video::gpu_device());
    if (!g_device.device) {
        std::fputs("cannot initialise the SDL_GPU renderer: no device\n", stderr);
        g_device.failed = true;
        return false;
    }
    g_device.format = SDL_GetGPUShaderFormats(g_device.device);
    if (!create_shaders() || !create_pipelines() || !create_efb()) {
        std::fputs("cannot initialise the SDL_GPU renderer\n", stderr);
        g_device.failed = true;
        return false;
    }
    std::fprintf(stderr, "SDL_GPU renderer: %s\n", SDL_GetGPUDeviceDriver(g_device.device));
    prepare_texture_pack();
    g_device.ready = true;
    return true;
}

// --- command recording ----------------------------------------------------

SDL_GPUCommandBuffer* command() {
    if (!g_command) {
        g_command = SDL_AcquireGPUCommandBuffer(g_device.device);
    }
    return g_command;
}

void upload_pending(SDL_GPUCopyPass* pass) {
    for (Upload& upload : g_segment.uploads) {
        size_t bytes = upload.pixels.size() * 4;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = static_cast<Uint32>(bytes);
        SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(g_device.device, &info);
        void* mapped = SDL_MapGPUTransferBuffer(g_device.device, staging, false);
        std::memcpy(mapped, upload.pixels.data(), bytes);
        SDL_UnmapGPUTransferBuffer(g_device.device, staging);
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = staging;
        source.pixels_per_row = upload.width;
        source.rows_per_layer = upload.height;
        SDL_GPUTextureRegion region{};
        region.texture = upload.texture;
        region.mip_level = upload.level;
        region.w = upload.width;
        region.h = upload.height;
        region.d = 1;
        SDL_UploadToGPUTexture(pass, &source, &region, false);
        SDL_ReleaseGPUTransferBuffer(g_device.device, staging);
    }
}

// Record the collected batches: one copy pass (vertices + texture uploads), mipmap generation,
// then one EFB render pass.
void record_segment() {
    if (g_open_valid) {
        g_segment.batches.push_back(g_open);
        g_open_valid = false;
    }
    Segment& segment = g_segment;
    if (segment.batches.empty() && segment.uploads.empty() && segment.graveyard.empty()) {
        return;
    }
    auto started = std::chrono::steady_clock::now();
    SDL_GPUCommandBuffer* cmd = command();
    size_t uploaded_vertices = std::min<size_t>(segment.vertices.size(), kVertexCapacity);
    if (uploaded_vertices < segment.vertices.size()) {
        std::fprintf(stderr, "SDL_GPU: %zu vertices in one segment, drawing the first %u\n", segment.vertices.size(), kVertexCapacity);
    }
    if (uploaded_vertices || !segment.uploads.empty()) {
        if (uploaded_vertices) {
            void* mapped = SDL_MapGPUTransferBuffer(g_device.device, g_device.vertex_staging, true);
            std::memcpy(mapped, segment.vertices.data(), uploaded_vertices * sizeof(ScreenVertex));
            SDL_UnmapGPUTransferBuffer(g_device.device, g_device.vertex_staging);
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        if (uploaded_vertices) {
            SDL_GPUTransferBufferLocation source{g_device.vertex_staging, 0};
            SDL_GPUBufferRegion region{g_device.vertex_buffer, 0, static_cast<Uint32>(uploaded_vertices * sizeof(ScreenVertex))};
            SDL_UploadToGPUBuffer(copy, &source, &region, true);
        }
        upload_pending(copy);
        SDL_EndGPUCopyPass(copy);
        for (Upload& upload : segment.uploads) {
            if (upload.generate_mipmaps) {
                SDL_GenerateMipmapsForGPUTexture(cmd, upload.texture);
            }
        }
    }
    if (!segment.batches.empty()) {
        SDL_GPUColorTargetInfo color{};
        color.texture = g_device.efb_color;
        color.load_op = g_device.efb_fresh ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = g_device.efb_depth;
        depth.load_op = g_device.efb_fresh ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.clear_depth = 1.0f;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        g_device.efb_fresh = false;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
        SDL_GPUViewport viewport{0, 0, static_cast<float>(scaled(kEfbWidth)), static_cast<float>(scaled(kEfbHeight)), 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &viewport);
        SDL_GPUBufferBinding vertices{g_device.vertex_buffer, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        int32_t vertex_uniforms[4] = {1, 0, 0, 0};  // depth_zero_to_one
        SDL_PushGPUVertexUniformData(cmd, 0, vertex_uniforms, sizeof vertex_uniforms);
        for (const Batch& batch : segment.batches) {
            if (batch.first >= uploaded_vertices) {
                continue;
            }
            uint32_t count = std::min<uint32_t>(batch.count, static_cast<uint32_t>(uploaded_vertices) - batch.first);
            int left = std::max(static_cast<int>(((batch.top_left >> 12) & 0x7FF) - kScissorOffset), 0);
            int top = std::max(static_cast<int>((batch.top_left & 0x7FF) - kScissorOffset), 0);
            int right = std::min(static_cast<int>(((batch.bottom_right >> 12) & 0x7FF) - kScissorOffset + 1), kEfbWidth);
            int bottom = std::min(static_cast<int>((batch.bottom_right & 0x7FF) - kScissorOffset + 1), kEfbHeight);
            SDL_Rect scissor{scaled(left), scaled(top), std::max(0, scaled(right - left)), std::max(0, scaled(bottom - top))};
            if (scissor.w == 0 || scissor.h == 0) {
                continue;
            }
            SDL_SetGPUScissor(pass, &scissor);
            SDL_GPUTextureSamplerBinding bindings[kTextureMaps];
            for (uint32_t map = 0; map < kTextureMaps; map++) {
                bindings[map].texture = batch.views[map] ? batch.views[map] : g_device.dummy;
                bindings[map].sampler = batch.samplers[map] ? batch.samplers[map] : g_device.point_sampler;
            }
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, kTextureMaps);
            SDL_PushGPUFragmentUniformData(cmd, 0, &batch.constants, sizeof(Constants));
            const BlendState& blend = blend_for(batch.blend);
            bool split = uses_tev_factor(blend);
            for (uint32_t pass_mode : {split ? 1u : 0u, split ? 2u : 3u}) {
                if (pass_mode == 3) {
                    break;
                }
                SDL_GPUGraphicsPipeline* pipeline = tev_pipeline(batch.blend, batch.depth, batch.early_depth, pass_mode);
                if (!pipeline) {
                    continue;
                }
                uint32_t pass_uniforms[4] = {pass_mode, 0, 0, 0};
                SDL_PushGPUFragmentUniformData(cmd, 1, pass_uniforms, sizeof pass_uniforms);
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                SDL_DrawGPUPrimitives(pass, count, 1, batch.first, 0);
            }
        }
        SDL_EndGPURenderPass(pass);
    }
    for (SDL_GPUTexture* texture : segment.graveyard) {
        SDL_ReleaseGPUTexture(g_device.device, texture);
    }
    segment.vertices.clear();
    segment.batches.clear();
    segment.uploads.clear();
    segment.graveyard.clear();
    g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

// Submit the frame's commands; with wait, block until the GPU finished them.
void submit(bool wait) {
    record_segment();
    if (!g_command) {
        return;
    }
    if (wait) {
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_command);
        if (fence) {
            SDL_WaitForGPUFences(g_device.device, true, &fence, 1);
            SDL_ReleaseGPUFence(g_device.device, fence);
        }
    } else {
        SDL_SubmitGPUCommandBuffer(g_command);
    }
    g_command = nullptr;
}

// Draw a fullscreen pass into a colour target (copies), outside the EFB pass.
SDL_GPURenderPass* begin_target(SDL_GPUTexture* texture, uint32_t width, uint32_t height) {
    record_segment();
    SDL_GPUColorTargetInfo color{};
    color.texture = texture;
    color.load_op = SDL_GPU_LOADOP_DONT_CARE;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command(), &color, 1, nullptr);
    SDL_GPUViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    return pass;
}

void push_flip(SDL_GPUCommandBuffer* cmd) {
    float flip[4] = {1.0f, 0, 0, 0};  // top-left origin: no flip on SDL_GPU
    SDL_PushGPUVertexUniformData(cmd, 0, flip, sizeof flip);
}

uint32_t g_downloads = 0;
double g_download_seconds = 0.0;
uint32_t g_copies = 0;

// Read a colour target back (blocking).
bool download(SDL_GPUTexture* texture, uint32_t width, uint32_t height, std::vector<uint8_t>& bytes) {
    record_segment();
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = width * height * 4;
    SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(g_device.device, &info);
    if (!staging) {
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command());
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = width;
    region.h = height;
    region.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = staging;
    destination.pixels_per_row = width;
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    auto wait_started = std::chrono::steady_clock::now();
    submit(true);
    g_downloads++;
    g_download_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_started).count();
    bytes.resize(static_cast<size_t>(width) * height * 4);
    void* mapped = SDL_MapGPUTransferBuffer(g_device.device, staging, false);
    std::memcpy(bytes.data(), mapped, bytes.size());
    SDL_UnmapGPUTransferBuffer(g_device.device, staging);
    SDL_ReleaseGPUTransferBuffer(g_device.device, staging);
    return true;
}

// --- textures -------------------------------------------------------------

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

uint32_t full_levels(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    while ((width >> levels) != 0 || (height >> levels) != 0) {
        levels++;
    }
    return levels;
}

SDL_GPUTexture* upload_levels(std::vector<std::vector<uint32_t>>& images, uint32_t width, uint32_t height, bool generate) {
    uint32_t levels = generate ? full_levels(width, height) : static_cast<uint32_t>(images.size());
    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (generate ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    SDL_GPUTexture* texture = create_texture(width, height, levels, kColorFormat, usage);
    if (!texture) {
        return nullptr;
    }
    for (size_t level = 0; level < images.size(); level++) {
        bool last = level + 1 == images.size();
        g_segment.uploads.push_back(Upload{texture, static_cast<uint32_t>(level), std::max(width >> level, 1u), std::max(height >> level, 1u),
                                           std::move(images[level]), generate && last});
    }
    return texture;
}

SDL_GPUTexture* custom_texture(const std::string& key, bool mipmaps) {
    auto [found, inserted] = g_pack.loaded.try_emplace(key, nullptr);
    if (!inserted) {
        return found->second;
    }
    const std::filesystem::path* path = g_pack.index.find(key);
    std::vector<std::vector<uint32_t>> images(1);
    uint32_t width = 0;
    uint32_t height = 0;
    if (!path || !custom_textures::decode_png(*path, images[0], width, height)) {
        std::fprintf(stderr, "custom texture %s cannot be read, using the original\n", key.c_str());
        return nullptr;
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
        release_texture_later(entry.texture);
    }
    entry.texture = nullptr;
}

SDL_GPUTexture* texture_for(uint32_t map) {
    g_custom_maps &= ~(1u << map);
    TextureRequest request = describe_texture(map);
    if (!request.supported && !g_logged_palette) {
        g_logged_palette = true;
        std::fprintf(stderr, "unsupported texture format %u\n", request.format);
    }
    if (!request.valid) {
        return nullptr;
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
        return copied->second.converted.texture;
    }
    note_texture(map, address, width, height, format, copied != g_device.copies.end() ? "EFB copy rejected, read from RAM" : "RAM");
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
    SDL_GPUTexture* custom = nullptr;
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

SDL_GPUSamplerAddressMode gpu_wrap(uint32_t wrap) {
    return wrap == 1 ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : wrap == 2 ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

SDL_GPUSampler* sampler_for(uint32_t map) {
    SamplerParams params = sampler_params(map, (g_custom_maps >> map) & 1);
    auto found = g_device.samplers.find(params.key);
    if (found != g_device.samplers.end()) {
        return found->second;
    }
    SDL_GPUSampler* sampler = make_sampler(params.mag_linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
                                           params.mip_mode == 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                                           gpu_wrap(params.wrap_s), gpu_wrap(params.wrap_t), params.min_lod, params.unlimited_lod ? 1000.0f : params.max_lod,
                                           params.lod_bias, params.min_linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST);
    g_device.samplers[params.key] = sampler;
    return sampler;
}

// --- copies -----------------------------------------------------------------

void run_copy(const Target& target, uint32_t target_width, uint32_t target_height, int x, int y, int width, int height, uint32_t format, bool intensity,
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
    if (g_device.efb_fresh) {
        return;  // nothing has been drawn into the EFB yet
    }
    SDL_GPURenderPass* pass = begin_target(target.texture, target_width, target_height);
    SDL_GPUCommandBuffer* cmd = command();
    push_flip(cmd);
    SDL_PushGPUFragmentUniformData(cmd, 0, &constants, sizeof constants);
    SDL_GPUTextureSamplerBinding bindings[2] = {{g_device.efb_color, g_device.linear_sampler}, {g_device.efb_depth, g_device.point_sampler}};
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
    SDL_BindGPUGraphicsPipeline(pass, g_device.copy_pipeline);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

// Whether a copy to a texture is also written to guest memory, as the console does. That reads
// the picture back from the graphics card and waits for it, once per copy; phones pay dearly for
// the wait and the copies are drawn from the card's own texture either way, so Android leaves it
// out (video.copies_to_ram or WP_COPIES_TO_RAM decides otherwise).
bool copies_reach_ram() {
    static const bool value = [] {
        std::string text = settings::text("video.copies_to_ram", "WP_COPIES_TO_RAM");
        if (!text.empty()) {
            return std::atoi(text.c_str()) != 0;
        }
#ifdef __ANDROID__
        return false;
#else
        return true;
#endif
    }();
    return value;
}

void release_copy(CopiedTexture& entry) {
    release_target(entry.converted);
}

void dump_copy(const CopiedTexture& entry);

bool write_copy_to_ram(uint32_t address, uint32_t stride, int x, int y, int width, int height, uint32_t logical_width, uint32_t logical_height, uint32_t format,
                       bool intensity, bool alpha, bool depth, const CopyFilter& filter) {
    if (!g_device.write_back.texture && !create_render_texture(g_device.write_back, kEfbWidth, kEfbHeight)) {
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
    run_copy(g_device.write_back, logical_width, logical_height, x, y, width, height, format, intensity, alpha, depth, filter);
    std::vector<uint8_t> full;
    if (!download(g_device.write_back.texture, kEfbWidth, kEfbHeight, full)) {
        return false;
    }
    size_t pitch = static_cast<size_t>(kEfbWidth) * 4;
    uint8_t block[64];
    for (uint32_t by = 0; by < blocks_y; by++) {
        uint8_t* row = host(address + by * row_bytes);
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            std::memset(block, 0, sizeof block);
            for (uint32_t ty = 0; ty < layout.block_height; ty++) {
                uint32_t py = std::min(by * layout.block_height + ty, logical_height - 1);
                for (uint32_t tx = 0; tx < layout.block_width; tx++) {
                    uint32_t px = std::min(bx * layout.block_width + tx, logical_width - 1);
                    encode_texel(block, texture_format, ty * layout.block_width + tx, full.data() + py * pitch + px * 4);
                }
            }
            std::memcpy(row + bx * bytes, block, bytes);
        }
    }
    return true;
}

bool read_target(SDL_GPUTexture* texture, uint32_t width, uint32_t height, std::vector<uint32_t>& pixels) {
    std::vector<uint8_t> bytes;
    if (!texture || !download(texture, width, height, bytes)) {
        return false;
    }
    pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = (static_cast<uint32_t>(bytes[i * 4]) << 16) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8) | bytes[i * 4 + 2];
    }
    return true;
}

void dump_target(SDL_GPUTexture* texture, uint32_t width, uint32_t height, const char* suffix) {
    std::vector<uint32_t> pixels;
    if (read_target(texture, width, height, pixels)) {
        std::string path = g_copy_dump_prefix + "_copy" + std::to_string(g_copy_dump_index) + suffix + ".png";
        video::save_png(path.c_str(), pixels, width, height);
    }
}

void dump_copy(const CopiedTexture& entry) {
    if (g_copy_dump_prefix.empty()) {
        return;
    }
    g_copy_dump_index++;
    dump_target(g_device.efb_color, static_cast<uint32_t>(scaled(kEfbWidth)), static_cast<uint32_t>(scaled(kEfbHeight)), "_efb");
    dump_target(entry.converted.texture, entry.converted.width, entry.converted.height, "");
}

void apply_scale() {
    int scale = requested_scale();
    if (scale == g_scale) {
        return;
    }
    record_segment();
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
        }
    }
}

}  // namespace

const char* api_name() {
    return "SDL_GPU";
}

void draw(const ScreenVertex* vertices, uint32_t count) {
    if (count == 0 || !initialize()) {
        return;
    }
    const uint32_t* bp = bp_registers();
    Constants constants;
    fill_constants(constants, g_scale);
    SDL_GPUTexture* views[kTextureMaps] = {};
    SDL_GPUSampler* samplers[kTextureMaps] = {};
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
    g_batches++;
    g_batch_vertices += count;
    bool same = g_open_valid && g_open.blend == blend && g_open.depth == depth && g_open.early_depth == early_depth && g_open.top_left == bp[0x20] &&
                g_open.bottom_right == bp[0x21] && std::memcmp(&g_open.constants, &constants, sizeof constants) == 0 &&
                std::memcmp(g_open.views, views, sizeof views) == 0 && std::memcmp(g_open.samplers, samplers, sizeof samplers) == 0;
    if (!same) {
        if (g_open_valid) {
            g_segment.batches.push_back(g_open);
        }
        if (g_segment.vertices.size() + count > kVertexCapacity) {
            record_segment();
        }
        g_open.constants = constants;
        std::memcpy(g_open.views, views, sizeof views);
        std::memcpy(g_open.samplers, samplers, sizeof samplers);
        g_open.blend = blend;
        g_open.depth = depth;
        g_open.early_depth = early_depth;
        g_open.top_left = bp[0x20];
        g_open.bottom_right = bp[0x21];
        g_open.first = static_cast<uint32_t>(g_segment.vertices.size());
        g_open.count = 0;
        g_open_valid = true;
    }
    g_segment.vertices.insert(g_segment.vertices.end(), vertices, vertices + count);
    g_open.count += count;
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
    record_segment();
    g_copies++;
    CopiedTexture& entry = g_device.copies[address];
    uint32_t w = static_cast<uint32_t>(width);
    uint32_t h = static_cast<uint32_t>(height);
    entry.logical_width = half ? std::max(1u, w / 2) : w;
    entry.logical_height = half ? std::max(1u, h / 2) : h;
    uint32_t target_width = std::max(1u, entry.logical_width * g_scale);
    uint32_t target_height = std::max(1u, entry.logical_height * g_scale);
    if (!entry.converted.texture || entry.converted.width != target_width || entry.converted.height != target_height) {
        release_copy(entry);
        if (!create_render_texture(entry.converted, target_width, target_height)) {
            release_copy(entry);
            g_device.copies.erase(address);
            return;
        }
    }
    run_copy(entry.converted, target_width, target_height, x, y, width, height, format, intensity, alpha || depth, depth, filter);
    if (copies_reach_ram()) {
        write_copy_to_ram(address, stride, x, y, width, height, entry.logical_width, entry.logical_height, format, intensity, alpha || depth, depth, filter);
    }
    entry.bytes = entry.logical_width * entry.logical_height * copy_bits(format) / 8;
    entry.guest_hash = guest_range_valid(address, entry.bytes) ? sample_hash(host(address), entry.bytes) : 0;
    dump_copy(entry);
}

void copy_to_framebuffer(int x, int y, int width, int height, bool depth, const CopyFilter& filter) {
    if (!initialize()) {
        return;
    }
    record_segment();
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
    Target& frame = g_device.frames[g_device.frame_write];
    if (!frame.texture || frame.width != physical_width || frame.height != physical_height) {
        release_target(frame);
        if (!create_render_texture(frame, physical_width, physical_height)) {
            release_target(frame);
            return;
        }
    }
    run_copy(frame, physical_width, physical_height, x, y, width, height, kXfbFormat, false, false, depth, filter);
}

// GX thread: finish the frame and hand it to the window thread.
bool present_frame(void* window, double) {
    if (!g_device.ready) {
        return false;
    }
    submit(false);
    if (log::enabled()) {
        static auto report_at = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - report_at >= std::chrono::seconds(1)) {
            log::write("copies", "%u EFB copies to textures, %u read back in %.0f ms (scale %d)", g_copies, g_downloads, g_download_seconds * 1000.0, g_scale);
            g_copies = 0;
            g_downloads = 0;
            g_download_seconds = 0.0;
            report_at = now;
        }
    }
    {
        std::lock_guard<std::mutex> hold(g_publish_lock);
        if (g_device.frames[g_device.frame_write].texture) {
            g_device.frame_published = g_device.frame_write;
            g_device.frame_write ^= 1;
        }
    }
    if (window) {
        video::request_present();
    }
    apply_scale();
    return true;
}

// Window thread: draw the latest published frame (and the options overlay) to the swapchain.
bool present_to_window(void* window_handle, double aspect) {
    if (!g_device.ready || !window_handle) {
        return false;
    }
    SDL_Window* window = static_cast<SDL_Window*>(window_handle);
    if (!g_device.present_pipeline) {
        g_device.swapchain_format = SDL_GetGPUSwapchainTextureFormat(g_device.device, window);
        g_device.present_pipeline = fullscreen_pipeline(g_device.shaders.present, g_device.swapchain_format, no_blend(kRgba), false, false);
        SDL_GPUColorTargetBlendState alpha{};
        alpha.enable_blend = true;
        alpha.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        alpha.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        alpha.color_blend_op = SDL_GPU_BLENDOP_ADD;
        alpha.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        alpha.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        alpha.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        g_device.overlay_pipeline = fullscreen_pipeline(g_device.shaders.overlay, g_device.swapchain_format, alpha, false, false);
    }
    static const bool log_present = std::getenv("WP_LOG_PRESENT") != nullptr;
    static auto last_present = std::chrono::steady_clock::now();
    static auto report_at = last_present;
    static double worst_gap = 0.0;
    static double acquire_total = 0.0;
    static int presents = 0;
    auto started = std::chrono::steady_clock::now();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_device.device);
    options::Image image;
    if (options::take_overlay(g_device.overlay_version, image, g_device.overlay_visible) && !image.pixels.empty() &&
        image.pixels.size() == static_cast<size_t>(image.width) * image.height) {
        if (g_device.overlay_texture) {
            SDL_ReleaseGPUTexture(g_device.device, g_device.overlay_texture);
        }
        g_device.overlay_texture = create_texture(image.width, image.height, 1, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        g_device.overlay_width = image.width;
        g_device.overlay_height = image.height;
        SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, image.width * image.height * 4, 0};
        SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(g_device.device, &info);
        std::memcpy(SDL_MapGPUTransferBuffer(g_device.device, staging, false), image.pixels.data(), info.size);
        SDL_UnmapGPUTransferBuffer(g_device.device, staging);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo source{staging, 0, image.width, image.height};
        SDL_GPUTextureRegion region{};
        region.texture = g_device.overlay_texture;
        region.w = image.width;
        region.h = image.height;
        region.d = 1;
        SDL_UploadToGPUTexture(copy, &source, &region, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(g_device.device, staging);
    }
    std::vector<hud::Sprite> sprites;
    if (hud::take(g_device.hud_version, sprites)) {
        for (SDL_GPUTexture* texture : g_device.hud_textures) {
            SDL_ReleaseGPUTexture(g_device.device, texture);
        }
        g_device.hud_textures.clear();
        g_device.hud.clear();
        for (hud::Sprite& sprite : sprites) {
            const options::Image& image = sprite.image;
            if (image.width == 0 || image.height == 0 || image.pixels.size() != static_cast<size_t>(image.width) * image.height) {
                continue;
            }
            SDL_GPUTexture* texture = create_texture(image.width, image.height, 1, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
            SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, image.width * image.height * 4, 0};
            SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(g_device.device, &info);
            std::memcpy(SDL_MapGPUTransferBuffer(g_device.device, staging, false), image.pixels.data(), info.size);
            SDL_UnmapGPUTransferBuffer(g_device.device, staging);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo source{staging, 0, image.width, image.height};
            SDL_GPUTextureRegion region{};
            region.texture = texture;
            region.w = image.width;
            region.h = image.height;
            region.d = 1;
            SDL_UploadToGPUTexture(copy, &source, &region, false);
            SDL_EndGPUCopyPass(copy);
            SDL_ReleaseGPUTransferBuffer(g_device.device, staging);
            sprite.image.pixels.clear();
            g_device.hud_textures.push_back(texture);
            g_device.hud.push_back(std::move(sprite));
        }
    }
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 client_width = 0;
    Uint32 client_height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &client_width, &client_height) || !swapchain) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    if (log_present) {
        auto now = std::chrono::steady_clock::now();
        acquire_total += std::chrono::duration<double>(now - started).count();
        worst_gap = std::max(worst_gap, std::chrono::duration<double>(now - last_present).count());
        last_present = now;
        presents++;
        if (now - report_at >= std::chrono::seconds(1)) {
            std::fprintf(stderr, "present: %d/s, worst gap %.1f ms, acquire wait %.1f ms/s\n", presents, worst_gap * 1000.0, acquire_total * 1000.0);
            presents = 0;
            worst_gap = 0.0;
            acquire_total = 0.0;
            report_at = now;
        }
    }
    SDL_GPUColorTargetInfo color{};
    color.texture = swapchain;
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.clear_color = {0, 0, 0, 1};
    color.store_op = SDL_GPU_STOREOP_STORE;
    static Uint32 logged_width = 0;
    static Uint32 logged_height = 0;
    if (logged_width != client_width || logged_height != client_height) {
        logged_width = client_width;
        logged_height = client_height;
        std::fprintf(stderr, "SDL_GPU: presenting to a %ux%u swapchain\n", client_width, client_height);
    }
    hud::set_window_size(client_width, client_height);
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
    float flip[4] = {1.0f, 0, 0, 0};
    SDL_PushGPUVertexUniformData(cmd, 0, flip, sizeof flip);
    SDL_GPUTexture* frame = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_publish_lock);
        if (g_device.frame_published >= 0) {
            frame = g_device.frames[g_device.frame_published].texture;
        }
    }
    if (frame) {
        double width = client_width;
        double height = width / aspect;
        if (height > client_height) {
            height = client_height;
            width = height * aspect;
        }
        SDL_GPUViewport viewport{static_cast<float>((client_width - width) / 2), static_cast<float>((client_height - height) / 2),
                                 static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &viewport);
        SDL_GPUTextureSamplerBinding binding{frame, g_device.linear_sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_BindGPUGraphicsPipeline(pass, g_device.present_pipeline);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        // HUD sprites, placed in the game picture.
        hud::set_picture_size(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        for (size_t i = 0; i < g_device.hud.size(); i++) {
            const hud::Sprite& sprite = g_device.hud[i];
            SDL_GPUViewport placed{viewport.x + sprite.x * viewport.w, viewport.y + sprite.y * viewport.h, sprite.width * viewport.w,
                                   sprite.height * viewport.h, 0.0f, 1.0f};
            SDL_SetGPUViewport(pass, &placed);
            SDL_GPUTextureSamplerBinding sprite_binding{g_device.hud_textures[i], g_device.linear_sampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &sprite_binding, 1);
            SDL_BindGPUGraphicsPipeline(pass, g_device.overlay_pipeline);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        }
    }
    if (g_device.overlay_visible && g_device.overlay_texture) {
        options::Rect placed = options::place(static_cast<int>(client_width), static_cast<int>(client_height), static_cast<int>(g_device.overlay_width),
                                              static_cast<int>(g_device.overlay_height));
        if (placed.width > 0 && placed.height > 0) {
            SDL_GPUViewport viewport{static_cast<float>(placed.x), static_cast<float>(placed.y), static_cast<float>(placed.width),
                                     static_cast<float>(placed.height), 0.0f, 1.0f};
            SDL_SetGPUViewport(pass, &viewport);
            SDL_GPUTextureSamplerBinding binding{g_device.overlay_texture, g_device.linear_sampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_BindGPUGraphicsPipeline(pass, g_device.overlay_pipeline);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        }
    }
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
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
    if (!g_device.ready) {
        return false;
    }
    int index = g_device.frame_published >= 0 ? g_device.frame_published : g_device.frame_write;
    const Target& frame = g_device.frames[index];
    if (!frame.texture) {
        return false;
    }
    width = frame.width;
    height = frame.height;
    return read_target(frame.texture, width, height, pixels);
}

void clear(int x, int y, int width, int height) {
    if (!initialize()) {
        return;
    }
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
    SDL_GPUGraphicsPipeline* pipeline = clear_pipeline(color_enable, alpha_enable, depth_enable);
    if (!pipeline) {
        return;
    }
    record_segment();
    SDL_GPUCommandBuffer* cmd = command();
    SDL_GPUColorTargetInfo color{};
    color.texture = g_device.efb_color;
    color.load_op = g_device.efb_fresh ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth_target{};
    depth_target.texture = g_device.efb_depth;
    depth_target.load_op = g_device.efb_fresh ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    depth_target.store_op = SDL_GPU_STOREOP_STORE;
    depth_target.clear_depth = 1.0f;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    g_device.efb_fresh = false;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth_target);
    SDL_GPUViewport viewport{0, 0, static_cast<float>(scaled(kEfbWidth)), static_cast<float>(scaled(kEfbHeight)), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_Rect scissor{scaled(x), scaled(y), scaled(x + width) - scaled(x), scaled(y + height) - scaled(y)};
    SDL_SetGPUScissor(pass, &scissor);
    push_flip(cmd);
    float values[8] = {red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f, static_cast<float>(depth) / 16777215.0f, 0, 0, 0};
    SDL_PushGPUFragmentUniformData(cmd, 0, values, sizeof values);
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

}
