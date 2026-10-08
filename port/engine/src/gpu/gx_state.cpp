#include "wp/gx_state.h"

namespace wp::gx {

namespace {

struct LogicApproximation {
    bool enable;
    BlendOperation operation;
    BlendFactor source;
    BlendFactor destination;
    LogicSource output;
    bool exact;
};

constexpr LogicApproximation kLogicApproximations[16] = {
    {false, BlendOperation::Add, BlendFactor::One, BlendFactor::Zero, LogicSource::Zero, true},
    {true, BlendOperation::Add, BlendFactor::DestinationColor, BlendFactor::Zero, LogicSource::Tev, false},
    {true, BlendOperation::Add, BlendFactor::InverseDestinationColor, BlendFactor::Zero, LogicSource::Tev, false},
    {false, BlendOperation::Add, BlendFactor::One, BlendFactor::Zero, LogicSource::Tev, true},
    {true, BlendOperation::Add, BlendFactor::Zero, BlendFactor::InverseSourceColor, LogicSource::Tev, false},
    {false, BlendOperation::Add, BlendFactor::One, BlendFactor::Zero, LogicSource::Tev, true},
    {true, BlendOperation::Add, BlendFactor::InverseDestinationColor, BlendFactor::InverseSourceColor, LogicSource::Tev, false},
    {true, BlendOperation::Add, BlendFactor::One, BlendFactor::InverseSourceColor, LogicSource::Tev, false},
    {true, BlendOperation::Add, BlendFactor::InverseDestinationColor, BlendFactor::Zero, LogicSource::Inverted, false},
    {true, BlendOperation::Add, BlendFactor::InverseDestinationColor, BlendFactor::InverseSourceColor, LogicSource::Inverted, false},
    {true, BlendOperation::Add, BlendFactor::InverseDestinationColor, BlendFactor::Zero, LogicSource::One, true},
    {true, BlendOperation::Add, BlendFactor::InverseDestinationColor, BlendFactor::TevColor, LogicSource::One, false},
    {false, BlendOperation::Add, BlendFactor::One, BlendFactor::Zero, LogicSource::Inverted, true},
    {true, BlendOperation::Add, BlendFactor::One, BlendFactor::InverseSourceColor, LogicSource::Inverted, false},
    {true, BlendOperation::Subtract, BlendFactor::One, BlendFactor::TevColor, LogicSource::One, false},
    {false, BlendOperation::Add, BlendFactor::One, BlendFactor::Zero, LogicSource::One, true},
};

constexpr const char* kLogicNames[16] = {"clear", "and",   "and_reverse",   "copy",        "and_inverted", "noop", "xor",  "or",
                                         "nor",   "equiv", "invert", "or_reverse", "copy_inverted", "or_inverted", "nand", "set"};

BlendFactor source_factor(uint32_t code) {
    static const BlendFactor kFactors[] = {BlendFactor::Zero,     BlendFactor::One,             BlendFactor::DestinationColor, BlendFactor::InverseDestinationColor,
                                           BlendFactor::TevAlpha, BlendFactor::InverseTevAlpha, BlendFactor::DestinationAlpha, BlendFactor::InverseDestinationAlpha};
    return kFactors[code & 7];
}

BlendFactor destination_factor(uint32_t code) {
    static const BlendFactor kFactors[] = {BlendFactor::Zero,     BlendFactor::One,             BlendFactor::SourceColor,      BlendFactor::InverseSourceColor,
                                           BlendFactor::TevAlpha, BlendFactor::InverseTevAlpha, BlendFactor::DestinationAlpha, BlendFactor::InverseDestinationAlpha};
    return kFactors[code & 7];
}

BlendFactor without_destination_alpha(BlendFactor factor) {
    if (factor == BlendFactor::DestinationAlpha) {
        return BlendFactor::One;
    }
    if (factor == BlendFactor::InverseDestinationAlpha) {
        return BlendFactor::Zero;
    }
    return factor;
}

BlendFactor blend_alpha(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::SourceColor:
        return BlendFactor::TevAlpha;
    case BlendFactor::InverseSourceColor:
        return BlendFactor::InverseTevAlpha;
    case BlendFactor::DestinationColor:
        return BlendFactor::DestinationAlpha;
    case BlendFactor::InverseDestinationColor:
        return BlendFactor::InverseDestinationAlpha;
    default:
        return factor;
    }
}

BlendFactor logic_alpha(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::SourceColor:
        return BlendFactor::SourceAlpha;
    case BlendFactor::InverseSourceColor:
        return BlendFactor::InverseSourceAlpha;
    case BlendFactor::DestinationColor:
        return BlendFactor::DestinationAlpha;
    case BlendFactor::InverseDestinationColor:
        return BlendFactor::InverseDestinationAlpha;
    case BlendFactor::TevColor:
        return BlendFactor::TevAlpha;
    default:
        return factor;
    }
}

}

BlendState blend_state(uint32_t mode, bool target_has_alpha, bool constant_alpha, bool alpha_test_can_pass) {
    BlendState state;
    state.color_update = (mode & (1u << 3)) != 0 && alpha_test_can_pass;
    state.alpha_update = (mode & (1u << 4)) != 0 && target_has_alpha && alpha_test_can_pass;
    bool constant = constant_alpha && state.alpha_update;
    if ((mode & 1) && (mode & (1u << 11))) {
        state.enable = true;
        state.operation = BlendOperation::ReverseSubtract;
        state.source = BlendFactor::One;
        state.destination = BlendFactor::One;
        state.alpha_operation = constant ? BlendOperation::Add : BlendOperation::ReverseSubtract;
        state.source_alpha = BlendFactor::One;
        state.destination_alpha = constant ? BlendFactor::Zero : BlendFactor::One;
    } else if (mode & 1) {
        state.enable = true;
        BlendFactor source = source_factor((mode >> 8) & 7);
        BlendFactor destination = destination_factor((mode >> 5) & 7);
        if (!target_has_alpha) {
            source = without_destination_alpha(source);
            destination = without_destination_alpha(destination);
        }
        state.source = source;
        state.destination = destination;
        state.source_alpha = constant ? BlendFactor::One : blend_alpha(source);
        state.destination_alpha = constant ? BlendFactor::Zero : blend_alpha(destination);
    } else if (mode & 2) {
        state.logic_op = true;
        state.logic_mode = (mode >> 12) & 15;
        if (state.logic_mode == 5) {
            state.color_update = false;
            state.alpha_update = constant;
            return state;
        }
        const LogicApproximation& approximation = kLogicApproximations[state.logic_mode];
        state.output = approximation.output;
        if (approximation.enable) {
            state.enable = true;
            state.operation = approximation.operation;
            state.source = approximation.source;
            state.destination = approximation.destination;
            state.alpha_operation = constant ? BlendOperation::Add : approximation.operation;
            state.source_alpha = constant ? BlendFactor::One : logic_alpha(approximation.source);
            state.destination_alpha = constant ? BlendFactor::Zero : logic_alpha(approximation.destination);
        }
    }
    return state;
}

bool logic_op_exact(uint32_t logic_mode) {
    return kLogicApproximations[logic_mode & 15].exact;
}

const char* logic_op_name(uint32_t logic_mode) {
    return kLogicNames[logic_mode & 15];
}

LinePointOffsets line_point_offsets(const uint32_t* bp) {
    static const float kOffsets[8] = {0.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f, 1.0f, 1.0f, 1.0f};
    LinePointOffsets offsets;
    offsets.line = kOffsets[(bp[0x22] >> 16) & 7];
    offsets.point = kOffsets[(bp[0x22] >> 19) & 7];
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t s = bp[0x30 + 2 * i];
        offsets.line_coordinates |= ((s >> 18) & 1) << i;
        offsets.point_coordinates |= ((s >> 19) & 1) << i;
    }
    if (offsets.line == 0.0f) {
        offsets.line_coordinates = 0;
    }
    if (offsets.point == 0.0f) {
        offsets.point_coordinates = 0;
    }
    return offsets;
}

bool line_offset_negative_side(float dx, float dy, bool tall) {
    return tall ? dy < 0.0f : dx > 0.0f;
}

void offset_texture_coordinates(ScreenVertex& vertex, uint32_t coordinates, float s, float t) {
    for (uint32_t i = 0; i < 8; i++) {
        if (coordinates & (1u << i)) {
            vertex.uv[i][0] += s * vertex.uv[i][2];
            vertex.uv[i][1] += t * vertex.uv[i][2];
        }
    }
}

}
