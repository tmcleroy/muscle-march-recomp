#pragma once

#include <cstddef>
#include <cstdint>

namespace wp::audio {

void update();
void start_output();
void push(const int16_t* frames, size_t count, uint32_t rate);

}
