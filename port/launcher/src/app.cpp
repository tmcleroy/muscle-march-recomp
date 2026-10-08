#include "app.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <fstream>
#include <memory>

#include "embedded.h"
#include "keys.h"
#include "payload.h"
#include "prefs.h"
#include "sha256.h"
#include "shortcut.h"
#include "wp/screenshot.h"
#include "subprocess.h"
#include "texts.h"
#include "wp/keymap.h"
#include "wp/options.h"
#include "wp/settings.h"
#include "wp/ui_text.h"

#ifdef _WIN32
#include <dwmapi.h>
#include <windows.h>
#endif

using ui::Font;
using ui::Kind;
using ui::px;

namespace {

struct Background {
    std::mutex mutex;
    Release release;
    Release current;
    bool found = false;
    std::atomic<bool> estimating{false};
    std::atomic<int> minutes{0};
    std::atomic<bool> full{false};
};

const char* region_name(const std::string& id) {
    const Texts& t = texts();
    char region = id.size() >= 4 ? id[3] : 'P';
    return region == 'E' ? t.region_usa : region == 'J' ? t.region_japan : region == 'K' ? t.region_korea : t.region_pal;
}

std::string read_disc_id(const std::filesystem::path& extracted) {
    std::ifstream file(extracted / "sys" / "boot.bin", std::ios::binary);
    char id[6] = {};
    file.read(id, sizeof(id));
    return file ? std::string(id, sizeof(id)) : std::string();
}

std::string tail_of(const std::filesystem::path& path, size_t lines) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> kept;
    for (std::string line; std::getline(file, line);) {
        kept.push_back(line);
        if (kept.size() > lines) {
            kept.erase(kept.begin());
        }
    }
    std::string text;
    for (const std::string& line : kept) {
        text += line + "\n";
    }
    return text.empty() ? "(none)\n" : text;
}

std::string folder_url(const std::filesystem::path& folder) {
    std::string url = "file:///" + utf8_of(folder.lexically_normal());
    std::replace(url.begin(), url.end(), '\\', '/');
    return url;
}

Background& background() {
    static Background* state = new Background;
    return *state;
}

constexpr float kSidebarWidth = 232.0f;
constexpr float kPagePaddingX = 48.0f;
constexpr float kPagePaddingTop = 44.0f;
constexpr float kContentMaxWidth = 720.0f;
constexpr size_t kLogLimit = 6000;
constexpr size_t kLongestRoot = 100;

std::atomic<bool> g_wake_pending{false};
Uint32 g_wake_event = 0;

struct License {
    const char* blob;
    const char* name;
};

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

std::string first_disc(const std::filesystem::path& folder) {
    std::error_code error;
    std::vector<std::string> found;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".iso" || extension == ".wbfs" || extension == ".rvz" || extension == ".ciso" || extension == ".wia" || extension == ".gcm") {
            found.push_back(utf8_of(entry.path()));
        }
    }
    std::sort(found.begin(), found.end());
    return found.empty() ? std::string() : found.front();
}

std::string backup_date(const std::string& name) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    if (std::sscanf(name.c_str(), "%4d-%2d-%2d_%2d-%2d", &year, &month, &day, &hour, &minute) != 5 || month < 1 || month > 12) {
        return name;
    }
    static const char* english[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char* spanish[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
    char buffer[96];
    if (std::string(texts().game) == "Juego") {
        std::snprintf(buffer, sizeof(buffer), "%d de %s de %d, %02d:%02d", day, spanish[month - 1], year, hour, minute);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%d %s %d, %02d:%02d", day, english[month - 1], year, hour, minute);
    }
    return buffer;
}

std::string folder_kilobytes(const std::filesystem::path& folder) {
    std::error_code error;
    uintmax_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, error)) {
        if (entry.is_regular_file(error)) {
            total += entry.file_size(error);
        }
    }
    return std::to_string((total + 1023) / 1024);
}

std::string shown_keys(const std::string& value) {
    std::string text = wp::keymap::format(wp::keymap::parse(value).codes, ", ");
    return text.empty() ? texts().keys_none : text;
}

std::string gigabytes(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    return buffer;
}

int compile_jobs() {
    int cores = std::max(1, SDL_GetNumLogicalCPUCores());
    int memory = SDL_GetSystemRAM();
    int by_memory = memory > 0 ? (memory - 1536) / 640 : cores;
    return std::clamp(by_memory, 1, cores);
}

void apply_interface_language(const std::string& value) {
    bool spanish = value == "es";
    use_spanish(spanish);
    wp::ui::set_language(spanish ? wp::ui::Language::Spanish : wp::ui::Language::English);
}

void state_icon(StepState state, ImVec2 center) {
    const ui::Palette& p = ui::palette();
    float size = px(12);
    switch (state) {
    case StepState::Running:
        ui::spinner(center, px(6.5f), p.accent);
        break;
    case StepState::Done:
        ui::check_mark(center, size, p.text);
        break;
    case StepState::Failed:
        ui::cross_mark(center, size, p.danger);
        break;
    case StepState::Skipped:
        ImGui::GetWindowDrawList()->AddLine(center - ImVec2(px(4), 0), center + ImVec2(px(4), 0), p.tertiary, px(1.8f));
        break;
    case StepState::Waiting:
        ImGui::GetWindowDrawList()->AddCircleFilled(center, px(2.5f), p.tertiary, 12);
        break;
    }
}

}

void wake_main_loop() {
    if (g_wake_event == 0) {
        g_wake_event = SDL_RegisterEvents(1);
    }
    if (!g_wake_pending.exchange(true)) {
        SDL_Event event{};
        event.type = g_wake_event;
        SDL_PushEvent(&event);
    }
}

App::App(SDL_Window* window, Project project, bool install_mode)
    : window_(window), project_(std::move(project)), toolchain_(project_.root / "build" / "deps" / "toolchain", launcher_path().parent_path() / "tools"), install_mode_(install_mode) {
    use_root(project_.root);
    disc_ = pref("disc");
    if (disc_.empty()) {
        disc_ = first_disc(project_.game_folder() / "disc");
    }
    remove_old_launcher();
    if (pref("seen_version").empty()) {
        set_pref("seen_version", WP_LAUNCHER_VERSION);
    }
    start_update_check();
    start_estimate();
    wake_main_loop();
}

App::~App() {
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
}

void App::start_update_check() {
    if (pref("check_updates") == "0" || pref("offline") == "1") {
        return;
    }
    std::thread([] {
        Release release;
        Release current;
        std::string error;
        bool listed = latest_release(release, current, WP_LAUNCHER_VERSION, error);
        std::lock_guard<std::mutex> lock(background().mutex);
        background().current = current;
        if (listed && newer_version(release.version, WP_LAUNCHER_VERSION)) {
            background().release = release;
            background().found = true;
        }
        wake_main_loop();
    }).detach();
}

void App::start_estimate() {
    if (!project_.outdated() || background().estimating.exchange(true)) {
        return;
    }
    std::filesystem::path root = project_.root;
    int jobs = compile_jobs();
    std::thread([root, jobs] {
        std::string message;
        if (stage_payload(root, message)) {
            PayloadChanges changes = payload_changes(root);
            background().full = changes.full_rebuild;
            background().minutes = changes.full_rebuild ? std::max(5, (120 + jobs - 1) / std::max(jobs, 1)) : 2;
        }
        background().estimating = false;
        wake_main_loop();
    }).detach();
}

void App::update_launcher() {
    Release release;
    {
        std::lock_guard<std::mutex> lock(background().mutex);
        release = background().release;
    }
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
    launcher_state_ = 1;
    launcher_error_.clear();
    download_thread_ = std::thread([this, release] {
        std::filesystem::path target = launcher_path();
        target += ".download";
        std::string error;
        bool ok = download(release.url, target, error);
        if (ok && sha256_of(target) != release.sha256) {
            error = texts().hash_mismatch;
            ok = false;
        }
        if (ok) {
            ok = replace_launcher(target, error);
        }
        if (!ok) {
            std::error_code ignored;
            std::filesystem::remove(target, ignored);
            std::lock_guard<std::mutex> lock(background().mutex);
            launcher_error_ = error;
        }
        launcher_state_ = ok ? 3 : 2;
        wake_main_loop();
    });
}

void App::launcher_update_card(float width) {
    Release release;
    std::string failure;
    {
        std::lock_guard<std::mutex> lock(background().mutex);
        if (!background().found || launcher_state_ == 3) {
            return;
        }
        release = background().release;
        failure = launcher_error_;
    }
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    std::string title = format(t.launcher_update_title, "v" + release.version);
    bool failed = launcher_state_ == 2;
    std::string detail = failed ? format(t.launcher_update_failed, failure) : std::string(t.launcher_update_detail);
    Card card(width);
    ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(top + ImVec2(px(20), px(18)));
    ImGui::BeginGroup();
    ui::text(title.c_str(), Font::Semibold, ui::size::kBody, p.text);
    float text_width = width - px(release.page.empty() ? 230 : 420);
    if (!failed && !release.summary.empty()) {
        ui::gap(4);
        ui::text(release.summary.c_str(), Font::Regular, ui::size::kDetail, p.text, text_width);
    }
    ui::gap(4);
    ui::text(detail.c_str(), Font::Regular, ui::size::kDetail, failed ? p.danger : p.secondary, text_width);
    ImGui::EndGroup();
    float bottom = ImGui::GetItemRectMax().y + px(18);
    ImGui::SetCursorScreenPos(ImVec2(top.x + width - px(20) - px(180), top.y + std::round((bottom - top.y - px(36)) * 0.5f)));
    bool downloading = launcher_state_ == 1;
    if (ui::button(downloading ? t.launcher_downloading : t.launcher_update, Kind::Secondary, 180.0f, !downloading && !runner_.running())) {
        update_launcher();
    }
    if (!release.page.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(top.x + width - px(20) - px(180) - px(8) - px(170), top.y + std::round((bottom - top.y - px(36)) * 0.5f)));
        if (ui::button(t.release_notes, Kind::Ghost, 170.0f)) {
            SDL_OpenURL(release.page.c_str());
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
    ImGui::Dummy(ImVec2(width, 0));
    card.end();
    ui::gap(20);
}

void App::use_root(const std::filesystem::path& root) {
    project_.root = root;
    disc_line_.clear();
    std::error_code error;
    std::filesystem::create_directories(project_.game_folder(), error);
    std::filesystem::current_path(root, error);
    toolchain_ = Toolchain(root / "build" / "deps" / "toolchain", launcher_path().parent_path() / "tools");
    toolchain_.add_to_path();
    wp::settings::load(project_.settings_file());
    apply_interface_language(value("system.interface_language"));
}

void App::apply_theme() {
    ui::set_dark(SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK);
    ui::apply_style(SDL_GetWindowDisplayScale(window_));
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd) {
        BOOL dark = ui::dark();
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        COLORREF caption = ui::dark() ? RGB(26, 26, 29) : RGB(245, 245, 247);
        DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    }
#endif
    ui::request_frames(2);
}

void App::store(const std::string& key, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(project_.game_folder(), error);
    wp::settings::store(key.c_str(), text);
    if (key == "system.interface_language") {
        apply_interface_language(text);
    }
}

std::string App::value(const std::string& key) const {
    return wp::settings::text(key.c_str(), nullptr);
}

void App::wake() {
    wake_main_loop();
}

bool App::busy() const {
    return runner_.running() || capturing_ >= 0;
}

bool App::quit() const {
    return quit_;
}

int App::exit_code() const {
    return exit_code_;
}

void App::set_page(Page page) {
    if (page == page_) {
        return;
    }
    stop_capture();
    page_ = page;
    ui::set_value(ImHashStr("page-transition"), 0.0f);
    if (page == Page::Saves) {
        refresh_backups();
    }
}

bool App::consume(const SDL_Event& event) {
    if (event.type == g_wake_event) {
        g_wake_pending = false;
        return true;
    }
    if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED || event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        apply_theme();
        return false;
    }
    if (capturing_ < 0) {
        if (swallow_release_ && event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            swallow_release_ = false;
            return true;
        }
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (event.key.repeat) {
            return true;
        }
        if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
            stop_capture();
            return true;
        }
        std::string name = key_event_name(event.key);
        if (name.empty()) {
            return true;
        }
        if (wp::keymap::reserved(wp::keymap::key_code(name))) {
            capture_reserved_ = true;
            return true;
        }
        bind(static_cast<size_t>(capturing_), name);
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (!capture_rect_.Contains(ImVec2(event.button.x, event.button.y))) {
            stop_capture();
            return false;
        }
        std::string name = mouse_button_name(event.button.button);
        if (!name.empty()) {
            swallow_release_ = true;
            bind(static_cast<size_t>(capturing_), name);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        float x = 0;
        float y = 0;
        SDL_GetMouseState(&x, &y);
        if (event.wheel.y != 0.0f && capture_rect_.Contains(ImVec2(x, y))) {
            bind(static_cast<size_t>(capturing_), event.wheel.y > 0.0f ? "WheelUp" : "WheelDown");
            return true;
        }
        return false;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        stop_capture();
        return false;
    default:
        return false;
    }
}

void App::bind(size_t action, const std::string& name) {
    std::string key = wp::keymap::setting_key(action);
    std::vector<int> codes = wp::keymap::parse(value(key)).codes;
    int code = wp::keymap::key_code(name);
    if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
        codes.push_back(code);
    }
    store(key, wp::keymap::format(codes));
    stop_capture();
}

void App::stop_capture() {
    capturing_ = -1;
    capture_reserved_ = false;
}

void App::frame() {
    {
        std::lock_guard<std::mutex> lock(picked_mutex_);
        if (!picked_disc_.empty()) {
            disc_ = picked_disc_;
            set_pref("disc", disc_);
            picked_disc_.clear();
            result_.clear();
        }
        if (!picked_added_disc_.empty()) {
            add_disc_ = picked_added_disc_;
            picked_added_disc_.clear();
            install(true);
        }
        if (!picked_folder_.empty()) {
            std::filesystem::path folder = path_from(picked_folder_);
            if (folder.filename() != kInstallFolderName) {
                folder /= kInstallFolderName;
            }
            set_pref("root", utf8_of(folder));
            use_root(folder);
            picked_folder_.clear();
        }
    }
    runner_.take_output(log_);
    if (log_.size() > kLogLimit) {
        log_.erase(log_.begin(), log_.begin() + static_cast<long>(log_.size() - kLogLimit));
    }
    bool running = runner_.running();
    if (was_running_ && !running) {
        finish_install();
    }
    was_running_ = running;
    if (launcher_state_ == 3) {
        quit_ = true;
    }
    if (install_mode_ && !started_) {
        started_ = true;
        install();
        if (!runner_.running()) {
            quit_ = true;
            exit_code_ = 2;
        }
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
    ImGui::GetWindowDrawList()->AddRectFilled(viewport->Pos + ImVec2(px(kSidebarWidth) - 1.0f, 0), viewport->Pos + ImVec2(px(kSidebarWidth), height), p.separator);
    sidebar(height);
    ImGui::SetCursorScreenPos(viewport->Pos + ImVec2(px(kSidebarWidth), 0));
    page(viewport->Size.x - px(kSidebarWidth), height);
    restore_modal();
    repair_modal();
    ImGui::End();
}

void App::sidebar(float height) {
    const ui::Palette& p = ui::palette();
    ImVec2 origin = ImGui::GetMainViewport()->Pos;
    float inner = kSidebarWidth - 40.0f;
    ui::die(origin + ImVec2(px(24), px(28)), px(40));
    ImGui::SetCursorScreenPos(origin + ImVec2(px(76), px(30)));
    ui::text("Wii Party", Font::Semibold, 16.0f, p.text);
    ImGui::SetCursorScreenPos(origin + ImVec2(px(76), px(51)));
    ui::text("Recomp", Font::Regular, ui::size::kDetail, p.secondary);
    const Texts& t = texts();
    const char* names[] = {t.game, t.settings, t.saves, t.controls, t.page_textures, t.licenses};
    float top = px(100);
    float step = px(36) + px(2);
    ImGuiID indicator = ImHashStr("nav-indicator");
    float at = ui::animate(indicator, static_cast<float>(page_), 18.0f);
    ImVec2 pill = origin + ImVec2(px(20), top + at * step);
    ImGui::GetWindowDrawList()->AddRectFilled(pill, pill + ImVec2(px(inner), px(36)), p.control_hover, px(10));
    for (int i = 0; i < static_cast<int>(Page::Count); i++) {
        ImGui::SetCursorScreenPos(origin + ImVec2(px(20), top + step * static_cast<float>(i)));
        ImGui::PushID(i);
        if (ui::nav_item(names[i], static_cast<int>(page_) == i, inner)) {
            set_page(static_cast<Page>(i));
        }
        ImGui::PopID();
    }
    std::string version = std::string(WP_LAUNCHER_VERSION) + "  ·  " + WP_LAUNCHER_BUILD;
    ImGui::SetCursorScreenPos(origin + ImVec2(px(24), height - px(36)));
    ui::text(version.c_str(), Font::Regular, ui::size::kSmall, p.tertiary);
}

void App::page(float width, float height) {
    ImGuiID transition = ImHashStr("page-transition");
    float shown = ui::ease_out(ui::animate(transition, 1.0f, 9.0f));
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(kPagePaddingX), px(kPagePaddingTop) + std::round((1.0f - shown) * px(10))));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::palette().background);
    bool scrolls = page_ == Page::Settings || page_ == Page::Saves || page_ == Page::Controls || page_ == Page::Textures;
    ImGui::BeginChild(static_cast<int>(page_) + 100, ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      scrolls ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    float content = std::min(width - px(kPagePaddingX) * 2, px(kContentMaxWidth));
    float available = height - px(kPagePaddingTop) - px(32);
    switch (page_) {
    case Page::Game:
        game_page(content, available);
        break;
    case Page::Settings:
        settings_page(content);
        break;
    case Page::Saves:
        saves_page(content);
        break;
    case Page::Controls:
        controls_page(content);
        break;
    case Page::Textures:
        textures_page(content);
        break;
    case Page::Licenses:
        licenses_page(content, available);
        break;
    case Page::Count:
        break;
    }
    if (scrolls) {
        ui::gap(40);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (shown < 1.0f) {
        ImGui::GetForegroundDrawList()->AddRectFilled(origin, origin + ImVec2(width, height), ui::fade(ui::palette().background, 1.0f - shown));
    }
}

void App::heading(const char* title, const char* intro, float width) {
    const ui::Palette& p = ui::palette();
    ui::text(title, Font::Semibold, ui::size::kHeading, p.text);
    if (intro) {
        ui::gap(8);
        ui::text(intro, Font::Regular, ui::size::kLead, p.secondary, std::min(width, px(620)));
    }
    ui::gap(28);
}

void App::game_page(float width, float height) {
    if (!progress_view_) {
        float before = ImGui::GetCursorScreenPos().y;
        launcher_update_card(width);
        whats_new_card(width);
        height -= ImGui::GetCursorScreenPos().y - before;
    }
    if (progress_view_) {
        progress_panel(width, height);
    } else if (project_.built() && project_.extracted()) {
        play_panel(width, height);
    } else {
        install_panel(width);
    }
}

void App::play_panel(float width, float height) {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    ImVec2 start = ImGui::GetCursorScreenPos();
    float hero = px(72 + 28 + 48 + 6 + 20 + 24 + 32 + 48) + (project_.outdated() ? px(36 + 96) : 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + std::max(0.0f, std::round((height - px(40) - hero) * 0.42f))));
    ui::die(ImGui::GetCursorScreenPos(), px(72));
    ImGui::Dummy(ImVec2(px(72), px(72)));
    ui::gap(28);
    ui::text("Wii Party", Font::Semibold, ui::size::kDisplay, p.text);
    ui::gap(6);
    ui::text(t.status_ready, Font::Regular, ui::size::kLead, p.secondary);
    if (disc_line_.empty()) {
        std::string id = project_.disc_id();
        disc_line_ = id + "  ·  " + region_name(id);
        std::vector<std::string> languages = project_.disc_languages();
        for (size_t i = 0; i < languages.size(); i++) {
            disc_line_ += (i == 0 ? "  ·  " : ", ") + languages[i];
        }
    }
    ui::gap(6);
    ui::text(disc_line_.c_str(), Font::Regular, ui::size::kDetail, p.tertiary, width);
    ui::gap(26);
    if (ui::button(t.play, Kind::Primary, 180.0f, true, 48.0f)) {
        play();
    }
    if (!result_.empty()) {
        ui::gap(14);
        ui::text(result_.c_str(), Font::Regular, ui::size::kDetail, outcome_ == Outcome::Success ? p.secondary : p.danger, width);
    }
    if (project_.outdated()) {
        ui::gap(36);
        Card card(width);
        ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(top + ImVec2(px(20), px(18)));
        ImGui::BeginGroup();
        ui::text(t.update_title, Font::Semibold, ui::size::kBody, p.text);
        ui::gap(4);
        ui::text(t.update_detail, Font::Regular, ui::size::kDetail, p.secondary, width - px(180));
        if (background().minutes > 0) {
            ui::gap(4);
            std::string estimate = format(background().full ? t.update_full : t.update_quick, std::to_string(background().minutes.load()));
            ui::text(estimate.c_str(), Font::Regular, ui::size::kDetail, p.secondary, width - px(180));
        }
        ImGui::EndGroup();
        float bottom = ImGui::GetItemRectMax().y + px(18);
        ImGui::SetCursorScreenPos(ImVec2(top.x + width - px(20) - px(110), top.y + std::round((bottom - top.y - px(36)) * 0.5f)));
        if (ui::button(t.update, Kind::Secondary, 110.0f)) {
            install();
        }
        ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
        ImGui::Dummy(ImVec2(width, 0));
        card.end();
    }
    const std::pair<const char*, const char*> shortcuts[] = {
        {"F10", t.shortcut_options}, {"F11", t.shortcut_fullscreen}, {"F9", t.shortcut_screenshot}, {"F12", t.shortcut_capture}};
    float x = start.x;
    float y = start.y + height - px(24);
    for (const auto& [key, name] : shortcuts) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        float label_x = x + ui::keycap(key) + px(8);
        ImVec2 size = ui::text_size(name, Font::Regular, ui::size::kDetail);
        ImGui::SetCursorScreenPos(ImVec2(label_x, y + std::round((px(24) - size.y) * 0.5f)));
        ui::text(name, Font::Regular, ui::size::kDetail, p.secondary);
        x = label_x + size.x + px(24);
    }
}

void App::install_panel(float width) {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    ImVec2 start = ImGui::GetCursorScreenPos();
    ui::die(start, px(56));
    ImGui::Dummy(ImVec2(px(56), px(56)));
    ui::gap(24);
    ui::text("Wii Party", Font::Semibold, ui::size::kDisplay, p.text);
    ui::gap(6);
    ui::text(t.status_install, Font::Regular, ui::size::kLead, p.secondary, width);
    ui::gap(28);
    Card card(width);
    auto line = [&](const char* label, const std::string& shown, bool muted, const char* action, bool enabled, auto on_click) {
        ImVec2 top = ImGui::GetCursorScreenPos();
        float pad = px(18);
        ImGui::SetCursorScreenPos(top + ImVec2(pad, px(16)));
        ImGui::BeginGroup();
        ui::text(label, Font::Semibold, ui::size::kDetail, p.secondary);
        ui::gap(4);
        std::string fitted = ui::fit_start(shown, Font::Regular, ui::size::kBody, width - pad * 2 - px(130));
        ui::text(fitted.c_str(), Font::Regular, ui::size::kBody, muted ? p.tertiary : p.text);
        ImGui::EndGroup();
        float bottom = ImGui::GetItemRectMax().y + px(16);
        ImGui::SetCursorScreenPos(ImVec2(top.x + width - pad - px(100), top.y + std::round((bottom - top.y - px(36)) * 0.5f)));
        ImGui::PushID(label);
        if (ui::button(action, Kind::Secondary, 100.0f, enabled)) {
            on_click();
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
        ImGui::Dummy(ImVec2(width, 0));
    };
    if (project_.packaged) {
        line(t.install_folder, utf8_of(project_.root.lexically_normal().make_preferred()), false, t.change, true, [this] { choose_folder(); });
        inset_separator(width);
    }
    if (project_.extracted()) {
        line(t.disc, t.disc_extracted, false, t.choose, false, [] {});
    } else {
        std::string name = disc_.empty() ? t.disc_none : utf8_of(path_from(disc_).filename());
        line(t.disc, name, disc_.empty(), t.choose, true, [this] { choose_disc(); });
    }
    card.end();
    ui::gap(16);
    double free = project_.free_gigabytes();
    if (free >= 0.0) {
        bool enough = free >= project_.needed_gigabytes();
        std::string space = format(enough ? t.space_needed : t.space_short, gigabytes(project_.needed_gigabytes()), gigabytes(free));
        ui::text(space.c_str(), Font::Semibold, ui::size::kDetail, enough ? p.text : p.danger, std::min(width, px(620)));
        ui::gap(6);
    }
    ui::text(t.install_intro, Font::Regular, ui::size::kDetail, p.secondary, std::min(width, px(620)));
    ui::gap(28);
    if (ui::button(t.install, Kind::Primary, 160.0f, true, 44.0f)) {
        install();
    }
    if (!result_.empty()) {
        ui::gap(14);
        ui::text(result_.c_str(), Font::Regular, ui::size::kDetail, p.danger, width);
    }
}

void App::progress_panel(float width, float height) {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    RunnerView view = runner_.view();
    bool running = runner_.running();
    ImVec2 start = ImGui::GetCursorScreenPos();
    const char* title = running ? t.installing : outcome_ == Outcome::Cancelled ? t.build_cancelled : t.build_failed;
    ui::text(running ? t.installing : t.install, Font::Semibold, ui::size::kHeading, p.text);
    ui::gap(8);
    std::string status;
    if (running && view.current >= 0 && view.current < static_cast<int>(view.titles.size())) {
        status = view.titles[static_cast<size_t>(view.current)];
    } else if (!running) {
        status = title;
    }
    ui::text(status.c_str(), Font::Regular, ui::size::kLead, running ? p.secondary : (outcome_ == Outcome::Failure ? p.danger : p.secondary), width);
    ui::gap(20);
    ImGuiID bar = ImHashStr("install-progress");
    ui::progress_bar(bar, view.progress, width / ui::scale(), 6.0f);
    ui::gap(10);
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(std::round(view.progress * 100.0f)));
    ImVec2 under = ImGui::GetCursorScreenPos();
    ui::text(percent, Font::Semibold, ui::size::kDetail, p.text);
    if (running && view.remaining_seconds > 0.0) {
        int minutes = static_cast<int>(std::ceil(view.remaining_seconds / 60.0));
        std::string left = minutes <= 1 ? t.minute_left : format(t.minutes_left, std::to_string(minutes));
        ImVec2 size = ui::text_size(left.c_str(), Font::Regular, ui::size::kDetail);
        ImGui::SetCursorScreenPos(ImVec2(under.x + width - size.x, under.y));
        ui::text(left.c_str(), Font::Regular, ui::size::kDetail, p.secondary);
    }
    ui::gap(24);

    float footer = px(36) + px(24);
    float top = ImGui::GetCursorScreenPos().y;
    float room = start.y + height - footer - top;
    float steps_height = std::min(show_details_ ? std::max(px(120), room * 0.42f) : room, px(30) * static_cast<float>(view.titles.size()) + px(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(8)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(14));
    ImGui::BeginChild("steps", ImVec2(width, steps_height), ImGuiChildFlags_AlwaysUseWindowPadding);
    for (size_t i = 0; i < view.titles.size(); i++) {
        ImVec2 at = ImGui::GetCursorScreenPos();
        float line_height = px(30);
        StepState state = view.states[i];
        if (state == StepState::Running) {
            ImGui::GetWindowDrawList()->AddRectFilled(at, at + ImVec2(ImGui::GetContentRegionAvail().x, line_height), p.control, px(8));
        }
        state_icon(state, at + ImVec2(px(18), line_height * 0.5f));
        ImU32 ink = state == StepState::Waiting || state == StepState::Skipped ? p.tertiary : state == StepState::Failed ? p.danger : p.text;
        ui::push_font(state == StepState::Running ? Font::Semibold : Font::Regular, ui::size::kBody - 0.5f);
        float font_height = ImGui::GetFontSize();
        ImGui::GetWindowDrawList()->AddText(at + ImVec2(px(36), std::round((line_height - font_height) * 0.5f)), ink, view.titles[i].c_str());
        ui::pop_font();
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, line_height));
        if (state == StepState::Running && last_step_ != static_cast<int>(i)) {
            last_step_ = static_cast<int>(i);
            ImGui::SetScrollHereY(0.5f);
        }
    }
    ImGui::EndChild();
    if (show_details_) {
        ui::gap(12);
        ImGui::BeginChild("log", ImVec2(width, std::max(px(80), room - steps_height - px(12))), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar);
        bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(4);
        ui::push_font(Font::Regular, ui::size::kDetail);
        ImGui::PushStyleColor(ImGuiCol_Text, p.secondary);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(log_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                ImGui::TextUnformatted(log_[static_cast<size_t>(i)].c_str());
            }
        }
        ImGui::PopStyleColor();
        ui::pop_font();
        if (at_bottom) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height - px(36)));
    if (ui::button(show_details_ ? t.hide_details : t.show_details, Kind::Ghost, 0.0f)) {
        show_details_ = !show_details_;
    }
    if (running) {
        std::error_code error;
        if (project_.packaged && std::filesystem::exists(project_.installed_executable(), error)) {
            ImGui::SetCursorScreenPos(ImVec2(start.x + width - px(120) - px(10) - px(230), start.y + height - px(36)));
            if (ui::button(t.play_current, Kind::Secondary, 230.0f)) {
                play();
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - px(120), start.y + height - px(36)));
        if (ui::button(t.cancel, Kind::Secondary, 120.0f)) {
            runner_.cancel();
        }
    } else {
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - px(120) - px(10) - px(110), start.y + height - px(36)));
        if (ui::button(t.back, Kind::Ghost, 110.0f)) {
            progress_view_ = false;
            result_.clear();
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - px(120), start.y + height - px(36)));
        if (ui::button(t.try_again, Kind::Primary, 120.0f)) {
            install();
        }
    }
}

void App::settings_page(float width) {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    heading(t.settings, t.settings_intro, width);
    std::vector<std::string> keys = wp::settings::keys();
    size_t i = 0;
    while (i < keys.size()) {
        std::string group = keys[i].substr(0, keys[i].find('.'));
        ui::text(group_name(keys[i]), Font::Semibold, ui::size::kDetail, p.secondary);
        ui::gap(10);
        Card card(width);
        bool first = true;
        for (; i < keys.size() && keys[i].rfind(group + ".", 0) == 0; i++) {
            const std::string& key = keys[i];
            if (!first) {
                inset_separator(width);
            }
            first = false;
            std::vector<std::string> choices = wp::options::choices(key);
            std::string current = value(key);
            bool binary = choices.size() == 2 && choices[0] == "0" && choices[1] == "1";
            ImGui::PushID(key.c_str());
            if (binary) {
                row(width, wp::ui::label(key.c_str()), option_detail(key), 42.0f, 24.0f, [&] {
                    bool on = !(current == "0" || current == "false" || current == "off" || current == "no");
                    if (ui::toggle("toggle", on)) {
                        store(key, on ? "0" : "1");
                    }
                });
            } else {
                std::vector<std::string> labels;
                int index = -1;
                for (size_t c = 0; c < choices.size(); c++) {
                    labels.push_back(wp::options::shown_value(key, choices[c]));
                    if (choices[c] == current) {
                        index = static_cast<int>(c);
                    }
                }
                if (index < 0) {
                    labels.push_back(current);
                    choices.push_back(current);
                    index = static_cast<int>(choices.size()) - 1;
                }
                row(width, wp::ui::label(key.c_str()), option_detail(key), 200.0f, 36.0f, [&] {
                    if (ui::dropdown("choice", labels, index, 200.0f)) {
                        store(key, choices[static_cast<size_t>(index)]);
                    }
                });
            }
            ImGui::PopID();
        }
        card.end();
        ui::gap(28);
    }
    versions_section(width);
    ui::text(t.group_launcher, Font::Semibold, ui::size::kDetail, p.secondary);
    ui::gap(10);
    Card card(width);
    ImGui::PushID("launcher");
    row(width, t.offline_mode, t.offline_mode_detail, 42.0f, 24.0f, [&] {
        bool on = pref("offline") == "1";
        if (ui::toggle("offline", on)) {
            set_pref("offline", on ? "0" : "1");
        }
    });
    inset_separator(width);
    row(width, t.check_updates, t.check_updates_detail, 42.0f, 24.0f, [&] {
        bool on = pref("check_updates") != "0" && pref("offline") != "1";
        if (ui::toggle("toggle", on)) {
            set_pref("check_updates", on ? "0" : "1");
            if (!on) {
                set_pref("offline", "0");
            }
        }
    });
    inset_separator(width);
    row(width, t.shortcut_label, t.shortcut_detail, 230.0f, 36.0f, [&] {
        ImVec2 at = ImGui::GetCursorScreenPos();
        std::error_code missing;
        std::filesystem::path icon = std::filesystem::absolute(project_.root / "games" / kGame / "res" / (std::string(kGame) + ".svg"), missing);
        if (ui::button(t.shortcut_launcher, Kind::Secondary, 110.0f)) {
            std::string error;
            launcher_message_failed_ = !create_desktop_shortcut(launcher_path(), launcher_path().parent_path(), icon, "Wii Party Recomp", error);
            launcher_message_ = launcher_message_failed_ ? format(t.shortcut_failed, error) : std::string(t.shortcut_done);
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x + px(120), at.y));
        if (ui::button(t.shortcut_game, Kind::Secondary, 110.0f, std::filesystem::exists(project_.executable(), missing))) {
            std::string error;
            launcher_message_failed_ = !create_desktop_shortcut(std::filesystem::absolute(project_.executable(), missing),
                                                                std::filesystem::absolute(project_.root, missing), icon, "Wii Party", error);
            launcher_message_ = launcher_message_failed_ ? format(t.shortcut_failed, error) : std::string(t.shortcut_done);
        }
    });
    inset_separator(width);
    row(width, t.screenshots_label, t.screenshots_detail, 140.0f, 36.0f, [&] {
        if (ui::button(t.open_folder, Kind::Secondary, 140.0f)) {
            std::filesystem::path folder = project_.root / wp::screenshot::kFolder;
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            std::string url = "file:///" + utf8_of(folder.lexically_normal());
            std::replace(url.begin(), url.end(), '\\', '/');
            SDL_OpenURL(url.c_str());
        }
    });
    inset_separator(width);
    row(width, t.report_label, t.report_detail, 140.0f, 36.0f, [&] {
        if (ui::button(t.report_create, Kind::Secondary, 140.0f)) {
            create_report();
        }
    });
    if (project_.built() && project_.extracted()) {
        inset_separator(width);
        row(width, t.repair_label, t.repair_detail, 110.0f, 36.0f, [&] {
            if (ui::button(t.repair, Kind::Secondary, 110.0f, !runner_.running())) {
                repair_confirm_ = true;
            }
        });
    }
    ImGui::PopID();
    card.end();
    if (!launcher_message_.empty()) {
        ui::gap(10);
        ui::text(launcher_message_.c_str(), Font::Regular, ui::size::kDetail, launcher_message_failed_ ? p.danger : p.secondary, width);
    }
    ui::gap(28);
}

void App::repair() {
    std::error_code error;
    std::filesystem::remove_all(project_.root / "build" / "out", error);
    std::filesystem::remove_all(project_.root / "build" / kGame, error);
    set_page(Page::Game);
    install();
}

void App::repair_modal() {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    if (repair_confirm_ && !ImGui::IsPopupOpen("repair")) {
        ImGui::OpenPopup("repair");
    }
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(440), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(22)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(16));
    if (ImGui::BeginPopupModal("repair", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        float inner = px(440) - px(48);
        int jobs = compile_jobs();
        std::string minutes = std::to_string(std::max(5, (120 + jobs - 1) / std::max(jobs, 1)));
        ui::text(t.repair_title, Font::Semibold, 17.0f, p.text);
        ui::gap(8);
        ui::text(format(t.repair_question, minutes).c_str(), Font::Regular, ui::size::kBody, p.secondary, inner);
        ui::gap(24);
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(at.x + inner - px(110) - px(10) - px(110), at.y));
        if (ui::button(t.cancel, Kind::Ghost, 110.0f)) {
            repair_confirm_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x + inner - px(110), at.y));
        if (ui::button(t.repair, Kind::Primary, 110.0f)) {
            repair_confirm_ = false;
            ImGui::CloseCurrentPopup();
            repair();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void App::refresh_backups() {
    backups_ = wp::saves::list(project_.backups_folder());
    backup_sizes_.clear();
    for (const wp::saves::Backup& backup : backups_) {
        backup_sizes_.push_back(format(texts().backup_size, folder_kilobytes(backup.path)));
    }
}

void App::saves_page(float width) {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    ImVec2 top = ImGui::GetCursorScreenPos();
    heading(t.saves, t.saves_intro, width - px(150));
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(top.x + width - px(140), top.y));
    if (ui::button(t.open_folder, Kind::Secondary, 140.0f)) {
        std::error_code error;
        std::filesystem::create_directories(project_.backups_folder(), error);
        std::string url = "file:///" + utf8_of(project_.backups_folder().lexically_normal());
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
    }
    ImGui::SetCursorScreenPos(after);
    Card card(width);
    if (backups_.empty()) {
        bool disabled = value("saves.backups") == "0";
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(at + ImVec2(px(18), px(18)));
        ui::text(disabled ? t.saves_disabled : t.saves_empty, Font::Regular, ui::size::kBody, p.secondary, width - px(36));
        ui::gap(18);
    }
    for (size_t i = 0; i < backups_.size(); i++) {
        if (i > 0) {
            inset_separator(width);
        }
        ImGui::PushID(static_cast<int>(i));
        std::string date = backup_date(backups_[i].name);
        row(width, date.c_str(), backup_sizes_[i].c_str(), 110.0f, 36.0f, [&] {
            if (ui::button(t.restore, Kind::Secondary, 110.0f)) {
                restore_index_ = static_cast<int>(i);
                ImGui::OpenPopup("restore");
                ui::set_value(ImHashStr("restore-fade"), 0.0f);
            }
        });
        ImGui::PopID();
    }
    card.end();
    if (!saves_message_.empty()) {
        ui::gap(14);
        ui::text(saves_message_.c_str(), Font::Regular, ui::size::kDetail, p.secondary, width);
    }
}

void App::restore_modal() {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    if (restore_index_ >= 0 && !ImGui::IsPopupOpen("restore")) {
        ImGui::OpenPopup("restore");
    }
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(440), 0));
    float shown = ui::ease_out(ui::animate(ImHashStr("restore-fade"), 1.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(22)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(16));
    if (ImGui::BeginPopupModal("restore", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        if (restore_index_ < 0 || restore_index_ >= static_cast<int>(backups_.size())) {
            restore_index_ = -1;
            ImGui::CloseCurrentPopup();
        } else {
            const wp::saves::Backup backup = backups_[static_cast<size_t>(restore_index_)];
            std::string date = backup_date(backup.name);
            float inner = px(440) - px(48);
            ui::text(t.restore_title, Font::Semibold, 17.0f, p.text);
            ui::gap(8);
            ui::text(format(t.restore_question, date).c_str(), Font::Regular, ui::size::kBody, p.secondary, inner);
            ui::gap(24);
            ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(at.x + inner - px(110) - px(10) - px(110), at.y));
            if (ui::button(t.cancel, Kind::Ghost, 110.0f)) {
                restore_index_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x + inner - px(110), at.y));
            if (ui::button(t.restore, Kind::Primary, 110.0f)) {
                int keep = std::atoi(value("saves.backups").c_str());
                wp::saves::Outcome outcome = wp::saves::restore(project_.nand_folder(), project_.backups_folder(), backup, keep, std::time(nullptr));
                switch (outcome.result) {
                case wp::saves::Result::Created:
                    saves_message_ = format(t.restore_done, date);
                    break;
                case wp::saves::Result::Unchanged:
                    saves_message_ = format(t.restore_unchanged, date);
                    break;
                default:
                    saves_message_ = format(t.restore_failed, outcome.message);
                    break;
                }
                restore_index_ = -1;
                refresh_backups();
                ImGui::CloseCurrentPopup();
            }
        }
        if (shown < 1.0f) {
            ImGuiWindow* modal = ImGui::GetCurrentWindow();
            modal->DrawList->AddRectFilled(modal->Pos, modal->Pos + modal->Size, ui::fade(ImGui::GetColorU32(ImGuiCol_PopupBg), 1.0f - shown), px(16));
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void App::controls_page(float width) {
    const Texts& t = texts();
    ImVec2 top = ImGui::GetCursorScreenPos();
    heading(t.controls, t.controls_intro, width - px(180));
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(top.x + width - px(170), top.y));
    if (ui::button(t.controls_reset, Kind::Secondary, 170.0f)) {
        stop_capture();
        for (size_t i = 0; i < wp::keymap::kActionCount; i++) {
            store(wp::keymap::setting_key(i), wp::keymap::action(i).defaults);
        }
    }
    ImGui::SetCursorScreenPos(after);
    gamepads_section(width);
    Card card(width);
    for (size_t i = 0; i < wp::keymap::kActionCount; i++) {
        if (i > 0) {
            inset_separator(width);
        }
        const wp::keymap::ActionInfo& info = wp::keymap::action(i);
        std::string key = wp::keymap::setting_key(i);
        bool capturing = capturing_ == static_cast<int>(i);
        std::string shown = capturing ? (capture_reserved_ ? t.keys_reserved : t.keys_press) : shown_keys(value(key));
        ImGui::PushID(static_cast<int>(i));
        row(width, action_name(info.name), action_detail(info.name), 290.0f + 8.0f + 76.0f, 34.0f, [&] {
            ImRect rect;
            if (ui::key_chip("chip", shown.c_str(), capturing, 290.0f, rect)) {
                capturing_ = static_cast<int>(i);
                capture_reserved_ = false;
            }
            if (capturing) {
                capture_rect_ = rect;
            }
            ImGui::SameLine(0, px(8));
            if (ui::button(t.keys_clear, Kind::Ghost, 76.0f, true, 34.0f)) {
                stop_capture();
                store(key, "");
            }
        });
        ImGui::PopID();
    }
    card.end();
}

void App::licenses_page(float width, float height) {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    ImVec2 start = ImGui::GetCursorScreenPos();
    heading(t.licenses, t.licenses_intro, width);
    const License licenses[] = {
        {"license_project", t.license_project}, {"license_notices", t.license_notices}, {"license_sdl", "SDL 3"},
        {"license_imgui", "Dear ImGui"},        {"license_inter", "Inter"},              {"license_winpthreads", "mingw-w64 winpthreads"},
        {"license_gcc", t.license_runtime},
    };
    float list_width = 210.0f;
    float room = start.y + height - ImGui::GetCursorScreenPos().y;
    ImGui::BeginGroup();
    for (int i = 0; i < static_cast<int>(std::size(licenses)); i++) {
        ImGui::PushID(i);
        if (ui::list_item(licenses[i].name, license_ == i, list_width)) {
            license_ = i;
        }
        ImGui::PopID();
        ui::gap(2);
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, px(20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(14));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface);
    ImGui::BeginChild(license_ + 500, ImVec2(width - px(list_width) - px(20), room), ImGuiChildFlags_AlwaysUseWindowPadding);
    std::string_view body = blob(licenses[license_].blob);
    ui::push_font(Font::Regular, ui::size::kDetail);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(body.data(), body.data() + body.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::pop_font();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void App::choose_disc() {
    static const SDL_DialogFileFilter filter[] = {{nullptr, "iso;wbfs;rvz;ciso;wia;gcm"}};
    static std::string name;
    name = texts().disc_filter;
    SDL_DialogFileFilter filters[] = {{name.c_str(), filter[0].pattern}};
    std::string start = disc_.empty() ? utf8_of(project_.game_folder()) : utf8_of(path_from(disc_).parent_path());
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
            if (files && files[0]) {
                App* app = static_cast<App*>(self);
                std::lock_guard<std::mutex> lock(app->picked_mutex_);
                app->picked_disc_ = files[0];
            }
            wake_main_loop();
        },
        this, window_, filters, 1, start.c_str(), false);
}

bool App::use_version_now(const std::string& id) {
    return project_.switch_version(id);
}

void App::add_on_start(const std::string& disc) {
    started_ = true;
    std::lock_guard<std::mutex> lock(picked_mutex_);
    picked_added_disc_ = disc;
}

void App::choose_added_disc() {
    static const SDL_DialogFileFilter filter[] = {{nullptr, "iso;wbfs;rvz;ciso;wia;gcm"}};
    static std::string name;
    name = texts().disc_filter;
    SDL_DialogFileFilter filters[] = {{name.c_str(), filter[0].pattern}};
    std::string start = disc_.empty() ? utf8_of(project_.game_folder()) : utf8_of(path_from(disc_).parent_path());
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
            if (files && files[0]) {
                App* app = static_cast<App*>(self);
                std::lock_guard<std::mutex> lock(app->picked_mutex_);
                app->picked_added_disc_ = files[0];
            }
            wake_main_loop();
        },
        this, window_, filters, 1, start.c_str(), false);
}

void App::switch_version(const std::string& id) {
    if (runner_.running()) {
        return;
    }
    versions_failed_ = !project_.switch_version(id);
    versions_message_ = versions_failed_ ? texts().version_switch_failed : "";
    disc_line_.clear();
    std::error_code error;
    if (!versions_failed_ && (!project_.packaged || !std::filesystem::exists(project_.installed_executable(), error))) {
        set_page(Page::Game);
        install();
    }
}

void App::versions_section(float width) {
    if (!project_.extracted()) {
        return;
    }
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    ui::text(t.versions_title, Font::Semibold, ui::size::kDetail, p.secondary);
    ui::gap(10);
    Card card(width);
    std::string active = project_.disc_id();
    std::string active_label = active + "  ·  " + region_name(active);
    row(width, active_label.c_str(), t.versions_detail, 110.0f, 20.0f, [&] {
        ui::text(t.version_active, Font::Semibold, ui::size::kDetail, p.secondary);
    });
    for (const std::string& id : project_.stored_versions()) {
        inset_separator(width);
        std::string label = id + "  ·  " + region_name(id);
        std::error_code error;
        bool built = std::filesystem::exists(project_.versions_folder() / id / "bin", error);
        ImGui::PushID(id.c_str());
        row(width, label.c_str(), built ? "" : t.version_needs_build, 110.0f, 36.0f, [&] {
            if (ui::button(t.version_use, Kind::Secondary, 110.0f, !runner_.running())) {
                switch_version(id);
            }
        });
        ImGui::PopID();
    }
    inset_separator(width);
    row(width, t.version_add, t.version_add_detail, 110.0f, 36.0f, [&] {
        if (ui::button(t.choose, Kind::Secondary, 110.0f, !runner_.running())) {
            choose_added_disc();
        }
    });
    card.end();
    if (!versions_message_.empty()) {
        ui::gap(10);
        ui::text(versions_message_.c_str(), Font::Regular, ui::size::kDetail, versions_failed_ ? p.danger : p.secondary, width);
    }
    ui::gap(28);
}

void App::gamepads_section(float width) {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    ui::text(t.gamepads_title, Font::Semibold, ui::size::kDetail, p.secondary);
    ui::gap(10);
    Card card(width);
    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    if (count == 0) {
        row(width, t.gamepads_none, "", 0.0f, 0.0f, [] {});
    }
    for (int i = 0; i < count && i < 4; i++) {
        if (i > 0) {
            inset_separator(width);
        }
        const char* name = SDL_GetGamepadNameForID(pads[i]);
        std::string slot = format(t.gamepad_slot, std::to_string(i + 1));
        ImGui::PushID(i);
        row(width, name ? name : "?", "", 120.0f, 20.0f, [&] {
            ui::text(slot.c_str(), Font::Semibold, ui::size::kDetail, p.secondary);
        });
        ImGui::PopID();
    }
    SDL_free(pads);
    card.end();
    ui::gap(28);
}

void App::whats_new_card(float width) {
    Release current;
    {
        std::lock_guard<std::mutex> lock(background().mutex);
        current = background().current;
    }
    if (current.summary.empty() || pref("seen_version") == WP_LAUNCHER_VERSION) {
        return;
    }
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    std::string title = format(t.whats_new_title, current.version);
    Card card(width);
    ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(top + ImVec2(px(20), px(18)));
    ImGui::BeginGroup();
    ui::text(title.c_str(), Font::Semibold, ui::size::kBody, p.text);
    ui::gap(4);
    ui::text(current.summary.c_str(), Font::Regular, ui::size::kDetail, p.secondary, width - px(330));
    ImGui::EndGroup();
    float bottom = ImGui::GetItemRectMax().y + px(18);
    float middle = top.y + std::round((bottom - top.y - px(36)) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(top.x + width - px(20) - px(110), middle));
    if (ui::button(t.dismiss, Kind::Secondary, 110.0f)) {
        set_pref("seen_version", WP_LAUNCHER_VERSION);
    }
    ImGui::SetCursorScreenPos(ImVec2(top.x + width - px(20) - px(110) - px(8) - px(170), middle));
    if (ui::button(t.release_notes, Kind::Ghost, 170.0f)) {
        SDL_OpenURL(current.page.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
    ImGui::Dummy(ImVec2(width, 0));
    card.end();
    ui::gap(20);
}

void App::create_report() {
    const Texts& t = texts();
    std::error_code error;
    std::filesystem::path folder = project_.root / "reports";
    std::filesystem::create_directories(folder, error);
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
    std::filesystem::path path = folder / (std::string("report-") + stamp + ".txt");
    std::ofstream out(path, std::ios::binary);
    std::string id = project_.disc_id();
    out << "Wii Party Recomp report\n\n";
    out << "Launcher: " << WP_LAUNCHER_VERSION << " (build " << WP_LAUNCHER_BUILD << "), files " << payload_revision() << "\n";
    out << "Platform: " << SDL_GetPlatform() << ", " << SDL_GetNumLogicalCPUCores() << " logical cores, " << SDL_GetSystemRAM() << " MB RAM\n";
    out << "Disc: " << (id.empty() ? "(not extracted)" : id + " " + region_name(id)) << "\n";
    for (const std::string& stored : project_.stored_versions()) {
        out << "Other disc version: " << stored << "\n";
    }
    out << "Install folder: " << utf8_of(project_.root) << "\n";
    out << "Game built: " << (project_.built() ? "yes" : "no") << "\n";
    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetGamepadNameForID(pads[i]);
        out << "Gamepad " << i + 1 << ": " << (name ? name : "?") << "\n";
    }
    SDL_free(pads);
    out << "\n--- settings ---\n" << tail_of(project_.game_folder() / "settings.ini", 400);
    out << "\n--- last install (end) ---\n" << tail_of(project_.root / "build" / "install.log", 200);
    out << "\n--- last game session (end) ---\n" << tail_of(project_.root / "logs" / "game.log", 400);
    out.close();
    launcher_message_failed_ = !out;
    launcher_message_ = format(t.report_done, utf8_of(path.filename()));
    SDL_OpenURL(folder_url(folder).c_str());
    SDL_OpenURL("https://github.com/arelkair/wiiparty-recomp/issues/new");
}

void App::textures_page(float width) {
    const ui::Palette& p = ui::palette();
    const Texts& t = texts();
    std::filesystem::path load = project_.game_folder() / "textures" / "load";
    std::filesystem::path off = project_.game_folder() / "textures" / "disabled";
    ImVec2 top = ImGui::GetCursorScreenPos();
    heading(t.page_textures, t.textures_intro, width - px(150));
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(top.x + width - px(140), top.y));
    if (ui::button(t.open_folder, Kind::Secondary, 140.0f)) {
        std::error_code error;
        std::filesystem::create_directories(load, error);
        SDL_OpenURL(folder_url(load).c_str());
        packs_dirty_ = true;
    }
    ImGui::SetCursorScreenPos(after);
    if (packs_dirty_) {
        packs_dirty_ = false;
        packs_.clear();
        std::error_code error;
        for (const auto& [folder, enabled] : {std::pair{load, true}, std::pair{off, false}}) {
            for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
                if (!entry.is_directory(error)) {
                    continue;
                }
                TexturePack pack;
                pack.name = utf8_of(entry.path().filename());
                pack.enabled = enabled;
                for (auto it = std::filesystem::recursive_directory_iterator(entry.path(), error); !error && it != std::filesystem::recursive_directory_iterator();
                     it.increment(error)) {
                    if (it->is_regular_file(error)) {
                        pack.files++;
                        pack.bytes += it->file_size(error);
                    }
                }
                packs_.push_back(pack);
            }
        }
        std::sort(packs_.begin(), packs_.end(), [](const TexturePack& a, const TexturePack& b) { return a.name < b.name; });
    }
    if (value("video.custom_textures") == "0") {
        ui::text(t.textures_off, Font::Regular, ui::size::kDetail, p.secondary, width);
        ui::gap(14);
    }
    Card card(width);
    if (packs_.empty()) {
        row(width, t.textures_empty, "", 0.0f, 0.0f, [] {});
    }
    for (size_t i = 0; i < packs_.size(); i++) {
        if (i > 0) {
            inset_separator(width);
        }
        TexturePack& pack = packs_[i];
        char size[32];
        std::snprintf(size, sizeof(size), "%.1f MB", static_cast<double>(pack.bytes) / (1024.0 * 1024.0));
        std::string detail = format(t.textures_files, std::to_string(pack.files), size);
        ImGui::PushID(static_cast<int>(i));
        row(width, pack.name.c_str(), detail.c_str(), 42.0f, 24.0f, [&] {
            if (ui::toggle("toggle", pack.enabled)) {
                std::error_code error;
                std::filesystem::path from = (pack.enabled ? load : off) / path_from(pack.name);
                std::filesystem::path to = (pack.enabled ? off : load) / path_from(pack.name);
                std::filesystem::create_directories(to.parent_path(), error);
                std::filesystem::rename(from, to, error);
                packs_dirty_ = true;
            }
        });
        ImGui::PopID();
    }
    card.end();
}

void App::choose_folder() {
    std::string start = utf8_of(project_.root.parent_path());
    SDL_ShowOpenFolderDialog(
        [](void* self, const char* const* files, int) {
            if (files && files[0]) {
                App* app = static_cast<App*>(self);
                std::lock_guard<std::mutex> lock(app->picked_mutex_);
                app->picked_folder_ = files[0];
            }
            wake_main_loop();
        },
        this, window_, start.c_str(), false);
}

void App::install(bool adding) {
    const Texts& t = texts();
    if (runner_.running()) {
        return;
    }
    if (!project_.extracted() && disc_.empty()) {
        result_ = t.choose_disc_first;
        return;
    }
    if (adding && add_disc_.empty()) {
        return;
    }
    if (project_.root.native().size() > kLongestRoot) {
        result_ = t.path_too_long;
        return;
    }
    double free = project_.free_gigabytes();
    if (free >= 0.0 && free < project_.needed_gigabytes()) {
        result_ = format(t.space_short, gigabytes(project_.needed_gigabytes()), gigabytes(free));
        return;
    }
    while (background().estimating) {
        SDL_Delay(20);
    }
    log_.clear();
    result_.clear();
    outcome_ = Outcome::None;
    last_step_ = -1;
    std::error_code error;
    std::filesystem::create_directories(project_.root, error);
    std::vector<ToolStatus> tools = toolchain_.inspect();
    for (const ToolStatus& tool : tools) {
        log_.push_back(tool.bundled  ? format(t.tool_bundled, tool.program, tool.version)
                       : tool.usable ? format(t.tool_ready, tool.program, tool.version)
                                     : format(t.tool_to_download, tool.program));
    }
    int jobs = compile_jobs();
    log_.push_back(format(t.compile_jobs, std::to_string(jobs)));
    std::vector<Step> steps;
    if (project_.packaged) {
        Step prepare;
        prepare.title = t.step_prepare;
        std::filesystem::path root = project_.root;
        prepare.action = [root](std::string& message) { return extract_payload(root, message); };
        steps.push_back(prepare);
    }
    for (Step& step : toolchain_.preparation(tools, pref("offline") == "1")) {
        steps.push_back(std::move(step));
    }
    std::string python = toolchain_.python();
    Project project = project_;
    auto program = [&](const char* title, std::string name, std::vector<std::string> arguments, float weight) {
        Step step;
        step.title = title;
        step.program = std::move(name);
        step.arguments = std::move(arguments);
        step.weight = weight;
        steps.push_back(step);
    };
    std::string disc = adding ? add_disc_ : disc_;
    Step verify;
    verify.title = t.step_verify_disc;
    verify.skip = [project, adding] { return !adding && project.extracted(); };
    verify.action = [disc](std::string& message) {
        std::string output = capture("nodtool", {"--no-color", "verify", disc});
        if (output.find("\xE2\x9D\x8C") != std::string::npos) {
            message = texts().disc_damaged;
            return false;
        }
        size_t found = output.find("Redump: ");
        if (found == std::string::npos) {
            message = texts().disc_unknown;
        } else {
            size_t end = output.find_first_of("\r\n", found);
            message = format(texts().disc_verified, output.substr(found + 8, end == std::string::npos ? std::string::npos : end - found - 8));
        }
        return true;
    };
    steps.push_back(verify);
    std::string incoming = std::string("games/") + kGame + "/versions/incoming";
    program(t.step_extract, "nodtool", {"extract", disc, adding ? incoming : std::string("games/") + kGame + "/extracted"}, 2.0f);
    steps.back().skip = [project, adding] { return !adding && project.extracted(); };
    if (adding) {
        std::error_code stale;
        std::filesystem::remove_all(project_.versions_folder() / "incoming", stale);
        Step change;
        change.title = t.step_switch_version;
        change.action = [project](std::string& message) {
            std::filesystem::path incoming_folder = project.versions_folder() / "incoming";
            std::string id = read_disc_id(incoming_folder);
            std::error_code error;
            if (id.size() != 6 || id == project.disc_id()) {
                std::filesystem::remove_all(incoming_folder, error);
                message = texts().version_already;
                return id.size() == 6;
            }
            std::filesystem::path target = project.versions_folder() / id;
            if (std::filesystem::exists(target / "extracted", error)) {
                std::filesystem::remove_all(incoming_folder, error);
            } else {
                std::filesystem::create_directories(target, error);
                std::filesystem::rename(incoming_folder, target / "extracted", error);
            }
            if (!project.switch_version(id)) {
                message = texts().version_switch_failed;
                return false;
            }
            return true;
        };
        steps.push_back(change);
    }
    std::vector<std::string> sdl_arguments = {"tools/fetch_sdl.py"};
    std::filesystem::path sdl_archive = launcher_path().parent_path() / "tools" / "sdl3" / "SDL3-devel-3.4.16-mingw.tar.gz";
    std::error_code absent;
    if (std::filesystem::exists(sdl_archive, absent)) {
        sdl_arguments.push_back("--archive");
        sdl_arguments.push_back(utf8_of(sdl_archive));
    }
    if (pref("offline") == "1") {
        sdl_arguments.push_back("--offline");
    }
    program(t.step_sdl, python, sdl_arguments, 1.0f);
    program(t.step_unpack, python, {"recompiler/unpack_rels.py"}, 1.0f);
    program(t.step_dol, python, {"recompiler/recomp.py"}, 1.0f);
    program(t.step_modules, python, {"recompiler/recomp_rel.py", "--all"}, 3.0f);
    program(t.step_links, python, {"recompiler/recomp.py"}, 1.0f);
    program(t.step_module_calls, python, {"recompiler/recomp_rel.py", "--all"}, 3.0f);
    program(t.step_dsp, python, {"recompiler/dsp/recomp_dsp.py"}, 1.0f);
    auto translated = std::make_shared<int>(-1);
    for (size_t i = steps.size() - 6; i < steps.size(); i++) {
        steps[i].skip = [project, translated] {
            if (*translated < 0) {
                *translated = project.translation_current() ? 1 : 0;
            }
            return *translated == 1;
        };
    }
    std::vector<std::string> configure = {"-S", ".", "-B", "build/out", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", std::string("-DWP_GAME=") + kGame};
#ifndef _WIN32
    configure.push_back("-DWP_REQUIRE_SDL3=ON");
    configure.push_back(pref("offline") == "1" ? "-DWP_FETCH_SDL3=OFF" : "-DWP_FETCH_SDL3=ON");
#endif
    program(t.step_configure, "cmake", configure, 1.0f);
    program(t.step_compile, "cmake", {"--build", "build/out", "--parallel", std::to_string(jobs), "--target", kGame}, 24.0f);
    progress_view_ = true;
    show_details_ = false;
    ui::set_value(ImHashStr("install-progress"), 0.0f);
    runner_.start(std::move(steps), project_.root, [] { wake_main_loop(); });
    was_running_ = true;
}

void App::finish_install() {
    const Texts& t = texts();
    RunnerView view = runner_.view();
    outcome_ = view.outcome;
    runner_.take_output(log_);
    if (outcome_ == Outcome::Success) {
        std::error_code error;
        for (const char* test : {"lifted_tests", "runtime_tests", "translator_tests"}) {
            std::filesystem::path built = project_.root / "build" / "out" / test;
            std::filesystem::remove(built, error);
            std::filesystem::remove(built.concat(".exe"), error);
        }
        progress_view_ = false;
        result_ = t.build_finished;
        background().minutes = 0;
        disc_line_.clear();
        project_.store_translation_stamp();
        if (project_.packaged) {
            std::string message;
            swap_pending_ = !project_.install_game(message);
            if (swap_pending_) {
                result_ = t.swap_pending;
            }
        }
    } else {
        show_details_ = outcome_ == Outcome::Failure;
        result_ = outcome_ == Outcome::Cancelled ? t.build_cancelled : t.build_failed;
    }
    if (install_mode_) {
        std::error_code error;
        std::filesystem::create_directories(project_.root / "build", error);
        std::ofstream file(project_.root / "build" / "install.log", std::ios::binary | std::ios::trunc);
        for (const std::string& line : log_) {
            file << line << '\n';
        }
        quit_ = true;
        exit_code_ = outcome_ == Outcome::Success ? 0 : 1;
    }
}

void App::play() {
    bool building = runner_.running();
    std::error_code logs;
    std::filesystem::create_directories(project_.root / "logs", logs);
    SDL_setenv_unsafe("WP_LOG_FILE", utf8_of(std::filesystem::absolute(project_.root / "logs" / "game.log", logs)).c_str(), 1);
    if (swap_pending_ && !building) {
        std::string error;
        swap_pending_ = !project_.install_game(error);
    }
    if (!start_detached(utf8_of(project_.executable()), project_.root)) {
        result_ = texts().start_failed;
        outcome_ = Outcome::Failure;
        return;
    }
    if (!building) {
        quit_ = true;
    }
}
