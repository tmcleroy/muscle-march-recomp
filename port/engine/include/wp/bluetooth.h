// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace wp::bluetooth {

bool handles(const std::string& path);
int32_t ioctlv(uint32_t request, uint32_t command, uint32_t input_count, uint32_t output_count, uint32_t vectors);
void close();
void update();
bool take_completion(uint32_t& request, int32_t& result);

}
