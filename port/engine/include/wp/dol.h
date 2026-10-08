#pragma once

#include <cstdint>
#include <string>

namespace wp {

bool load_dol(const std::string& path, uint32_t& entry);

}
