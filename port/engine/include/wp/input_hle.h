#pragma once

#include <cstdint>

#include "wp/cpu.h"

// Native controller input at the WPAD API level: WPADRead fills the game's
// WPADStatus/WPADFSStatus directly from host input, bypassing the emulated
// Bluetooth Wii Remote. input.native / WP_NATIVE_INPUT: 2 (default) = no
// Bluetooth at all, 1 = native WPADRead only, 0 = emulated remote.
namespace wp::input_hle {

bool enabled();
void wpad_read(Cpu& c);

struct Handler {
    const char* name;
    void (*function)(Cpu&);
};
// Entry points replaced for input.native=2 (each falls back to the original otherwise).
extern const Handler kHandlers[];

}
