// crusader: runs the game from a saved game (milestone 1: the world runs, the
// camera moves with the arrow keys).
//
//   crusader [data | game.iso | GAME.GOG] [--mod DIR] [--save N | --new-game | --map N [--egg E]] [-laurie]  (neither: the start-up movies, then the menu) [--movie NAME | --movie-overlay NAME] [--frames N [--scroll DX DY] [--walk] [--fire] [--use ID] [--menu | --menu-load | --menu-credits | --menu-options | --menu-gamepad | --menu-new] --png out.png [--wav out.wav]]
//
// With --png, runs N frames of 1/60 s without a window, moving the camera by
// (DX, DY) usecode units per frame or making the player walk, and writes the
// last frame; --wav also writes the sound mixed meanwhile. --new-game starts
// the first mission instead of loading a saved game. --use uses an item
// (as Enter on the selected item) before the first frame. --movie plays a
// cutscene (FLICS/NAME.AVI, as MovieGump::I_playMovieCutscene) right away,
// --movie-overlay as I_playMovieOverlay.
// --mod loads a folder made by the level editor (crusader-editor): its
// files replace those of the data. --map starts a new game on that map, on
// its teleport egg E (frame 1, quality low byte E; 0x1E by default, where
// every mission starts).
//
// Controls of the original (CRUSADER.C 12CF): arrows (shift runs), keypad,
// Ctrl / Alt moves; see engine/world/avatar_mover.hpp. Escape opens the menu
// (keyboard or left mouse button; "quit" leaves).

#include "core/iso_image.hpp"
#include "core/png_writer.hpp"
#include "crusader_version.hpp"
#include "engine/world/inventory.hpp"
#include "game/game.hpp"
#include "platform/sdl_audio.hpp"
#include "platform/sdl_gamepad.hpp"
#include "platform/sdl_input.hpp"
#include "platform/sdl_presenter.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <vector>

using namespace crusader;

namespace {

constexpr std::uint32_t kFrameUs = 1'000'000 / 60;

// @orig none: the port's window title (taskbar, title bar)
constexpr const char* kWindowTitle = "Crusader: No Remorse — DECOMP";

// @orig none: start-up diagnostics of the port
std::string diagnostics(Game& game, const RenderStats& stats, double fps) {
    std::size_t unbound = 0;
    for (const auto& [n, calls] : game.machine().unbound_intrinsic_calls()) unbound += calls;
    const GameState& s = game.world().state;
    char text[256];
    std::snprintf(text, sizeof text,
                  "crusader  camera (%d, %d, %d)  %.0f fps  processes %zu  items %zu drawn %zu  "
                  "usecode faults %zu  missing intrinsic calls %zu",
                  s.camera_x, s.camera_y, s.camera_z, fps, game.kernel().process_count(), game.world().item_count(),
                  stats.drawn, game.machine().faults().size(), unbound);
    return text;
}

// Mono 16-bit PCM WAV.
// @orig none: WAV export for the tools
void write_wav(const std::string& path, const std::vector<std::int16_t>& samples, int rate) {
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate * 2));
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), bytes);
}

void print_faults(Game& game) {
    for (const auto& f : game.machine().faults()) {
        std::fprintf(stderr, "usecode fault: %s (class 0x%04X ip 0x%04X)\n", f.message.c_str(), f.class_id, f.ip);
    }
}

}  // namespace

// @orig none: command line and SDL start-up of the port
int main(int argc, char** argv) {
    SDL_SetMainReady();
    std::filesystem::path data = "data", iso, mod;
    int start_map = 0, start_egg = 0x1E;
    int save = 1, frames = 0;
    bool new_game = false, save_given = false, fullscreen = false, debug_title = false;
    int png_width = Game::kScreenWidth, png_height = Game::kScreenHeight;
    bool classic = false;  // --classic: the original 640 x 480 view, stretched to the window
    bool laurie = false;
    double gamma = 1.0;
    std::int32_t scroll_x = 0, scroll_y = 0;
    bool walk = false, fire = false, menu = false, menu_load = false, menu_credits = false, menu_options = false, menu_gamepad = false,
         menu_new = false;
    std::string png, wav, movie;
    int use = 0;
    MovieKind movie_kind = MovieKind::kCutscene;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("crusader %s\n", CRUSADER_VERSION);
            return 0;
        }
        if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) save = std::stoi(argv[++i]), save_given = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) png = argv[++i];
        else if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) wav = argv[++i];
        else if (std::strcmp(argv[i], "--new-game") == 0) new_game = true;
        else if (std::strcmp(argv[i], "--mod") == 0 && i + 1 < argc) mod = argv[++i];
        else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) start_map = std::stoi(argv[++i], nullptr, 0), new_game = true;
        else if (std::strcmp(argv[i], "--egg") == 0 && i + 1 < argc) start_egg = std::stoi(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--fullscreen") == 0) fullscreen = true;
        else if (std::strcmp(argv[i], "--debug-title") == 0) debug_title = true;  // diagnostics in the window title
        else if (std::strcmp(argv[i], "--classic") == 0) classic = true;
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {  // with --png: the screen's size, e.g. 960x540
            std::sscanf(argv[++i], "%dx%d", &png_width, &png_height);
        }
        else if (std::strcmp(argv[i], "--gamma") == 0 && i + 1 < argc) gamma = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--movie") == 0 && i + 1 < argc) movie = argv[++i];
        else if (std::strcmp(argv[i], "--movie-overlay") == 0 && i + 1 < argc) {
            movie = argv[++i];
            movie_kind = MovieKind::kOverlay;
        }
        else if (std::strcmp(argv[i], "--walk") == 0) walk = true;
        else if (std::strcmp(argv[i], "--fire") == 0) fire = true;
        else if (std::strcmp(argv[i], "--use") == 0 && i + 1 < argc) use = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--menu") == 0) menu = true;
        else if (std::strcmp(argv[i], "--menu-load") == 0) menu = menu_load = true;
        else if (std::strcmp(argv[i], "--menu-credits") == 0) menu = menu_credits = true;
        else if (std::strcmp(argv[i], "--menu-options") == 0) menu = menu_options = true;
        else if (std::strcmp(argv[i], "--menu-gamepad") == 0) menu = menu_options = menu_gamepad = true;
        else if (std::strcmp(argv[i], "--menu-new") == 0) menu = menu_new = true;
        else if (std::strcmp(argv[i], "--scroll") == 0 && i + 2 < argc) {
            scroll_x = std::stoi(argv[++i]);
            scroll_y = std::stoi(argv[++i]);
        }
        else if (std::strncmp(argv[i], "-laurie", 7) == 0) laurie = true;  // CRUSYS_1050_093D: the cheats enabled
        else if (std::filesystem::path p = argv[i]; is_disc_image(p)) iso = p;
        else data = argv[i];
    }

    try {
        // The data folder holds an install of the game, or an ISO of the CD
        // that is extracted into it on the first launch.
        prepare_game_data(data, iso);
        if (!std::filesystem::exists(data / "STATIC")) {
            throw std::runtime_error("no game data in '" + data.string() +
                                     "': copy the files of an install of the game (French 1.08, US 1.23 or English 1.01) there, or its .iso image (GAME.GOG in the GOG release)");
        }
        Game game(data, {}, mod);
        game.set_port_version(CRUSADER_VERSION);
        if (start_map > 0) game.set_start(start_map, static_cast<std::uint16_t>(start_egg));
        if (laurie) game.enable_cheats();
        // No game asked for: the start-up, as when the original is launched
        // (the movies, then the menu). The tools (--png, --movie) keep
        // loading save 1.
        const bool start_up = !new_game && !save_given && png.empty() && movie.empty();
        if (new_game) game.new_game(0);
        else if (start_up) game.start_up();
        else game.load_save(save);
        if (!movie.empty()) game.play_movie(movie, movie_kind);
        Surface8 screen(png_width, png_height);
        RenderStats stats;

        if (!png.empty()) {
            // --walk: the up arrow is held for the whole run.
            if (walk) game.key(0x48, true, true, 0);
            game.capture_sound(!wav.empty());
            if (use) use_item(game.world(), static_cast<ObjId>(use));
            for (int i = 0; i < frames; ++i) {
                if (scroll_x || scroll_y) game.move_camera(scroll_x, scroll_y);
                // --fire: space tapped every 40 frames
                if (fire && i % 40 == 0) {
                    game.key(0x39, false, true, 0, ' ');
                    game.key(0x39, false, false, 0);
                }
                game.update(kFrameUs);
            }
            if (!wav.empty()) write_wav(wav, game.captured_sound(), AsylumMixer::kMixRate);
            if (menu) {  // --menu: Escape, as pressed in play
                game.key(0x01, false, true, 0, 0x1B);
                game.mouse_move(320, 240);
            }
            if (menu_load) {  // then down to "load" and Enter
                game.key(0x50, true, true, 0);
                game.key(0x1C, false, true, 0, 0x0D);
            }
            if (menu_credits) {  // right, down to "credits", Enter, then --frames N
                game.key(0x4D, true, true, 0);
                game.key(0x4D, true, false, 0);
                game.key(0x50, true, true, 0);
                game.key(0x50, true, false, 0);
                game.key(0x1C, false, true, 0, 0x0D);
                game.key(0x1C, false, false, 0);
                for (int i = 0; i < frames; ++i) game.update(kFrameUs);
            }
            if (menu_options) {  // right to "options", Enter
                game.key(0x4D, true, true, 0);
                game.key(0x4D, true, false, 0);
                game.key(0x1C, false, true, 0, 0x0D);
                game.key(0x1C, false, false, 0);
            }
            if (menu_gamepad) {  // then the "manette" button, clicked
                game.mouse_move(440, 330);
                game.mouse_button(1, true, 440, 330);
                game.mouse_button(1, false, 440, 330);
            }
            if (menu_new) {  // Enter on "new game": the difficulty screen
                game.key(0x1C, false, true, 0, 0x0D);
                game.key(0x1C, false, false, 0);
            }
            game.render(screen, &stats);
            write_png(png, screen, game.palette());
            std::printf("%s\n", diagnostics(game, stats, 0).c_str());
            for (const auto& [n, calls] : game.machine().unbound_intrinsic_calls()) {
                std::printf("  missing intrinsic 0x%03X: %u calls\n", n, calls);
            }
            print_faults(game);
            return game.machine().faults().empty() ? 0 : 2;
        }

        // The options screen saves its settings, as the original does.
        game.set_config_file(data / "CRUSADER.CFG");
        SdlPresenter presenter(kWindowTitle, Game::kScreenWidth, Game::kScreenHeight, 2, fullscreen);
        presenter.set_gamma(gamma);
        if (!classic && !fullscreen) presenter.maximize();
        // Native resolution: the game is drawn at the largest whole fraction of
        // the window that is still 640 x 480 or more, so the pixels keep their
        // shape and a wider window shows more of the surroundings.
        auto fit_screen = [&]() {
            int w = 0, h = 0;
            presenter.output_size(&w, &h);
            if (classic || !game.config().native_resolution) w = Game::kScreenWidth, h = Game::kScreenHeight;
            int scale = std::max(1, std::min(w / Game::kScreenWidth, h / Game::kScreenHeight));
            game.set_view_zoom_limit(scale);
            if (const int zoom = game.config().view_zoom) scale = std::min(scale, zoom);
            const int vw = std::max(Game::kScreenWidth, w / scale), vh = std::max(Game::kScreenHeight, h / scale);
            if (vw != screen.width || vh != screen.height) {
                screen = Surface8(vw, vh);
                presenter.resize(vw, vh);
            }
        };
        std::unique_ptr<SdlAudio> audio;
        if (game.audio()) {
            audio = std::make_unique<SdlAudio>(game.audio()->mixer);
            game.set_audio_device(audio->ok());
        }
        SDL_ShowCursor(SDL_DISABLE);  // the game draws its own cursor
        // In play the game puts the mouse back at (320, 200) after each move
        // (world_mouse.hpp): SDL's relative mode gives the same motion
        // without warping the system pointer.
        bool relative_mouse = false;
        Gamepad gamepad;
        gamepad.init();
        game.set_vibration_handler([&gamepad](std::uint16_t low, std::uint16_t high, std::uint32_t duration) {
            gamepad.rumble(low, high, duration);
        });
        const bool pad_debug = std::getenv("CRUSADER_PAD_DEBUG") != nullptr;  // see sdl_gamepad.cpp
        if (pad_debug) std::setvbuf(stdout, nullptr, _IOLBF, 0);
        gamepad.set_settings(&game.config().pad);
        std::uint64_t last = SDL_GetPerformanceCounter();
        const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
        double fps = 0;
        bool running = true;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                if (gamepad.feed(ev)) {
                    if (gamepad.quit_requested()) running = false;
                    continue;
                }
                // Alt+Enter: fullscreen on / off, not sent to the game.
                if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && ev.key.keysym.sym == SDLK_RETURN &&
                    (ev.key.keysym.mod & KMOD_ALT)) {
                    if (ev.type == SDL_KEYDOWN && !ev.key.repeat) presenter.toggle_fullscreen();
                    continue;
                }
                if (auto k = translate_key(ev)) {
                    if (pad_debug) std::printf("[key] sdl keyboard %02x%s %s\n", k->scancode, k->extended ? " ext" : "", k->pressed ? "down" : "up");
                    game.key(k->scancode, k->extended, k->pressed, k->flags, k->ascii);
                }
                if (auto m = translate_mouse(ev)) {
                    if (relative_mouse) {
                        const bool move = m->kind == MouseEvent::Kind::Move;
                        m->x = game.cursor().x() + (move ? m->dx : 0);
                        m->y = game.cursor().y() + (move ? m->dy : 0);
                    } else {  // the menus are 640 x 480, centred in the screen
                        m->x -= game.ui_x();
                        m->y -= game.ui_y();
                    }
                    if (m->kind == MouseEvent::Kind::Move) game.mouse_move(m->x, m->y);
                    else game.mouse_button(m->button, m->kind == MouseEvent::Kind::ButtonDown, m->x, m->y);
                }
            }
            const std::uint64_t now = SDL_GetPerformanceCounter();
            const double seconds = static_cast<double>(now - last) / freq;
            last = now;
            fps = fps * 0.95 + (seconds > 0 ? 0.05 / seconds : 0);

            // The controller: A/B confirm/cancel in menus, its stick moves the mouse.
            gamepad.set_menu(!game.mouse_in_play());
            gamepad.set_capture(game.waiting_for_pad());
            gamepad.set_directional(game.config().directional_move);
            gamepad.set_directional_aim(game.config().directional_aim);
            gamepad.set_weapon_out(game.weapon_out());
            double aim_deg = 0;
            const bool aim_pushed = gamepad.aim_stick(&aim_deg);  // (argument order is unspecified)
            game.set_aim_stick(aim_pushed, aim_deg);
            for (int b : gamepad.take_captured()) game.pad_button(b);
            for (const KeyEvent& k : gamepad.take_keys()) {
                if (pad_debug) std::printf("[key] pad %02x%s %s\n", k.scancode, k.extended ? " ext" : "", k.pressed ? "down" : "up");
                game.key(k.scancode, k.extended, k.pressed, k.flags, k.ascii);
            }
            if (int dx, dy; (gamepad.mouse_motion(seconds, &dx, &dy), dx || dy)) {
                game.mouse_move(game.cursor().x() + dx, game.cursor().y() + dy);
            }

            game.update(static_cast<std::uint32_t>(seconds * 1'000'000));
            if (game.quit_requested()) running = false;
            if (game.mouse_in_play() != relative_mouse) {
                relative_mouse = game.mouse_in_play();
                SDL_SetRelativeMouseMode(relative_mouse ? SDL_TRUE : SDL_FALSE);
                // Back in a menu: the system pointer where the game's is.
                if (!relative_mouse) presenter.warp_mouse(game.cursor().x() + game.ui_x(), game.cursor().y() + game.ui_y());
            }
            fit_screen();
            game.render(screen, &stats);
            if (debug_title) {
                int ow = 0, oh = 0;
                presenter.output_size(&ow, &oh);
                char size[64];
                std::snprintf(size, sizeof size, "  view %dx%d in %dx%d", screen.width, screen.height, ow, oh);
                presenter.set_title(diagnostics(game, stats, fps) + size);
            }
            presenter.present(screen, game.palette());
        }
        print_faults(game);
        SDL_Quit();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "crusader", e.what(), nullptr);
        return 1;
    }
}
