#include "project.h"

#include <SDL3/SDL.h>

#include "embedded.h"
#include "payload.h"
#include "sha256.h"
#include "subprocess.h"

#include <algorithm>
#include <fstream>
#include <iterator>

#include <vector>

namespace {

bool contains_game(const std::filesystem::path& folder) {
    std::error_code error;
    return std::filesystem::exists(folder / "games" / kGame / "game.toml", error);
}

bool search_up(std::filesystem::path folder, std::filesystem::path& root) {
    while (!folder.empty()) {
        if (contains_game(folder)) {
            root = folder;
            return true;
        }
        std::filesystem::path parent = folder.parent_path();
        if (parent == folder) {
            break;
        }
        folder = parent;
    }
    return false;
}

}

std::filesystem::path Project::game_folder() const {
    return root / "games" / kGame;
}

std::filesystem::path Project::extracted_folder() const {
    return game_folder() / "extracted";
}

std::filesystem::path Project::nand_folder() const {
    return game_folder() / "nand";
}

std::filesystem::path Project::backups_folder() const {
    return game_folder() / "backups";
}

std::filesystem::path Project::built_executable() const {
#ifdef _WIN32
    return root / "build" / "out" / (std::string(kGame) + ".exe");
#else
    return root / "build" / "out" / kGame;
#endif
}

std::filesystem::path Project::installed_executable() const {
    return root / "bin" / built_executable().filename();
}

std::filesystem::path Project::executable() const {
    std::error_code error;
    return packaged && std::filesystem::exists(installed_executable(), error) ? installed_executable() : built_executable();
}

bool Project::install_game(std::string& message) const {
    std::error_code error;
    std::filesystem::path folder = installed_executable().parent_path();
    std::filesystem::create_directories(folder, error);
    std::vector<std::filesystem::path> files = {built_executable()};
    for (const auto& entry : std::filesystem::directory_iterator(built_executable().parent_path(), error)) {
        std::string extension = entry.path().extension().string();
        if (extension == ".dll" || extension == ".so" || extension == ".dylib") {
            files.push_back(entry.path());
        }
    }
    for (const std::filesystem::path& file : files) {
        std::filesystem::path staged = folder / (file.filename().string() + ".new");
        std::filesystem::copy_file(file, staged, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            message = error.message();
            return false;
        }
    }
    for (const std::filesystem::path& file : files) {
        std::filesystem::path staged = folder / (file.filename().string() + ".new");
        std::filesystem::rename(staged, folder / file.filename(), error);
        if (error) {
            message = error.message();
            return false;
        }
    }
    return true;
}

std::string Project::settings_file() const {
    return std::string("games/") + kGame + "/settings.ini";
}

bool Project::extracted() const {
    std::error_code error;
    return std::filesystem::exists(extracted_folder() / "sys" / "main.dol", error);
}

bool Project::built() const {
    std::error_code error;
    return std::filesystem::exists(executable(), error);
}

bool Project::outdated() const {
    return packaged && built() && stored_revision(root) != payload_revision();
}

double Project::needed_gigabytes() const {
    if (built()) {
        return 1.0;
    }
    return extracted() ? 4.0 : 5.0;
}

double Project::free_gigabytes() const {
    std::error_code error;
    std::filesystem::path folder = root;
    while (!folder.empty() && !std::filesystem::exists(folder, error)) {
        std::filesystem::path parent = folder.parent_path();
        if (parent == folder) {
            break;
        }
        folder = parent;
    }
    std::filesystem::space_info info = std::filesystem::space(folder, error);
    return error ? -1.0 : static_cast<double>(info.available) / (1024.0 * 1024.0 * 1024.0);
}

bool find_checkout(std::filesystem::path& root) {
    const char* base = SDL_GetBasePath();
    if (base && search_up(path_from(base), root)) {
        return true;
    }
    std::error_code error;
    return search_up(std::filesystem::current_path(error), root);
}

std::filesystem::path default_install_folder() {
#ifdef _WIN32
    std::string local = environment_value("LOCALAPPDATA");
    if (!local.empty()) {
        return path_from(local) / kInstallFolderName;
    }
#endif
    char* data = SDL_GetPrefPath(nullptr, kInstallFolderName);
    std::filesystem::path folder = data ? path_from(data) : std::filesystem::path(kInstallFolderName);
    SDL_free(data);
    return folder;
}

namespace {

std::filesystem::path translation_stamp(const std::filesystem::path& root) {
    return root / "build" / kGame / "translation.stamp";
}

std::string file_digest(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return sha256_of_bytes(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

}

std::string Project::translation_inputs() const {
    std::vector<std::string> lines;
    std::error_code error;
    auto add_tree = [&](const std::filesystem::path& folder) {
        for (auto it = std::filesystem::recursive_directory_iterator(folder, error); !error && it != std::filesystem::recursive_directory_iterator();
             it.increment(error)) {
            if (it->is_regular_file(error) && it->path().string().find("__pycache__") == std::string::npos) {
                lines.push_back(std::filesystem::relative(it->path(), root, error).generic_u8string() + " " + file_digest(it->path()));
            }
        }
    };
    add_tree(root / "recompiler");
    add_tree(game_folder() / "analysis");
    for (const char* name : {"game.toml"}) {
        lines.push_back(std::string(name) + " " + file_digest(game_folder() / name));
    }
    for (const char* name : {"sys/boot.bin", "sys/main.dol"}) {
        lines.push_back(std::string(name) + " " + file_digest(extracted_folder() / name));
    }
    for (const auto& entry : std::filesystem::directory_iterator(extracted_folder() / "files" / "rel", error)) {
        lines.push_back(entry.path().filename().generic_u8string() + " " + std::to_string(entry.file_size(error)));
    }
    std::sort(lines.begin(), lines.end());
    std::string joined;
    for (const std::string& line : lines) {
        joined += line + "\n";
    }
    return sha256_of_bytes(joined);
}

bool Project::translation_current() const {
    std::error_code error;
    if (!std::filesystem::exists(root / "build" / kGame / "generated" / "dol" / "function_table.cpp", error)) {
        return false;
    }
    std::ifstream file(translation_stamp(root), std::ios::binary);
    std::string stored;
    std::getline(file, stored);
    return !stored.empty() && stored == translation_inputs();
}

void Project::store_translation_stamp() const {
    std::ofstream file(translation_stamp(root), std::ios::binary | std::ios::trunc);
    file << translation_inputs() << '\n';
}

std::string Project::disc_id() const {
    std::ifstream file(extracted_folder() / "sys" / "boot.bin", std::ios::binary);
    char id[6] = {};
    file.read(id, sizeof(id));
    return file ? std::string(id, sizeof(id)) : std::string();
}

std::vector<std::string> Project::disc_languages() const {
    static const std::pair<const char*, const char*> kNames[] = {{"en", "English"},   {"fr", "Français"}, {"sp", "Español"}, {"ge", "Deutsch"},
                                                                  {"it", "Italiano"},  {"du", "Nederlands"}};
    std::vector<std::string> found;
    std::error_code error;
    for (const auto& [code, name] : kNames) {
        for (const auto& entry : std::filesystem::directory_iterator(extracted_folder() / "files" / "locale", error)) {
            if (entry.path().filename().string().rfind(std::string(code) + "_", 0) == 0) {
                found.push_back(name);
                break;
            }
        }
    }
    return found;
}

std::filesystem::path Project::versions_folder() const {
    return game_folder() / "versions";
}

std::vector<std::string> Project::stored_versions() const {
    std::vector<std::string> ids;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(versions_folder(), error)) {
        std::string name = entry.path().filename().string();
        if (name.size() == 6 && std::filesystem::exists(entry.path() / "extracted" / "sys" / "boot.bin", error)) {
            ids.push_back(name);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool Project::store_active_version() const {
    std::string active = disc_id();
    if (active.size() != 6) {
        return false;
    }
    std::error_code error;
    std::filesystem::path parked = versions_folder() / active;
    std::filesystem::create_directories(parked, error);
    std::filesystem::rename(extracted_folder(), parked / "extracted", error);
    if (error) {
        return false;
    }
    if (std::filesystem::exists(installed_executable().parent_path(), error)) {
        std::filesystem::rename(installed_executable().parent_path(), parked / "bin", error);
        if (error) {
            std::error_code ignored;
            std::filesystem::rename(parked / "extracted", extracted_folder(), ignored);
            return false;
        }
    }
    return true;
}

bool Project::switch_version(const std::string& id) const {
    std::filesystem::path stored = versions_folder() / id;
    std::error_code error;
    if (!std::filesystem::exists(stored / "extracted", error) || !store_active_version()) {
        return false;
    }
    std::filesystem::rename(stored / "extracted", extracted_folder(), error);
    if (error) {
        return false;
    }
    if (std::filesystem::exists(stored / "bin", error)) {
        std::filesystem::rename(stored / "bin", installed_executable().parent_path(), error);
    }
    std::filesystem::remove_all(stored, error);
    return true;
}
