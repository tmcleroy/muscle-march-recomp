#include "wp/android.h"

#include <SDL3/SDL.h>
#include <android/log.h>
#include <csignal>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "wp/game.h"

namespace wp::android {

namespace {

constexpr const char* kAssetFolder = "game";           // in the APK's assets (a development build)
constexpr const char* kManifest = "game/manifest.txt"; // a stamp, then one file per line
constexpr const char* kInstalledStamp = "game/.installed";
constexpr const char* kFirstSettings = "[features]\npreset=enhanced\n";

struct sigaction g_system_handlers[NSIG];

// The system log takes lines; the game writes to stdout and stderr as on a computer.
void forward_output() {
    static int descriptors[2];
    if (pipe(descriptors) != 0) {
        return;
    }
    dup2(descriptors[1], STDOUT_FILENO);
    dup2(descriptors[1], STDERR_FILENO);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([] {
        const char* tag = game::description().short_name;
        std::string line;
        char buffer[1024];
        ssize_t count;
        while ((count = read(descriptors[0], buffer, sizeof buffer)) > 0) {
            for (ssize_t i = 0; i < count; i++) {
                if (buffer[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, tag, line.c_str());
                    line.clear();
                } else {
                    line.push_back(buffer[i]);
                }
            }
        }
    }).detach();
}

void apply_environment_file() {
    std::ifstream file("env.txt");
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos || equals == 0) {
            continue;
        }
        setenv(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str(), 1);
        std::fprintf(stderr, "env.txt: %s\n", line.c_str());
    }
}

std::string read_asset(const char* path) {
    size_t size = 0;
    void* data = SDL_LoadFile(path, &size);
    if (!data) {
        return {};
    }
    std::string text(static_cast<const char*>(data), size);
    SDL_free(data);
    return text;
}

bool copy_asset(const std::string& name) {
    std::string source = std::string(kAssetFolder) + "/" + name;
    std::filesystem::path target = std::filesystem::path(kAssetFolder) / name;
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    SDL_IOStream* in = SDL_IOFromFile(source.c_str(), "rb");
    std::FILE* out = std::fopen(target.c_str(), "wb");
    bool ok = in && out;
    static char buffer[1 << 20];
    while (ok) {
        size_t count = SDL_ReadIO(in, buffer, sizeof buffer);
        if (count == 0) {
            ok = SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF;
            break;
        }
        ok = std::fwrite(buffer, 1, count, out) == count;
    }
    if (in) {
        SDL_CloseIO(in);
    }
    if (out) {
        ok = std::fclose(out) == 0 && ok;
    }
    return ok;
}

// A development build carries the builder's own game files as assets (never a published build:
// docs/BUILDING.md). They are unpacked once per build, as the engine reads plain files.
bool install_packed_game() {
    std::string manifest = read_asset(kManifest);
    if (manifest.empty()) {
        return true;  // nothing packed
    }
    std::stringstream lines(manifest);
    std::string stamp;
    std::getline(lines, stamp);
    std::stringstream installed;
    installed << std::ifstream(kInstalledStamp, std::ios::binary).rdbuf();
    if (installed.str() == stamp) {
        return true;
    }
    std::fprintf(stderr, "installing the game files packed into this build\n");
    std::string name;
    size_t count = 0;
    while (std::getline(lines, name)) {
        if (name.empty()) {
            continue;
        }
        if (!copy_asset(name)) {
            std::fprintf(stderr, "cannot unpack %s\n", name.c_str());
            return false;
        }
        count++;
    }
    std::ofstream(kInstalledStamp, std::ios::binary | std::ios::trunc) << stamp;
    std::fprintf(stderr, "installed %zu game files\n", count);
    return true;
}

}  // namespace

std::vector<std::string> prepare() {
    forward_output();
    const char* folder = SDL_GetAndroidExternalStoragePath();
    if (!folder || !*folder || chdir(folder) != 0) {
        folder = SDL_GetAndroidInternalStoragePath();
        if (!folder || chdir(folder) != 0) {
            std::fprintf(stderr, "cannot enter the app's folder\n");
            return {};
        }
    }
    std::fprintf(stderr, "app folder: %s\n", folder);
    apply_environment_file();
    std::string game = std::string(folder) + "/" + kAssetFolder;
    std::error_code error;
    if (!install_packed_game() || !std::filesystem::exists(game + "/main.dol", error)) {
        std::string message = "This build has no game files.\n\nCopy the files extracted from your own copy of the game to\n" + game;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, game::description().title, message.c_str(), nullptr);
        return {};
    }
    // Without a launcher to choose them, a first start gets the optional features a launcher
    // would have offered (settings.ini in the app's folder changes that).
    const char* settings = game::description().settings_file;
    if (!std::filesystem::exists(settings, error)) {
        std::ofstream(settings, std::ios::binary) << kFirstSettings;
    }
    return {game::description().short_name, game, "0", std::string(folder) + "/" + game::description().nand_directory};
}

void keep_system_handler(int signal) {
    if (signal > 0 && signal < NSIG) {
        sigaction(signal, nullptr, &g_system_handlers[signal]);
    }
}

void finish_crash(int signal) {
    usleep(200000);  // the log thread writes the report out
    if (signal > 0 && signal < NSIG) {
        sigaction(signal, &g_system_handlers[signal], nullptr);  // returning faults again: the system's crash dump
    }
}

}  // namespace wp::android
