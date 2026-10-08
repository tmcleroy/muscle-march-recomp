#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstring>
#include <string>

#include "app.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "payload.h"
#include "prefs.h"
#include "subprocess.h"
#include "ui.h"

namespace {

Project locate_project() {
    Project project;
    if (find_checkout(project.root)) {
        return project;
    }
    project.packaged = payload_available();
    std::string root = pref("root");
    project.root = root.empty() ? default_install_folder() : path_from(root);
    return project;
}

void render(SDL_Renderer* renderer, const ui::Palette& palette) {
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    ImVec4 clear = ImGui::ColorConvertU32ToFloat4(palette.background);
    SDL_SetRenderDrawColorFloat(renderer, clear.x, clear.y, clear.z, 1.0f);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

bool save_picture(SDL_Renderer* renderer, const std::string& path) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) {
        return false;
    }
    bool saved = SDL_SavePNG(surface, path.c_str());
    SDL_DestroySurface(surface);
    return saved;
}

}

int main(int argc, char** argv) {
    bool install_mode = false;
    std::string screenshot_prefix;
    std::string added_disc;
    std::string used_version;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--install") == 0) {
            install_mode = true;
        } else if (std::strcmp(argv[i], "--use-version") == 0 && i + 1 < argc) {
            used_version = argv[++i];
        } else if (std::strcmp(argv[i], "--add-disc") == 0 && i + 1 < argc) {
            added_disc = argv[++i];
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_prefix = argv[++i];
        }
    }
    SDL_SetAppMetadata("Wii Party Recomp", WP_LAUNCHER_VERSION, "io.github.arelkair.wiipartyrecomp");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wii Party Recomp", SDL_GetError(), nullptr);
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    SDL_Window* window = SDL_CreateWindow("Wii Party Recomp", static_cast<int>(1040 * scale), static_cast<int>(680 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wii Party Recomp", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(820 * scale), static_cast<int>(560 * scale));
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ui::load_fonts();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    App app(window, locate_project(), install_mode || !added_disc.empty());
    if (!added_disc.empty()) {
        app.add_on_start(added_disc);
    }
    app.apply_theme();
    SDL_ShowWindow(window);

    if (!used_version.empty()) {
        return app.use_version_now(used_version) ? 0 : 1;
    }
    Uint64 previous = SDL_GetTicksNS();
    int shot_page = 0;
    int shot_frames = 0;
    bool running = true;
    while (running) {
        bool idle = screenshot_prefix.empty() && !ui::animating() && !app.busy();
        SDL_Event event;
        if (idle && SDL_WaitEventTimeout(&event, 500)) {
            if (!app.consume(event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            running = event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            ui::request_frames(3);
        }
        while (SDL_PollEvent(&event)) {
            if (!app.consume(event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
            ui::request_frames(3);
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(50);
            continue;
        }
        Uint64 now = SDL_GetTicksNS();
        ui::begin_frame(static_cast<float>(now - previous) / 1e9f);
        previous = now;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (!screenshot_prefix.empty() && shot_frames == 0) {
            app.set_page(static_cast<Page>(shot_page));
        }
        app.frame();
        render(renderer, ui::palette());
        if (!screenshot_prefix.empty() && ++shot_frames == 45) {
            save_picture(renderer, screenshot_prefix + std::to_string(shot_page) + ".png");
            shot_frames = 0;
            if (++shot_page == static_cast<int>(Page::Count)) {
                running = false;
            }
        }
        SDL_RenderPresent(renderer);
        if (app.quit()) {
            running = false;
        }
    }
    int code = app.exit_code();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}
