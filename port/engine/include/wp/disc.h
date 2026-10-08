#pragma once

#include <cstdint>
#include <string>

namespace wp::disc {

bool mount(const std::string& extracted_directory);
void read(uint64_t offset, uint32_t length, uint32_t destination);

}
