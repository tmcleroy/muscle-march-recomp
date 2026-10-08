#pragma once

#include <string>

#include "wp/cpu.h"

namespace wp {

std::string format_guest(const Cpu& c, uint32_t format, uint32_t first_argument_register);

}
