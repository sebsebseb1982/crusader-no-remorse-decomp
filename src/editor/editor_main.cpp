// crusader-no-remorse-editor: level editor for Crusader: No Remorse maps.
//
//   crusader-no-remorse-editor [data | game folder] [--mod DIR] [--map N] [--new-map] [--select-shape S]
//                               [--prefabs DIR]
//                              [--screenshot out.bmp [--frames N]]
//
// --screenshot writes the window after N frames (60 by default) and quits.
//
// It reads the game data (never written) and saves the edited files to the
// mod folder (default mods/mon-niveau); the game plays them with
//   crusader-no-remorse data --mod DIR --map N
// or F5 in the editor. See docs/EDITEUR.md.

#define SDL_MAIN_HANDLED
#include "core/data_info.hpp"
#include "core/language.hpp"
#include "editor/editor_app.hpp"
#include "editor/generator/campaign.hpp"

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
    return dir / CRUSADER_GAME_EXE;
}

// Crusader: No Limit without a window: --generate SEED writes new levels
// into the mod folder, --regenerate DIR the levels of DIR's NOLIMIT.JSON
// (a shared one); both record the seeds in the mod's NOLIMIT.JSON.
int generate(const std::filesystem::path& data, const std::filesystem::path& mod, std::uint32_t seed, int levels,
             int biome, const std::filesystem::path& regenerate_from) {
    using namespace crusader::editor;
    try {
        const gen::GameData game = gen::GameData::load(data);
        std::printf("%s, version %d. %zu biomes:\n", gen::kGeneratorName, gen::kGeneratorVersion, game.biomes.size());
        for (std::size_t i = 0; i < game.biomes.size(); ++i) {
            std::printf("  %2zu  %s\n", i, game.biomes[i].name().c_str());
        }
        Project project(data, mod);
        std::string error;
        std::vector<gen::GeneratedLevel> generated;
        if (!regenerate_from.empty()) {
            const auto manifest = gen::Manifest::read(regenerate_from, &error);
            if (manifest) generated = gen::regenerate_levels(project, game, *manifest, error);
        } else {
            generated = gen::generate_levels(project, game, seed, levels, biome, error);
        }
        if (generated.empty()) {
            std::fprintf(stderr, "generation failed: %s\n", error.c_str());
            return 1;
        }
        project.save();
        gen::record_levels(mod, generated);
        for (const auto& g : generated) {
            std::printf("map %d:\n", g.map);
            for (const std::string& note : g.result.notes) std::printf("  %s\n", note.c_str());
        }
        std::printf("written to %s (seeds in %s); play with:\n  crusader-no-remorse %s --mod %s --map %d\n",
                    mod.string().c_str(), gen::kManifestFile, data.string().c_str(), mod.string().c_str(),
                    generated.front().map);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();
    crusader::editor::EditorOptions options;
    std::string screenshot;
    int frames = 60;
    long long generate_seed = -1;
    int generate_count = 1, generate_biome = -1;
    bool mod_given = false;
    std::filesystem::path regenerate_from;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mod") == 0 && i + 1 < argc) options.mod = argv[++i], mod_given = true;
        else if (std::strcmp(argv[i], "--generate") == 0 && i + 1 < argc) generate_seed = std::stoll(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--regenerate") == 0 && i + 1 < argc) regenerate_from = argv[++i];
        else if (std::strcmp(argv[i], "--levels") == 0 && i + 1 < argc) generate_count = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--biome") == 0 && i + 1 < argc) generate_biome = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) options.map = std::stoi(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--new-map") == 0) options.new_map = true;
        else if (std::strcmp(argv[i], "--prefabs") == 0 && i + 1 < argc) options.prefabs = argv[++i];
        else if (std::strcmp(argv[i], "--select-shape") == 0 && i + 1 < argc) {
            options.select_shape = std::stoi(argv[++i], nullptr, 0);
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("crusader-no-remorse-editor [data] [--mod DIR] [--map N]\n"
                        "crusader-no-remorse-editor [data] --generate SEED [--levels N] [--biome B] [--mod DIR]\n"
                        "crusader-no-remorse-editor [data] --regenerate DIR [--mod DIR]   (DIR/NOLIMIT.JSON)\n");
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
    if (generate_seed >= 0 || !regenerate_from.empty()) {
        if (!mod_given) {
            options.mod = !regenerate_from.empty() ? regenerate_from
                                                   : std::filesystem::path("mods/gen-" + std::to_string(generate_seed));
        }
        return generate(options.data, options.mod, static_cast<std::uint32_t>(generate_seed < 0 ? 0 : generate_seed),
                        generate_count, generate_biome, regenerate_from);
    }

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
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "crusader-no-remorse-editor", e.what(), window);
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
