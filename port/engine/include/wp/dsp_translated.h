#pragma once

#include <cstddef>
#include <cstdint>

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"

namespace wp::dsp {

using TranslatedFunction = int (*)(DSP::Interpreter::Interpreter& interpreter, DSP::SDSP& state, int cycles, bool& idle);

struct TranslatedCode {
    uint32_t checksum;
    TranslatedFunction function;
};

extern const TranslatedCode g_translated_code[];
extern const size_t g_translated_code_count;

}
