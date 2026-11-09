// crusader-editor: level editor for Crusader: No Remorse maps.
//
//   crusader-editor [data | game folder] [--mod DIR] [--map N] [--new-map] [--select-shape S]
//                   [--screenshot out.bmp [--frames N]]
//
// --screenshot writes the window after N frames (60 by default) and quits.
//
// It reads the game data (never written) and saves the edited files to the
// mod folder (default mods/mon-niveau); the game plays them with
//   crusader data --mod DIR --map N
// or F5 in the editor. See docs/EDITEUR.md.

#define SDL_MAIN_HANDLED
#include "core/data_info.hpp"
#include "core/language.hpp"
#include "editor/editor_app.hpp"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

namespace {

// A font with the French letters (œ is outside Latin-1, ImGui's default font).
void load_font() {
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = {0x0020, 0x017F, 0x2010, 0x206F, 0x2190, 0x21FF, 0};
    const char* candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path) && io.Fonts->AddFontFromFileTTF(path, 17.0f, nullptr, ranges)) return;
    }
    io.Fonts->AddFontDefault();
}

std::filesystem::path game_executable() {
    std::filesystem::path dir;
    if (char* base = SDL_GetBasePath()) {
        dir = base;
        SDL_free(base);
    }
#ifdef _WIN32
    return dir / "crusader.exe";
#else
    return dir / "crusader";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();
    crusader::editor::EditorOptions options;
    std::string screenshot;
    int frames = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mod") == 0 && i + 1 < argc) options.mod = argv[++i];
        else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) options.map = std::stoi(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--new-map") == 0) options.new_map = true;
        else if (std::strcmp(argv[i], "--select-shape") == 0 && i + 1 < argc) {
            options.select_shape = std::stoi(argv[++i], nullptr, 0);
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("crusader-editor [data] [--mod DIR] [--map N]\n");
            return 0;
        } else {
            options.data = argv[i];
        }
    }
    options.game_exe = game_executable();
    if (!std::filesystem::exists(options.data / "STATIC/FIXED.DAT")) {
        std::fprintf(stderr, "no game data in '%s' (STATIC/FIXED.DAT): give the game's data folder\n",
                     options.data.string().c_str());
        return 1;
    }
    crusader::set_text_language(crusader::detect_data_info(options.data).text_language());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    SDL_Window* window = SDL_CreateWindow(crusader::tr("Crusader - éditeur de niveaux", "Crusader - level editor"),
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_MAXIMIZED);
    SDL_Renderer* renderer =
        window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    load_font();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    int status = 0;
    try {
        crusader::editor::EditorApp app(renderer, options);
        bool running = true;
        while (running && !app.quit_requested()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                ImGui_ImplSDL2_ProcessEvent(&e);
                if (e.type == SDL_QUIT ||
                    (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                     e.window.windowID == SDL_GetWindowID(window))) {
                    if (app.request_quit()) running = false;
                }
            }
            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (!screenshot.empty() && --frames <= 0) {
                int w = 0, h = 0;
                SDL_GetRendererOutputSize(renderer, &w, &h);
                if (SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888)) {
                    SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch);
                    SDL_SaveBMP(shot, screenshot.c_str());
                    SDL_FreeSurface(shot);
                }
                running = false;
            }
            SDL_RenderPresent(renderer);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "crusader-editor", e.what(), window);
        status = 1;
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
