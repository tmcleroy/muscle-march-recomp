#pragma once

#include <cstddef>

namespace wp::dsp {

struct RomBlob {
    const unsigned char* data;
    size_t size;
};

RomBlob instruction_rom();
RomBlob coefficient_rom();

}
