#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wp/gx_render.h"
#include "wp/gx_state.h"

namespace wp::gx::render {

constexpr int kEfbWidth = 640;
constexpr int kEfbHeight = 528;
constexpr uint32_t kMaxStages = 16;
constexpr uint32_t kTextureMaps = 8;
constexpr float kScissorOffset = 342.0f;
constexpr uint32_t kTlutSize = 0x100000;
constexpr uint32_t kTlutMask = 0x7FE00;
constexpr uint32_t kXfbFormat = 13;

struct Constants {
    float initial[4][4];
    float konst[4][4];
    uint32_t stage[kMaxStages][4];
    uint32_t header[4];
    uint32_t swaps[4];
    float texdims[8][4];
    int32_t indmtx[6][4];
    uint32_t indirect[4];
    uint32_t tevind[kMaxStages][4];
    float fog[4];
    int32_t fog_integer[4];
    float fog_range[3][4];
};

struct Layout {
    uint32_t block_width;
    uint32_t block_height;
    bool supported;
};

struct AlphaTestResult {
    bool can_pass;
    bool can_fail;
};

struct TextureRequest {
    bool valid = false;
    uint32_t address = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t levels = 1;
    size_t size = 0;
    size_t total = 0;
    size_t offsets[12] = {};
    const uint8_t* source = nullptr;
    const uint8_t* tlut = nullptr;
    uint32_t tlut_format = 0;
    uint32_t entries = 0;
    uint64_t key = 0;
    bool mipmaps = false;
    bool supported = true;
};

struct SamplerParams {
    uint32_t wrap_s = 0;
    uint32_t wrap_t = 0;
    bool min_linear = false;
    bool mag_linear = false;
    uint32_t mip_mode = 0;
    float min_lod = 0.0f;
    float max_lod = 0.0f;
    float lod_bias = 0.0f;
    bool unlimited_lod = false;
    uint64_t key = 0;
};

extern uint8_t g_tlut[kTlutSize];

TextureRequest describe_texture(uint32_t map);
uint64_t texture_hash(const TextureRequest& request);
std::vector<std::vector<uint32_t>> decode_levels(const TextureRequest& request);
SamplerParams sampler_params(uint32_t map, bool custom);

Layout layout_for(uint32_t format);
uint32_t block_bytes(uint32_t format);
std::vector<uint32_t> decode_texture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t format, const uint8_t* tlut, uint32_t tlut_format);
uint64_t hash_bytes(const uint8_t* data, size_t size);
uint64_t sample_hash(const uint8_t* data, size_t size);
BlendState blend_state_for(uint32_t word);
void fill_constants(Constants& constants, int scale);
AlphaTestResult alpha_test_result(uint32_t test);
uint32_t copy_bits(uint32_t format);
uint32_t copy_texture_format(uint32_t format);
uint32_t scale_bits(uint32_t value, int bits);
void encode_texel(uint8_t* block, uint32_t format, uint32_t index, const uint8_t* rgba);
uint32_t expand(uint32_t value, int bits);
uint32_t quantize(uint32_t value, int bits);

}
