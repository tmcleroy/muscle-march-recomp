#pragma once
// The Muscle March launcher: the player's game file, the optional features, controllers, Play.

#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "game_process.h"
#include "ini.h"
#include "install.h"
#include "steam.h"
#include "wad.h"

// --play: run the game set up earlier (the player's folder, its built game files and settings),
// without the launcher's window; returns the game's exit code.
int play_directly();

enum class Page { Game, Features, Video, Controls, Count };

struct Feature {
    std::string name;
    std::string category;
    bool enhanced = false;  // on in the Enhanced preset
    std::string title;
    std::string description;
};

class App {
public:
    App(SDL_Window* window);
    ~App();
    // Window events the launcher handles itself (dropped files); false: pass to ImGui.
    bool consume(const SDL_Event& event);
    void frame();
    void set_page(Page page) { page_ = page; }
    bool busy() const;      // background work or the game running (screenshots wait for it)
    bool working() const;   // background work that needs redraws (not the game itself)

private:
    // Paths.
    std::filesystem::path game_program() const;
    std::filesystem::path data_folder() const;      // per player: settings, saves
    std::filesystem::path game_data() const;        // the game's files (built from the WAD)
    std::filesystem::path installed_data() const;   // where the launcher builds them

    // Game file.
    void choose_game_file();
    void use_game_file(const std::string& path);
    void play();

    // Common key, and building the game's files from the WAD.
    void choose_key_file();
    void use_key_file(const std::string& path);
    void use_key(std::vector<uint8_t> supplied);   // checked against the game file (a thread)
    void maybe_install();                          // builds the files when file and key are ready
    void key_card(float width, bool file_ok);

    // Features.
    void load_features();
    std::string preset() const;
    bool feature_on(const Feature& feature) const;
    void set_preset(const std::string& preset);
    void set_feature(const Feature& feature, bool on);

    void sidebar(float height);
    void report_bug();
    void page(float width, float height);
    void heading(const char* title, const char* intro, float width);
    void game_page(float width);
    void features_page(float width);
    void video_page(float width);
    void controllers_page(float width);

    SDL_Window* window_;
    Page page_ = Page::Game;
    Ini prefs_;     // launcher.ini: the game file's location
    Ini settings_;  // the game's settings.ini: features, video

    mutable std::mutex mutex_;
    std::string picked_;          // from the file dialog (another thread)
    std::string wad_path_;
    WadCheck wad_;
    bool checking_ = false;
    std::thread checker_;

    std::string picked_key_;      // from the key file dialog (another thread)
    std::atomic<bool> have_key_{false};  // key_ decrypts the game file (saved in launcher.ini)
    CommonKey key_{};
    std::atomic<bool> key_checking_{false};
    std::string key_message_;     // why the last key was refused (guarded by mutex_)
    std::thread key_thread_;
    char key_text_[96] = {};

    std::atomic<bool> installing_{false};
    InstallProgress install_progress_;
    std::string install_error_;   // guarded by mutex_
    std::string install_tried_;   // the WAD SHA-1 last built (or failed), so a failure isn't retried every frame
    std::thread install_thread_;

    std::vector<Feature> features_;
    std::string features_error_;

    GameProcess game_;
    std::string play_error_;
    bool hidden_for_game_ = false;  // the window is hidden while the game runs
    std::string steam_message_;
    bool steam_ok_ = false;
    int selected_pad_ = 0;
};
