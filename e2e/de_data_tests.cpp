// Checks on the German data (v1.07): its CRUSADER.EXE is the French 1.08 one
// translated (data_info.hpp), so the port runs it with the French intrinsic
// numbers, the German texts of languages/de/port.txt and the button places of
// the German executable. Run with the German data folder; skipped without it.

#include "core/data_info.hpp"
#include "core/dtable.hpp"
#include "core/flex.hpp"
#include "core/font.hpp"
#include "core/language.hpp"
#include "core/png_writer.hpp"
#include "engine/ui/load_screen.hpp"
#include "engine/ui/message_gump.hpp"
#include "engine/ui/shape_dialog.hpp"
#include "engine/ui/weasel_gump.hpp"
#include "engine/usecode/intrinsic_numbering.hpp"
#include "engine/world/player_commands.hpp"
#include "engine/world/render.hpp"
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

// --png DIR: screenshots of the screens with German texts and pictures.
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
    const auto dir = std::filesystem::temp_directory_path() / "crusader_de_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

void test_detection(const std::filesystem::path& data) {
    const DataInfo info = detect_data_info(data);
    CHECK(info.version == "1.07" && info.german() && !info.us());
    CHECK(info.text_language() == Language::German);
    // The French numbering: the German usecode calls the French intrinsics.
    CHECK(intrinsic_numbering(info).empty());
    CHECK(std::filesystem::file_size(data / "USECODE/UNKCOFF.DAT") == 309 * 4);
}

void test_german_texts(const std::filesystem::path& data) {
    Game game(data, saves_dir());
    CHECK(game.data_language() == "DE" && game.data_version() == "1.07");
    CHECK(text_language() == Language::German && german_version() && !us_version());
    // The names of the German executable (DS:24B0..25AE).
    CHECK(std::string(dtable::item_name(0x4D4)) == "DATENSCHIRM");
    CHECK(std::string(dtable::ammo_name(831)) == "RP-22 MAGAZIN");
    CHECK(std::string(tr("TARGETING RETICLE ACTIVE.")) == "FADENKREUZ AKTIV.");
    CHECK(std::string(tr("<MORE>")) == "<MEHR>");
    // Code page 850: "daß" (0xE1) as in the German executable.
    CHECK(std::string(tr("Are you sure you\nwant to buy this?")) == "Sind Sie sicher,\nda\xE1 Sie das kaufen\nwollen?");
    // The proposed save name: day first, with dots, every character drawn by
    // the slots' font, as are the umlauts the German keyboard gives.
    std::tm t{};
    t.tm_year = 2026 - 1900, t.tm_mon = 10, t.tm_mday = 18, t.tm_hour = 23, t.tm_min = 59;
    CHECK(LoadScreen::dated_name(t) == "18.11.2026 23:59");
    const TextWriter writer(game.fonts(), LoadScreen::kFont);
    for (char c : std::string("0123456789.: ")) CHECK(writer.char_width(static_cast<unsigned char>(c)) > 0);
    for (char c : std::string("\x8E\x99\x9A")) CHECK(writer.char_width(static_cast<unsigned char>(c)) > 0);
}

// Typing a save name on the German data: the umlaut keys give capital
// umlauts, < : ; and quotes are not taken (1008:09B7).
void test_save_name(const std::filesystem::path& data) {
    const FlexArchive gumps_flex = FlexArchive::open(data / "STATIC/GUMPS.FLX");
    ShapeCache gumps(gumps_flex, ShapeLayout::Flat2D);
    const FontSet fonts = FontSet::load(data / "STATIC/FONTS.FLX");
    LoadScreen::Names names{};
    int selected = 0;
    LoadScreen screen(gumps, fonts, names, selected, LoadScreen::Mode::kSave);
    screen.key(0x0D);
    CHECK(screen.editing());
    // g a-umlaut r U-umlaut n : < " ; o-umlaut . (code page 850)
    for (char c : std::string("g\x84r\x9An:<\";\x94.")) screen.key(static_cast<unsigned char>(c));
    CHECK(screen.name() == "G\x8ER\x9AN\x99.");
}

// The start of mission 1, as on the French data: the briefing, the arrival
// scene, every intrinsic called being bound; then the German screens.
void test_new_game(const std::filesystem::path& data) {
    Game game(data, saves_dir());
    game.new_game(0);
    World& w = game.world();
    game.update(kFrameUs);
    CHECK(w.current_map() == 1);
    CHECK(game.movie() && game.movie()->kind() == MovieKind::kCutscene);
    game.key(0x01, false, true, 0, 0x1B);
    for (int frames = 0; game.movie() && frames < 200; ++frames) game.update(kFrameUs);
    game.key(0x01, false, false, 0);
    CHECK(!game.movie());

    for (int i = 0; i < 60; ++i) game.update(kFrameUs);
    const Item* player = w.get(1);
    const ObjId arrival = find_teleport_egg(w, 0x63);
    CHECK(arrival && player->x == w.get(arrival)->x && player->y == w.get(arrival)->y);
    int scene = 0;
    for (; w.state.avatar_in_stasis && scene < 600; ++scene) game.update(kFrameUs);
    std::printf("de new game: stasis over after %d frames\n", scene);
    CHECK(!w.state.avatar_in_stasis && player_may_act(w));

    // F5 (the player free to act): the quick save question, its buttons
    // placed for the German shape.
    press(game, 0x3F, false);
    const auto* ask = dynamic_cast<const ShapeDialog*>(game.screen());
    CHECK(ask && ask->shape() == ShapeDialog::kQuickSaveQuestion);
    shot(game, "quicksave.png");
    press(game, 0x01, false, 0x1B);
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    CHECK(!game.screen());

    // A minute of the mission's start, the player standing.
    for (int i = 0; i < 3600; ++i) game.update(kFrameUs);
    for (const auto& [number, calls] : game.machine().unbound_intrinsic_calls()) {
        std::fprintf(stderr, "unbound intrinsic 0x%03X: %u calls\n", number, calls);
    }
    for (const auto& f : game.machine().faults()) std::fprintf(stderr, "fault: %s\n", f.message.c_str());
    CHECK(game.machine().unbound_intrinsic_calls().empty());
    CHECK(game.machine().faults().empty());
    shot(game, "game.png");

    // R: the message of the German executable.
    press(game, 0x13, false, 'r');
    CHECK(game.message() && game.message()->text() == "FADENKREUZ INAKTIV.");
    shot(game, "message.png");
    press(game, 0x1C, false, 0x0D);
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    CHECK(!game.message());

    // The weasel's shop, its movies skipped.
    game.show_weasel(2);
    const auto gump = [&game] { return dynamic_cast<const WeaselGump*>(game.screen()); };
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
    press(game, 0x01, false, 0x1B);
    skip_movie();
    press(game, 0x01, false, 0x1B);
    CHECK(!game.screen());

    // F1: the help screen and its German buttons.
    press(game, 0x3B, false);
    shot(game, "help.png");
    press(game, 0x01, false, 0x1B);
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);

    // The menu, the difficulty screen of a new game (one centred picture, as
    // in the US version), then the options.
    game.open_menu();
    game.mouse_move(320, 240);
    shot(game, "menu.png");
    press(game, 0x1C, false, 0x0D);
    shot(game, "difficulty.png");
    press(game, 0x01, false, 0x1B);
    game.update(kFrameUs);
    press(game, 0x4D, true);
    press(game, 0x1C, false, 0x0D);
    shot(game, "options.png");
    game.mouse_move(440, 330);
    game.mouse_button(1, true, 440, 330);
    game.mouse_button(1, false, 440, 330);
    shot(game, "enhancements.png");
    CHECK(game.machine().faults().empty());
}

}  // namespace

int main(int argc, char** argv) {
    set_languages_dir(CRUSADER_LANGUAGES_DIR);  // the German texts (languages/de/port.txt)
    std::filesystem::path data = "data_de";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--png" && i + 1 < argc) g_png_dir = argv[++i];
        else data = argv[i];
    }
    if (!std::filesystem::exists(data / "STATIC/FIXED.DAT")) {
        std::printf("no German game data, skipped\n");
        return 0;
    }
    test_detection(data);
    test_german_texts(data);
    test_save_name(data);
    test_new_game(data);
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("all German data tests passed\n");
    return failures ? 1 : 0;
}
