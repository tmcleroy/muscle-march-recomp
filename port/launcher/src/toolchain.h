#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "runner.h"

struct ToolStatus {
    std::string program;
    std::string version;
    bool usable = false;
    bool bundled = false;
};

class Toolchain {
public:
    Toolchain(std::filesystem::path folder, std::filesystem::path bundled);

    void add_to_path() const;
    std::vector<ToolStatus> inspect() const;
    std::vector<Step> preparation(const std::vector<ToolStatus>& tools, bool offline) const;
    std::string python() const;

private:
    std::filesystem::path folder_;
    std::filesystem::path bundled_;
};
