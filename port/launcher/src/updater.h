#pragma once

#include <filesystem>
#include <string>

struct Release {
    std::string version;
    std::string url;
    std::string sha256;
    std::string summary;
    std::string page;
};

bool newer_version(const std::string& candidate, const std::string& current);
bool latest_release(Release& release, Release& current, const std::string& current_version, std::string& error);
bool download(const std::string& url, const std::filesystem::path& target, std::string& error);
bool replace_launcher(const std::filesystem::path& downloaded, std::string& error);
void remove_old_launcher();
std::filesystem::path launcher_path();
