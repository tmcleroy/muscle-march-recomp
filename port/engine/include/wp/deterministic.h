#pragma once

#include <chrono>
#include <cstdint>

// Deterministic mode (WP_DETERMINISTIC=1, implied by input recording/replay): everything the
// game can observe follows from the inputs. Guest time advances only while the game idles (a
// fixed amount per interrupt poll in OSSelectThread, see deterministic.cpp) instead of with the
// wall clock; interrupts, IPC replies and GX command processing also happen only at those idle
// polls, so where poll sites fall inside busy code (which lifting a function changes) does not
// matter. The clock starts at a fixed date. Execution is throttled to real time unless
// WP_DETERMINISTIC_FAST=1, which never changes the result.
namespace wp::det {

bool enabled();
// Guest time base (60.75 MHz ticks since 2000-01-01).
uint64_t ticks();
// One interrupt poll; called from poll_interrupts. Advances guest time while the guest idles
// (waits in OSSelectThread) and returns whether events may be delivered at this poll: at idle
// polls, and at every poll once the guest has spun for a long time without idling.
bool advance(bool idle);
// Called at each VI retrace (statistics for WP_LOG_POLLS).
void retrace();
// Host clock for subsystems that pace guest-visible events (VI, audio): virtual time when
// deterministic, steady_clock otherwise.
std::chrono::steady_clock::time_point now();

}
