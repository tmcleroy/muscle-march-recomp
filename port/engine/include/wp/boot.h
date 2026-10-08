#pragma once

#include <cstdint>
#include <string>

namespace wp {

bool init_boot_memory(const std::string& extracted_directory);
bool init_title_memory(const uint8_t* game_id, uint32_t ios);

}
