#pragma once

#include <cstdint>

// Native AX mixer: runs the AX library's DSP command lists in C++ (see ax_hle.cpp).
namespace wp::ax {

// Dolphin's hash of an AX microcode (HashEctor over its IRAM image).
uint32_t ucode_hash(uint32_t address, uint32_t length);
bool supports(uint32_t ucode_hash);
void start(uint32_t ucode_hash);
// Process one frame's command list (size in 16-bit words), writing the output to guest memory.
void run_command_list(uint32_t address, uint16_t words);
void take_statistics(uint64_t& frames, uint64_t& voices);

}
