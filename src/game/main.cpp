// crusader: runs the game from a saved game (milestone 1: the world runs, the
// camera moves with the arrow keys).
//
//   crusader [data | game.iso | GAME.GOG] [--mod DIR] [--save N | --new-game | --map N [--egg E]] [-laurie] [--fullscreen] [--auto-save-names]  (neither: the start-up movies, then the menu) [--movie NAME | --movie-overlay NAME] [--frames N [--scroll DX DY] [--walk] [--fire] [--use ID] [--hit ID] [--menu | --menu-load | --menu-credits | --menu-options | --menu-gamepad | --menu-display | --menu-new | --menu-no-limit] --png out.png [--wav out.wav] [--save-to FILE]]
//
// With --png, runs N frames of 1/60 s without a window, moving the camera by
// (DX, DY) usecode units per frame or making the player walk, and writes the
// last frame; --wav also writes the sound mixed meanwhile. --new-game starts
// the first mission instead of loading a saved game. --use uses an item
// (as Enter on the selected item) before the first frame, --hit shoots
// one (200 damage of fire type 2, from the player). --movie plays a
// cutscene (FLICS/NAME.AVI, as MovieGump::I_playMovieCutscene) right away,
// --movie-overlay as I_playMovieOverlay. --save-to writes a saved game of
// the state after the frames to FILE (named after it), e.g. with --map and
// --egg to make a save at a teleport egg.
// Online news and update notice (core/news.hpp): --offline leaves them out;
// --check-online fetches them, prints what the main menu would show and
// exits; --online-from DIR reads DIR/release.json (the GitHub API's answer)
// and DIR/news.json instead of the network, as if the player agreed
// (--online-ask: as if not asked yet); --menu-news, with --png, opens the
// news from the menu.
// Self-update (core/self_update.hpp): the update line of the main menu installs
// the new version and restarts the game. --port-version V makes the program
// believe it is that version (to test with --online-from, whose DIR then also
// holds the files of the release: its assets are read there by name);
// --install-update DIR installs the latest release from the network into the
// folder DIR (which holds an install, e.g. a copy of this one), prints the
// steps and exits, without restarting.
// --mod loads a folder made by the level editor (crusader-no-remorse-editor): its
// files replace those of the data. --map starts a new game on that map, on
// its teleport egg E (frame 1, quality low byte E; 0x1E by default, where
// every mission starts). --auto-save-names (the PortMaster launcher, for
// consoles without a keyboard): the save screens propose the date and time
// as the name, confirmed with the pad's A.
//
// Controls of the original (CRUSADER.C 12CF): arrows (shift runs), keypad,
// Ctrl / Alt moves; see engine/world/avatar_mover.hpp. Escape opens the menu
// (keyboard or left mouse button; "quit" leaves).

#include "core/iso_image.hpp"
#include "core/language.hpp"
#include "core/news.hpp"
#include "core/png_writer.hpp"
#include "platform/gl_lighting.hpp"
#include "core/self_update.hpp"
#include "platform/http_get.hpp"
#include "platform/process.hpp"
#include "crusader_version.hpp"
#include "engine/ui/christmas_hat.hpp"
#include "engine/world/ceiling_lights.hpp"
#include "engine/world/inventory.hpp"
#include "engine/world/projectile.hpp"
#include "game/game.hpp"
#include "platform/real_shift.hpp"
#include "platform/sdl_audio.hpp"
#include "platform/sdl_gamepad.hpp"
#include "platform/sdl_input.hpp"
#include "platform/sdl_presenter.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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

// --check-online: what the main menu would show, from the network.
// @orig none: port's online news
int check_online() {
    const auto release_json = http_get(kLatestReleaseUrl);
    const auto news_json = http_get(kNewsUrl);
    const auto current = parse_semver(CRUSADER_VERSION);
    std::printf("this version: %s\n", CRUSADER_VERSION);
    if (!release_json) std::printf("latest release: could not be fetched\n");
    else if (const auto latest = parse_latest_release(*release_json)) {
        std::printf("latest release: %s (%s)%s\n", latest->version.text().c_str(), latest->url.c_str(),
                    current && latest->version > *current ? " - newer, the menu tells it" : "");
    } else {
        std::printf("latest release: unreadable answer\n");
    }
    if (!news_json) {
        std::printf("news: could not be fetched\n");
        return 1;
    }
    for (const char* language : {"en", "fr", "de"}) {
        const auto news = select_news(*news_json, static_cast<std::int64_t>(std::time(nullptr)), current, language);
        std::printf("news (%s): %zu\n", language, news.size());
        for (const NewsItem& n : news) std::printf("  %s  %s\n", format_day(n.date).c_str(), n.title.c_str());
    }
    return release_json ? 0 : 1;
}

// The network as the self-update uses it, or the files of `from` when it is
// given (--online-from): the asset a URL names is the file of that name.
// @orig none: port's self-update
UpdateHooks update_hooks(const std::filesystem::path& from) {
    UpdateHooks hooks;
    if (from.empty()) {
        hooks.fetch = [](const std::string& url) { return http_get(url); };
        hooks.download = [](const std::string& url, const std::filesystem::path& file, std::uint64_t size,
                            const std::function<bool(std::uint64_t)>& progress) {
            return http_download(url, file, size, progress);
        };
        return hooks;
    }
    hooks.fetch = [from](const std::string& url) -> std::optional<std::string> {
        std::ifstream in(from / std::filesystem::path(url.substr(url.rfind('/') + 1)), std::ios::binary);
        if (!in) return std::nullopt;
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    hooks.download = [from](const std::string& url, const std::filesystem::path& file, std::uint64_t,
                            const std::function<bool(std::uint64_t)>& progress) {
        std::ifstream in(from / std::filesystem::path(url.substr(url.rfind('/') + 1)), std::ios::binary);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!in || !out) return false;
        std::vector<char> buffer(1 << 16);
        std::uint64_t written = 0;
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            out.write(buffer.data(), in.gcount());
            written += static_cast<std::uint64_t>(in.gcount());
            if (!progress(written)) return false;
        }
        return static_cast<bool>(out);
    };
    return hooks;
}

// --install-update DIR: the latest release, installed into DIR.
// @orig none: port's self-update
int install_update(const std::filesystem::path& dir, const std::string& exe_name) {
    const auto release_json = http_get(kLatestReleaseUrl);
    const auto release = release_json ? parse_latest_release(*release_json) : std::nullopt;
    if (!release) {
        std::fprintf(stderr, "cannot read the latest release\n");
        return 1;
    }
    const char* suffix = platform_archive_suffix();
    SelfUpdater updater(update_hooks({}), dir, exe_name, suffix ? suffix : "");
    if (!updater.can_install(*release)) {
        std::fprintf(stderr, "release %s has no package for this system\n", release->version.text().c_str());
        return 1;
    }
    std::printf("installing %s into %s (replacing %s)\n", release->version.text().c_str(), dir.string().c_str(),
                exe_name.c_str());
    updater.start(*release);
    for (UpdatePhase last = UpdatePhase::kIdle;;) {
        const UpdateStatus status = updater.status();
        if (status.phase != last || status.phase == UpdatePhase::kDownloading) {
            std::printf("\rphase %d: %llu / %llu bytes   ", static_cast<int>(status.phase),
                        static_cast<unsigned long long>(status.done), static_cast<unsigned long long>(status.total));
            std::fflush(stdout);
            last = status.phase;
        }
        if (status.phase == UpdatePhase::kDone) {
            std::printf("\ndone: the old files are kept as .old, listed in .update-backup.txt\n");
            return 0;
        }
        if (status.phase == UpdatePhase::kFailed) {
            std::printf("\nfailed (%d): %s\n", static_cast<int>(status.failure), status.detail.c_str());
            return 1;
        }
        SDL_Delay(50);
    }
}

// The browser, on a page (the release of the update notice).
// @orig none: port's online news
void open_url(const std::string& url) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_OpenURL(url.c_str()) != 0) std::fprintf(stderr, "cannot open %s: %s\n", url.c_str(), SDL_GetError());
#else
    std::fprintf(stderr, "cannot open %s: SDL too old\n", url.c_str());
#endif
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
    // The file this program runs from, before anything renames it (an update).
    const std::filesystem::path exe = current_executable();
    std::string port_version = CRUSADER_VERSION;  // --port-version
    std::filesystem::path install_update_dir;     // --install-update
    bool restart = false;                         // an update was installed: start again
    std::filesystem::path data = "data", iso, mod;
    int start_map = 0, start_egg = 0x1E;
    int save = 1, frames = 0;
    bool new_game = false, save_given = false, fullscreen = false, debug_title = false, auto_save_names = false;
    int png_width = Game::kScreenWidth, png_height = Game::kScreenHeight;
    bool classic = false;  // --classic: the original 640 x 480 view, stretched to the window
    bool lighting = false;  // --lighting: the 3D lighting on for this run (GameConfig::lighting)
    bool cpu_lighting = false;  // --cpu-lighting: the 3D lighting on the processor, not the GPU
    bool laurie = false;
    double gamma = 1.0;
    std::int32_t scroll_x = 0, scroll_y = 0;
    bool walk = false, fire = false, menu = false, menu_load = false, menu_credits = false, menu_options = false, menu_gamepad = false, menu_display = false,
         menu_new = false, menu_no_limit = false, menu_news = false;
    bool offline = false, online_ask = false;
    std::filesystem::path online_from;  // --online-from DIR: release.json and news.json, not the network
    std::string png, wav, movie;
    std::filesystem::path save_to;  // --save-to FILE: the state after the frames, saved there
    int use = 0;
    int hit = 0;  // --hit ID: the item shot (200 damage of fire type 2, from the player) before the frames
    MovieKind movie_kind = MovieKind::kCutscene;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("crusader-no-remorse %s\n", CRUSADER_VERSION);
            return 0;
        }
        if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) save = std::stoi(argv[++i]), save_given = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) png = argv[++i];
        else if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) wav = argv[++i];
        else if (std::strcmp(argv[i], "--save-to") == 0 && i + 1 < argc) save_to = argv[++i];
        else if (std::strcmp(argv[i], "--new-game") == 0) new_game = true;
        else if (std::strcmp(argv[i], "--mod") == 0 && i + 1 < argc) mod = argv[++i];
        else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) start_map = std::stoi(argv[++i], nullptr, 0), new_game = true;
        else if (std::strcmp(argv[i], "--egg") == 0 && i + 1 < argc) start_egg = std::stoi(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--fullscreen") == 0) fullscreen = true;
        else if (std::strcmp(argv[i], "--auto-save-names") == 0) auto_save_names = true;  // consoles without a keyboard
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
        else if (std::strcmp(argv[i], "--lighting") == 0) lighting = true;
        else if (std::strcmp(argv[i], "--cpu-lighting") == 0) cpu_lighting = true;
        else if (std::strcmp(argv[i], "--use") == 0 && i + 1 < argc) use = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--hit") == 0 && i + 1 < argc) hit = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--menu") == 0) menu = true;
        else if (std::strcmp(argv[i], "--menu-load") == 0) menu = menu_load = true;
        else if (std::strcmp(argv[i], "--menu-credits") == 0) menu = menu_credits = true;
        else if (std::strcmp(argv[i], "--menu-options") == 0) menu = menu_options = true;
        else if (std::strcmp(argv[i], "--menu-gamepad") == 0) menu = menu_options = menu_gamepad = true;
        else if (std::strcmp(argv[i], "--menu-display") == 0) menu = menu_options = menu_display = true;
        else if (std::strcmp(argv[i], "--menu-new") == 0) menu = menu_new = true;
        else if (std::strcmp(argv[i], "--menu-no-limit") == 0) menu = menu_no_limit = true;
        else if (std::strcmp(argv[i], "--menu-news") == 0) menu = menu_news = true;
        else if (std::strcmp(argv[i], "--offline") == 0) offline = true;
        else if (std::strcmp(argv[i], "--check-online") == 0) return check_online();
        else if (std::strcmp(argv[i], "--online-from") == 0 && i + 1 < argc) online_from = argv[++i];
        else if (std::strcmp(argv[i], "--port-version") == 0 && i + 1 < argc) port_version = argv[++i];
        else if (std::strcmp(argv[i], "--install-update") == 0 && i + 1 < argc) install_update_dir = argv[++i];
        else if (std::strcmp(argv[i], "--online-ask") == 0) online_ask = true;
        else if (std::strcmp(argv[i], "--scroll") == 0 && i + 2 < argc) {
            scroll_x = std::stoi(argv[++i]);
            scroll_y = std::stoi(argv[++i]);
        }
        else if (std::strncmp(argv[i], "-laurie", 7) == 0) laurie = true;  // CRUSYS_1050_093D: the cheats enabled
        else if (std::filesystem::path p = argv[i]; is_disc_image(p)) iso = p;
        else data = argv[i];
    }

    if (!install_update_dir.empty()) {
        SDL_Init(SDL_INIT_TIMER);  // SDL_Delay
        return install_update(install_update_dir, exe.filename().string());
    }

    try {
        // The data folder holds an install of the game, or an ISO of the CD
        // that is extracted into it on the first launch.
        prepare_game_data(data, iso);
        if (!std::filesystem::exists(data / "STATIC")) {
            throw std::runtime_error("no game data in '" + data.string() +
                                     "': copy the files of an install of the game (French 1.08, US 1.23 or English 1.01) there, or its .iso image (GAME.GOG in the GOG release)");
        }
        // The language files (port addition): the "languages" folder next to
        // the executable, else in the current folder.
        {
            std::filesystem::path languages = "languages";
            if (char* base = SDL_GetBasePath()) {
                const std::filesystem::path beside = std::filesystem::path(base) / "languages";
                SDL_free(base);
                if (std::error_code error; std::filesystem::is_directory(beside, error)) languages = beside;
            }
            set_languages_dir(languages);
        }
        // The ceiling lights (port addition, engine/world/ceiling_lights.hpp):
        // the "ceiling_lights" folder of the mod, then the one next to the
        // executable, else in the current folder.
        {
            std::vector<std::filesystem::path> dirs;
            if (!mod.empty()) dirs.push_back(mod / "ceiling_lights");
            if (char* base = SDL_GetBasePath()) {
                dirs.push_back(std::filesystem::path(base) / "ceiling_lights");
                SDL_free(base);
            }
            dirs.push_back("ceiling_lights");
            set_ceiling_lights_dirs(std::move(dirs));
        }
        // SDL_Quit runs last, after the destructors of the window, the audio, the
        // controller and the GPU lighting, which still call SDL (and the GL library
        // it unloads): closing the controller after SDL_Quit crashed with
        // sdl2-compat (SDL3) on exit (issue #23), deleting the GL objects after it
        // crashed too (issue #28). Declared first, so destroyed last.
        struct SdlQuitLast {
            ~SdlQuitLast() { SDL_Quit(); }
        } sdl_quit_last;
        Game game(data, {}, mod);
        game.set_port_version(port_version);
        // The online news and update notice (port addition): in play, from
        // the network once the player agreed; --online-from reads files.
        if (!online_from.empty()) {
            game.config().check_updates = online_ask ? -1 : 1;
            game.enable_online(
                [online_from](const std::string& url) -> std::optional<std::string> {
                    std::ifstream in(online_from / (url == kLatestReleaseUrl ? "release.json" : "news.json"),
                                     std::ios::binary);
                    if (!in) return std::nullopt;
                    return std::string(std::istreambuf_iterator<char>(in), {});
                },
                open_url, true);
        } else if (!offline && png.empty()) {
            game.enable_online([](const std::string& url) { return http_get(url); }, open_url);
        }
        // The self-update (port addition): the folder of the executable; first
        // the files an earlier update left (its ".old" files, now unused).
        if (!exe.empty() && !offline && png.empty()) {
            clean_after_update(exe.parent_path());
            game.enable_self_update(update_hooks(online_from), exe.parent_path(), exe.filename().string(),
                                    platform_archive_suffix());
        } else if (!exe.empty() && !online_from.empty()) {  // tools: no thread
            game.enable_self_update(update_hooks(online_from), exe.parent_path(), exe.filename().string(),
                                    platform_archive_suffix(), true);
        }

        if (lighting) game.config().lighting = true;
        // The 3D lighting on the GPU when there is one (else on the processor),
        // made the first time the option is on; --cpu-lighting keeps it on the
        // processor.
        std::unique_ptr<GlLighting> lighting_gpu;
        bool lighting_gpu_tried = cpu_lighting;
        auto start_lighting_gpu = [&] {
            if (lighting_gpu_tried || !game.config().lighting) return;
            lighting_gpu_tried = true;
            std::string why;
            lighting_gpu = GlLighting::create(&why);
            if (lighting_gpu) game.set_lighting_gpu(lighting_gpu.get());
            else std::fprintf(stderr, "3D lighting on the processor: %s\n", why.c_str());
        };
        start_lighting_gpu();
        game.set_auto_save_names(auto_save_names);
        {
            const std::time_t now = std::time(nullptr);
            std::tm date{};
#ifdef _WIN32
            const bool known = localtime_s(&date, &now) == 0;
#else
            const bool known = localtime_r(&now, &date) != nullptr;
#endif
            game.set_christmas(known && is_christmas(date));
        }
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
            if (hit) apply_damage(game.kernel(), game.world(), game.animations(), static_cast<ObjId>(hit), 1, 0, 200, 2);
            for (int i = 0; i < frames; ++i) {
                if (scroll_x || scroll_y) game.move_camera(scroll_x, scroll_y);
                // --fire: space tapped every 40 frames
                if (fire && i % 40 == 0) {
                    game.key(0x39, false, true, 0, ' ');
                    game.key(0x39, false, false, 0);
                }
                game.update(kFrameUs);
                // The lighting's smoke starts from what each frame shows, as in play.
                if (lighting) game.render(screen, &stats);
            }
            if (!wav.empty()) write_wav(wav, game.captured_sound(), AsylumMixer::kMixRate);
            if (!save_to.empty()) {
                // Saved in slot 1 of a scratch folder (the game's own slots
                // untouched), then moved to FILE.
                const auto dir = std::filesystem::temp_directory_path() / "crusader-save-to";
                std::filesystem::remove_all(dir);
                std::filesystem::create_directories(dir);
                game.set_save_dir(dir);
                game.set_save_name(1, save_to.stem().string());
                game.save_game(1);
                std::filesystem::copy_file(dir / "SAVEGAME.1", save_to, std::filesystem::copy_options::overwrite_existing);
                std::filesystem::remove_all(dir);
            }
            if (menu) {  // --menu: Escape, as pressed in play
                game.key(0x01, false, true, 0, 0x1B);
                game.mouse_move(320, 240);
            }
            if (menu_news) {  // right to "options", up to "news", Enter
                for (const std::uint8_t scancode : {std::uint8_t{0x4D}, std::uint8_t{0x48}}) {
                    game.key(scancode, true, true, 0);
                    game.key(scancode, true, false, 0);
                }
                game.key(0x1C, false, true, 0, 0x0D);
                game.key(0x1C, false, false, 0);
            }
            if (menu_load) {  // then down to "load" and Enter
                game.key(0x50, true, true, 0);
                game.key(0x1C, false, true, 0, 0x0D);
            }
            if (menu_credits || menu_no_limit) {  // right, down to "crusader / no limit"
                game.key(0x4D, true, true, 0);
                game.key(0x4D, true, false, 0);
                game.key(0x50, true, true, 0);
                game.key(0x50, true, false, 0);
            }
            if (menu_credits) {  // down to "credits", Enter, then --frames N
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
            if (menu_gamepad || menu_display) {  // then the enhancements button, clicked
                game.mouse_move(440, 330);
                game.mouse_button(1, true, 440, 330);
                game.mouse_button(1, false, 440, 330);
                auto press = [&](std::uint8_t scancode, std::uint8_t ascii) {
                    game.key(scancode, scancode != 0x1C, true, 0, ascii);
                    game.key(scancode, scancode != 0x1C, false, 0);
                };
                if (menu_display) press(0x50, 0);  // down to DISPLAY
                press(0x1C, 0x0D);                 // the page (CONTROLS first)
                if (menu_gamepad) press(0x1C, 0x0D);  // CONTROLLER, first on it
            }
            if (menu_new) {  // Enter on "new game": the difficulty screen
                game.key(0x1C, false, true, 0, 0x0D);
                game.key(0x1C, false, false, 0);
            }
            game.render(screen, &stats);
            if (std::vector<std::uint32_t> lit; game.render_lit(screen, lit)) {
                std::vector<std::uint8_t> rgb(lit.size() * 3);
                for (std::size_t i = 0; i < lit.size(); ++i) {
                    rgb[i * 3] = static_cast<std::uint8_t>(lit[i] >> 16);
                    rgb[i * 3 + 1] = static_cast<std::uint8_t>(lit[i] >> 8);
                    rgb[i * 3 + 2] = static_cast<std::uint8_t>(lit[i]);
                }
                write_png_rgb(png, screen.width, screen.height, rgb.data());
            } else {
                write_png(png, screen, game.palette());
            }
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
        install_real_shift();  // Windows: Shift + keypad with Num Lock on (issue #14)
        if (!classic && !fullscreen) presenter.maximize();
        // Native resolution: the game is drawn at the largest whole fraction of
        // the window that is still 640 x 480 or more, so the pixels keep their
        // shape and a wider window shows more of the surroundings.
        std::vector<std::uint32_t> lit;  // the lit frame (GameConfig::lighting)
        auto fit_screen = [&]() {
            int w = 0, h = 0;
            presenter.output_size(&w, &h);
            if (classic || !game.config().native_resolution) w = Game::kScreenWidth, h = Game::kScreenHeight;
            // Pixel size in hundredths: the biggest the window allows (x1 at least), then
            // the one chosen (x1.5 fits a 1360 x 768 screen, where the whole multiple is 1).
            const int limit = std::max(100, std::min(w * 100 / Game::kScreenWidth, h * 100 / Game::kScreenHeight));
            game.set_view_zoom_limit(limit);
            int scale = limit / 100 * 100;  // AUTO: whole multiples only
            if (const int zoom = game.config().view_zoom_pct) scale = zoom <= limit ? zoom : scale;
            const int vw = std::max(Game::kScreenWidth, w * 100 / scale), vh = std::max(Game::kScreenHeight, h * 100 / scale);
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
        // Unbuffered: MSVC has no line buffering (_IOLBF is full buffering) and
        // rejects a size of 0 with it, which ended the game at once.
        if (pad_debug) std::setvbuf(stdout, nullptr, _IONBF, 0);
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
                    if (pad_debug) std::printf("[key] sdl keyboard %02x%s %s%s\n", k->scancode, k->extended ? " ext" : "", k->pressed ? "down" : "up", k->shift() ? " shift" : "");
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
            gamepad.update(SDL_GetTicks());  // the sticks, now that both axes of each are in
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
            presenter.set_smooth(game.config().smooth_scaling);
            start_lighting_gpu();  // when the option has just been switched on
            game.render(screen, &stats);
            if (debug_title) {
                int ow = 0, oh = 0;
                presenter.output_size(&ow, &oh);
                char size[64];
                std::snprintf(size, sizeof size, "  view %dx%d in %dx%d", screen.width, screen.height, ow, oh);
                presenter.set_title(diagnostics(game, stats, fps) + size);
            }
            // Port addition (lighting option): the world lit in 3D, in true colour.
            if (game.render_lit(screen, lit)) presenter.present_argb(lit.data(), screen.width, screen.height);
            else presenter.present(screen, game.palette());
        }
        print_faults(game);
        restart = game.restart_requested();
        if (!restart) return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "crusader-no-remorse", e.what(), nullptr);
        return 1;
    }
    // An update was installed: the window, the audio and SDL are closed, the
    // new executable takes over (core/self_update.hpp).
    if (!relaunch(exe, argc, argv)) {
        std::fprintf(stderr, "the update is installed, but %s could not be started again: start it by hand\n",
                     exe.string().c_str());
        return 1;
    }
    return 0;
}
