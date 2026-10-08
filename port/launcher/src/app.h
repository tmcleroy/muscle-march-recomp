#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "project.h"
#include "runner.h"
#include "toolchain.h"
#include "ui.h"
#include "updater.h"
#include "wp/save_backup.h"

struct TexturePack {
    std::string name;
    bool enabled = false;
    size_t files = 0;
    uintmax_t bytes = 0;
};

enum class Page { Game, Settings, Saves, Controls, Textures, Licenses, Count };

class App {
public:
    App(SDL_Window* window, Project project, bool install_mode);
    ~App();

    bool consume(const SDL_Event& event);
    void frame();
    void set_page(Page page);
    bool busy() const;
    bool quit() const;
    int exit_code() const;
    void apply_theme();
    void add_on_start(const std::string& disc);
    bool use_version_now(const std::string& id);

private:
    void use_root(const std::filesystem::path& root);
    void store(const std::string& key, const std::string& value);
    std::string value(const std::string& key) const;
    void wake();

    void sidebar(float height);
    void page(float width, float height);
    void game_page(float width, float height);
    void play_panel(float width, float height);
    void install_panel(float width);
    void progress_panel(float width, float height);
    void settings_page(float width);
    void saves_page(float width);
    void controls_page(float width);
    void licenses_page(float width, float height);
    void textures_page(float width);
    void versions_section(float width);
    void gamepads_section(float width);
    void whats_new_card(float width);
    void create_report();
    void choose_added_disc();
    void switch_version(const std::string& id);
    void restore_modal();
    void launcher_update_card(float width);
    void repair_modal();
    void repair();
    void start_update_check();
    void start_estimate();
    void update_launcher();
    void heading(const char* title, const char* intro, float width);

    void choose_disc();
    void choose_folder();
    void install(bool adding = false);
    void finish_install();
    void play();
    void refresh_backups();
    void bind(size_t action, const std::string& name);
    void stop_capture();

    SDL_Window* window_;
    Project project_;
    Toolchain toolchain_;
    Runner runner_;
    bool install_mode_;
    bool quit_ = false;
    int exit_code_ = 0;
    Page page_ = Page::Game;
    bool progress_view_ = false;
    bool was_running_ = false;
    bool show_details_ = false;
    bool started_ = false;
    std::vector<std::string> log_;
    std::string result_;
    Outcome outcome_ = Outcome::None;
    std::string disc_;
    std::vector<wp::saves::Backup> backups_;
    std::vector<std::string> backup_sizes_;
    std::string saves_message_;
    int restore_index_ = -1;
    int license_ = 0;
    int capturing_ = -1;
    bool capture_reserved_ = false;
    bool swallow_release_ = false;
    ImRect capture_rect_;
    std::mutex picked_mutex_;
    std::string picked_disc_;
    std::string picked_folder_;
    std::string picked_added_disc_;
    std::string add_disc_;
    std::vector<TexturePack> packs_;
    bool packs_dirty_ = true;
    std::string versions_message_;
    bool versions_failed_ = false;
    int last_step_ = -1;
    std::thread download_thread_;
    std::string disc_line_;
    bool repair_confirm_ = false;
    std::string launcher_message_;
    bool launcher_message_failed_ = false;
    std::atomic<int> launcher_state_{0};
    std::string launcher_error_;
    bool swap_pending_ = false;
};

void wake_main_loop();
