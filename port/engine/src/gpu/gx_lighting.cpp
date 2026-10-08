// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "wp/gx_lighting.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wp::gx::lighting {

namespace {

constexpr uint32_t kNormalMatrices = 0x400;
constexpr uint32_t kLights = 0x600;
constexpr uint32_t kLightSize = 16;
constexpr uint32_t kAmbientColor = 0x100A;
constexpr uint32_t kMaterialColor = 0x100C;
constexpr uint32_t kColorControl = 0x100E;
constexpr uint32_t kAlphaControl = 0x1010;

enum AttenuationFunc : uint32_t { kAttenuationNone = 0, kAttenuationSpec = 1, kAttenuationDir = 2, kAttenuationSpot = 3 };
enum DiffuseFunc : uint32_t { kDiffuseNone = 0, kDiffuseSign = 1, kDiffuseClamp = 2 };

struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator/(const Vec3& a, float d) {
    return {a.x / d, a.y / d, a.z / d};
}

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length_squared(const Vec3& a) {
    return dot(a, a);
}

Vec3 normalized(const Vec3& a) {
    float length = std::sqrt(length_squared(a));
    if (length == 0.0f) {
        return a;
    }
    return a / length;
}

struct Channel {
    bool material_from_vertex;
    bool enable_lighting;
    bool ambient_from_vertex;
    uint32_t diffuse;
    uint32_t attenuation;
    uint32_t mask;
};

Channel decode_channel(uint32_t hex) {
    Channel channel;
    channel.material_from_vertex = (hex & 1) != 0;
    channel.enable_lighting = ((hex >> 1) & 1) != 0;
    channel.ambient_from_vertex = ((hex >> 6) & 1) != 0;
    channel.diffuse = (hex >> 7) & 3;
    channel.attenuation = (hex >> 9) & 3;
    channel.mask = channel.enable_lighting ? (((hex >> 2) & 15) | (((hex >> 11) & 15) << 4)) : 0;
    return channel;
}

struct Light {
    uint8_t color[4];
    Vec3 cosatt;
    Vec3 distatt;
    Vec3 pos;
    Vec3 dir;
};

float word_float(const uint32_t* xf, uint32_t index) {
    float value;
    std::memcpy(&value, &xf[index], sizeof value);
    return value;
}

Vec3 word_vec3(const uint32_t* xf, uint32_t index) {
    return {word_float(xf, index), word_float(xf, index + 1), word_float(xf, index + 2)};
}

void word_rgba(uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

Light read_light(const uint32_t* xf, uint32_t number) {
    uint32_t base = kLights + number * kLightSize;
    Light light;
    word_rgba(xf[base + 3], light.color);
    light.cosatt = word_vec3(xf, base + 4);
    light.distatt = word_vec3(xf, base + 7);
    light.pos = word_vec3(xf, base + 10);
    light.dir = word_vec3(xf, base + 13);
    return light;
}

float safe_divide(float n, float d) {
    return (d == 0) ? (n > 0 ? 1 : 0) : n / d;
}

float light_attenuation(const Light& light, Vec3& ldir, const Vec3& normal, const Channel& channel) {
    float attn = 1.0f;
    switch (channel.attenuation) {
    case kAttenuationNone:
    case kAttenuationDir:
        ldir = normalized(ldir);
        if (ldir.x == 0.0f && ldir.y == 0.0f && ldir.z == 0.0f) {
            ldir = normal;
        }
        break;
    case kAttenuationSpec: {
        ldir = normalized(ldir);
        attn = dot(ldir, normal) >= 0.0f ? std::max(0.0f, dot(light.dir, normal)) : 0.0f;
        Vec3 att_len{1.0f, attn, attn * attn};
        Vec3 cos_attn = light.cosatt;
        Vec3 dist_attn = light.distatt;
        if (channel.diffuse != kDiffuseNone) {
            dist_attn = normalized(dist_attn);
        }
        attn = safe_divide(std::max(0.0f, dot(att_len, cos_attn)), dot(att_len, dist_attn));
        break;
    }
    case kAttenuationSpot: {
        float dist2 = length_squared(ldir);
        float dist = std::sqrt(dist2);
        ldir = ldir / dist;
        attn = std::max(0.0f, dot(ldir, light.dir));
        float cos_att = light.cosatt.x + (light.cosatt.y * attn) + (light.cosatt.z * attn * attn);
        float dist_att = light.distatt.x + (light.distatt.y * dist) + (light.distatt.z * dist2);
        attn = safe_divide(std::max(0.0f, cos_att), dist_att);
        break;
    }
    }
    return attn;
}

float light_factor(const Light& light, const Vec3& position, const Vec3& normal, const Channel& channel) {
    Vec3 ldir = light.pos - position;
    float attn = light_attenuation(light, ldir, normal, channel);
    float dif_attn = dot(ldir, normal);
    switch (channel.diffuse) {
    case kDiffuseSign:
        return attn * dif_attn;
    case kDiffuseClamp:
        return attn * std::max(0.0f, dif_attn);
    default:
        return attn;
    }
}

uint8_t modulate(uint8_t material, float light) {
    int value = std::clamp(static_cast<int>(light), 0, 255);
    return static_cast<uint8_t>((material * (value + (value >> 7))) >> 8);
}

}

void transform_normal(const uint32_t* xf, uint32_t position_matrix, const float* normal, float* out) {
    uint32_t base = kNormalMatrices + (position_matrix & 31) * 3;
    Vec3 result;
    result.x = word_float(xf, base) * normal[0] + word_float(xf, base + 1) * normal[1] + word_float(xf, base + 2) * normal[2];
    result.y = word_float(xf, base + 3) * normal[0] + word_float(xf, base + 4) * normal[1] + word_float(xf, base + 5) * normal[2];
    result.z = word_float(xf, base + 6) * normal[0] + word_float(xf, base + 7) * normal[1] + word_float(xf, base + 8) * normal[2];
    result = normalized(result);
    out[0] = result.x;
    out[1] = result.y;
    out[2] = result.z;
}

void light_channels(const uint32_t* xf, const float* position, const float* normal, const uint8_t vertex_color[2][4],
                    uint8_t out[2][4]) {
    Vec3 pos{position[0], position[1], position[2]};
    Vec3 nrm{normal[0], normal[1], normal[2]};
    for (uint32_t k = 0; k < 2; k++) {
        uint8_t material[4];
        uint8_t ambient[4];
        word_rgba(xf[kMaterialColor + k], material);
        word_rgba(xf[kAmbientColor + k], ambient);

        Channel color = decode_channel(xf[kColorControl + k]);
        const uint8_t* color_material = color.material_from_vertex ? vertex_color[k] : material;
        if (color.enable_lighting) {
            const uint8_t* base = color.ambient_from_vertex ? vertex_color[k] : ambient;
            float light[3] = {static_cast<float>(base[0]), static_cast<float>(base[1]), static_cast<float>(base[2])};
            for (uint32_t i = 0; i < 8; i++) {
                if (color.mask & (1u << i)) {
                    Light source = read_light(xf, i);
                    float factor = light_factor(source, pos, nrm, color);
                    for (uint32_t c = 0; c < 3; c++) {
                        light[c] += source.color[c] * factor;
                    }
                }
            }
            for (uint32_t c = 0; c < 3; c++) {
                out[k][c] = modulate(color_material[c], light[c]);
            }
        } else {
            for (uint32_t c = 0; c < 3; c++) {
                out[k][c] = color_material[c];
            }
        }

        Channel alpha = decode_channel(xf[kAlphaControl + k]);
        uint8_t alpha_material = alpha.material_from_vertex ? vertex_color[k][3] : material[3];
        if (alpha.enable_lighting) {
            float light = alpha.ambient_from_vertex ? vertex_color[k][3] : ambient[3];
            for (uint32_t i = 0; i < 8; i++) {
                if (alpha.mask & (1u << i)) {
                    Light source = read_light(xf, i);
                    light += source.color[3] * light_factor(source, pos, nrm, alpha);
                }
            }
            out[k][3] = modulate(alpha_material, light);
        } else {
            out[k][3] = alpha_material;
        }
    }
}

}
