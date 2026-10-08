#pragma once

#include "wp/cpu.h"

namespace wp {

using HleFunction = void (*)(Cpu&);

// Program arguments (for restarting after a guest reset).
extern char** g_argv;

HleFunction find_replacement(const char* name);
// Handlers defined outside the engine (game code, lifted functions) register themselves.
struct HandlerRegistrar {
    HandlerRegistrar(const char* name, HleFunction function);
};
// Run the translated original of a replaced function (by symbol name).
void call_original(Cpu& c, const char* name);
void interrupt_left();

}
