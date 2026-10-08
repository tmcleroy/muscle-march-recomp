#include "wp/options.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <utility>

#include "wp/settings.h"
#include "wp/ui_text.h"

namespace wp::options {

namespace {

constexpr const char* kLiveKeys[] = {
    "video.scale",         "video.fullscreen",    "video.copy_filter",   "input.gamepads", "input.auto_grip",
    "input.wake_on_mouse", "input.hide_cursor",   "system.skip_notices", "system.options_menu", "audio.mute", "saves.backups", "system.interface_language",
};

struct Language {
    const char* code;
    const char* name;
};

constexpr Language kLanguages[] = {{"auto", nullptr}, {"en", "English"},  {"de", "Deutsch"},   {"fr", "Français"},
                                   {"es", "Español"}, {"it", "Italiano"}, {"nl", "Nederlands"}};

constexpr int kMaxScale = 6;

constexpr const char* kBackupCounts[] = {"0", "1", "3", "5", "10", "20"};

bool known(const std::string& key) {
    std::vector<std::string> all = settings::keys();
    return std::find(all.begin(), all.end(), key) != all.end();
}

bool enabled_text(const std::string& value) {
    return !(value == "0" || value == "false" || value == "off" || value == "no");
}

std::mutex g_overlay_mutex;
Image g_overlay;
bool g_overlay_visible = false;
uint64_t g_overlay_version = 0;

}

bool live(const std::string& key) {
    for (const char* entry : kLiveKeys) {
        if (key == entry) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> choices(const std::string& key) {
    if (key == "video.scale") {
        std::vector<std::string> scales;
        for (int scale = 1; scale <= kMaxScale; scale++) {
            scales.push_back(std::to_string(scale));
        }
        return scales;
    }
    if (key == "system.interface_language") {
        return {"en", "es"};
    }
    if (key == "system.language") {
        std::vector<std::string> codes;
        for (const Language& language : kLanguages) {
            codes.push_back(language.code);
        }
        return codes;
    }
    if (key == "saves.backups") {
        return std::vector<std::string>(std::begin(kBackupCounts), std::end(kBackupCounts));
    }
    if (known(key)) {
        return {"0", "1"};
    }
    return {};
}

std::string shown_value(const std::string& key, const std::string& value) {
    const ui::Text& text = ui::text();
    if (key == "video.scale") {
        return value == "1" ? text.menu_native : value + "x";
    }
    if (key == "system.interface_language") {
        return value == "es" ? "Español" : "English";
    }
    if (key == "system.language") {
        for (const Language& language : kLanguages) {
            if (value == language.code) {
                return language.name ? language.name : text.menu_system;
            }
        }
        return value;
    }
    if (key == "saves.backups") {
        return value == "0" ? text.menu_off : value;
    }
    return enabled_text(value) ? text.menu_on : text.menu_off;
}

std::string step_value(const std::string& key, const std::string& value, int step) {
    std::vector<std::string> list = choices(key);
    if (list.empty()) {
        return value;
    }
    auto found = std::find(list.begin(), list.end(), value);
    int index = 0;
    if (found != list.end()) {
        index = static_cast<int>(found - list.begin());
    } else if (list.size() == 2 && list[0] == "0" && list[1] == "1") {
        index = enabled_text(value) ? 1 : 0;
    } else if (key == "saves.backups") {
        int current = std::atoi(value.c_str());
        index = 0;
        while (index + 1 < static_cast<int>(list.size()) && std::atoi(list[static_cast<size_t>(index) + 1].c_str()) <= current) {
            index++;
        }
    } else {
        return list.front();
    }
    int count = static_cast<int>(list.size());
    index = ((index + step) % count + count) % count;
    return list[static_cast<size_t>(index)];
}

void Menu::set_open(bool open) {
    open_ = open;
}

void Menu::select(size_t index) {
    size_t count = settings::keys().size();
    if (index < count) {
        selection_ = index;
    }
}

std::vector<Row> Menu::rows() const {
    std::vector<Row> result;
    for (const std::string& key : settings::keys()) {
        Row row;
        row.key = key;
        row.label = ui::label(key.c_str());
        row.value = shown_value(key, settings::text(key.c_str(), nullptr));
        row.live = live(key);
        result.push_back(row);
    }
    return result;
}

std::string Menu::handle(Action action) {
    std::vector<std::string> all = settings::keys();
    if (all.empty()) {
        return "";
    }
    size_t count = all.size();
    selection_ = std::min(selection_, count - 1);
    switch (action) {
    case Action::Up:
        selection_ = (selection_ + count - 1) % count;
        return "";
    case Action::Down:
        selection_ = (selection_ + 1) % count;
        return "";
    case Action::Close:
        open_ = false;
        return "";
    case Action::Previous:
    case Action::Next: {
        const std::string& key = all[selection_];
        std::string current = settings::text(key.c_str(), nullptr);
        std::string next = step_value(key, current, action == Action::Next ? 1 : -1);
        if (next == current) {
            return "";
        }
        settings::store(key.c_str(), next);
        return key;
    }
    }
    return "";
}

uint32_t Repeat::update(uint32_t held, int64_t now) {
    uint32_t pressed = held & ~previous_;
    uint32_t fired = pressed;
    uint32_t repeating = held & previous_ & repeating_;
    if (pressed & repeating_) {
        next_ = now + delay_;
    } else if (repeating && now >= next_) {
        fired |= repeating;
        next_ = now + interval_;
    }
    previous_ = held;
    return fired;
}

Layout layout(int client_height, size_t rows) {
    Layout result;
    result.unit = std::clamp(client_height / 40, 14, 40);
    int unit = result.unit;
    result.padding = unit + unit / 2;
    result.title_height = unit * 3;
    result.row_height = unit * 2;
    result.footer_height = unit * 2 + unit / 2;
    result.rows = rows;
    result.width = unit * 40;
    result.height = result.padding * 2 + result.title_height + static_cast<int>(rows) * result.row_height + result.footer_height;
    return result;
}

Rect place(int client_width, int client_height, int width, int height) {
    Rect result;
    if (client_width <= 0 || client_height <= 0 || width <= 0 || height <= 0) {
        return result;
    }
    double scale = std::min({1.0, static_cast<double>(client_width) / width, static_cast<double>(client_height) / height});
    result.width = std::max(1, static_cast<int>(width * scale));
    result.height = std::max(1, static_cast<int>(height * scale));
    result.x = (client_width - result.width) / 2;
    result.y = (client_height - result.height) / 2;
    return result;
}

int row_at(const Layout& layout, const Rect& placed, int x, int y) {
    if (placed.width <= 0 || placed.height <= 0 || layout.row_height <= 0) {
        return -1;
    }
    int64_t bx = static_cast<int64_t>(x - placed.x) * layout.width / placed.width;
    int64_t by = static_cast<int64_t>(y - placed.y) * layout.height / placed.height;
    int64_t top = layout.padding + layout.title_height;
    if (x < placed.x || y < placed.y || bx < layout.padding || bx >= layout.width - layout.padding || by < top) {
        return -1;
    }
    int64_t index = (by - top) / layout.row_height;
    return index < static_cast<int64_t>(layout.rows) ? static_cast<int>(index) : -1;
}

void show_overlay(Image image) {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    g_overlay = std::move(image);
    g_overlay_visible = true;
    g_overlay_version++;
}

void hide_overlay() {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    if (!g_overlay_visible) {
        return;
    }
    g_overlay_visible = false;
    g_overlay_version++;
}

bool take_overlay(uint64_t& version, Image& image, bool& visible) {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    if (version == g_overlay_version) {
        return false;
    }
    version = g_overlay_version;
    visible = g_overlay_visible;
    image = std::move(g_overlay);
    g_overlay = Image{};
    return true;
}

}
