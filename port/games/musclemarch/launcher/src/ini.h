#pragma once
// A small INI file editor that keeps everything it does not touch: the game owns settings.ini
// and rewrites it with the keys it knows, the launcher only changes a few of them.

#include <filesystem>
#include <string>
#include <vector>

class Ini {
public:
    explicit Ini(std::filesystem::path path);
    void load();
    bool save() const;
    std::string get(const std::string& section, const std::string& key) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::vector<std::string> lines_;
};
