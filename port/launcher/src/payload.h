#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct PayloadChanges {
    std::vector<std::string> changed;
    std::vector<std::string> removed;
    bool full_rebuild = false;
};

bool payload_available();
std::string stored_revision(const std::filesystem::path& root);
const std::string& payload_revision();
bool stage_payload(const std::filesystem::path& root, std::string& message);
PayloadChanges payload_changes(const std::filesystem::path& root);
bool extract_payload(const std::filesystem::path& root, std::string& message);
