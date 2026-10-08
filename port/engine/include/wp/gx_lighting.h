// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace wp::gx::lighting {

void transform_normal(const uint32_t* xf, uint32_t position_matrix, const float* normal, float* out);
void light_channels(const uint32_t* xf, const float* position, const float* normal, const uint8_t vertex_color[2][4],
                    uint8_t out[2][4]);

}
