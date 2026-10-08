#include "wp/video.h"

#include "gl_window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "wp/dsp.h"
#include "wp/game.h"
#include "wp/gx.h"
#include "wp/gx_render.h"
#include "wp/hud.h"
#include "wp/input.h"
#include "wp/keymap.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/nand.h"
#include "wp/options_window.h"
#include "wp/screenshot.h"
#include "wp/settings.h"
#include "wp/ui_text.h"

namespace wp::video {

namespace {

constexpr uint32_t kDisplayConfig = 0xCC002002;
constexpr uint32_t kGameCodeAddress = 0x80000000;
constexpr uint32_t kGameCodeLength = 6;
constexpr int kWindowWidth = 1280;
constexpr double kStandardAspect = 4.0 / 3.0;
constexpr double kWideAspect = 16.0 / 9.0;
constexpr int kMenuPeriodMs = 16;
constexpr int kLeftButton = 0x01;
constexpr int kRightButton = 0x02;
constexpr int kMiddleButton = 0x04;
constexpr int kExtraButton1 = 0x05;
constexpr int kExtraButton2 = 0x06;
constexpr int kF10 = 0x79;
constexpr int kF11 = 0x7A;
constexpr int kF12 = 0x7B;

struct KeyPair {
    int vk;
    SDL_Scancode scancode;
};

constexpr KeyPair kKeys[] = {
    {0x08, SDL_SCANCODE_BACKSPACE}, {0x09, SDL_SCANCODE_TAB},        {0x0D, SDL_SCANCODE_RETURN},       {0x13, SDL_SCANCODE_PAUSE},
    {0x14, SDL_SCANCODE_CAPSLOCK},  {0x1B, SDL_SCANCODE_ESCAPE},     {0x20, SDL_SCANCODE_SPACE},        {0x21, SDL_SCANCODE_PAGEUP},
    {0x22, SDL_SCANCODE_PAGEDOWN},  {0x23, SDL_SCANCODE_END},        {0x24, SDL_SCANCODE_HOME},         {0x25, SDL_SCANCODE_LEFT},
    {0x26, SDL_SCANCODE_UP},        {0x27, SDL_SCANCODE_RIGHT},      {0x28, SDL_SCANCODE_DOWN},         {0x2D, SDL_SCANCODE_INSERT},
    {0x2E, SDL_SCANCODE_DELETE},    {0x60, SDL_SCANCODE_KP_0},       {0x61, SDL_SCANCODE_KP_1},         {0x62, SDL_SCANCODE_KP_2},
    {0x63, SDL_SCANCODE_KP_3},      {0x64, SDL_SCANCODE_KP_4},       {0x65, SDL_SCANCODE_KP_5},         {0x66, SDL_SCANCODE_KP_6},
    {0x67, SDL_SCANCODE_KP_7},      {0x68, SDL_SCANCODE_KP_8},       {0x69, SDL_SCANCODE_KP_9},         {0x6A, SDL_SCANCODE_KP_MULTIPLY},
    {0x6B, SDL_SCANCODE_KP_PLUS},   {0x6D, SDL_SCANCODE_KP_MINUS},   {0x6E, SDL_SCANCODE_KP_PERIOD},    {0x6F, SDL_SCANCODE_KP_DIVIDE},
    {0xA0, SDL_SCANCODE_LSHIFT},    {0xA1, SDL_SCANCODE_RSHIFT},     {0xA2, SDL_SCANCODE_LCTRL},        {0xA3, SDL_SCANCODE_RCTRL},
    {0xA4, SDL_SCANCODE_LALT},      {0xA5, SDL_SCANCODE_RALT},       {0xBA, SDL_SCANCODE_SEMICOLON},    {0xBB, SDL_SCANCODE_EQUALS},
    {0xBC, SDL_SCANCODE_COMMA},     {0xBD, SDL_SCANCODE_MINUS},      {0xBE, SDL_SCANCODE_PERIOD},       {0xBF, SDL_SCANCODE_SLASH},
    {0xC0, SDL_SCANCODE_GRAVE},     {0xDB, SDL_SCANCODE_LEFTBRACKET}, {0xDC, SDL_SCANCODE_BACKSLASH},   {0xDD, SDL_SCANCODE_RIGHTBRACKET},
    {0xDE, SDL_SCANCODE_APOSTROPHE},
};

SDL_Scancode scancode_for(int vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (vk - 'A'));
    }
    if (vk >= '1' && vk <= '9') {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (vk - '1'));
    }
    if (vk == '0') {
        return SDL_SCANCODE_0;
    }
    if (vk >= 0x70 && vk <= 0x7B) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (vk - 0x70));
    }
    if (vk >= 0x7C && vk <= 0x87) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_F13 + (vk - 0x7C));
    }
    for (const KeyPair& pair : kKeys) {
        if (pair.vk == vk) {
            return pair.scancode;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

int vk_for(SDL_Scancode scancode) {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return 'A' + (scancode - SDL_SCANCODE_A);
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
        return '1' + (scancode - SDL_SCANCODE_1);
    }
    if (scancode == SDL_SCANCODE_0) {
        return '0';
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        return 0x70 + (scancode - SDL_SCANCODE_F1);
    }
    if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F24) {
        return 0x7C + (scancode - SDL_SCANCODE_F13);
    }
    for (const KeyPair& pair : kKeys) {
        if (pair.scancode == scancode) {
            return pair.vk;
        }
    }
    return 0;
}

int vk_for_button(uint8_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return kLeftButton;
    case SDL_BUTTON_RIGHT:
        return kRightButton;
    case SDL_BUTTON_MIDDLE:
        return kMiddleButton;
    case SDL_BUTTON_X1:
        return kExtraButton1;
    case SDL_BUTTON_X2:
        return kExtraButton2;
    default:
        return 0;
    }
}

struct Window {
    std::mutex lock;
    std::condition_variable changed;
    bool finished = false;
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    SDL_GPUDevice* gpu = nullptr;
};

Window g_state;

struct MainRequest {
    std::mutex lock;
    std::condition_variable changed;
    bool requested = false;
    bool visible = true;
};

MainRequest g_main_request;
std::atomic<SDL_Window*> g_window{nullptr};
std::atomic<bool> g_screenshot_requested{false};
std::atomic<bool> g_title_changed{false};
std::atomic<bool> g_present_requested{false};
Uint32 g_present_event = 0;
double g_aspect = kStandardAspect;
std::mutex g_title_mutex;
std::string g_title;
bool g_fullscreen = false;

bool hide_cursor() {
    static const settings::LiveFlag value("input.hide_cursor", "WP_HIDE_CURSOR");
    return value();
}

void apply_interface_language() {
    bool spanish = settings::text("system.interface_language", "WP_INTERFACE_LANGUAGE") == "es";
    ui::set_language(spanish ? ui::Language::Spanish : ui::Language::English);
}

void set_fullscreen(SDL_Window* window, bool enable) {
    SDL_SetWindowFullscreen(window, enable);
    g_fullscreen = enable;
}

void note_press(int code) {
    if (code != 0 && input::bindings().has(keymap::Action::Screenshot, code)) {
        g_screenshot_requested = true;
    }
}

void apply_option(SDL_Window* window, const std::string& key) {
    if (key == "system.interface_language") {
        apply_interface_language();
        options::refresh_menu(window);
    }
    if (key == "video.fullscreen") {
        bool enable = settings::flag("video.fullscreen", nullptr);
        if (enable != g_fullscreen) {
            set_fullscreen(window, enable);
        }
    }
}

std::string game_code() {
    std::string code;
    for (uint32_t i = 0; i < kGameCodeLength; i++) {
        char ch = static_cast<char>(rd8(kGameCodeAddress + i));
        if (ch != 0) {
            code.push_back(ch);
        }
    }
    return code;
}

std::string window_title(double fps) {
    const ui::Text& text = ui::text();
    bool pal = (rd16(kDisplayConfig) & 0x0300) == 0x0100;
    std::string code = game_code();
    bool pal_disc = code.size() >= 4 && code[3] == 'P';
    const char* region = pal ? "PAL" : pal_disc ? "PAL60" : "NTSC";
    dsp::Status status = dsp::status();
    char statistics[128];
    if (status.loaded) {
        std::snprintf(statistics, sizeof statistics, "%s: %.0f | %s: %s (%.0f%%)", text.fps, fps, text.dsp,
                      status.native ? text.dsp_native : text.dsp_interpreted, status.load_percent);
    } else {
        std::snprintf(statistics, sizeof statistics, "%s: %.0f | %s: %s", text.fps, fps, text.dsp, text.dsp_stopped);
    }
    return std::string(game::description().title) + " (" + code + ") | " + gx::render::api_name() + " | " + region + " | " + statistics;
}

void handle_event(SDL_Window* window, const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        std::_Exit(0);
    case SDL_EVENT_KEY_DOWN: {
        int vk = vk_for(event.key.scancode);
        bool repeat = event.key.repeat;
        if (vk == kF12 && !repeat) {
            gx::request_capture();
            return;
        }
        if (vk == kF11 && !repeat) {
            set_fullscreen(window, !g_fullscreen);
            settings::store("video.fullscreen", g_fullscreen ? "1" : "0");
            options::refresh_menu(window);
            return;
        }
        if (vk == kF10) {
            if (!repeat) {
                options::toggle_menu(window);
            }
            return;
        }
        if (options::menu_open()) {
            apply_option(window, options::menu_key(window, static_cast<unsigned>(vk), repeat));
            return;
        }
        if (!repeat) {
            note_press(vk);
            if (vk == 0xA0 || vk == 0xA1) {
                note_press(0x10);
            } else if (vk == 0xA2 || vk == 0xA3) {
                note_press(0x11);
            } else if (vk == 0xA4 || vk == 0xA5) {
                note_press(0x12);
            }
        }
        return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        int vk = vk_for_button(event.button.button);
        if (options::menu_open()) {
            if (vk == kLeftButton || vk == kRightButton) {
                float density = SDL_GetWindowPixelDensity(window);
                apply_option(window, options::menu_click(window, static_cast<int>(event.button.x * density), static_cast<int>(event.button.y * density),
                                                         vk == kRightButton));
            }
            return;
        }
        note_press(vk);
        return;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        int delta = static_cast<int>(event.wheel.y * 120.0f);
        if (options::menu_open()) {
            options::menu_wheel(window, delta);
            return;
        }
        input::note_wheel(delta);
        if (delta != 0) {
            note_press(delta > 0 ? keymap::kWheelUp : keymap::kWheelDown);
        }
        return;
    }
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        options::refresh_menu(window);
        return;
    default:
        return;
    }
}

void drain_events(SDL_Window* window) {
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_JOYSTICK_AXIS_MOTION - 1) > 0 ||
           SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FINGER_DOWN, SDL_EVENT_LAST) > 0) {
        handle_event(window, event);
    }
}

void finish_start(SDL_Window* window, SDL_GLContext context, SDL_GPUDevice* gpu = nullptr) {
    std::lock_guard<std::mutex> hold(g_state.lock);
    g_state.window = window;
    g_state.context = context;
    g_state.gpu = gpu;
    g_state.finished = true;
    g_state.changed.notify_all();
}

void window_thread(bool visible) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "cannot open a window: %s\n", SDL_GetError());
        finish_start(nullptr, nullptr);
        return;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    int client_width = kWindowWidth;
    int client_height = static_cast<int>(client_width / g_aspect + 0.5);
    SDL_Rect usable{};
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (display && SDL_GetDisplayUsableBounds(display, &usable)) {
        while ((client_width > usable.w || client_height + 48 > usable.h) && client_width > 320) {
            client_width -= 16;
            client_height = static_cast<int>(client_width / g_aspect + 0.5);
        }
    }
#ifdef WP_RENDERER_GPU
    // The device works without a window, so headless runs render (and save frames) too.
    SDL_GPUDevice* gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, std::getenv("WP_GPU_DEBUG") != nullptr, nullptr);
    if (!gpu) {
        std::fprintf(stderr, "cannot create an SDL_GPU device: %s\n", SDL_GetError());
        finish_start(nullptr, nullptr);
        return;
    }
    SDL_Window* window = nullptr;
    if (visible) {
        window = SDL_CreateWindow(window_title(0.0).c_str(), client_width, client_height, SDL_WINDOW_RESIZABLE);
        if (!window || !SDL_ClaimWindowForGPUDevice(gpu, window)) {
            std::fprintf(stderr, "cannot open a window: %s\n", SDL_GetError());
            finish_start(nullptr, nullptr, gpu);
            return;
        }
        // Mailbox presents without waiting for vsync when available; the game paces itself.
        if (SDL_WindowSupportsGPUPresentMode(gpu, window, SDL_GPU_PRESENTMODE_MAILBOX)) {
            SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_MAILBOX);
        }
        g_present_event = SDL_RegisterEvents(1);
    }
    finish_start(window, nullptr, gpu);
#else
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (visible ? 0 : SDL_WINDOW_HIDDEN);
    SDL_Window* window = SDL_CreateWindow(window_title(0.0).c_str(), client_width, client_height, flags);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!context) {
        std::fprintf(stderr, "cannot create an OpenGL 4.1 context: %s\n", SDL_GetError());
        if (window) {
            SDL_DestroyWindow(window);
        }
        finish_start(nullptr, nullptr);
        return;
    }
    SDL_GL_MakeCurrent(window, nullptr);
    finish_start(window, context);
#endif
    if (!visible) {
        return;
    }
    g_window = window;
    if (settings::flag("video.fullscreen", "WP_FULLSCREEN")) {
        set_fullscreen(window, true);
    }
    bool cursor_hidden = false;
    while (true) {
        SDL_WaitEventTimeout(nullptr, kMenuPeriodMs);
        drain_events(window);
#ifdef WP_RENDERER_GPU
        if (g_present_requested.exchange(false)) {
            gx::render::present_to_window(window, g_aspect);
        }
#endif
        if (g_title_changed.exchange(false)) {
            std::string title;
            {
                std::lock_guard<std::mutex> hold(g_title_mutex);
                title = g_title;
            }
            SDL_SetWindowTitle(window, title.c_str());
        }
        apply_option(window, options::poll_gamepads(window));
        bool hide = hide_cursor() && !options::menu_open();
        if (hide != cursor_hidden) {
            cursor_hidden = hide;
            if (hide) {
                SDL_HideCursor();
            } else {
                SDL_ShowCursor();
            }
        }
    }
}

std::tm local_time() {
    std::time_t now = std::time(nullptr);
    std::tm time{};
    localtime_r(&now, &time);
    return time;
}

void save_screenshot() {
    std::vector<uint32_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!gx::render::read_frame(pixels, width, height)) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(screenshot::kFolder, error);
    std::tm time = local_time();
    std::string path;
    FILE* file = nullptr;
    for (int attempt = 1; attempt < 1000 && !file; attempt++) {
        path = std::string(screenshot::kFolder) + "/" + screenshot::file_name(game::description().short_name, time, attempt);
        file = std::fopen(path.c_str(), "wbx");
        if (!file && !std::filesystem::exists(path, error)) {
            break;
        }
    }
    if (!file) {
        std::fprintf(stderr, "screenshot could not be saved to %s\n", path.c_str());
        log::write("video", "screenshot could not be saved to %s", path.c_str());
        return;
    }
    std::thread([file, path, pixels = std::move(pixels), width, height] {
        std::vector<uint8_t> png = encode_png(pixels, width, height);
        bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
        ok = std::fclose(file) == 0 && ok;
        if (!ok) {
            std::remove(path.c_str());
        }
        std::fprintf(stderr, ok ? "screenshot saved to %s\n" : "screenshot could not be written to %s\n", path.c_str());
        log::write("video", ok ? "screenshot saved to %s" : "screenshot could not be written to %s", path.c_str());
    }).detach();
}

std::string g_next_frame_path;

void present_on_gpu(SDL_Window* window, double aspect) {
#ifdef WP_RENDERER_GPU
    gx::render::present_frame(window, aspect);  // also submits the frame's commands when headless
#else
    if (window) {
        gx::render::present_frame(window, aspect);
    }
#endif
    if (g_screenshot_requested.exchange(false)) {
        save_screenshot();
    }
    if (!g_next_frame_path.empty()) {
        std::vector<uint32_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        if (gx::render::read_frame(pixels, width, height)) {
            static const bool with_hud = std::getenv("WP_CAPTURE_HUD") != nullptr;  // tests: captures with the HUD sprites
            if (with_hud) {
                hud::composite(pixels, width, height);
            }
            save_png(g_next_frame_path.c_str(), pixels, width, height);
        }
        g_next_frame_path.clear();
    }
    static int frames = 0;
    const char* save = std::getenv("WP_SAVE_FRAME");
    static const int every = std::getenv("WP_SAVE_EVERY") ? std::atoi(std::getenv("WP_SAVE_EVERY")) : 100;
    if (save && ++frames % every == 0) {
        std::vector<uint32_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        if (gx::render::read_frame(pixels, width, height)) {
            char path[512];
            std::snprintf(path, sizeof path, save, frames / every);
            save_png(path, pixels, width, height);
        }
    }
}

}

bool make_gl_current() {
    std::unique_lock<std::mutex> hold(g_state.lock);
    g_state.changed.wait(hold, [] { return g_state.finished; });
    if (!g_state.context || !SDL_GL_MakeCurrent(g_state.window, g_state.context)) {
        return false;
    }
    SDL_GL_SetSwapInterval(0);
    return true;
}

void* gpu_device() {
    std::unique_lock<std::mutex> hold(g_state.lock);
    g_state.changed.wait(hold, [] { return g_state.finished; });
    return g_state.gpu;
}

void request_present() {
    if (!g_present_requested.exchange(true) && g_present_event != 0) {
        SDL_Event event{};
        event.type = g_present_event;
        SDL_PushEvent(&event);  // wakes the window loop's SDL_WaitEventTimeout
    }
}

void start() {
    log::write("video", "build %s", WP_BUILD);
    g_aspect = nand::widescreen() ? kWideAspect : kStandardAspect;
    apply_interface_language();
    bool visible = std::getenv("WP_HEADLESS") == nullptr;
#ifdef __APPLE__
    std::lock_guard<std::mutex> hold(g_main_request.lock);
    g_main_request.visible = visible;
    g_main_request.requested = true;
    g_main_request.changed.notify_all();
#else
    std::thread(window_thread, visible).detach();
#endif
}

void run_main_thread() {
#ifdef __APPLE__
    // Cocoa only lets the main thread create windows and pump events.
    bool visible;
    {
        std::unique_lock<std::mutex> hold(g_main_request.lock);
        g_main_request.changed.wait(hold, [] { return g_main_request.requested; });
        visible = g_main_request.visible;
    }
    window_thread(visible);
#endif
}

void* window_handle() {
    return g_window.load();
}

bool image_point(long x, long y, long client_width, long client_height, float& out_x, float& out_y) {
    if (client_width <= 0 || client_height <= 0) {
        return false;
    }
    double width = client_width;
    double height = width / g_aspect;
    if (height > client_height) {
        height = client_height;
        width = height * g_aspect;
    }
    double left = (client_width - width) / 2;
    double top = (client_height - height) / 2;
    double u = (x - left) / width;
    double v = (y - top) / height;
    if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) {
        return false;
    }
    out_x = static_cast<float>(u * 2.0 - 1.0);
    out_y = static_cast<float>(v * 2.0 - 1.0);
    return true;
}

bool window_focused() {
    SDL_Window* window = g_window;
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool client_size(int& width, int& height) {
    SDL_Window* window = g_window;
    return window && SDL_GetWindowSizeInPixels(window, &width, &height);
}

bool key_down(int key) {
    if (key == kLeftButton || key == kRightButton || key == kMiddleButton || key == kExtraButton1 || key == kExtraButton2) {
        SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
        uint8_t button = key == kLeftButton ? SDL_BUTTON_LEFT
                         : key == kRightButton ? SDL_BUTTON_RIGHT
                         : key == kMiddleButton ? SDL_BUTTON_MIDDLE
                         : key == kExtraButton1 ? SDL_BUTTON_X1
                                                : SDL_BUTTON_X2;
        return (buttons & SDL_BUTTON_MASK(button)) != 0;
    }
    const bool* state = SDL_GetKeyboardState(nullptr);
    if (!state) {
        return false;
    }
    switch (key) {
    case 0x10:
        return state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT];
    case 0x11:
        return state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL];
    case 0x12:
        return state[SDL_SCANCODE_LALT] || state[SDL_SCANCODE_RALT];
    default: {
        SDL_Scancode scancode = scancode_for(key);
        return scancode != SDL_SCANCODE_UNKNOWN && state[scancode];
    }
    }
}

bool cursor_position(long& x, long& y) {
    if (!g_window.load()) {
        return false;
    }
    float fx = 0.0f;
    float fy = 0.0f;
    SDL_GetMouseState(&fx, &fy);
    x = static_cast<long>(fx);
    y = static_cast<long>(fy);
    return true;
}

bool cursor_on_image(float& x, float& y) {
    SDL_Window* window = g_window;
    if (!window || SDL_GetMouseFocus() != window) {
        return false;
    }
    float fx = 0.0f;
    float fy = 0.0f;
    SDL_GetMouseState(&fx, &fy);
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    return image_point(static_cast<long>(fx), static_cast<long>(fy), width, height, x, y);
}

void update_statistics(double fps) {
    if (!g_window.load()) {
        return;
    }
    std::string title = window_title(fps);
    {
        std::lock_guard<std::mutex> hold(g_title_mutex);
        if (title == g_title) {
            return;
        }
        g_title = title;
    }
    g_title_changed = true;
}

void save_next_frame(const char* path) {
    g_next_frame_path = path;
}

void present() {
    SDL_Window* window = g_window;
#ifdef WP_RENDERER_GPU
    static const bool saving = true;  // the frame task submits the GPU commands
#else
    static const bool saving = std::getenv("WP_SAVE_FRAME") || std::getenv("WP_CAPTURE_AT");
#endif
    if (!window && !saving) {
        gx::process();
        return;
    }
    double aspect = g_aspect;
    gx::run_frame_task([window, aspect] { present_on_gpu(window, aspect); });
}

}
