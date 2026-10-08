#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct Project {
    std::filesystem::path root;
    bool packaged = false;

    std::filesystem::path game_folder() const;
    std::filesystem::path extracted_folder() const;
    std::filesystem::path nand_folder() const;
    std::filesystem::path backups_folder() const;
    std::filesystem::path executable() const;
    std::filesystem::path built_executable() const;
    std::filesystem::path installed_executable() const;
    bool install_game(std::string& error) const;
    std::string translation_inputs() const;
    std::string disc_id() const;
    std::filesystem::path versions_folder() const;
    std::vector<std::string> stored_versions() const;
    bool switch_version(const std::string& id) const;
    bool store_active_version() const;
    std::vector<std::string> disc_languages() const;
    bool translation_current() const;
    void store_translation_stamp() const;
    std::string settings_file() const;
    bool extracted() const;
    bool built() const;
    bool outdated() const;
    double needed_gigabytes() const;
    double free_gigabytes() const;
};

constexpr const char* kGame = "wiiparty";
constexpr const char* kInstallFolderName = "WiiPartyRecomp";

bool find_checkout(std::filesystem::path& root);
std::filesystem::path default_install_folder();
