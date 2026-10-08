// Copyright 2010 Dolphin Emulator Project
// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace wp::wiimote {

using Sender = std::function<void(const std::vector<uint8_t>&)>;

void reset(uint32_t index);
void output_report(uint32_t index, const uint8_t* data, uint32_t size, const Sender& send);
void update(uint32_t index, const Sender& send);
// WPADFSStatus (0x32 bytes, big-endian) for the native input path (input_hle).
void native_status(uint32_t index, uint8_t out[0x32]);

}
