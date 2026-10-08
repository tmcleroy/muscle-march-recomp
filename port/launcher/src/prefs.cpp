#include "prefs.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <map>

#include "subprocess.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::filesystem::path prefs_file() {
    char* folder = SDL_GetPrefPath("wiiparty-recomp", "launcher");
    std::filesystem::path path = folder ? path_from(folder) / "launcher.ini" : std::filesystem::path("launcher.ini");
    SDL_free(folder);
    return path;
}

std::string registry_value(const char* key) {
#ifdef _WIN32
    wchar_t value[1024];
    DWORD size = sizeof(value);
    std::wstring name(key, key + std::char_traits<char>::length(key));
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\wiiparty-recomp\\launcher", name.c_str(), RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
        std::filesystem::path text(value);
        return utf8_of(text);
    }
#else
    (void)key;
#endif
    return {};
}

std::map<std::string, std::string>& values() {
    static std::map<std::string, std::string> map = [] {
        std::map<std::string, std::string> result;
        std::ifstream file(prefs_file(), std::ios::binary);
        std::string line;
        while (std::getline(file, line)) {
            size_t equals = line.find('=');
            if (equals != std::string::npos) {
                result[line.substr(0, equals)] = line.substr(equals + 1);
            }
        }
        return result;
    }();
    return map;
}

}

std::string pref(const char* key) {
    auto it = values().find(key);
    if (it != values().end()) {
        return it->second;
    }
    return registry_value(key);
}

void set_pref(const char* key, const std::string& value) {
    values()[key] = value;
    std::ofstream file(prefs_file(), std::ios::binary | std::ios::trunc);
    for (const auto& [name, text] : values()) {
        file << name << '=' << text << '\n';
    }
}
