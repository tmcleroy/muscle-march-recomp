#pragma once

#include <ctime>
#include <string>

namespace wp::screenshot {

constexpr const char* kFolder = "screenshots";

std::string file_name(const char* prefix, const std::tm& time, int attempt);

}
