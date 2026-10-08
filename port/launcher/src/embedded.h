#pragma once

#include <string_view>

const char* source_revision();
std::string_view blob(const char* name);
