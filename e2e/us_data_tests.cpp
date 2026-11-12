// Checks on the US data (v1.23): the port is written from the French
// CRUSADER.EXE, and runs the US data through the table of intrinsic numbers
// and the English texts of the US executable. Run with the US data folder;
// skipped without it.

#include "core/data_info.hpp"
#include "core/dtable.hpp"
#include "core/language.hpp"
#include "core/png_writer.hpp"
#include "engine/ui/load_screen.hpp"
#include "engine/ui/message_gump.hpp"
#include "engine/ui/weasel_gump.hpp"
#include "engine/usecode/intrinsic_numbering.hpp"
#include "engine/world/inventory.hpp"
#include "engine/world/player_commands.hpp"
#include "engine/world/teleport.hpp"
#include "game/game.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace crusader;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

constexpr std::uint32_t kFrameUs = 1'000'000 / 60;

// --png DIR: screenshots of the screens with English texts, to look at.
std::filesystem::path g_png_dir;

void shot(Game& game, const char* name) {
    if (g_png_dir.empty()) return;
    game.update(kFrameUs);
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    game.render(screen);
    write_png(g_png_dir / name, screen, game.palette());
}

void press(Game& game, std::uint8_t scancode, bool extended, std::uint8_t ascii = 0) {
    game.key(scancode, extended, true, 0, ascii);
    game.key(scancode, extended, false, 0, 0);
}

// The game writes nothing here: the tests only start new games.
std::filesystem::path saves_dir() {
    const auto dir = std::filesystem::temp_directory_path() / "crusader_us_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

void test_detection(const std::filesystem::path& data) {
    const DataInfo info = detect_data_info(data);
    CHECK(info.version == "1.23" && info.us());
    // Without CRUSADER.EXE: told by the size of UNKCOFF.DAT, version unknown.
    const auto bare = std::filesystem::temp_directory_path() / "crusader_us_no_exe";
    std::filesystem::create_directories(bare / "USECODE");
    std::filesystem::copy_file(data / "USECODE/UNKCOFF.DAT", bare / "USECODE/UNKCOFF.DAT",
                               std::filesystem::copy_options::overwrite_existing);
    const DataInfo bare_info = detect_data_info(bare);
    CHECK(bare_info.us() && bare_info.version.empty());
    // One French number per entry of the US table (311 far pointers), each a
    // number of the French table (309).
    CHECK(std::filesystem::file_size(data / "USECODE/UNKCOFF.DAT") == 4 * us_intrinsic_numbering().size());
    for (const std::uint16_t n : us_intrinsic_numbering()) CHECK(n < 309);
    // A few that moved: destroyContents (US 0x03D, French 0x02D), the fade
    // from black (0x07A, 0x07B), setActivity (0x131, 0x0F2).
    CHECK(us_intrinsic_numbering()[0x03D] == 0x02D);
    CHECK(us_intrinsic_numbering()[0x07A] == 0x07B);
    CHECK(us_intrinsic_numbering()[0x131] == 0x0F2);
}

void test_english_texts(const std::filesystem::path& data) {
    Game game(data, saves_dir());
    CHECK(game.data_language() == "US" && text_language() == Language::English);
    CHECK(std::string(dtable::item_name(0x4D4)) == "DATA LINK");
    CHECK(std::string(dtable::ammo_name(831)) == "RP-22 CLIP");
    // The proposed save name (--auto-save-names): month first, every
    // character drawn by the slots' font.
    std::tm t{};
    t.tm_year = 2026 - 1900, t.tm_mon = 10, t.tm_mday = 12, t.tm_hour = 23, t.tm_min = 59;
    CHECK(LoadScreen::dated_name(t) == "11/12/2026 23:59");
    const TextWriter writer(game.fonts(), LoadScreen::kFont);
    for (char c : std::string("0123456789/: ")) CHECK(writer.char_width(static_cast<unsigned char>(c)) > 0);
}

// The start of mission 1, as test_new_game does on the French data: the
// briefing, the trigger egg to the mission's start and the arrival scene,
// every intrinsic called being bound.
void test_new_game(const std::filesystem::path& data) {
    Game game(data, saves_dir());
    game.new_game(0);
    World& w = game.world();
    game.update(kFrameUs);
    CHECK(w.current_map() == 1);
    const Item* player = w.get(1);
    CHECK(player && player->x == 6399 && player->y == 6399);
    CHECK(game.movie() && game.movie()->kind() == MovieKind::kCutscene);
    game.key(0x01, false, true, 0, 0x1B);
    int frames = 0;
    for (; game.movie() && frames < 200; ++frames) game.update(kFrameUs);
    game.key(0x01, false, false, 0);
    CHECK(!game.movie());

    for (int i = 0; i < 60; ++i) game.update(kFrameUs);
    player = w.get(1);
    const ObjId arrival = find_teleport_egg(w, 0x63);
    CHECK(arrival && player->x == w.get(arrival)->x && player->y == w.get(arrival)->y);
    CHECK(w.state.avatar_in_stasis);
    int scene = 0;
    for (; w.state.avatar_in_stasis && scene < 600; ++scene) game.update(kFrameUs);
    std::printf("us new game: stasis over after %d frames\n", scene);
    CHECK(!w.state.avatar_in_stasis && player_may_act(w));

    // A minute of the mission's start, the player standing.
    for (int i = 0; i < 3600; ++i) game.update(kFrameUs);
    for (const auto& [number, calls] : game.machine().unbound_intrinsic_calls()) {
        std::fprintf(stderr, "unbound intrinsic 0x%03X: %u calls\n", number, calls);
    }
    for (const auto& f : game.machine().faults()) std::fprintf(stderr, "fault: %s\n", f.message.c_str());
    CHECK(game.machine().unbound_intrinsic_calls().empty());
    CHECK(game.machine().faults().empty());

    // R: the message of the US executable.
    press(game, 0x13, false, 'r');
    CHECK(game.message() && game.message()->text() == "TARGETING RETICLE INACTIVE.");
    shot(game, "message.png");
    press(game, 0x1C, false, 0x0D);
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    CHECK(!game.message());

    // The weasel's shop, its movies skipped.
    game.show_weasel(2);
    const auto gump = [&game] { return dynamic_cast<const WeaselGump*>(game.screen()); };
    // Each step plays a movie at the next pass; Escape, held, stops it.
    const auto skip_movie = [&game] {
        for (int n = 0; n < 10 && !game.movie(); ++n) game.update(kFrameUs);
        CHECK(game.movie() != nullptr);
        game.key(0x01, false, true, 0, 0x1B);
        for (int n = 0; n < 600 && game.movie(); ++n) game.update(kFrameUs);
        game.key(0x01, false, false, 0);
        CHECK(!game.movie());
    };
    skip_movie();
    CHECK(gump() && gump()->state() == WeaselGump::kBrowsing);
    shot(game, "weasel.png");
    // Escape: the leaving movie, then Escape again closes the shop.
    press(game, 0x01, false, 0x1B);
    skip_movie();
    press(game, 0x01, false, 0x1B);
    CHECK(!game.screen());

    // The menu, its options and the controller screen of the port.
    game.open_menu();
    game.mouse_move(320, 240);
    shot(game, "menu.png");
    press(game, 0x4D, true);
    press(game, 0x1C, false, 0x0D);
    shot(game, "options.png");
    game.mouse_move(440, 330);
    game.mouse_button(1, true, 440, 330);
    game.mouse_button(1, false, 440, 330);
    shot(game, "gamepad.png");
    CHECK(game.machine().faults().empty());
}

// U (the controller's d-pad up): the energy cube, then the datalink, whose
// overlay movie MVA01 is JYV1 here (5 strips), not RRV2 (13) as in French.
void test_use_datalink(const std::filesystem::path& data) {
    Game game(data, saves_dir());
    game.new_game(0);
    World& w = game.world();
    game.update(kFrameUs);
    game.key(0x01, false, true, 0, 0x1B);
    for (int i = 0; game.movie() && i < 200; ++i) game.update(kFrameUs);
    game.key(0x01, false, false, 0);
    for (int i = 0; i < 60; ++i) game.update(kFrameUs);
    for (int i = 0; i < 600 && w.state.avatar_in_stasis; ++i) game.update(kFrameUs);
    CHECK(player_may_act(w));
    bool overlay_seen = false;
    for (int n = 0; n < 2; ++n) {
        press(game, 0x16, false, 'u');
        for (int i = 0; i < 1200; ++i) {
            game.update(kFrameUs);
            overlay_seen = overlay_seen || (game.movie() && game.movie()->kind() == MovieKind::kOverlay);
        }
    }
    CHECK(overlay_seen && !game.movie());
    CHECK(game.machine().faults().empty());
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path data = "data_us";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--png" && i + 1 < argc) g_png_dir = argv[++i];
        else data = argv[i];
    }
    if (!std::filesystem::exists(data / "STATIC/FIXED.DAT")) {
        std::printf("no US game data, skipped\n");
        return 0;
    }
    test_detection(data);
    test_english_texts(data);
    test_new_game(data);
    test_use_datalink(data);
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("all US data tests passed\n");
    return failures ? 1 : 0;
}
