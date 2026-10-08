// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace wp::dsp {

constexpr uint32_t kInterruptAudioDma = 5;
constexpr uint32_t kInterruptAram = 6;
constexpr uint32_t kInterruptDsp = 7;

struct Status {
    bool loaded;
    bool native;
    double load_percent;
};

void update();
Status status();
uint16_t read16(uint32_t address);
void write16(uint32_t address, uint16_t value);
uint32_t pending_interrupt();

}
