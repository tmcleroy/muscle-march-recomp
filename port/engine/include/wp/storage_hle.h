#pragma once

#include "wp/input_hle.h"

// Native storage at the SDK API level: CNT (content archives) reads the
// unpacked archives in <extracted>/files/NN/ with host file I/O instead of
// going through emulated ES/IPC. storage.native=1 (default) / WP_NATIVE_STORAGE.
namespace wp::storage_hle {

extern const input_hle::Handler kHandlers[];

}
