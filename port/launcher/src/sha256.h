#pragma once

#include <filesystem>
#include <string>
#include <string_view>

std::string sha256_of(const std::filesystem::path& path);
std::string sha256_of_bytes(std::string_view data);
