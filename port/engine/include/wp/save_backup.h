#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace wp::saves {

enum class Result { Created, Unchanged, NoSave, Invalid, Failed };

struct Backup {
    std::string name;
    std::filesystem::path path;
};

struct Outcome {
    Result result = Result::Failed;
    std::string message;
    std::filesystem::path backup;
};

std::filesystem::path save_relative(uint32_t title_low);
std::filesystem::path backups_beside(const std::filesystem::path& nand_root);
std::filesystem::path saved_relative(const Backup& backup);
std::vector<Backup> list(const std::filesystem::path& backups_root);
bool validate(const std::filesystem::path& save, std::string& problem);
bool same_contents(const std::filesystem::path& first, const std::filesystem::path& second);
Outcome back_up(const std::filesystem::path& nand_root, const std::filesystem::path& relative, const std::filesystem::path& backups_root,
                int keep, std::time_t now);
Outcome restore(const std::filesystem::path& nand_root, const std::filesystem::path& backups_root, const Backup& backup, int keep,
                std::time_t now);
void rotate(const std::filesystem::path& backups_root, int keep);

}
