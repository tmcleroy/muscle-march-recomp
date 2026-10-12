// First start on Android: there is no separate launcher there, so the game app itself asks for
// the player's game file and Wii common key and builds the game's files in the app's folder, as
// the launcher does on a computer (install.cpp). Runs before the game, on SDL's thread, with its
// own window; the game opens its own afterwards.
//
// Android's file picker hands out content:// addresses rather than paths. SDL can read those, so
// a picked file is copied into the app's folder first and the install reads that copy (it is
// removed when the screen closes). The addresses often carry no file name, so none is shown. The key is used once and not kept.

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "install.h"
#include "ui.h"
#include "wad.h"
#include "wp/android.h"

namespace {

constexpr const char* kWadCopy = "import.wad.tmp";
constexpr size_t kLargestKeyFile = 1u << 16;

enum class Pick { None, Wad, Key };

struct Setup {
    std::mutex lock;  // everything below, between the screen, the file picker and the worker
    Pick picking = Pick::None;
    std::string picked;         // an address the picker returned, waiting for the screen
    Pick picked_for = Pick::None;

    bool working = false;       // the worker is busy (one job at a time)
    std::string activity;       // what it is doing, for the player

    bool wad_ready = false;
    std::string wad_sha1;
    std::string wad_message;
    bool wad_warning = false;

    std::vector<uint8_t> supplied;  // the key as given, kept until a game file is there to try it on
    bool key_ready = false;
    CommonKey key{};
    std::string key_message;

    std::string error;
    bool installed = false;
};

Setup g_setup;
InstallProgress g_progress;
std::thread g_worker;

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Reads a picked file (a path or a content:// address) into memory, up to `limit` bytes.
bool read_picked(const std::string& address, size_t limit, std::vector<uint8_t>& data) {
    SDL_IOStream* in = SDL_IOFromFile(address.c_str(), "rb");
    if (!in) {
        return false;
    }
    uint8_t buffer[4096];
    bool ok = true;
    while (true) {
        size_t count = SDL_ReadIO(in, buffer, sizeof buffer);
        if (count == 0) {
            ok = SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF;
            break;
        }
        data.insert(data.end(), buffer, buffer + count);
        if (data.size() > limit) {
            break;
        }
    }
    SDL_CloseIO(in);
    return ok;
}

bool copy_picked(const std::string& address, const char* target) {
    SDL_IOStream* in = SDL_IOFromFile(address.c_str(), "rb");
    std::FILE* out = std::fopen(target, "wb");
    bool ok = in && out;
    std::vector<char> buffer(1 << 20);
    while (ok) {
        size_t count = SDL_ReadIO(in, buffer.data(), buffer.size());
        if (count == 0) {
            ok = SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF;
            break;
        }
        ok = std::fwrite(buffer.data(), 1, count, out) == count;
    }
    if (in) {
        SDL_CloseIO(in);
    }
    if (out) {
        ok = std::fclose(out) == 0 && ok;
    }
    return ok;
}

// Tries the key the player gave on the game file. Called on the worker with both present.
void try_key() {
    std::vector<uint8_t> supplied;
    {
        std::lock_guard<std::mutex> hold(g_setup.lock);
        supplied = g_setup.supplied;
        g_setup.activity = "Checking the key";
    }
    KeySearch search = find_common_key(kWadCopy, supplied);
    std::lock_guard<std::mutex> hold(g_setup.lock);
    g_setup.key_ready = search.found;
    g_setup.key = search.key;
    g_setup.key_message = search.found ? "This key opens the game file." : search.message;
}

void start(std::function<void()> job, const char* activity) {
    {
        std::lock_guard<std::mutex> hold(g_setup.lock);
        if (g_setup.working) {
            return;
        }
        g_setup.working = true;
        g_setup.activity = activity;
        g_setup.error.clear();
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_worker = std::thread([job = std::move(job)] {
        job();
        std::lock_guard<std::mutex> hold(g_setup.lock);
        g_setup.working = false;
    });
}

void take_wad(const std::string& address) {
    start([address] {
        std::error_code ignored;
        bool copied = copy_picked(address, kWadCopy);
        WadCheck check;
        if (copied) {
            check = check_wad(kWadCopy);
        } else {
            check.message = "The file can't be read.";
        }
        bool usable = check.result == WadCheck::Result::Ok || check.result == WadCheck::Result::OtherRevision;
        if (!usable) {
            std::filesystem::remove(kWadCopy, ignored);
        }
        bool have_key;
        {
            std::lock_guard<std::mutex> hold(g_setup.lock);
            g_setup.wad_ready = usable;
            g_setup.wad_sha1 = check.sha1;
            g_setup.wad_message = check.message;
            g_setup.wad_warning = check.result != WadCheck::Result::Ok;
            g_setup.key_ready = false;
            have_key = !g_setup.supplied.empty();
            if (have_key) {
                g_setup.key_message.clear();
            }
        }
        if (usable && have_key) {
            try_key();
        }
    }, "Reading the game file");
}

// The key as text or as a file's bytes; tried at once when the game file is there.
void take_key(std::vector<uint8_t> supplied) {
    bool have_wad;
    {
        std::lock_guard<std::mutex> hold(g_setup.lock);
        g_setup.supplied = std::move(supplied);
        g_setup.key_ready = false;
        have_wad = g_setup.wad_ready;
        g_setup.key_message = have_wad ? "" : "Taken. It is checked once the game file is chosen.";
    }
    if (have_wad) {
        start(try_key, "Checking the key");
    }
}

void take_key_file(const std::string& address) {
    start([address] {
        std::vector<uint8_t> data;
        if (!read_picked(address, kLargestKeyFile, data) || data.empty()) {
            std::lock_guard<std::mutex> hold(g_setup.lock);
            g_setup.key_message = "The key file can't be read.";
            return;
        }
        bool have_wad;
        {
            std::lock_guard<std::mutex> hold(g_setup.lock);
            g_setup.supplied = data;
            g_setup.key_ready = false;
            have_wad = g_setup.wad_ready;
            g_setup.key_message = have_wad ? "" : "Taken. It is checked once the game file is chosen.";
        }
        if (have_wad) {
            try_key();
        }
    }, "Reading the key file");
}

void install(const std::string& folder) {
    g_progress.fraction = 0.0f;
    start([folder] {
        std::string sha1;
        CommonKey key;
        {
            std::lock_guard<std::mutex> hold(g_setup.lock);
            sha1 = g_setup.wad_sha1;
            key = g_setup.key;
        }
        std::string error;
        bool ok = install_game(kWadCopy, sha1, key, folder, g_progress, error);
        std::lock_guard<std::mutex> hold(g_setup.lock);
        g_setup.installed = ok;
        g_setup.error = ok ? "" : (error.empty() ? "The game files could not be built." : error);
    }, "Building the game files");
}

// SDL calls this from wherever the system's picker reports back.
void SDLCALL picked(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> hold(g_setup.lock);
    if (files && files[0]) {
        g_setup.picked = files[0];
        g_setup.picked_for = g_setup.picking;
    }
    g_setup.picking = Pick::None;
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
}

void pick(SDL_Window* window, Pick what) {
    {
        std::lock_guard<std::mutex> hold(g_setup.lock);
        if (g_setup.picking != Pick::None) {
            return;
        }
        g_setup.picking = what;
    }
    // No filters: Android filters by content type, and it has none for .wad or key files.
    SDL_ShowOpenFileDialog(picked, nullptr, window, nullptr, 0, nullptr, false);
}

void status_line(const std::string& message, bool good, bool warning) {
    if (message.empty()) {
        return;
    }
    const ui::Palette& colors = ui::palette();
    ImU32 color = good ? (warning ? colors.secondary : colors.success) : colors.danger;
    ui::text(message.c_str(), ui::Font::Regular, ui::size::kDetail, color, ImGui::GetContentRegionAvail().x);
}

// One frame of the screen.
void draw(SDL_Window* window, const std::string& folder) {
    static char typed[80] = "";
    bool working, wad_ready, wad_warning, key_ready, have_supplied;
    std::string activity, wad_message, key_message, error;
    {
        std::lock_guard<std::mutex> hold(g_setup.lock);
        working = g_setup.working;
        activity = g_setup.activity;
        wad_ready = g_setup.wad_ready;
        wad_warning = g_setup.wad_warning;
        wad_message = g_setup.wad_message;
        key_ready = g_setup.key_ready;
        key_message = g_setup.key_message;
        have_supplied = !g_setup.supplied.empty();
        error = g_setup.error;
    }
    const ui::Palette& colors = ui::palette();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(28), ui::px(18)));
    ImGui::Begin("setup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    float full = ImGui::GetContentRegionAvail().x;
    float left = full * 0.36f;
    float gutter = ui::px(28);
    float right = full - left - gutter;

    // Left: what this is.
    ImGui::BeginChild("about", ImVec2(left, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ui::text("Muscle March", ui::Font::Semibold, ui::size::kHeading, colors.text);
    ui::gap(8);
    ui::text("To play, this app needs your own copy of the game and your own Wii's common key. Neither comes with the app.", ui::Font::Regular,
             ui::size::kBody, colors.secondary, left);
    ui::gap(8);
    ui::text("Both are only read, on this phone, to build the game's files in the app's folder. The key is not kept.", ui::Font::Regular,
             ui::size::kDetail, colors.tertiary, left);
    ImGui::EndChild();
    ImGui::SameLine(0, gutter);

    // Right: the two things to give, then Install.
    ImGui::BeginChild("steps", ImVec2(right, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    float button_height = 44.0f;
    ui::text("1   Game file", ui::Font::Semibold, ui::size::kLead, colors.text);
    ui::text("Muscle March (USA) (WiiWare).wad", ui::Font::Regular, ui::size::kDetail, colors.tertiary);
    ui::gap(6);
    if (ui::button(wad_ready ? "Choose another file" : "Choose the WAD file", wad_ready ? ui::Kind::Secondary : ui::Kind::Primary, 0.0f, !working,
                   button_height)) {
        pick(window, Pick::Wad);
    }
    ui::gap(4);
    status_line(wad_message, wad_ready, wad_warning);

    ui::gap(12);
    ui::text("2   Wii common key", ui::Font::Semibold, ui::size::kLead, colors.text);
    ui::text("32 hexadecimal digits, or a key file such as keys.bin", ui::Font::Regular, ui::size::kDetail, colors.tertiary);
    ui::gap(6);
    if (ui::button("Paste", key_ready ? ui::Kind::Secondary : ui::Kind::Primary, 0.0f, !working, button_height)) {
        char* text = SDL_GetClipboardText();
        std::string pasted = text ? text : "";
        SDL_free(text);
        if (pasted.empty()) {
            std::lock_guard<std::mutex> hold(g_setup.lock);
            g_setup.key_message = "There is no text on the clipboard.";
        } else {
            typed[0] = 0;
            take_key(std::vector<uint8_t>(pasted.begin(), pasted.end()));
        }
    }
    ImGui::SameLine(0, ui::px(10));
    if (ui::button("Choose a key file", ui::Kind::Secondary, 0.0f, !working, button_height)) {
        pick(window, Pick::Key);
    }
    ImGui::SameLine(0, ui::px(10));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui::px(4));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(12), ui::px(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui::px(8));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.control);
    ImGui::PushStyleColor(ImGuiCol_Text, colors.text);
    ui::push_font(ui::Font::Regular, ui::size::kBody);
    ImGui::SetNextItemWidth(std::max(ui::px(120), ImGui::GetContentRegionAvail().x));
    // Typed digits are shown as dots; the key never appears on screen.
    if (ImGui::InputTextWithHint("##key", "or type it", typed, sizeof typed, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsNoBlank)) {
        size_t digits = 0;
        for (const char* c = typed; *c; c++) {
            digits += hex_value(*c) >= 0 ? 1 : 0;
        }
        if (digits == 32) {
            std::string text = typed;
            take_key(std::vector<uint8_t>(text.begin(), text.end()));
        }
    }
    ui::pop_font();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ui::gap(4);
    status_line(key_message, key_ready || (have_supplied && !wad_ready), !key_ready);

    ui::gap(14);
    if (working) {
        ui::text((activity + "...").c_str(), ui::Font::Regular, ui::size::kBody, colors.secondary);
        ui::gap(6);
        ui::progress_bar(ImGui::GetID("progress"), g_progress.fraction.load(), std::min(right, ui::px(420)), ui::px(6));
        ui::request_frames(2);
    } else {
        if (ui::button("Install and play", ui::Kind::Primary, 220.0f, wad_ready && key_ready, button_height)) {
            install(folder);
        }
        if (!error.empty()) {
            ui::gap(6);
            status_line(error, false, false);
        }
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

// The screen, until the game's files are built (true) or the player leaves (false).
bool run_setup(const std::string& folder) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "setup: %s\n", SDL_GetError());
        return false;
    }
    SDL_Window* window = SDL_CreateWindow("Muscle March", 1280, 720, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "setup: %s\n", SDL_GetError());
        if (window) {
            SDL_DestroyWindow(window);
        }
        return false;
    }
    SDL_SetRenderVSync(renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ui::load_fonts();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    ui::set_dark(SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT);
    // A phone's landscape screen is short: size the page so it fits the height.
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    float scale = std::max(1.0f, std::min(SDL_GetWindowDisplayScale(window), static_cast<float>(height) / 400.0f));
    ui::apply_style(scale);

    bool running = true;
    bool done = false;
    Uint64 previous = SDL_GetTicksNS();
    while (running && !done) {
        bool busy;
        {
            std::lock_guard<std::mutex> hold(g_setup.lock);
            busy = g_setup.working;
        }
        SDL_Event event;
        bool idle = !ui::animating() && !busy;
        if (idle && SDL_WaitEventTimeout(&event, 250)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            running = event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_TERMINATING;
            ui::request_frames(3);
        }
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_TERMINATING) {
                running = false;
            }
            ui::request_frames(3);
        }
        std::string address;
        Pick purpose = Pick::None;
        {
            std::lock_guard<std::mutex> hold(g_setup.lock);
            address.swap(g_setup.picked);
            purpose = g_setup.picked_for;
            g_setup.picked_for = Pick::None;
            done = g_setup.installed && !g_setup.working;
        }
        if (!address.empty() && purpose == Pick::Wad) {
            take_wad(address);
        } else if (!address.empty() && purpose == Pick::Key) {
            take_key_file(address);
        }
        Uint64 now = SDL_GetTicksNS();
        ui::begin_frame(static_cast<float>(now - previous) / 1e9f);
        previous = now;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw(window, folder);
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImVec4 clear = ImGui::ColorConvertU32ToFloat4(ui::palette().background);
        SDL_SetRenderDrawColorFloat(renderer, clear.x, clear.y, clear.z, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    {
        std::lock_guard<std::mutex> hold(g_setup.lock);
        done = g_setup.installed;
        g_setup.supplied.clear();
        g_setup.key = {};
    }
    std::error_code ignored;
    std::filesystem::remove(kWadCopy, ignored);
    SDL_StopTextInput(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    return done;
}

// Linked into the game app directly, so this runs before main() asks for the game files.
const bool g_registered = (wp::android::set_setup(run_setup), true);

}  // namespace
