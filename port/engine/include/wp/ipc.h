#pragma once

#include <cstdint>

namespace wp::ipc {

constexpr uint32_t kInterrupt = 27;

uint32_t read32(uint32_t address);
void write32(uint32_t address, uint32_t value);
void reply(uint32_t request, int32_t result, uint64_t ticks);
void update();
bool interrupt_pending();
void report();

}
