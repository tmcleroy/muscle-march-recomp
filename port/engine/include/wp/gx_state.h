#pragma once

#include <cstdint>

#include "wp/gx_render.h"

namespace wp::gx {

enum class BlendFactor : uint8_t {
    Zero,
    One,
    SourceColor,
    InverseSourceColor,
    SourceAlpha,
    InverseSourceAlpha,
    DestinationColor,
    InverseDestinationColor,
    DestinationAlpha,
    InverseDestinationAlpha,
    TevColor,
    TevAlpha,
    InverseTevAlpha,
};

enum class BlendOperation : uint8_t {
    Add,
    Subtract,
    ReverseSubtract,
};

enum class LogicSource : uint8_t {
    Tev,
    Zero,
    One,
    Inverted,
};

struct BlendState {
    bool enable = false;
    BlendOperation operation = BlendOperation::Add;
    BlendOperation alpha_operation = BlendOperation::Add;
    BlendFactor source = BlendFactor::One;
    BlendFactor destination = BlendFactor::Zero;
    BlendFactor source_alpha = BlendFactor::One;
    BlendFactor destination_alpha = BlendFactor::Zero;
    bool color_update = false;
    bool alpha_update = false;
    bool logic_op = false;
    uint32_t logic_mode = 0;
    LogicSource output = LogicSource::Tev;
};

BlendState blend_state(uint32_t mode, bool target_has_alpha, bool constant_alpha, bool alpha_test_can_pass);
bool logic_op_exact(uint32_t logic_mode);
const char* logic_op_name(uint32_t logic_mode);

struct LinePointOffsets {
    float line = 0.0f;
    float point = 0.0f;
    uint32_t line_coordinates = 0;
    uint32_t point_coordinates = 0;
};

LinePointOffsets line_point_offsets(const uint32_t* bp);
bool line_offset_negative_side(float dx, float dy, bool tall);
void offset_texture_coordinates(ScreenVertex& vertex, uint32_t coordinates, float s, float t);

}
