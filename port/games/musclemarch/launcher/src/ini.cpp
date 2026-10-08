#include "ini.h"

#include <fstream>

namespace {

std::string trimmed(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

bool section_line(const std::string& line, std::string& name) {
    std::string t = trimmed(line);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
        name = trimmed(t.substr(1, t.size() - 2));
        return true;
    }
    return false;
}

bool key_line(const std::string& line, std::string& key, std::string& value) {
    std::string t = trimmed(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') {
        return false;
    }
    size_t equals = t.find('=');
    if (equals == std::string::npos) {
        return false;
    }
    key = trimmed(t.substr(0, equals));
    value = trimmed(t.substr(equals + 1));
    return true;
}

}  // namespace

Ini::Ini(std::filesystem::path path) : path_(std::move(path)) {}

void Ini::load() {
    lines_.clear();
    std::ifstream file(path_, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines_.push_back(line);
    }
}

bool Ini::save() const {
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    for (const std::string& line : lines_) {
        file << line << '\n';
    }
    return static_cast<bool>(file);
}

std::string Ini::get(const std::string& section, const std::string& key) const {
    std::string current;
    for (const std::string& line : lines_) {
        std::string name;
        std::string k;
        std::string v;
        if (section_line(line, name)) {
            current = name;
        } else if (current == section && key_line(line, k, v) && k == key) {
            return v;
        }
    }
    return "";
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    std::string current;
    size_t section_end = std::string::npos;  // where to insert into an existing section
    for (size_t i = 0; i < lines_.size(); i++) {
        std::string name;
        std::string k;
        std::string v;
        if (section_line(lines_[i], name)) {
            current = name;
            continue;
        }
        if (current != section) {
            continue;
        }
        section_end = i + 1;
        if (key_line(lines_[i], k, v) && k == key) {
            lines_[i] = key + "=" + value;
            return;
        }
    }
    if (section_end == std::string::npos) {
        for (size_t i = 0; i < lines_.size(); i++) {
            std::string name;
            if (section_line(lines_[i], name) && name == section) {
                section_end = i + 1;
            }
        }
    }
    if (section_end == std::string::npos) {
        if (!lines_.empty() && !lines_.back().empty()) {
            lines_.push_back("");
        }
        lines_.push_back("[" + section + "]");
        section_end = lines_.size();
    }
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(section_end), key + "=" + value);
}
