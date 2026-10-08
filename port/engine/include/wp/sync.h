#pragma once

#include <cstdint>

#include "wp/cpu.h"

namespace wp {

// Called at the entry of the functions listed in the game's sync_hooks.csv.
// With WP_SYNC_OUT set, records a snapshot of guest memory at each hit so a
// run can be compared with Dolphin (tools/lockstep.py).
void sync_hook(Cpu& c, uint32_t address);

}
