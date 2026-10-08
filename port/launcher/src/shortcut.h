#pragma once

#include <filesystem>
#include <string>

bool create_desktop_shortcut(const std::filesystem::path& target, const std::filesystem::path& folder, const std::filesystem::path& icon, const std::string& name,
                             std::string& error);
