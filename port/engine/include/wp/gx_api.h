#pragma once

#include "wp/input_hle.h"

// Native GX: the SDK graphics library (gx.a) ported to C++ (gx_api.cpp).
// gx.native=1 (default) / WP_NATIVE_GX=0; WP_GX_VERIFY=1 compares with the original.
namespace wp::gx_api {

bool enabled();
// WP_GX_VERIFY: mismatch count and calls per function.
void print_report();

extern const input_hle::Handler kHandlers[];

}
