#pragma once

#include <cstdint>

#include "wp/input_hle.h"

// Native audio at the SDK level: dsp.a, ai.a and the OS audio bring-up are
// host functions and AX command lists run in the native mixer (ax_hle.cpp).
// audio.native=1 (default) / WP_NATIVE_AUDIO=0 for the emulated DSP.
namespace wp::audio_hle {

bool enabled();
// The AX task has been started on the native mixer.
bool active();
double load_percent();
// Advance the AI playback clock (CPU thread, at interrupt poll points).
void update();
// The next guest callback to run as an interrupt: DSP task callbacks, then the AI DMA callback.
bool take_interrupt(uint32_t& function, uint32_t& argument);

extern const input_hle::Handler kHandlers[];

}
