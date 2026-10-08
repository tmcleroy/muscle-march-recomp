#include "app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include "ui.h"

using ui::Font;
using ui::Kind;
using ui::px;

namespace {

constexpr float kSidebarWidth = 220.0f;
constexpr float kPagePaddingX = 44.0f;
constexpr float kPagePaddingTop = 40.0f;
constexpr float kContentMaxWidth = 720.0f;

// A rounded panel behind its contents.
class Card {
public:
    explicit Card(float width) : list_(ImGui::GetWindowDrawList()), start_(ImGui::GetCursorScreenPos()), width_(width) {
        list_->ChannelsSplit(2);
        list_->ChannelsSetCurrent(1);
    }
    void end() {
        float bottom = ImGui::GetCursorScreenPos().y;
        list_->ChannelsSetCurrent(0);
        list_->AddRectFilled(start_, ImVec2(start_.x + width_, bottom), ui::palette().surface, px(14));
        list_->ChannelsMerge();
    }

private:
    ImDrawList* list_;
    ImVec2 start_;
    float width_;
};

// A card row: label and detail on the left, a control on the right.
template <typename Control>
void row(float width, const char* label, const char* detail, float control_width, float control_height, Control control) {
    const ui::Palette& p = ui::palette();
    ImVec2 top = ImGui::GetCursorScreenPos();
    float pad = px(18);
    ImGui::SetCursorScreenPos(top + ImVec2(pad, px(14)));
    ImGui::BeginGroup();
    float text_width = width - pad * 2 - px(control_width) - px(24);
    ui::text(label, Font::Regular, ui::size::kBody, p.text, text_width);
    if (detail && *detail) {
        ui::gap(3);
        ui::text(detail, Font::Regular, ui::size::kDetail, p.secondary, text_width);
    }
    ImGui::EndGroup();
    float bottom = ImGui::GetItemRectMax().y + px(14);
    float height = bottom - top.y;
    ImGui::SetCursorScreenPos(ImVec2(top.x + width - pad - px(control_width), top.y + std::round((height - px(control_height)) * 0.5f)));
    control();
    ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
    ImGui::Dummy(ImVec2(width, 0));
}

void inset_separator(float width) {
    ImVec2 at = ImGui::GetCursorScreenPos();
    float thickness = std::max(1.0f, std::floor(ui::scale()));
    ImGui::GetWindowDrawList()->AddRectFilled(at + ImVec2(px(18), 0), at + ImVec2(width - px(18), thickness), ui::palette().separator);
    ImGui::Dummy(ImVec2(width, thickness));
}

std::filesystem::path base_path() {
    const char* base = SDL_GetBasePath();
    return base ? std::filesystem::path(base) : std::filesystem::current_path();
}

void wake() {
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
}

const char* kPresetNames[] = {"pure", "enhanced", "custom"};
constexpr const char* kIssuesUrl = "https://github.com/tmcleroy/muscle-march-recomp/issues/new";  // the public repository

}  // namespace

App::App(SDL_Window* window) : window_(window), prefs_(data_folder() / "launcher.ini"), settings_(data_folder() / "settings.ini") {
    prefs_.load();
    settings_.load();
    // The launcher's default is the Enhanced preset (the game alone, with no settings, runs Pure).
    if (prefs_.get("launcher", "preset_chosen").empty()) {
        if (settings_.get("features", "preset").empty()) {
            settings_.set("features", "preset", "enhanced");
            settings_.save();
        }
        prefs_.set("launcher", "preset_chosen", "1");
        prefs_.save();
    }
    load_features();
    have_key_ = key_from_hex(prefs_.get("game", "common_key"), key_);
    std::string remembered = prefs_.get("game", "wad");
    if (!remembered.empty()) {
        use_game_file(remembered);
    }
}

App::~App() {
    for (std::thread* thread : {&checker_, &key_thread_, &install_thread_}) {
        if (thread->joinable()) {
            thread->join();
        }
    }
}

bool App::busy() const {
    return working() || game_.running();
}

bool App::working() const {
    return checking_ || key_checking_ || installing_;
}

namespace {

std::filesystem::path game_program_path() {
#ifdef _WIN32
    return base_path() / "musclemarch.exe";
#else
    std::error_code error;
    std::filesystem::path beside = base_path() / "musclemarch";
    std::filesystem::path bundle = base_path() / ".." / "MacOS" / "musclemarch";  // in a macOS app, the base path is Resources
    return !std::filesystem::exists(beside, error) && std::filesystem::exists(bundle, error) ? bundle : beside;
#endif
}

std::filesystem::path player_folder() {
    if (const char* home = std::getenv("MUSCLE_MARCH_HOME"); home && *home) {  // tests: another folder
        return home;
    }
    char* path = SDL_GetPrefPath("", "Muscle March");
    std::filesystem::path folder = path ? std::filesystem::path(path) : base_path() / "user";
    SDL_free(path);
    return folder;
}

}  // namespace

int play_directly() {
    std::filesystem::path folder = player_folder();
    std::filesystem::path data = folder / "game";
    std::ifstream source(data / "source.txt");
    std::string sha1;
    std::getline(source, sha1);
    if (sha1.empty() || !installed(data, sha1)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Muscle March",
                                 "The game isn't set up yet. Open the launcher (without --play) once to choose your game file and key.", nullptr);
        return 1;
    }
    GameProcess game;
    std::string error;
    std::error_code ignored;
    std::filesystem::create_directories(folder / "logs", ignored);
    if (!game.start(game_program_path(), {data.string(), "0", "nand"}, folder, error, folder / "logs" / "game.log")) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Muscle March", error.c_str(), nullptr);
        return 1;
    }
    while (game.running()) {
        SDL_Delay(100);
    }
    return game.exit_code();
}

std::filesystem::path App::game_program() const {
    return game_program_path();
}

std::filesystem::path App::data_folder() const {
    return player_folder();
}

std::filesystem::path App::installed_data() const {
    return data_folder() / "game";
}

std::filesystem::path App::game_data() const {
    std::string sha1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sha1 = wad_.sha1;
    }
    if (!sha1.empty() && !installing_ && installed(installed_data(), sha1)) {
        return installed_data();
    }
    std::string configured = const_cast<Ini&>(prefs_).get("game", "data");
    std::vector<std::filesystem::path> candidates;
    if (!configured.empty()) {
        candidates.push_back(configured);
    }
    candidates.push_back(base_path() / ".." / ".." / ".." / "extracted");  // a development checkout
    candidates.push_back(base_path() / "extracted");
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate / "main.dol", error)) {
            return std::filesystem::weakly_canonical(candidate, error);
        }
    }
    return {};
}

// ---- game file ---------------------------------------------------------------------------

void App::choose_game_file() {
    static const SDL_DialogFileFilter filters[] = {{"WiiWare WAD", "wad"}};
    std::string start = wad_path_.empty() ? std::string() : std::filesystem::path(wad_path_).parent_path().string();
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
            if (files && files[0]) {
                App* app = static_cast<App*>(self);
                std::lock_guard<std::mutex> lock(app->mutex_);
                app->picked_ = files[0];
            }
            wake();
        },
        this, window_, filters, 1, start.empty() ? nullptr : start.c_str(), false);
}

void App::use_game_file(const std::string& path) {
    if (checking_) {
        return;
    }
    if (checker_.joinable()) {
        checker_.join();
    }
    wad_path_ = path;
    checking_ = true;
    // Remember where it is; the file itself is only ever read.
    prefs_.set("game", "wad", path);
    prefs_.save();
    checker_ = std::thread([this, path] {
        WadCheck result = check_wad(path);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wad_ = result;
        }
        checking_ = false;
        wake();
    });
}

void App::play() {
    play_error_.clear();
    std::filesystem::path data = game_data();
    std::error_code error;
    std::filesystem::create_directories(data_folder(), error);
    settings_.save();
    std::filesystem::create_directories(data_folder() / "logs", error);
    if (!game_.start(game_program(), {data.string(), "0", "nand"}, data_folder(), play_error_, data_folder() / "logs" / "game.log")) {
        return;
    }
    SDL_HideWindow(window_);
    hidden_for_game_ = true;
}

// ---- common key and building the game's files ---------------------------------------------

void App::choose_key_file() {
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
            if (files && files[0]) {
                App* app = static_cast<App*>(self);
                std::lock_guard<std::mutex> lock(app->mutex_);
                app->picked_key_ = files[0];
            }
            wake();
        },
        this, window_, nullptr, 0, nullptr, false);
}

void App::use_key_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> data;
    if (file) {
        file.seekg(0, std::ios::end);
        if (file.tellg() <= (1 << 16)) {
            file.seekg(0);
            data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
    }
    if (data.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        key_message_ = "That file can't be used as a key file.";
        return;
    }
    use_key(std::move(data));
}

void App::use_key(std::vector<uint8_t> supplied) {
    if (key_checking_ || wad_path_.empty()) {
        return;
    }
    if (key_thread_.joinable()) {
        key_thread_.join();
    }
    key_checking_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        key_message_.clear();
    }
    std::string wad = wad_path_;
    key_thread_ = std::thread([this, wad, supplied = std::move(supplied)] {
        KeySearch search = find_common_key(wad, supplied);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (search.found) {
                key_ = search.key;
                have_key_ = true;
                install_tried_.clear();
                install_error_.clear();
            } else {
                key_message_ = search.message;
            }
        }
        if (search.found) {
            // Kept in the player's own launcher.ini, so it is asked for once.
            prefs_.set("game", "common_key", key_to_hex(search.key));
            prefs_.save();
        }
        key_checking_ = false;
        wake();
    });
}

void App::maybe_install() {
    if (installing_ || checking_ || key_checking_ || game_.running()) {
        return;
    }
    WadCheck wad;
    CommonKey key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wad = wad_;
        key = key_;
    }
    bool file_ok = wad.result == WadCheck::Result::Ok || wad.result == WadCheck::Result::OtherRevision;
    if (!file_ok || !have_key_ || install_tried_ == wad.sha1 || installed(installed_data(), wad.sha1)) {
        return;
    }
    if (install_thread_.joinable()) {
        install_thread_.join();
    }
    install_tried_ = wad.sha1;
    installing_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        install_error_.clear();
    }
    std::string path = wad_path_;
    install_thread_ = std::thread([this, path, wad, key] {
        std::string error;
        std::error_code ignored;
        std::filesystem::create_directories(data_folder(), ignored);
        if (!install_game(path, wad.sha1, key, installed_data(), install_progress_, error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            install_error_ = error;
        }
        installing_ = false;
        wake();
    });
}

// ---- features ----------------------------------------------------------------------------

void App::load_features() {
    features_.clear();
    features_error_.clear();
    std::error_code error;
    if (!std::filesystem::exists(game_program(), error)) {
        features_error_ = "The game program wasn't found next to the launcher.";
        return;
    }
    std::istringstream lines(GameProcess::output_of(game_program(), {"--list-features"}));
    std::string line;
    while (std::getline(lines, line)) {
        std::vector<std::string> parts;
        std::stringstream fields(line);
        std::string part;
        while (std::getline(fields, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() < 5) {
            continue;
        }
        Feature feature;
        feature.name = parts[0];
        feature.category = parts[1];
        feature.enhanced = (std::strtoul(parts[2].c_str(), nullptr, 10) & 2u) != 0;
        feature.title = parts[3];
        feature.description = parts[4];
        features_.push_back(feature);
    }
}

std::string App::preset() const {
    std::string value = const_cast<Ini&>(settings_).get("features", "preset");
    return value.empty() ? "pure" : value;
}

bool App::feature_on(const Feature& feature) const {
    std::string value = const_cast<Ini&>(settings_).get("features", feature.name);
    if (!value.empty()) {
        return !(value == "0" || value == "off" || value == "false");
    }
    return preset() == "enhanced" && feature.enhanced;
}

void App::set_preset(const std::string& name) {
    settings_.set("features", "preset", name);
    if (name != "custom") {
        for (const Feature& feature : features_) {
            settings_.set("features", feature.name, "");  // follow the preset
        }
    }
    settings_.save();
}

void App::set_feature(const Feature& changed, bool on) {
    // Any individual change makes the set custom: write every feature's current state.
    if (preset() != "custom") {
        for (const Feature& feature : features_) {
            settings_.set("features", feature.name, feature_on(feature) ? "1" : "0");
        }
        settings_.set("features", "preset", "custom");
    }
    settings_.set("features", changed.name, on ? "1" : "0");
    settings_.save();
}

// ---- frame -------------------------------------------------------------------------------

bool App::consume(const SDL_Event& event) {
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
        // A WAD is the game file; anything small is taken as a key file.
        std::filesystem::path dropped = event.drop.data;
        std::string extension = dropped.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::error_code error;
        if (extension != ".wad" && std::filesystem::file_size(dropped, error) <= (1u << 16) && !error) {
            use_key_file(dropped.string());
        } else {
            use_game_file(dropped.string());
        }
        page_ = Page::Game;
        return true;
    }
    return false;
}

void App::frame() {
    std::string picked;
    std::string picked_key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        picked.swap(picked_);
        picked_key.swap(picked_key_);
    }
    if (!picked.empty()) {
        use_game_file(picked);
    }
    if (!picked_key.empty()) {
        use_key_file(picked_key);
    }
    maybe_install();
    if (hidden_for_game_ && !game_.running()) {
        hidden_for_game_ = false;
        SDL_ShowWindow(window_);
        SDL_RaiseWindow(window_);
    }
    const ui::Palette& p = ui::palette();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    float height = viewport->Size.y;
    ImGui::GetWindowDrawList()->AddRectFilled(viewport->Pos, viewport->Pos + ImVec2(px(kSidebarWidth), height), p.sidebar);
    ImGui::GetWindowDrawList()->AddRectFilled(viewport->Pos + ImVec2(px(kSidebarWidth) - 1.0f, 0), viewport->Pos + ImVec2(px(kSidebarWidth), height),
                                              p.separator);
    sidebar(height);
    ImGui::SetCursorScreenPos(viewport->Pos + ImVec2(px(kSidebarWidth), 0));
    page(viewport->Size.x - px(kSidebarWidth), height);
    ImGui::End();
}

void App::sidebar(float height) {
    const ui::Palette& p = ui::palette();
    ImVec2 origin = ImGui::GetMainViewport()->Pos;
    float inner = kSidebarWidth - 40.0f;
    ImGui::SetCursorScreenPos(origin + ImVec2(px(24), px(28)));
    ui::text("Muscle March", Font::Semibold, 17.0f, p.text);
    ImGui::SetCursorScreenPos(origin + ImVec2(px(24), px(50)));
    ui::text("Version " MUSCLE_MARCH_VERSION, Font::Regular, ui::size::kDetail, p.secondary);
    const char* names[] = {"Game", "Features", "Controls"};
    float top = px(100);
    float step = px(38);
    ImGuiID indicator = ImHashStr("nav-indicator");
    float at = ui::animate(indicator, static_cast<float>(page_), 18.0f);
    ImVec2 pill = origin + ImVec2(px(20), top + at * step);
    ImGui::GetWindowDrawList()->AddRectFilled(pill, pill + ImVec2(px(inner), px(36)), p.control_hover, px(10));
    for (int i = 0; i < static_cast<int>(Page::Count); i++) {
        ImGui::SetCursorScreenPos(origin + ImVec2(px(20), top + step * static_cast<float>(i)));
        ImGui::PushID(i);
        if (ui::nav_item(names[i], static_cast<int>(page_) == i, inner)) {
            page_ = static_cast<Page>(i);
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(origin + ImVec2(px(20), height - px(150)));
    if (ui::button("Report a bug", Kind::Secondary, inner)) {
        report_bug();
    }
    ImGui::SetCursorScreenPos(origin + ImVec2(px(20), height - px(106)));
    if (ui::button("Open folder", Kind::Secondary, inner)) {
        std::error_code error;
        std::filesystem::create_directories(data_folder(), error);
        SDL_OpenURL(("file://" + data_folder().generic_string()).c_str());
    }
    ImGui::SetCursorScreenPos(origin + ImVec2(px(24), height - px(54)));
    ui::text("Not affiliated with Nintendo or Namco Bandai.", Font::Regular, ui::size::kSmall, p.tertiary, px(inner));
}

// A new GitHub issue, filled in with what a bug report needs (the player adds the rest).
void App::report_bug() {
    std::string features;
    for (const Feature& feature : features_) {
        if (feature_on(feature)) {
            features += (features.empty() ? "" : ", ") + feature.name;
        }
    }
    std::string controllers;
    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    for (int i = 0; pads && i < count; i++) {
        const char* name = SDL_GetGamepadNameForID(pads[i]);
        controllers += (controllers.empty() ? "" : ", ") + std::string(name ? name : "unknown");
    }
    SDL_free(pads);
    std::string body = "**What happened, and what did you expect?**\n\n\n**How to make it happen again:**\n\n\n**System**\n";
    body += "- Version: " MUSCLE_MARCH_VERSION "\n";
    body += std::string("- OS: ") + SDL_GetPlatform() + " (add the version)\n";
    body += "- CPU / GPU: \n";
    body += "- Controller: " + (controllers.empty() ? std::string("none connected") : controllers) + "\n";
    body += "- Preset: " + preset() + (features.empty() ? std::string() : " (" + features + ")") + "\n\n";
    body += "**Log:** please attach `logs/game.log` from the launcher's folder (Open folder).\n";
    std::string url = std::string(kIssuesUrl) + "?labels=bug&body=";
    for (unsigned char c : body) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            url += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            url += hex;
        }
    }
    SDL_OpenURL(url.c_str());
}

void App::page(float width, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(kPagePaddingX), px(kPagePaddingTop)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::palette().background);
    ImGui::BeginChild(static_cast<int>(page_) + 100, ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    float content = std::min(width - px(kPagePaddingX) * 2, px(kContentMaxWidth));
    switch (page_) {
    case Page::Game:
        game_page(content);
        break;
    case Page::Features:
        features_page(content);
        break;
    case Page::Controls:
        controllers_page(content);
        break;
    case Page::Count:
        break;
    }
    ui::gap(40);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void App::heading(const char* title, const char* intro, float width) {
    const ui::Palette& p = ui::palette();
    ui::text(title, Font::Semibold, ui::size::kHeading, p.text);
    if (intro) {
        ui::gap(8);
        ui::text(intro, Font::Regular, ui::size::kLead, p.secondary, std::min(width, px(620)));
    }
    ui::gap(26);
}

// ---- Game --------------------------------------------------------------------------------

void App::game_page(float width) {
    const ui::Palette& p = ui::palette();
    heading("Muscle March", "Choose your own copy of the game (the WiiWare WAD). It stays where it is: the launcher only reads it.", width);

    WadCheck wad;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wad = wad_;
    }
    ui::text("GAME FILE", Font::Semibold, ui::size::kDetail, p.secondary);
    ui::gap(10);
    Card card(width);
    std::string shown = wad_path_.empty() ? std::string("No game file chosen. Drop it on this window or choose it.") : wad_path_;
    shown = ui::fit_start(shown, Font::Regular, ui::size::kBody, width - px(200));
    std::string status;
    if (checking_) {
        status = "Checking…";
    } else if (!wad_path_.empty()) {
        status = wad.message;
    }
    ImVec2 row_top = ImGui::GetCursorScreenPos();
    row(width, shown.c_str(), status.c_str(), 130.0f, 36.0f, [&] {
        if (ui::button("Choose…", Kind::Secondary, 130.0f, !checking_)) {
            choose_game_file();
        }
    });
    if (!wad_path_.empty() && !checking_) {
        // A mark at the card's left edge: good file, usable file, or not usable.
        ImVec2 center(row_top.x - px(14), row_top.y + px(24));
        if (wad.result == WadCheck::Result::Ok) {
            ui::check_mark(center, px(12), p.success);
        } else if (wad.result == WadCheck::Result::OtherRevision) {
            ui::check_mark(center, px(12), p.accent);
        } else {
            ui::cross_mark(center, px(12), p.danger);
        }
    }
    card.end();
    ui::gap(24);

    bool file_ok = !checking_ && (wad.result == WadCheck::Result::Ok || wad.result == WadCheck::Result::OtherRevision);
    key_card(width, file_ok);
    ui::gap(24);

    std::filesystem::path data = game_data();
    std::string install_error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        install_error = install_error_;
    }
    ui::text("GAME DATA", Font::Semibold, ui::size::kDetail, p.secondary);
    ui::gap(10);
    Card data_card(width);
    if (installing_) {
        row(width, "Building the game's files…", "Decrypting and unpacking your game file. This takes a few seconds.", 0.0f, 0.0f, [] {});
        ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(px(18), 0));
        ui::progress_bar(ImGui::GetID("install"), install_progress_.fraction.load(), width - px(36), px(6));
        ui::gap(16);
    } else if (!install_error.empty()) {
        row(width, "The game's files couldn't be built", install_error.c_str(), 130.0f, 36.0f, [&] {
            if (ui::button("Try again", Kind::Secondary, 130.0f)) {
                install_tried_.clear();
            }
        });
    } else if (data.empty()) {
        const char* waiting = !file_ok ? "Choose your game file first." : "Waiting for the common key.";
        row(width, "Not built yet", waiting, 0.0f, 0.0f, [] {});
    } else {
        std::string label = ui::fit_start(data.string(), Font::Regular, ui::size::kBody, width - px(60));
        const char* detail = data == installed_data() ? "Built from your game file." : "A development copy (no build from your game file yet).";
        row(width, label.c_str(), detail, 0.0f, 0.0f, [] {});
    }
    data_card.end();
    ui::gap(28);

    bool ready = file_ok && !data.empty() && !installing_ && !game_.running();
    std::string summary = preset() == "pure" ? "Features: Pure (exactly as on the Wii)"
                          : preset() == "enhanced" ? "Features: Enhanced"
                                                   : "Features: Custom";
    ui::text(summary.c_str(), Font::Regular, ui::size::kDetail, p.secondary);
    ui::gap(12);
    if (ui::button(game_.running() ? "Playing…" : "Play", Kind::Primary, 200.0f, ready, 48.0f)) {
        play();
    }
    if (!play_error_.empty()) {
        ui::gap(10);
        ui::text(play_error_.c_str(), Font::Regular, ui::size::kDetail, p.danger, width);
    } else if (!game_.running() && game_.exit_code() != 0) {
        ui::gap(10);
        std::string text = "The game stopped with code " + std::to_string(game_.exit_code()) + ".";
        ui::text(text.c_str(), Font::Regular, ui::size::kDetail, p.danger, width);
    }
    // Linux and Steam Deck: shortcuts for Steam's Game Mode.
    static const bool steam = can_add_to_steam();
    if (steam) {
        ui::gap(28);
        ui::text("STEAM", Font::Semibold, ui::size::kDetail, p.secondary);
        ui::gap(10);
        Card card(width);
        const char* detail = steam_message_.empty() ? "Adds \"Muscle March\" (straight into the game) and \"Muscle March Launcher\" to your Steam library, "
                                                      "to play from Game Mode."
                                                    : steam_message_.c_str();
        row(width, "Play from Steam", detail, 150.0f, 36.0f, [&] {
            if (ui::button("Add to Steam", Kind::Secondary, 150.0f, ready)) {
                steam_ok_ = add_to_steam(steam_message_);
            }
        });
        card.end();
    }
}

void App::key_card(float width, bool file_ok) {
    const ui::Palette& p = ui::palette();
    std::string message;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message = key_message_;
    }
    ui::text("WII COMMON KEY", Font::Semibold, ui::size::kDetail, p.secondary);
    ui::gap(10);
    Card card(width);
    ImVec2 row_top = ImGui::GetCursorScreenPos();
    const char* label = have_key_ ? "Your console's common key is saved" : "Your console's common key";
    std::string detail = key_checking_ ? std::string("Checking the key against your game file…")
                         : !message.empty() ? message
                         : have_key_ ? std::string("It decrypts your game file. It's kept only on this computer.")
                                     : std::string("Needed once, to decrypt your game file. Choose or drop a key file from your Wii "
                                                   "(for example keys.bin from a NAND backup), or enter the key as 32 hexadecimal digits.");
    bool enabled = file_ok && !key_checking_ && !installing_;
    row(width, label, detail.c_str(), 150.0f, 36.0f, [&] {
        if (ui::button(have_key_ ? "Change file…" : "Choose key file…", Kind::Secondary, 150.0f, enabled)) {
            choose_key_file();
        }
    });
    if (have_key_ && message.empty() && !key_checking_) {
        ui::check_mark(ImVec2(row_top.x - px(14), row_top.y + px(24)), px(12), p.success);
    } else if (!message.empty() && !key_checking_) {
        ui::cross_mark(ImVec2(row_top.x - px(14), row_top.y + px(24)), px(12), p.danger);
    }
    inset_separator(width);
    // Typing or pasting the key.
    ImVec2 top = ImGui::GetCursorScreenPos();
    float pad = px(18);
    float button_width = px(110);
    ImGui::SetCursorScreenPos(top + ImVec2(pad, px(12)));
    ui::push_font(Font::Regular, ui::size::kBody);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(8));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(10), (px(36) - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.background);
    ImGui::SetNextItemWidth(width - pad * 3 - button_width);
    ImGui::BeginDisabled(!enabled);
    bool entered = ImGui::InputTextWithHint("##key", "Key: 32 hexadecimal digits", key_text_, sizeof key_text_,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ui::pop_font();
    ImGui::SameLine(0, pad);
    bool typed = key_text_[0] != 0;
    if (ui::button("Use key", Kind::Secondary, 110.0f, enabled && typed) || (entered && enabled && typed)) {
        std::string text = key_text_;
        use_key(std::vector<uint8_t>(text.begin(), text.end()));
        std::memset(key_text_, 0, sizeof key_text_);
    }
    ImGui::SetCursorScreenPos(ImVec2(top.x, ImGui::GetItemRectMax().y + px(12)));
    ImGui::Dummy(ImVec2(width, 0));
    card.end();
}

// ---- Features ----------------------------------------------------------------------------

void App::features_page(float width) {
    const ui::Palette& p = ui::palette();
    heading("Features", "Pure plays the game exactly as it was on the Wii. Enhanced turns on the improvements; or pick your own.", width);

    std::vector<std::string> labels = {"Pure: as on the Wii", "Enhanced: recommended", "Custom"};
    int index = 0;
    for (int i = 0; i < 3; i++) {
        if (preset() == kPresetNames[i]) {
            index = i;
        }
    }
    if (ui::dropdown("##preset", labels, index, 280.0f)) {
        set_preset(kPresetNames[index]);
    }
    ui::gap(26);

    if (!features_error_.empty()) {
        ui::text(features_error_.c_str(), Font::Regular, ui::size::kBody, p.danger, width);
        return;
    }
    if (features_.empty()) {
        ui::text("No optional features yet.", Font::Regular, ui::size::kBody, p.secondary, width);
        return;
    }
    std::vector<std::string> categories;
    for (const Feature& feature : features_) {
        if (std::find(categories.begin(), categories.end(), feature.category) == categories.end()) {
            categories.push_back(feature.category);
        }
    }
    for (const std::string& category : categories) {
        std::string upper = category;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        ui::text(upper.c_str(), Font::Semibold, ui::size::kDetail, p.secondary);
        ui::gap(10);
        Card card(width);
        bool first = true;
        for (const Feature& feature : features_) {
            if (feature.category != category) {
                continue;
            }
            if (!first) {
                inset_separator(width);
            }
            first = false;
            ImGui::PushID(feature.name.c_str());
            bool on = feature_on(feature);
            row(width, feature.title.c_str(), feature.description.c_str(), 42.0f, 24.0f, [&] {
                if (ui::toggle("##on", on)) {
                    set_feature(feature, !on);
                }
            });
            ImGui::PopID();
        }
        card.end();
        ui::gap(24);
    }
}

// ---- Controls ----------------------------------------------------------------------------

void App::controllers_page(float width) {
    const ui::Palette& p = ui::palette();
    heading("Controls",
            "Strike the pose each wall shows before you reach it, and shake to run. Connected gamepads and Wii Remotes show up below; "
            "press buttons and move them to check they work.",
            width);

    // How to play with each kind of controller.
    struct Help {
        const char* title;
        const char* text;
    };
    const Help help[] = {
        {"Gamepad",
         "Left stick moves the left arm, right stick the right arm: point each up or down to strike the pose. Wiggle both sticks to "
         "run. A / B confirm and go back, Start or X pauses, the right stick points in menus. In Pure mode: the right stick picks a "
         "whole pose, hold the left trigger to run, X pauses."},
        {"Keyboard",
         "W, A, S, D (or the arrows, or I, J, K, L) strike the poses: up = both arms up, down = both down, left / right = that "
         "arm up. Left Shift runs. "
         "Enter / Backspace are A / B, 1 pauses, H is Home, the mouse points in menus. Keys can be changed in settings.ini (Open "
         "folder)."},
        {"Wii Remote",
         "Pair it in your system's Bluetooth settings: press the red sync button under the battery cover (or 1 and 2), then "
         "connect it there; it then appears below. Buttons work as on the Wii; motion controls with a real Wii Remote are "
         "experimental."},
    };
    ui::text("HOW TO PLAY", Font::Semibold, ui::size::kDetail, p.secondary);
    ui::gap(10);
    Card help_card(width);
    for (size_t i = 0; i < sizeof help / sizeof help[0]; i++) {
        if (i > 0) {
            inset_separator(width);
        }
        row(width, help[i].title, help[i].text, 0.0f, 0.0f, [] {});
    }
    help_card.end();
    ui::gap(24);

    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    ui::text("CONNECTED", Font::Semibold, ui::size::kDetail, p.secondary);
    ui::gap(10);
    Card card(width);
    if (count == 0) {
        row(width, "No controllers connected", "", 0.0f, 0.0f, [] {});
    }
    for (int i = 0; i < count && i < 4; i++) {
        if (i > 0) {
            inset_separator(width);
        }
        const char* name = SDL_GetGamepadNameForID(pads[i]);
        std::string slot = "Wii Remote " + std::to_string(i + 1);
        ImGui::PushID(i);
        row(width, name ? name : "?", slot.c_str(), 110.0f, 36.0f, [&] {
            if (ui::button(selected_pad_ == i ? "Testing" : "Test", selected_pad_ == i ? Kind::Primary : Kind::Secondary, 110.0f)) {
                selected_pad_ = i;
            }
        });
        ImGui::PopID();
    }
    card.end();
    ui::gap(24);

    if (count > 0) {
        int index = std::min(selected_pad_, count - 1);
        SDL_Gamepad* pad = SDL_OpenGamepad(pads[index]);  // stays open while the launcher runs
        if (pad) {
            ui::text("TEST", Font::Semibold, ui::size::kDetail, p.secondary);
            ui::gap(10);
            Card test(width);
            std::string pressed;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
                if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) {
                    const char* label = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b));
                    pressed += pressed.empty() ? "" : "  ";
                    pressed += label ? label : "?";
                }
            }
            row(width, "Buttons", pressed.empty() ? "(press any button)" : pressed.c_str(), 0.0f, 0.0f, [] {});
            inset_separator(width);
            auto axis = [&](SDL_GamepadAxis a) { return static_cast<float>(SDL_GetGamepadAxis(pad, a)) / 32767.0f; };
            char sticks[128];
            std::snprintf(sticks, sizeof sticks, "left %+.2f %+.2f   right %+.2f %+.2f", axis(SDL_GAMEPAD_AXIS_LEFTX), axis(SDL_GAMEPAD_AXIS_LEFTY),
                          axis(SDL_GAMEPAD_AXIS_RIGHTX), axis(SDL_GAMEPAD_AXIS_RIGHTY));
            row(width, "Sticks", sticks, 0.0f, 0.0f, [] {});
            inset_separator(width);
            std::string motion = "No motion sensor";
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
                float accel[3] = {};
                if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3)) {
                    char text[96];
                    std::snprintf(text, sizeof text, "x %+.1f  y %+.1f  z %+.1f m/s²", accel[0], accel[1], accel[2]);
                    motion = text;
                }
            }
            row(width, "Motion", motion.c_str(), 0.0f, 0.0f, [] {});
            test.end();
            ui::gap(24);
            ui::request_frames(2);  // keep the readings live
        }
    }
    SDL_free(pads);

}
