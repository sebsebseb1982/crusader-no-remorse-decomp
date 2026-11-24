// Milestone 1 checks on the original data: a saved game runs, and the camera
// can cross the whole map without usecode errors while the fast area follows.

#include "core/avi_movie.hpp"
#include "core/language.hpp"
#include "core/dtable.hpp"
#include "core/movie_subtitles.hpp"
#include "core/png_writer.hpp"
#include "engine/palette_fader.hpp"
#include "engine/ui/computer_gump.hpp"
#include "engine/ui/keypad_gump.hpp"
#include "engine/ui/weasel_gump.hpp"
#include "engine/ui/gump_keys.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/actor_create.hpp"
#include "engine/world/aim_mode.hpp"
#include "engine/usecode/item_intrinsics.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/activity.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/auto_firer.hpp"
#include "engine/world/chargers.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/glass_debris.hpp"
#include "engine/world/item_damage.hpp"
#include "engine/world/gravity.hpp"
#include "engine/world/inventory.hpp"
#include "engine/world/item_selection.hpp"
#include "engine/world/pathfind.hpp"
#include "engine/world/player_commands.hpp"
#include "engine/world/sprite_process.hpp"
#include "engine/world/projectile.hpp"
#include "engine/world/teleport.hpp"
#include "editor/level.hpp"
#include "editor/project.hpp"
#include "game/game.hpp"
#include "test_saves.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <tuple>

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

using Key = std::tuple<std::int32_t, std::int32_t, std::int32_t, std::uint16_t, std::uint16_t>;

// Glob items on the map, by position and look; counts those outside the fast area.
std::multiset<Key> glob_items(const World& w, std::size_t& outside) {
    std::multiset<Key> keys;
    outside = 0;
    w.for_each([&](const Item& i) {
        if ((i.flags & 0x82) != 0x82 || i.parent || (i.flags & kItemEthereal)) return;
        keys.emplace(i.x, i.y, i.z, i.shape, i.frame);
        outside += !w.fast_area().contains(i.x, i.y);
    });
    return keys;
}

void test_load_and_run(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    const Item* player = game.world().get(1);
    CHECK(player && game.world().state.camera_x == player->x && game.world().state.camera_y == player->y);

    // The view puts the camera 320 pixels from the left, 300 from the top.
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    RenderStats stats;
    game.render(screen, &stats);
    CHECK(stats.drawn > 100 && stats.missing_shape == 0);

    for (int i = 0; i < 600; ++i) game.update(kFrameUs);  // 10 s
    CHECK(game.machine().faults().empty());
    CHECK(game.kernel().ticks() >= 599);
}

void test_player_walks(const std::filesystem::path& data) {
    // Keys as the keyboard interrupt sees them: E0-extended arrows.
    constexpr std::uint8_t kUp = 0x48, kRight = 0x4D;
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) game.update(kFrameUs);
    };
    const Item start = *w.get(1);
    const int dir = w.npc_direction(1) & ~1;  // walking has 8 directions
    CHECK(dir == 2);

    // Holding up: the KeyDaemon repeats the key, the player walks north-east.
    // The original's clearance probes can shift him sideways at the railing
    // (shape 139 at y 15423), so his path need not be a straight diagonal.
    game.key(kUp, true, true, 0);
    run(120);
    const Item& now = *w.get(1);
    const int dx = now.x - start.x, dy = now.y - start.y;
    std::printf("player walked from (%d, %d) to (%d, %d), direction %d\n", start.x, start.y, now.x, now.y, dir);
    const int sx = kDirectionDx[dir], sy = kDirectionDy[dir];
    CHECK(dx * sx > 0 && dy * sy > 0);
    CHECK(position_free(w, now.shape, w.location(now), nullptr, 1));
    game.key(kUp, true, false, 0);
    run(30);

    // Right arrow: the stand animation turns him by 2 (8-direction action).
    game.key(kRight, true, true, 0);
    run(2);
    game.key(kRight, true, false, 0);
    run(30);
    CHECK(w.npc_direction(1) == 4);

    // Up again: he walks along +x, in half strides, showing walk frames.
    const Item before = *w.get(1);
    game.key(kUp, true, true, 0);
    run(90);
    const Item& walking = *w.get(1);
    CHECK(walking.x > before.x + 100 && walking.y == before.y);
    const AnimAction* walk = game.animations().action(walking.shape, static_cast<std::uint16_t>(ActorAction::kWalk));
    bool walk_frame = false;
    for (std::size_t f = 0; walk && f < walk->frame_count(); ++f) walk_frame |= walk->frame(4, f)->frame == walking.frame;
    CHECK(walk_frame);

    // Release: the current half stride ends, then he stands still.
    game.key(kUp, true, false, 0);
    run(60);
    const Item stopped = *w.get(1);
    run(30);
    CHECK(w.get(1)->x == stopped.x && w.get(1)->y == stopped.y);
    const AnimAction* stand = game.animations().action(stopped.shape, static_cast<std::uint16_t>(ActorAction::kStand));
    CHECK(stand && stand->frame(4, 0)->frame == stopped.frame);
    CHECK(w.cells_consistent());
    CHECK(game.machine().faults().empty());
}

void test_camera_follows(const std::filesystem::path& data) {
    // ITEMCACH.C 1D29: the view stays put while the player is inside the
    // margins, then jumps to bring him back to (320, 280) or to a side.
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    const Item& p = *w.get(1);
    auto screen = [&](const Item& i) {
        const GameState& s = w.state;
        return std::pair<int, int>{((i.x - i.y) >> 1) - s.view_left, ((i.x + i.y) >> 2) - s.view_top - i.z};
    };
    std::printf("player on screen at (%d, %d)\n", screen(p).first, screen(p).second);
    // moveTo rounds each quarter (x >> 2) + (y >> 2), the projection the sum: 300 or 301.
    CHECK(screen(p).first == 320 && (screen(p).second == 300 || screen(p).second == 301));
    const std::int16_t left = w.state.view_left, top = w.state.view_top;
    int jumps = 0;
    for (int step = 0; step < 120; ++step) {
        const auto before = screen(p);
        w.move_item(1, p.x + 4, p.y, p.z);  // +x: down-right on screen
        if (w.state.view_top != top || w.state.view_left != left) {
            ++jumps;
            std::printf("camera jump at step %d: before (%d, %d), after (%d, %d)\n", step, before.first,
                        before.second, screen(p).first, screen(p).second);
            // Jumped when the player passed 409 (70 from the bottom); being
            // then right of 520, he is brought back to x 120.
            CHECK(before.second <= 409 && before.first + 2 > 520);
            CHECK(screen(p).second == 280 && screen(p).first == 120);
            break;
        }
        CHECK(screen(p).second <= 409);
    }
    CHECK(jumps == 1);
}

void test_camera_sweep(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    const GameState start = w.state;
    std::size_t outside = 0;
    const auto initial = glob_items(w, outside);
    CHECK(!initial.empty() && outside == 0);

    // Bounds of the map's items.
    std::int32_t x0 = 0x7FFF, y0 = 0x7FFF, x1 = 0, y1 = 0;
    w.for_each([&](const Item& i) {
        if (i.parent || (i.flags & kItemEthereal)) return;
        x0 = std::min(x0, i.x), y0 = std::min(y0, i.y), x1 = std::max(x1, i.x), y1 = std::max(y1, i.y);
    });

    // Rows 1024 units apart, 64 units per frame along each row.
    std::size_t frames = 0, bad_positions = 0;
    for (std::int32_t y = y0; y <= y1; y += 1024) {
        for (std::int32_t x = x0; x <= x1; x += 64) {
            w.set_camera(x, y, start.camera_z);
            game.update(kFrameUs);
            ++frames;
            glob_items(w, outside);
            bad_positions += outside != 0;
        }
    }
    std::printf("map 1 sweep: x %d..%d, y %d..%d, %zu frames, %zu usecode faults, %zu processes at the end\n", x0, x1,
                y0, y1, frames, game.machine().faults().size(), game.kernel().process_count());
    for (const auto& f : game.machine().faults()) {
        std::fprintf(stderr, "  fault: %s (class 0x%04X ip 0x%04X)\n", f.message.c_str(), f.class_id, f.ip);
    }
    CHECK(game.machine().faults().empty());
    CHECK(bad_positions == 0);

    // Back to the start: the same glob items as in the saved game.
    w.set_camera(start.camera_x, start.camera_y, start.camera_z);
    const auto back = glob_items(w, outside);
    CHECK(back == initial);
}

}  // namespace

// Milestone 3: the status bar of SAVEGAME.13 (weapon with ammunition, an item
// in quantity, little energy).
void test_status_bar(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    const World& w = game.world();
    const StatusBar& bar = game.status_bar();
    CHECK(w.state.status_bar_visible);
    for (int i = 0; i < StatusBar::kBoxes; ++i) {
        const auto& r = bar.box(i);
        CHECK(r.x0 == 15 + 127 * i && r.y0 == 450 && r.x1 == r.x0 + 107 && r.y1 == 477);
    }

    const auto c = bar.contents(w);
    const Item* weapon = w.get(c.weapon);
    const Item* ammo = w.get(c.ammo);
    const Item* item = w.get(c.item);
    std::printf("SAVEGAME.13 status: weapon %u (shape %u, q %u), ammo shape %u item %u (q %u), item shape %u q %u, "
                "hp %u -> %d, energy %u\n",
                c.weapon, weapon ? weapon->shape : 0, weapon ? weapon->quality : 0, c.ammo_shape, c.ammo,
                ammo ? ammo->quality : 0, item ? item->shape : 0, item ? item->quality : 0, w.npc_hp(1),
                c.health_width, w.npc_mana(1));
    CHECK(weapon && ammo && ammo->shape == c.ammo_shape && item && item->quality == 16);
    CHECK(c.health_width == static_cast<std::uint16_t>(w.npc_hp(1) * 66) / 150 && c.energy_width == w.npc_mana(1) * 66 / 10000);

    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    game.render(screen);
    const auto& health = bar.box(3);
    CHECK(screen.row(health.y0 + 7)[health.x0 + 34] == 0x41);
    CHECK(screen.row(health.y0 + 20)[health.x0 + 34 + c.health_width] == 0x41);
    CHECK(screen.row(health.y0 + 7)[health.x0 + 35 + c.health_width] != 0x41);

    game.world().state.status_bar_visible = false;  // CruStatusGump::I_hideStatusGump
    Surface8 bare(Game::kScreenWidth, Game::kScreenHeight);
    game.render(bare);
    CHECK(bare.row(health.y0 + 7)[health.x0 + 34] != 0x41);
}

// Milestone 3: key codes, cursor and the in-game menu.
void test_keys_and_cursor(const std::filesystem::path& data) {
    GameKeyEvent e;
    e.pressed = true;
    e.ascii = 0x1B;
    e.scancode = 0x01;
    CHECK(gump_key_code(e) == gump_key::kEscape);
    e.ascii = 0;
    e.scancode = 0x48;
    CHECK(gump_key_code(e) == gump_key::kUp);
    e.scancode = 0x2D;
    e.flags = kLeftAlt | kAlt;
    CHECK(gump_key_code(e) == gump_key::kAltX);
    e.flags = kCtrl | kAlt;
    CHECK(gump_key_code(e) == 0x42D);
    e.flags = kLeftShift;
    e.ascii = 'X';
    CHECK(gump_key_code(e) == 'X');
    e.pressed = false;
    CHECK(gump_key_code(e) == 0);

    auto c = MouseCursor::load(data / "STATIC/MOUSE.SHP");
    CHECK(c.frame_count() == 40 && !c.visible());
    // Directions around (320, 200), 0 = up, clockwise; zone 2 beyond 30 x 25.
    CHECK(MouseCursor::direction(320, 100) == 0 && MouseCursor::direction(420, 100) == 1);
    CHECK(MouseCursor::direction(420, 200) == 2 && MouseCursor::direction(420, 300) == 3);
    CHECK(MouseCursor::direction(320, 300) == 4 && MouseCursor::direction(220, 300) == 5);
    CHECK(MouseCursor::direction(220, 200) == 6 && MouseCursor::direction(220, 100) == 7);
    CHECK(MouseCursor::zone(350, 225) == 1 && MouseCursor::zone(351, 200) == 2 && MouseCursor::zone(10, 10) == 2);
    c.move_to(420, 200);
    CHECK(c.frame() == 2 + 8);  // directional, running zone
    c.push(MouseCursor::kArrow);
    CHECK(c.frame() == 0x24 && c.level() == 1);
    c.reset(MouseCursor::kMenuArrow);
    CHECK(c.level() == 0 && c.frame() == 0x25 && c.pop() == 0x25 && c.level() == 0);
    c.set_bottom(439);
    c.move_to(700, 470);
    CHECK(c.x() == 639 && c.y() == 439);
}

void test_menu(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    for (int i = 0; i < 60; ++i) game.update(kFrameUs);
    CHECK(game.cursor().bottom() == 439 && !game.cursor().visible());
    const Palette before = game.palette();
    // The saved game's music (entry 70): track 1.
    CHECK(game.audio() && game.audio()->music.current() == 1 && game.audio()->mixer.music_playing());

    // Escape: the menu opens over a frozen world.
    game.key(0x01, false, true, 0, 0x1B);
    game.key(0x01, false, false, 0, 0);
    CHECK(game.menu() && game.cursor().visible() && game.cursor().frame() == 0x25);
    CHECK(!game.world().state.status_bar_visible && game.cursor().bottom() == 479);
    CHECK(!(game.palette() == before));
    CHECK(game.audio()->music.current() == Music::kMenu);
    const Item* player = game.world().get(1);
    const auto px = player->x, py = player->y;
    game.key(0x48, true, true, 0);  // up arrow: in the menu, not the player
    for (int i = 0; i < 120; ++i) game.update(kFrameUs);
    game.key(0x48, true, false, 0);
    CHECK(player->x == px && player->y == py);

    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    game.render(screen);
    CHECK(screen.row(0)[0] == 0);  // cleared outside the 638 x 240 area

    // Keys move the selection; right goes 3 forward from the left column.
    const int start = game.menu()->selected();
    game.key(0x50, true, true, 0);  // down
    game.key(0x50, true, false, 0);
    CHECK(game.menu()->selected() == std::min(start + 1, 5));

    // Mouse: press on "quit", drag out and back, release inside.
    const MainMenu::Rect quit = game.menu()->button(MainMenu::kQuit);
    game.mouse_button(true, quit.x0 + 2, quit.y0 + 2);
    CHECK(game.menu()->selected() == MainMenu::kQuit);
    game.mouse_move(10, 10);
    CHECK(game.menu()->selected() == MainMenu::kQuit);

    // Escape again: back to the game, the status bar returns.
    game.mouse_button(false, 10, 10);  // released outside: nothing
    CHECK(game.menu() != nullptr);
    game.key(0x01, false, true, 0, 0x1B);
    CHECK(!game.menu() && !game.quit_requested() && game.world().state.status_bar_visible);
    CHECK(!game.cursor().visible() && game.cursor().bottom() == 439 && game.palette() == before);
    CHECK(game.audio()->music.current() == 1);
    game.update(kFrameUs);
    CHECK(game.machine().faults().empty());

    // Quit through the mouse.
    game.key(0x01, false, true, 0, 0x1B);
    game.mouse_button(true, quit.x0 + 2, quit.y0 + 2);
    game.mouse_button(false, quit.x0 + 2, quit.y0 + 2);
    CHECK(!game.menu() && game.quit_requested());
}

void press(Game& game, std::uint8_t scancode, bool extended, std::uint8_t ascii = 0) {
    game.key(scancode, extended, true, 0, ascii);
    game.key(scancode, extended, false, 0, 0);
}

// Milestone 3: "load game" from the menu.
void test_load_from_menu(const std::filesystem::path& data) {
    // The original saves only, whatever the player saved since.
    const auto dir = std::filesystem::temp_directory_path() / "crusader_load_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (const int n : {1, 2, 3, 4, 5, 6, 7, 13}) {
        const std::string name = "SAVEGAME." + std::to_string(n);
        std::filesystem::copy_file(test_saves(data) / name, dir / name);
    }
    std::filesystem::copy_file(test_saves(data) / "SAVEGAME.2", dir / "SAVEGAME.12");
    const auto names = LoadScreen::read_names(dir);
    CHECK(names[0] == "SEB" && names[1] == "SEB2" && names[5] == "MISSION2" && names[7].empty() &&
          names[11] == "SEB2" && names[12].empty());

    Game reference(data, test_saves(data));
    reference.set_save_dir(dir);
    reference.load_save(12);
    const Item* expected = reference.world().get(1);

    Game game(data, test_saves(data));
    game.set_save_dir(dir);
    game.load_save(1);
    game.update(kFrameUs);
    press(game, 0x01, false, 0x1B);  // Escape: menu, button 0 selected
    press(game, 0x50, true);         // down: "load"
    CHECK(game.menu() && game.menu()->selected() == MainMenu::kLoad);
    press(game, 0x1C, false, 0x0D);  // Enter: load screen
    CHECK(game.menu() && game.menu()->load_screen());

    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    game.render(screen);
    const auto& r = game.menu()->load_screen()->slot_rect(0);
    CHECK(r.x0 == 183 && r.y0 == 109 && r.x1 == 183 + 313 && r.y1 == 109 + 16);
    CHECK(screen.row(r.y0)[r.x0] == LoadScreen::kSelectedColour);  // slot 0 outlined
    const auto& autosave_row = game.menu()->load_screen()->slot_rect(11);
    CHECK(autosave_row.y0 == 406 && autosave_row.x0 == 183);

    // An empty slot (SAVEGAME.8 does not exist) cannot be chosen.
    const auto empty = game.menu()->load_screen()->slot_rect(7);
    game.mouse_button(true, empty.x0 + 5, empty.y0 + 5);
    game.mouse_button(false, empty.x0 + 5, empty.y0 + 5);
    CHECK(game.menu() && game.menu()->load_screen() && game.menu()->load_screen()->slot() == 7);
    press(game, 0x01, false, 0x1B);  // Escape: back to the menu
    CHECK(game.menu() && !game.menu()->load_screen());

    press(game, 0x1C, false, 0x0D);  // "load" again, slot 7 still selected
    for (int i = 0; i < 6; ++i) press(game, 0x48, true);  // up to slot 1
    CHECK(game.menu()->load_screen()->slot() == 1);
    press(game, 0x1C, false, 0x0D);  // choose SAVEGAME.2
    CHECK(!game.menu() && !game.quit_requested());
    game.update(kFrameUs);
    const Item* player = game.world().get(1);
    CHECK(player && expected && player->x == expected->x && player->y == expected->y && player->z == expected->z);
    CHECK(game.world().state.status_bar_visible && game.machine().faults().empty());

    // The autosave is selectable from the load screen with the same arrows
    // and confirm controls as the regular slots (also mapped from a gamepad).
    press(game, 0x01, false, 0x1B);
    press(game, 0x1C, false, 0x0D);
    for (int i = 0; i < 10; ++i) press(game, 0x50, true);
    CHECK(game.menu() && game.menu()->load_screen() && game.menu()->load_screen()->slot() == 11);
    press(game, 0x1C, false, 0x0D);
    CHECK(!game.menu());
    game.update(kFrameUs);
    player = game.world().get(1);
    CHECK(player && expected && player->x == expected->x && player->y == expected->y && player->z == expected->z);
    CHECK(game.machine().faults().empty());
    std::filesystem::remove_all(dir);
}

// Milestone 6: a new game from the fixed data (GAMEPAL_1028_0000), the
// teleport eggs and changing maps.
// A level made with the level editor: a new map in a mod folder, played
// as `crusader-no-remorse data --mod DIR --map N`. The map holds the start pad of the
// first mission (its egg 0x1E and the globs of its cell), under a new number.
void test_editor_mod(const std::filesystem::path& data) {
    const auto mod = std::filesystem::temp_directory_path() / "crusader_game_tests_mod";
    std::filesystem::remove_all(mod);
    std::size_t slot = 0;
    std::int32_t ex = 0, ey = 0, ez = 0;
    {
        editor::Project project(data, mod);
        slot = project.free_map();
        const editor::Level map1 = project.load_map(1);
        editor::Level level;
        for (const editor::LevelItem& i : map1.items) {
            if (i.shape == 0x194 && i.frame == 1 && i.quality_low() == 0x1E) ex = i.x, ey = i.y, ez = i.z;
        }
        for (const editor::LevelItem& i : map1.items) {
            const bool start_egg = i.shape == 0x194 && i.frame == 1 && i.quality_low() == 0x1E;
            const bool glob_here = i.shape == 0x10 && (i.x & ~0x1FF) == (ex & ~0x1FF) && (i.y & ~0x1FF) == (ey & ~0x1FF);
            if (start_egg || glob_here) level.items.push_back(i);
        }
        CHECK(slot != 0 && level.items.size() >= 2);
        project.store_map(slot, level);
        project.save();
    }
    CHECK(std::filesystem::exists(mod / "STATIC/FIXED.DAT"));
    // Saves of a mod go to mod/SAVEGAME unless told otherwise.
    Game game(data, test_saves(data), mod);
    game.set_start(static_cast<int>(slot), 0x1E);
    game.new_game(3);
    for (int f = 0; f < 120; ++f) game.update(kFrameUs);
    const World& w = game.world();
    const Item* player = w.get(1);
    CHECK(w.current_map() == static_cast<int>(slot));
    CHECK(player && player->map_number == slot && player->x == ex && player->y == ey);
    const std::int32_t z = player ? player->z : -1;
    for (int f = 0; f < 120; ++f) game.update(kFrameUs);
    CHECK(player && w.get(1)->z == z && z >= ez);  // standing on the pad, not falling
    std::filesystem::remove_all(mod);
}

void test_new_game(const std::filesystem::path& data) {
    // Globals of a saved game: entry 60.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        const auto save = FlexArchive::open(test_saves(data) / "SAVEGAME.13");
        const auto entry = save.entry(60);
        const auto& g = game.machine().globals();
        CHECK(entry.size() == 0x42 && std::equal(entry.begin(), entry.end(), g.begin()));
        CHECK(game.world().current_map() == 5 && game.world().state.keycards != 0);
    }

    Game game(data, test_saves(data));
    game.new_game(0);
    World& w = game.world();
    std::ifstream in(data / "USECODE/UNKDS.DAT", std::ios::binary);
    const std::vector<std::uint8_t> unkds{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(unkds.size() == 0x42 && std::equal(unkds.begin(), unkds.end(), game.machine().globals().begin()));
    CHECK(w.state.difficulty == 3 && w.npc_hp(1) == 150 && w.state.battery == 0);
    // The datalink in item slot 11, active, quality 1; the container first.
    const Item* player = w.get(1);
    CHECK(player && player->shape == 1 && player->contents.size() == 2);
    const ObjId datalink = w.npc_equip(1, 11);
    const Item* d = w.get(datalink);
    CHECK(d && d->shape == 0x4D4 && d->z == 11 && (d->flags & 0x0200) && d->quality == 1 && d->parent == 1);
    CHECK(w.npc_word(1, kNpcActiveItem) == 11);
    CHECK(player && w.get(player->contents[0])->shape == 0x598 && player->contents[1] == datalink);
    CHECK(w.current_map() == -1);

    // First slice: map 1 loaded, the player on egg 0x1E (the start pad).
    game.update(kFrameUs);
    CHECK(w.current_map() == 1 && w.cells_consistent());
    player = w.get(1);
    CHECK(player->x == 6399 && player->y == 6399 && player->z == 0 && player->map_number == 1);
    // Items read in file order from id 0x100 (records with shape 0 skipped).
    const auto records = read_fixed_map(FlexArchive::open(data / "STATIC/FIXED.DAT"), 1);
    const Item* first = w.get(kFirstItemId + 2);  // after the datalink and the container
    CHECK(first && first->shape == records[0].shape && first->x == records[0].x && first->y == records[0].y);

    // Mission 1's egg (family 4, type 0x0D: class 0x90D MISS1EGG) caches in
    // with the map and plays the briefing, 01A.AVI, after a jump to black:
    // the fade to grey ends at once and Escape, held, stops the movie.
    CHECK(game.movie() && game.movie()->kind() == MovieKind::kCutscene && game.movie()->screen().width == 640);
    game.key(0x01, false, true, 0, 0x1B);
    int frames = 0;
    for (; game.movie() && frames < 200; ++frames) game.update(kFrameUs);
    game.key(0x01, false, false, 0);
    CHECK(!game.movie() && frames < 5);

    // The trigger egg next to the pad (0x63, frame 0) sends him to the
    // mission's start, egg 0x63 at frame 1.
    for (int i = 0; i < 60; ++i) game.update(kFrameUs);
    player = w.get(1);
    const ObjId arrival = find_teleport_egg(w, 0x63);
    CHECK(arrival && player->x == w.get(arrival)->x && player->y == w.get(arrival)->y);
    // The arrival script can centre a few units beside the landing point;
    // smooth following must not replace that explicit camera position.
    CHECK(std::abs(w.state.camera_x - player->x) <= 8 && std::abs(w.state.camera_y - player->y) <= 8);
    // MISS1EGG's arrival scene (class 0xA11, event 0x23) holds the player
    // in stasis (DS:7E10) until it ends.
    CHECK(w.state.avatar_in_stasis);
    CHECK(game.machine().faults().empty());
    // Then the script fades the game palette in (fadeToGamePal): its waiting
    // process resumes the colour cycling, and the stasis ends (0x076).
    CHECK(w.state.cru_stasis);
    int fading = 0;
    for (; w.state.cycle_paused && fading < 300; ++fading) game.update(kFrameUs);
    game.update(kFrameUs);
    std::printf("new game: colour cycling resumed after %d frames\n", fading);
    CHECK(!w.state.cycle_paused && !w.state.cru_stasis && game.palette() == w.state.game_palette);
    int scene = 0;
    for (; w.state.avatar_in_stasis && scene < 300; ++scene) game.update(kFrameUs);
    std::printf("new game: stasis over %d frames later\n", scene);
    CHECK(!w.state.avatar_in_stasis && player_may_act(w));
    CHECK(game.machine().unbound_intrinsic_calls().empty());

    // MainActor::I_switchMap(3) (mission 2): every process ends, map 3 replaces map 1;
    // the player keeps his inventory.
    start_switch_map(game.kernel(), w, 3);
    for (int i = 0; i < 3; ++i) game.update(kFrameUs);
    player = w.get(1);
    CHECK(w.current_map() == 3 && player->map_number == 3 && w.cells_consistent());
    const ObjId start = find_teleport_egg(w, kTeleportStartEgg);
    CHECK(start && player->x == w.get(start)->x && player->y == w.get(start)->y);
    CHECK(w.get(datalink) && w.get(datalink)->parent == 1);
    // From the menu: "new game" opens the difficulty screen (level 3 at
    // first, DIFF.PAL); Escape goes back, the level is kept; a click on
    // level 1 starts a new game at that level.
    {
        Game g(data, test_saves(data));
        g.load_save(1);
        press(g, 0x01, false, 0x1B);
        CHECK(g.menu() && g.menu()->selected() == MainMenu::kNewGame);
        const Palette menu_palette = g.palette();
        press(g, 0x1C, false, 0x0D);
        const DifficultyScreen* ds = g.menu() ? g.menu()->difficulty_screen() : nullptr;
        CHECK(ds && ds->selected() == 3 && !(g.palette() == menu_palette));
        if (ds) {
            CHECK(ds->button(1).x0 == 159 && ds->button(1).y0 == 120 && ds->button(4).x1 == 479 && ds->button(4).y0 == 297);
        }
        press(g, 0x50, true);  // down
        CHECK(ds && ds->selected() == 4);
        press(g, 0x01, false, 0x1B);
        CHECK(g.menu() && !g.menu()->difficulty_screen() && g.palette() == menu_palette);
        press(g, 0x1C, false, 0x0D);
        ds = g.menu() ? g.menu()->difficulty_screen() : nullptr;
        CHECK(ds && ds->selected() == 4);
        if (ds) {
            const auto b = ds->button(1);
            g.mouse_button(true, b.x0 + 10, b.y0 + 5);
            g.mouse_button(false, b.x0 + 10, b.y0 + 5);
        }
        CHECK(!g.menu());
        g.update(kFrameUs);  // the new game, then its first slice: the start pad
        CHECK(g.world().state.difficulty == 1 && g.world().current_map() == 1 && g.world().get(1)->x == 6399);
    }

    // Map 1's items are gone and map 3's took their slots, from the head of
    // the free list (the last ones freed): every item slot is either used or
    // free, none lost.
    const auto records2 = read_fixed_map(FlexArchive::open(data / "STATIC/FIXED.DAT"), 3);
    std::size_t items_used = 0;
    bool record_found = false;
    w.for_each([&](const Item& i) {
        if (!is_npc_id(i.id)) ++items_used;
        if (i.shape == records2[0].shape && i.x == records2[0].x && i.y == records2[0].y) record_found = true;
    });
    CHECK(record_found);
    CHECK(items_used + w.free_items().ids.size() == kItemSlots - kFirstItemId);

    CHECK(game.machine().faults().empty());
}

// Two transformers of map 1 broken together (TRANSFOR at 23295, 14959 and
// 23503, 14959): each one's gotHit fades the screen in from black, waits for
// the fade, then destroys the arcs within 160 of it. Remorse runs both fades
// (1440:0000 activates every one; No Regret's second one ends at once), so the
// arc only the second one reaches (SHOCKEW at 23599, 14911) goes too.
void test_transformers_broken_together(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.set_start(1, 0x1E);
    game.new_game(0);
    // The mission's movie freezes the game: skipped.
    for (int f = 0; f < 120; ++f) {
        game.update(kFrameUs);
        if (!game.movie()) continue;
        game.key(0x01, false, true, 0, 0x1B);
        game.update(kFrameUs);
        game.key(0x01, false, false, 0);
    }
    CHECK(!game.movie());
    World& w = game.world();
    const auto find = [&w](std::uint16_t shape, std::int32_t x, std::int32_t y) {
        ObjId found = 0;
        w.for_each([&](const Item& i) {
            if (i.shape == shape && i.x == x && i.y == y && !i.parent) found = i.id;
        });
        return found;
    };
    const ObjId a = find(0x1BF, 23295, 14959), b = find(0x1BF, 23503, 14959);
    CHECK(a && b && find(0x1C2, 23391, 14911) && find(0x1C2, 23599, 14911));
    if (!a || !b) return;
    // One blow of 200 each (fire type 5: bullets do a third of their damage
    // to items). The second one breaks a frame later, while the first one's
    // fade runs.
    apply_damage(game.kernel(), w, game.animations(), a, 1, 0, 200, 5);
    game.update(kFrameUs);
    apply_damage(game.kernel(), w, game.animations(), b, 1, 0, 200, 5);
    for (int f = 0; f < 240; ++f) game.update(kFrameUs);
    const bool broken = w.get(a) && w.get(a)->frame == 1 && w.get(b) && w.get(b)->frame == 1;
    // By place: a destroyed item's id is soon another item's.
    const bool shared_stays = find(0x1C2, 23391, 14911) != 0, far_stays = find(0x1C2, 23599, 14911) != 0;
    std::printf("transformers broken together: %s, shared arc %s, far arc %s\n", broken ? "both broken" : "not both",
                shared_stays ? "stays" : "gone", far_stays ? "stays" : "gone");
    CHECK(broken);
    CHECK(!shared_stays && !far_stays);
}

// Port lighting (smoke): a security camera shot leaves smoke where it was,
// then smoulders. Save CAM1: camera 7645 (shape 0x1B8, frame 4) on a wall
// in view. Its explosion sprite is raised over the scene and moved by its
// shape's offset (sprite_process.hpp); the smoke started under that anchor,
// some 45 pixels below the camera. Broken (frame 9), the camera keeps a thin
// wisp rising from it.
void test_camera_smoke(const std::filesystem::path& data) {
    constexpr ObjId kCamera = 7645;
    Game game(data, test_saves(data));
    game.config().lighting = true;
    game.config().volumetric_smoke = true;
    game.load_save_file(test_saves(data) / "camera-cassee-fumee.sav");
    World& w = game.world();
    const Item* camera = w.get(kCamera);
    CHECK(camera && camera->shape == 0x1B8 && camera->frame != 9);
    if (!camera) return;
    // Its screen place (lighting space projection, the view's corner aside).
    const float cx = float(world_from_usecode(camera->x)), cy = float(world_from_usecode(camera->y));
    const float cam_sx = (cx - cy) * 0.25f, cam_sy = (cx + cy) * 0.125f - float(camera->z);
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    std::vector<std::uint32_t> argb;
    auto frame = [&] {
        game.update(kFrameUs);
        game.render(screen);
        game.render_lit(screen, argb);
    };
    frame();
    apply_damage(game.kernel(), w, game.animations(), kCamera, 1, 0, 200, 2);
    // The blast's smoke, a few frames on: its middle on the screen.
    float sx = 0, sy = 0;
    int blast = 0;
    for (int f = 0; f < 6; ++f) frame();
    for (const PuffState& p : game.lighting()->puffs_shown()) {
        if (p.grey != 0.3f) continue;  // the explosion's puffs
        sx += (p.centre.x - p.centre.y) * 0.25f;
        sy += (p.centre.x + p.centre.y) * 0.125f - p.centre.z / kLightZScale;
        ++blast;
    }
    if (blast) sx /= blast, sy /= blast;
    std::printf("camera smoke: %d blast puffs, middle %+.0f %+.0f px from the camera\n", blast, sx - cam_sx,
                sy - cam_sy);
    CHECK(blast == 6);
    CHECK(std::abs(sx - cam_sx) < 20 && sy - cam_sy < -10 && sy - cam_sy > -70);
    CHECK(w.get(kCamera) && w.get(kCamera)->frame == 9);
    // Long after: no blast left, a wisp rising from the camera.
    for (int f = 0; f < 600; ++f) frame();
    int wisps = 0, blasts = 0;
    for (const PuffState& p : game.lighting()->puffs_shown()) {
        if (p.grey == 0.3f) ++blasts;
        const float px = (p.centre.x - p.centre.y) * 0.25f, py = (p.centre.x + p.centre.y) * 0.125f - p.centre.z / kLightZScale;
        if (p.feature == 9 && std::abs(px - cam_sx) < 16 && py < cam_sy && py > cam_sy - 80) ++wisps;
    }
    std::printf("camera smoke: %d wisp puffs over the broken camera\n", wisps);
    CHECK(blasts == 0 && wisps >= 2);
}

// Bug (issue #24): in mission 3, using the computer with the files (0x4FF,
// item 2580 of the save) showed its schematic MVA03B for a blink only. The
// key that uses it (Enter or Space of the selection mode) is one of those
// that stop a movie (1468:2869), and the overlay starts 2 frames after the
// press, before a finger lifts. The movie must play on until a key is
// pressed again; then EVENT waits and the transmission 3H follows.
void test_movie_ignores_held_use_key(const std::filesystem::path& data) {
    constexpr ObjId kComputer = 2580;
    for (const std::uint8_t key : {std::uint8_t{0x1C}, std::uint8_t{0x39}}) {
        Game game(data, test_saves(data));
        game.load_save_file(test_saves(data) / "issue24-schema-mission3-ordinateur.sav");
        World& w = game.world();
        CHECK(w.get(kComputer) && w.get(kComputer)->shape == 0x4FF);
        game.key(key, false, true, 0, key == 0x1C ? '\r' : ' ');
        use_item(w, kComputer);
        int frames = 0, last = 0;
        bool overlay = false;
        for (int f = 0; f < 60 * 25 && last <= 200 && (frames == 0 || game.movie()); ++f) {
            if (f == 6) game.key(key, false, false, 0);  // 100 ms
            game.update(kFrameUs);
            if (const MoviePlayer* m = game.movie()) {
                overlay = overlay || m->kind() == MovieKind::kOverlay;
                last = m->frame();
                ++frames;
            }
        }
        std::printf("held use key 0x%02X: overlay %d, %d frames on screen, movie frame %d\n", key, overlay, frames,
                    last);
        CHECK(overlay && last > 200 && game.movie());
        // A new press still stops it.
        game.key(key, false, true, 0);
        game.update(kFrameUs);
        game.key(key, false, false, 0);
        CHECK(!game.movie());
        CHECK(game.machine().faults().empty());
    }
}

// Bug: back at the rebel base after mission 1, Escape crashed the game.
// The map switch (MainActor::I_switchMap, GAMEPAL_1028_03f6) failed every
// process, the Controller included, which the game still sent the keys to.
// The original keeps it (first table slots) and resets it (1138:3181).
// Save TEST16: in front of the exit teleporter of mission 1, which plays
// R01 and switches to map 40, whose egg plays H01 then 2b: all skipped
// with Escape, as reported.
void test_back_to_base(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "fin-mission1-echap-videos-base.sav");
    World& w = game.world();
    const Process* controller = game.kernel().find_process(0, CombatControl::kType);
    CHECK(controller == &game.combat());
    // In line with the teleporter, then up (towards -x) onto it.
    w.move_item(1, 15611, 18036, 96);
    game.key(0x48, true, true, 0);
    int frame = 0;
    for (; frame < 600 && !game.movie(); ++frame) game.update(kFrameUs);
    game.key(0x48, true, false, 0);
    CHECK(game.movie());
    int movies = 0;
    bool in_movie = false;
    for (int i = 0; i < 6000; ++i) {
        if (game.movie() && !in_movie) ++movies;
        in_movie = game.movie() != nullptr;
        game.key(0x01, false, in_movie, 0, 0x1B);
        game.update(kFrameUs);
    }
    std::printf("back to base: map %d, %d movie(s) skipped\n", w.current_map(), movies);
    CHECK(w.current_map() == 40 && movies == 3 && !game.movie());
    CHECK(game.kernel().find_process(0, CombatControl::kType) == controller && controller == &game.combat());
    CHECK(!w.state.aim.active && w.state.aim.target == 0);
    // Escape opens the menu, the keys going through the Controller first.
    press(game, 0x01, false, 0x1B);
    CHECK(game.menu());
    CHECK(game.machine().faults().empty());
}

// Milestone 6: cutscenes (PLAYER.C, segment 1468; subtitles, segment 1470).
void test_movies(const std::filesystem::path& data) {
    CHECK(movie_base_name("1a") == "01A" && movie_base_name("mva5b") == "MVA05B" &&
          movie_base_name("mva10") == "MVA10" && movie_base_name("16a") == "16A" && movie_base_name("r01") == "R01");

    // 01A.AVI: every frame decodes, its pixels within the movie's colours.
    const AviMovie m = AviMovie::load(data / "FLICS/01A.AVI");
    CHECK(m.width() == 240 && m.height() == 180 && m.codec() == fourcc("RRV2") && m.strips() == 15 &&
          m.frame_count() == 940 && m.colours() == 236 && m.has_audio() && m.audio_initial_frames() == 4);
    Surface8 frame(m.width(), m.height());
    int frames = 0;
    for (const AviMovie::Chunk& c : m.chunks()) {
        if (c.id != AviMovie::kFrameChunk && c.id != AviMovie::kHalfWidthChunk) continue;
        for (int s = 0; s < m.strips(); ++s) decode_movie_strip(m.data(c), s, m.strips(), false, frame);
        ++frames;
    }
    const auto [lo, hi] = std::minmax_element(frame.pixels.begin(), frame.pixels.end());
    CHECK(frames == 940 && *lo >= 10 && *hi <= 245);
    // WEC.AVI (RRV1, 5 strips): only half-width frames.
    const AviMovie wec = AviMovie::load(data / "FLICS/WEC.AVI");
    CHECK(wec.strips() == 5 && wec.rrv() && wec.width() == 320);
    Surface8 wide(wec.width(), wec.height());
    for (const AviMovie::Chunk& c : wec.chunks()) {
        if (c.id == AviMovie::kFrameChunk) CHECK(false);
        if (c.id != AviMovie::kHalfWidthChunk) continue;
        for (int s = 0; s < wec.strips(); ++s) decode_movie_strip(wec.data(c), s, wec.strips(), true, wide);
    }

    // 01A.TXT: 15 entries; the first has no text.
    const FontSet fonts = FontSet::load(data / "STATIC/FONTS.FLX");
    const auto subtitles = load_movie_subtitles(data / "FLICS/01A.TXT", fonts, MoviePlayer::kSubtitleFont);
    CHECK(subtitles.size() == 15 && subtitles[0].delay == 105 && !subtitles[0].text);
    CHECK(subtitles.size() == 15 && subtitles[1].delay == 65 && subtitles[1].text &&
          subtitles[1].text->rfind("Je suis le g\x82n\x82ral Maxis.", 0) == 0 && subtitles[1].width > 300 &&
          subtitles[14].delay == -875);

    // From SAVEGAME.1, small movie with subtitles: the game is frozen, the
    // screen fades to grey (40 Hz), then 15 frames a second.
    Game game(data, test_saves(data));
    game.load_save(1);
    GameConfig config = game.config();
    config.big_video = false;
    config.subtitles = true;
    game.set_config(config);
    const Palette game_palette = game.palette();
    const std::uint32_t ticks = game.kernel().ticks();
    game.play_movie("nothing", MovieKind::kCutscene);
    CHECK(!game.movie());
    game.play_movie("1a", MovieKind::kCutscene);
    CHECK(game.movie() && game.palette() == game_palette);
    int fade = 0;
    for (; game.movie() && game.movie()->frame() == 0 && fade < 300; ++fade) game.update(kFrameUs);
    const auto vga = game.palette().to_vga();
    CHECK(fade > 30 && fade < 150 && vga[0] == 2 && vga[3 * 10] == reduce8(m.colour_table()[10].r) && vga[3 * 8] == 63);
    for (int i = 0; i < 600; ++i) game.update(kFrameUs);  // 10 s
    const MoviePlayer* p = game.movie();
    CHECK(p && p->frame() > 145 && p->frame() < 155 && game.kernel().ticks() == ticks);
    if (p) {
        // Centred at (200, 150), the background in grey levels, the
        // subtitle under it (frame 107 on: entry 2) at y 338.
        const Surface8& s = p->screen();
        const std::uint8_t inside = s.row(240)[320], outside = s.row(20)[20];
        CHECK(inside >= 10 && inside <= 245 && (outside < 8 || outside >= 248));
        int text = 0;
        for (int y = 338; y < 338 + 16; ++y) {
            for (int x = 20; x < 621; ++x) text += s.row(y)[x] != 0;
        }
        CHECK(text > 100);
    }
    game.key(0x01, false, true, 0, 0x1B);
    game.update(kFrameUs);
    game.key(0x01, false, false, 0);
    // The colour cycling, paused during the movie, resumes.
    CHECK(!game.movie() && game.palette() == game_palette && !game.world().state.cycle_paused);
    game.update(kFrameUs);
    game.update(kFrameUs);
    CHECK(game.kernel().ticks() > ticks);

    // An overlay keeps the game palette and has 13 strips. After intrinsic
    // 0x092 (DS:5FFE), the cycling stays paused when the movie ends.
    game.world().state.movie_keeps_cycle_paused = true;
    const Palette before_overlay = game.palette();
    game.play_movie("mva5b", MovieKind::kOverlay);
    for (int i = 0; i < 120; ++i) game.update(kFrameUs);
    CHECK(game.movie() && game.palette() == before_overlay && game.movie()->frame() > 25);
    game.key(0x01, false, true, 0, 0x1B);
    game.update(kFrameUs);
    game.key(0x01, false, false, 0);
    CHECK(!game.movie() && game.world().state.cycle_paused);
    CHECK(game.machine().faults().empty());
}

// Milestone 3: palette fades (FADE.C), one step per tick at 60 Hz.
void test_options(const std::filesystem::path& data) {
    using O = OptionsScreen;
    Game game(data, test_saves(data));
    game.load_save(1);
    // A CRUSADER.CFG as installed (the data's may have been changed by
    // playing): no volume keys (defaults), "bigvideo=true".
    game.set_config(GameConfig::parse("irq=7\r\nsoundcard=3\r\nbigvideo=true\r\n"));
    CHECK(game.config().sound_volume == 0x40 && game.config().music_volume == 0x20 && game.config().big_video);
    const auto cfg = std::filesystem::temp_directory_path() / "crusader_options_test.cfg";
    std::filesystem::remove(cfg);
    game.set_config_file(cfg);
    game.update(kFrameUs);

    // In play, '-' and '=' step the music volume (CRUSADER_1138_0896).
    press(game, 0x0C, false, '-');
    CHECK(game.config().music_volume == 0x1F && game.audio()->mixer.music_volume() == 0x1F);
    press(game, 0x0D, false, '=');
    CHECK(game.config().music_volume == 0x20);

    press(game, 0x01, false, 0x1B);  // Escape: the menu
    press(game, 0x4D, true);         // right: "options"
    press(game, 0x1C, false, 0x0D);  // Enter
    const O* o = game.menu() ? game.menu()->options() : nullptr;
    CHECK(o != nullptr);
    if (!o) return;
    CHECK(o->focus() == 0 && o->sound_step() == 22 && o->music_step() == 15);
    CHECK(o->chosen(O::kAnimationOn) && !o->chosen(O::kAnimationOff) && o->chosen(O::kVideoBig));
    CHECK(o->chosen(O::kFilterOn) && o->chosen(O::kFrameSkipOff) && o->chosen(O::kBlastsOff));
    const auto quit = o->rect(O::kQuit);
    CHECK(quit.x0 == 517 && quit.y0 == 318);

    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    game.render(screen);
    CHECK(screen.row(145)[22] == 0x0C && screen.row(176)[258] == 0x0C);  // line 0 outlined

    // Keys: down to the sound volume, left lowers it one step, right at the
    // top changes nothing.
    for (int i = 0; i < 5; ++i) press(game, 0x50, true);
    CHECK(o->focus() == 5);
    press(game, 0x4B, true);
    CHECK(o->sound_step() == 21 && game.audio()->mixer.sound_volume() == 0x3D);
    press(game, 0x4D, true);
    press(game, 0x4D, true);
    CHECK(o->sound_step() == 22 && game.audio()->mixer.sound_volume() == 0x40);

    // Mouse: "music -" held for one second acts at once, then every 11 ticks.
    const auto minus = o->rect(O::kMusicDown);
    game.mouse_button(true, minus.x0 + 3, minus.y0 + 3);
    CHECK(o->focus() == 6 && o->music_step() == 14 && o->pressed(O::kMusicDown));
    for (int i = 0; i < 60; ++i) game.update(kFrameUs);
    CHECK(o->music_step() == 9 && game.audio()->mixer.music_volume() == O::kMusicSteps[9]);
    game.mouse_move(10, 10);  // dragged out: no more repeats
    for (int i = 0; i < 30; ++i) game.update(kFrameUs);
    CHECK(o->music_step() == 9 && !o->pressed(O::kMusicDown));
    game.mouse_button(false, 10, 10);

    // A two-choice widget acts on the press.
    const auto filter_off = o->rect(O::kFilterOff);
    game.mouse_button(true, filter_off.x0 + 3, filter_off.y0 + 3);
    game.mouse_button(false, filter_off.x0 + 3, filter_off.y0 + 3);
    CHECK(o->focus() == 4 && o->chosen(O::kFilterOff) && !game.config().filter);
    press(game, 0x34, false, '4');  // '4': the first widget of the line
    CHECK(game.config().filter);
    press(game, 0x36, false, '6');
    CHECK(!game.config().filter);

    // Down to "quit" (selected), Enter: back to the menu, the file written.
    for (int i = 0; i < 6; ++i) press(game, 0x50, true);
    CHECK(o->focus() == O::kQuitLine && o->pressed(O::kQuit));
    press(game, 0x1C, false, 0x0D);
    CHECK(game.menu() && !game.menu()->options());
    const GameConfig saved = GameConfig::load(cfg);
    CHECK(saved.music_volume == O::kMusicSteps[9] && saved.sound_volume == 0x40 && !saved.filter);
    CHECK(saved.big_video && saved.mouse_speed == 2 && saved.text().find("irq=7\r\n") == 0);
    std::filesystem::remove(cfg);

    // The focus stays on "quit" for the next time.
    press(game, 0x1C, false, 0x0D);
    CHECK(game.menu()->options() && game.menu()->options()->focus() == O::kQuitLine);

    // The controller screen (port addition): left of "quit", Enter opens it.
    press(game, 0x4B, true);
    o = game.menu()->options();
    CHECK(o->focus() == O::kPadLine && !o->pressed(O::kQuit) && !o->gamepad());
    game.render(screen);
    CHECK(screen.row(o->enhancements_button().y0)[o->enhancements_button().x0 + 5] == 0x0C);  // outlined
    press(game, 0x4D, true);  // right: back to "quit"
    CHECK(o->focus() == O::kQuitLine && o->pressed(O::kQuit));
    press(game, 0x4B, true);
    press(game, 0x1C, false, 0x0D);
    // The enhancements window holds the controller screen, first row of its
    // first page ("controles").
    CHECK(o->enhancements() && !o->gamepad());
    game.render(screen);
    press(game, 0x1C, false, 0x0D);
    CHECK(o->enhancements()->page() == EnhancementsScreen::Page::Controls);
    press(game, 0x1C, false, 0x0D);
    const GamepadScreen* pad = o->gamepad();
    using G = GamepadScreen;
    CHECK(pad != nullptr && pad->focus() == 0 && !pad->waiting());
    if (pad) {
        // Left column: the actions; right: their button. Row 0 is "fire", on A.
        CHECK(G::action_of(0) == kPadFire && game.config().pad.button_of(kPadFire) == kPadRightTrigger);
        CHECK(!game.waiting_for_pad());
        // Enter: wait for a button. B is bound to jump; pressing B for "fire"
        // moves fire to B, leaves RT and jump unbound.
        press(game, 0x1C, false, 0x0D);
        CHECK(pad->waiting() && game.waiting_for_pad());
        game.render(screen);
        press(game, 0x4D, true);  // other keys do nothing while waiting
        press(game, 0x50, true);
        CHECK(pad->waiting() && pad->focus() == 0);
        game.pad_button(kPadB);
        CHECK(!pad->waiting() && !game.waiting_for_pad());
        const PadSettings& ps = game.config().pad;
        CHECK(ps.button_of(kPadFire) == kPadB && ps.buttons[kPadRightTrigger] == kPadNone && ps.button_of(kPadJump) < 0);
        // Escape cancels the wait without closing; a click elsewhere too; ten seconds as well.
        press(game, 0x1C, false, 0x0D);
        press(game, 0x01, false, 0x1B);
        CHECK(!pad->waiting() && game.menu()->options()->gamepad());
        press(game, 0x1C, false, 0x0D);
        game.pad_button(kPadX);  // while waiting: fire -> X
        CHECK(ps.button_of(kPadFire) == kPadX && ps.buttons[kPadB] == kPadNone);
        press(game, 0x1C, false, 0x0D);
        for (int i = 0; i < 620; ++i) game.update(kFrameUs);
        CHECK(!pad->waiting());
        game.pad_button(kPadY);  // not waiting: ignored
        CHECK(ps.button_of(kPadFire) == kPadX);
        // Delete unbinds the focused action.
        press(game, 0x53, true);
        CHECK(ps.button_of(kPadFire) < 0);
        // The mouse: a click on a value binds, on a label only focuses.
        const auto v = pad->value_rect(2);
        game.mouse_button(true, v.x0 + 5, v.y0 + 5);
        game.mouse_button(false, v.x0 + 5, v.y0 + 5);
        CHECK(pad->focus() == 2 && pad->waiting());
        game.pad_button(kPadRightStick);
        CHECK(ps.button_of(G::action_of(2)) == kPadRightStick);
        const auto label = pad->row_rect(3);
        game.mouse_button(true, label.x0 + 5, label.y0 + 5);
        game.mouse_button(false, label.x0 + 5, label.y0 + 5);
        CHECK(pad->focus() == 3 && !pad->waiting());
        // The stick rows: right at the end of the scale stops there.
        while (pad->focus() < G::kAimRow) press(game, 0x50, true);
        for (int i = 0; i < 6; ++i) press(game, 0x4D, true);
        CHECK(ps.aim == PadSettings::kAimLevels);
        const auto dz = pad->value_rect(G::kDeadZoneRow);
        game.mouse_button(true, dz.x0 + 5, dz.y0 + 5);
        game.mouse_button(false, dz.x0 + 5, dz.y0 + 5);
        CHECK(ps.dead_zone == PadSettings::kDefaultDeadZone - 1);
        game.render(screen);
        CHECK(screen.row(pad->row_rect(G::kDeadZoneRow).y0)[pad->row_rect(G::kDeadZoneRow).x0 + 5] == 0x0C);
        // "Par defaut" resets everything; "retour" closes the screen, not the options.
        const auto reset = pad->row_rect(G::kDefaultRow);
        game.mouse_button(true, reset.x0 + 5, reset.y0 + 5);
        game.mouse_button(false, reset.x0 + 5, reset.y0 + 5);
        CHECK(ps.is_default());
        press(game, 0x50, true);
        press(game, 0x1C, false, 0x0D);
        CHECK(game.menu()->options() && !game.menu()->options()->gamepad());
        // Reopened on the row it was left on; bound again, saved with the options.
        CHECK(game.menu()->options()->enhancements());
        press(game, 0x1C, false, 0x0D);
        const G* again = game.menu()->options()->gamepad();
        CHECK(again && again->focus() == G::kBackRow);
        const auto a = again->value_rect(0);
        game.mouse_button(true, a.x0 + 5, a.y0 + 5);
        game.mouse_button(false, a.x0 + 5, a.y0 + 5);
        game.pad_button(kPadLeftBumper);
        CHECK(game.config().pad.button_of(kPadFire) == kPadLeftBumper && game.config().pad.buttons[kPadLeftBumper] == kPadFire);
        press(game, 0x01, false, 0x1B);  // Escape closes the controller screen only
        CHECK(game.menu()->options() && !game.menu()->options()->gamepad() && game.menu()->options()->enhancements());
    }
    press(game, 0x01, false, 0x1B);  // then back to the first page
    CHECK(game.menu()->options() && game.menu()->options()->enhancements() &&
          game.menu()->options()->enhancements()->page() == EnhancementsScreen::Page::Main);
    press(game, 0x01, false, 0x1B);  // then the enhancements window
    CHECK(game.menu()->options() && !game.menu()->options()->enhancements());
    press(game, 0x01, false, 0x1B);
    CHECK(game.menu() && !game.menu()->options());
    const GameConfig with_pad = GameConfig::load(cfg);
    CHECK(with_pad.pad.buttons[kPadLeftBumper] == kPadFire && !with_pad.pad.is_default());
    std::filesystem::remove(cfg);
    CHECK(game.machine().faults().empty());
}

void test_fades(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    GameState& s = game.world().state;

    // The colour cycling runs once a game is loaded: entries 1..7 of the game
    // palette rotate, 8..14 change colour, and 1..14 go to the DAC.
    CHECK(!s.cycle_paused);
    const auto file = s.game_palette.to_vga();
    for (int i = 0; i < 31; ++i) game.update(kFrameUs);  // 15 ticks at 30 Hz
    auto cycled = s.game_palette.to_vga();
    int rotation = -1;
    for (int k = 0; k < 7 && rotation < 0; ++k) {
        bool same = true;
        for (int e = 1; e <= 7; ++e) {
            for (int c = 0; c < 3; ++c) same &= cycled[3 * e + c] == file[3 * ((e - 1 + k) % 7 + 1) + c];
        }
        if (same) rotation = k;
    }
    std::printf("fades: cycling rotated entries 1..7 by %d, entry 10 = (%d, %d, %d)\n", rotation, cycled[30],
                cycled[31], cycled[32]);
    CHECK(rotation > 0);
    CHECK(cycled[31] == 0 && cycled[32] == 0 && cycled[30] != 31);   // entry 10: red only, from 31
    CHECK(std::equal(cycled.begin() + 45, cycled.end(), file.begin() + 45));
    auto dac = s.screen_palette.to_vga();
    CHECK(std::equal(dac.begin() + 3, dac.begin() + 45, cycled.begin() + 3));

    // fadeToBlack(rate): the cycling pauses first and stays paused.
    const auto start = s.screen_palette.to_vga();
    int brightest = 0;
    for (auto c : start) brightest = std::max<int>(brightest, c);
    s.cycle_paused = true;
    const Pid fade = game.kernel().add(std::make_unique<PaletteFaderProcess>(
        s.screen_palette, Palette::filled(0, 0, 0), 60, 0, 1, &s.cycle_paused, false));
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    const auto after10 = s.screen_palette.to_vga();
    bool stepped = true;
    for (std::size_t i = 0; i < start.size(); ++i) stepped &= after10[i] == std::max(0, start[i] - 10);
    CHECK(stepped);
    for (int i = 0; i < brightest; ++i) game.update(kFrameUs);
    CHECK(s.screen_palette == Palette::filled(0, 0, 0));
    // Over (its pid may already serve another process).
    const Process* p = game.kernel().get(fade);
    CHECK((!dynamic_cast<const PaletteFaderProcess*>(p) || p->terminated()) && s.cycle_paused);

    // A count of 5 ends the fade after 4 writes, then its flag +0x41
    // (fadeToBlack(rate, count)) resumes the cycling, which writes 1..14.
    game.kernel().add(std::make_unique<PaletteFaderProcess>(s.screen_palette, Palette::filled(63, 63, 63), 60, 5, 1,
                                                            &s.cycle_paused, true));
    for (int i = 0; i < 20; ++i) game.update(kFrameUs);
    dac = s.screen_palette.to_vga();
    cycled = s.game_palette.to_vga();
    CHECK(!s.cycle_paused);
    CHECK(std::all_of(dac.begin() + 45, dac.end(), [](std::uint8_t c) { return c == 4; }));
    CHECK(std::equal(dac.begin() + 3, dac.begin() + 45, cycled.begin() + 3));

    // Rate 0 writes at once. fadeToGamePal's waiting process resumes the
    // cycling once the fade is over.
    game.kernel().add(std::make_unique<PaletteFaderProcess>(s.screen_palette, s.game_palette, 0, 0));
    CHECK(s.screen_palette == s.game_palette);
    s.cycle_paused = true;  // jumpToAllBlack
    s.screen_palette = Palette::filled(0, 0, 0);
    const Pid in = game.kernel().add(
        std::make_unique<PaletteFaderProcess>(s.screen_palette, s.game_palette, 60, 0, 1, &s.cycle_paused, false));
    const Pid wait = game.kernel().add(std::make_unique<FadeWaitProcess>(in, s.cycle_paused));
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    CHECK(s.cycle_paused && game.kernel().get(wait) && !game.kernel().get(wait)->terminated());
    for (int i = 0; i < 70; ++i) game.update(kFrameUs);
    CHECK(!s.cycle_paused && s.screen_palette == s.game_palette);

    // The menu opened during a fade that resumes the cycling when it fails:
    // the cycling stays paused, so the menu palette does not flicker.
    game.kernel().add(std::make_unique<PaletteFaderProcess>(s.screen_palette, Palette::filled(0, 0, 0), 60, 50, 1,
                                                            &s.cycle_paused, true));
    for (int i = 0; i < 4; ++i) game.update(kFrameUs);
    game.open_menu();
    const Palette menu = s.screen_palette;
    for (int i = 0; i < 31; ++i) game.update(kFrameUs);
    CHECK(game.menu() && s.cycle_paused && s.screen_palette == menu);
}

// Milestone 3: the credits from the menu.
void test_credits(const std::filesystem::path& data) {
    const auto cd = CreditsData::load(DataFiles(data));
    CHECK(cd.background.width == 640 && cd.background.height == 480 && cd.script.size() == 3601);

    Game game(data, test_saves(data));
    game.load_save(1);
    press(game, 0x01, false, 0x1B);  // menu
    press(game, 0x4D, true);         // right: options
    press(game, 0x50, true);         // down: credits
    CHECK(game.menu() && game.menu()->selected() == MainMenu::kCredits);
    press(game, 0x1C, false, 0x0D);
    game.update(kFrameUs);
    CHECK(!game.cursor().visible());

    // The first page fades in: "REALISATEUR" / "TONY ZUROVEC" at y 180.
    for (int i = 0; i < 30; ++i) game.update(kFrameUs);
    CHECK(game.audio()->music.current() == Music::kCredits);
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    game.render(screen);
    int changed = 0;
    for (int y = 170; y < 230; ++y)
        for (int x = 0; x < 640; ++x) changed += screen.row(y)[x] != cd.background.row(y)[x];
    CHECK(changed > 200);
    CHECK(game.palette() == cd.palette);

    // To the end (about 2 minutes), then back to the menu.
    int frames = 0;
    Credits* c = nullptr;
    const MainMenu* m = game.menu();
    for (; frames < 60 * 600; ++frames) {
        game.update(kFrameUs);
        if (!m) break;
        if (!const_cast<MainMenu*>(m)->credits()) break;
        c = const_cast<MainMenu*>(m)->credits();
    }
    std::printf("credits ran %d frames\n", frames);
    CHECK(c != nullptr && frames > 60 * 30 && frames < 60 * 600);
    CHECK(game.menu() && game.cursor().visible() && !(game.palette() == cd.palette));
    CHECK(game.audio()->music.current() == Music::kMenu);

    // A key stops them.
    press(game, 0x1C, false, 0x0D);
    game.update(kFrameUs);
    CHECK(const_cast<MainMenu*>(game.menu())->credits() != nullptr);
    press(game, 0x39, false, ' ');
    game.update(kFrameUs);
    CHECK(const_cast<MainMenu*>(game.menu())->credits() == nullptr);
    game.update(kFrameUs);
    CHECK(game.audio()->music.current() == Music::kMenu);
}

// Milestone 4: firing the shotgun of SAVEGAME.13 (shape 818, 1 round, 16 clips).
void settle(Game& game) {
    for (int i = 0; i < 600; ++i) {
        game.update(kFrameUs);
        if (!actor_busy(game.kernel(), 1) && !game.combat().shot_under_way()) return;
    }
}

std::uint16_t last_action(Game& game) {
    const auto r = game.world().npc_record(1);
    return static_cast<std::uint16_t>(r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8);
}

// The AutoFirer (1138:2FFB): weapon 909 (fire type 6) fires at 6 Hz while the
// fire key is held, through the firing frame of the shot action; the first
// firing frame seen once the key is released ends it.
void test_auto_firer(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    Item* weapon = w.get(w.active_weapon(1));
    CHECK(weapon != nullptr);
    if (!weapon) return;
    weapon->shape = AutoFirer::kWeapon;
    weapon->quality = 200;
    w.set_npc_word(1, 0x57, 1000);  // energy
    CHECK(dtable::weapon(weapon->shape).fire_type() == 6);
    settle(game);
    std::set<Pid> alive;
    int launched = 0;
    const auto count = [&] {
        std::set<Pid> now;
        game.kernel().for_each_process([&](const Process& p) {
            if (dynamic_cast<const ProjectileProcess*>(&p)) now.insert(p.pid());
        });
        for (const Pid pid : now) launched += !alive.count(pid);
        alive = std::move(now);
    };
    game.key(0x39, false, true, 0, ' ');
    bool started = false;
    for (int i = 0; i < 180; ++i) {
        game.update(kFrameUs);
        count();
        started |= w.state.auto_firer != 0;
    }
    const int held = launched;
    game.key(0x39, false, false, 0, 0);
    for (int i = 0; i < 180; ++i) {
        game.update(kFrameUs);
        count();
    }
    std::printf("auto firer: started %d, %d shots in 3 s held, %d after the release, running at the end %d\n", started,
                held, launched - held, w.state.auto_firer != 0);
    CHECK(started && w.state.auto_firer == 0);
    CHECK(held >= 14 && held <= 22);  // 6 Hz for 3 s, with the first shot of the action
    CHECK(launched - held <= 2);
}

// A 21:9 view (1720 x 720) shows bands beside the original 640 x 480 screen.
// A shot of the player wakes the NPCs on screen (1138:2DAF); an NPC in a band
// is drawn but stays out of the fight until it comes into the combat zone.
void test_combat_zone(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    w.set_view_size(1720, 720);
    settle(game);
    const World::ScreenRect zone = w.combat_rect();
    CHECK(zone.x0 == 540 && zone.x1 == 1179 && zone.y0 == 120 && zone.y1 == 599);
    const Item* player = w.get(1);
    const std::int32_t px = player->x, py = player->y, pz = player->z;
    // 450 pixels to the right of the player: in the band.
    w.move_item(6, px + 450, py - 450, pz);
    const bool drawn = w.is_partly_on_screen(*w.get(6)), fights = w.is_partly_in_combat_zone(*w.get(6));
    wake_npcs_on_screen(game.kernel(), w, game.animations());
    const std::uint16_t band_activity = w.npc_word(6, npc::kActivity);
    const bool band_combat = in_combat(w, 6);
    // 120 pixels away: in the combat zone.
    w.move_item(6, px - 120, py + 120, pz);
    wake_npcs_on_screen(game.kernel(), w, game.animations());
    std::printf("combat zone: band drawn %d, in zone %d, activity %u, combat %d; near activity %u, combat %d, "
                "target %u\n",
                drawn, fights, band_activity, band_combat, w.npc_word(6, npc::kActivity), in_combat(w, 6),
                w.npc_word(6, npc::kTarget));
    CHECK(drawn && !fights && band_activity == 0 && !band_combat);
    CHECK(w.npc_word(6, npc::kActivity) == 5 && in_combat(w, 6) && w.npc_word(6, npc::kTarget) == 1);
    // Without the original combat zone, the band fights too.
    w.set_original_combat_zone(false);
    w.move_item(6, px + 450, py - 450, pz);
    CHECK(w.is_partly_in_combat_zone(*w.get(6)));
}

// Port addition, the cheat "loot from remains" (death_loot.hpp, No Regret's
// GitHub issue #2): two humans of save 13 (NPCs 16 and 18) brought beside
// the player and burnt by plasma (fire type 6, shape 0x58F). NPCDEATH makes
// them no body and destroys what they carry. With the cheat, at the end of each death their weapons and what
// NPCDEATH's function 0x22 draws for the shape they had alive (into a box
// that is no body) drop on the floor beside them. What 0x22 draws is random:
// the test adds a clip to each box.
void test_remains_loot(const std::filesystem::path& data) {
    for (const bool cheat : {false, true}) {
        Game game(data, test_saves(data));
        game.config().remains_loot = cheat;
        game.load_save(13);
        game.update(kFrameUs);
        World& w = game.world();
        CHECK(w.remains_loot() == cheat);

        std::vector<std::uint16_t> called_with;  // the NPC's shape during each call
        std::vector<ObjId> dropped;              // the weapons carried, then what the boxes held
        std::vector<World::Point> remains;       // where the NPCs lay when their loot dropped
        WorldHooks hooks = w.hooks();
        hooks.call_class_event = [&w, call = hooks.call_class_event, &called_with, &dropped, &remains](
                                     std::string_view name, std::uint16_t event, ObjId npc,
                                     std::span<const std::uint8_t> args) {
            CHECK(name == "NPCDEATH" && event == 0x22 && args.size() == 2);
            const bool ok = call(name, event, npc, args);
            const Item* dead = w.get(npc);
            called_with.push_back(dead->shape);
            remains.push_back({dead->x, dead->y, dead->z});
            const auto box = static_cast<ObjId>(args[0] | args[1] << 8);
            if (w.create(0x55F, 0)) w.pop_to_container(box);
            for (ObjId id : w.get(box)->contents) dropped.push_back(id);
            return ok;
        };
        w.set_hooks(hooks);

        const Item* player = w.get(1);
        const std::int32_t px = player->x, py = player->y, pz = player->z;
        w.move_item(16, px + 120, py - 120, pz);
        w.move_item(18, px - 120, py - 120, pz);
        game.update(kFrameUs);
        std::vector<std::uint16_t> alive;
        std::vector<ObjId> burnt;
        for (const ObjId id : {16, 18}) {
            const Item* npc = w.get(id);
            if (!npc || w.is_dead(id) || !takes_full_bullet_damage(npc->shape)) continue;
            const std::uint16_t shape = npc->shape;
            std::vector<ObjId> weapons;
            for (ObjId c : npc->contents)
                if (w.family_of(w.get(c)->shape) == 10) weapons.push_back(c);
            apply_damage(game.kernel(), w, game.animations(), id, 1, 0, 250, 6, nullptr);
            npc = w.get(id);
            if (!npc || npc->shape != 0x58F) continue;
            alive.push_back(shape);
            burnt.push_back(id);
            if (cheat) dropped.insert(dropped.end(), weapons.begin(), weapons.end());
        }
        CHECK(burnt.size() == 2);
        CHECK(w.death_loot().size() == (cheat ? burnt.size() : 0));
        // The loot drops at the end of the death animations, beside the
        // NPCs where the fall left them.
        for (int i = 0; i < 600; ++i) game.update(kFrameUs);
        std::printf("remains loot (cheat %s): %zu burnt, %zu items dropped\n", cheat ? "on" : "off", burnt.size(),
                    dropped.size());
        std::ranges::sort(alive);
        std::ranges::sort(called_with);
        CHECK(called_with == (cheat ? alive : std::vector<std::uint16_t>{}));
        CHECK(dropped.size() >= (cheat ? burnt.size() : 0));
        for (ObjId id : dropped) {
            const Item* item = w.get(id);
            bool near = false;
            for (const World::Point& at : remains)
                near = near || (item && std::abs(item->x - at.x) <= 96 && std::abs(item->y - at.y) <= 96);
            CHECK(item && item->parent == 0 && !(item->flags & kItemEthereal) && near);
        }
        CHECK(w.death_loot().empty());
        CHECK(w.pending_count() == 0);
        CHECK(game.machine().faults().empty());
    }
}

void test_firing(const std::filesystem::path& data) {
    // Action remapping for the shotgun (record +0xC = 0, overlay 0x33B).
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    Item* weapon = w.get(w.active_weapon(1));
    CHECK(weapon && weapon->shape == 818);
    CHECK(remap_action(w, 1, 8) == 0x36 && remap_action(w, 1, 0x2A) == 0x32 && remap_action(w, 1, 7) == 0xC);
    CHECK(remap_action(w, 1, 1) == 1);
    const StatusBar& bar = game.status_bar();
    const Item* clips = w.get(bar.contents(w).ammo);
    CHECK(clips && clips->quality == 16 && weapon->quality == 1);

    settle(game);
    const std::uint16_t before = last_action(game);
    std::printf("firing: last action at start 0x%X\n", before);
    auto tap = [&] {
        press(game, 0x39, false, ' ');
        settle(game);
    };
    if (before == 0 || before == 1) {
        tap();  // raise the weapon: action 7 -> 0xC
        CHECK(last_action(game) == 0xC);
    }
    // Pump (action 0xA -> 0xF), with the shotgun's reload sound. The
    // conveyors of this map, started by their scripts (CONV_EL event 10,
    // Item::I_equip), keep the one-shot voices busy: the sound may be taken
    // over before the action ends, so it is checked meanwhile.
    const int reload_sfx = dtable::weapon(weapon->shape).reload_sfx();
    const int shot_sfx = dtable::weapon(weapon->shape).sound();
    std::printf("firing: shotgun sounds: shot 0x%X, reload 0x%X\n", shot_sfx, reload_sfx);
    press(game, 0x39, false, ' ');
    bool reload_heard = false;
    for (int i = 0; i < 600; ++i) {
        game.update(kFrameUs);
        reload_heard |= game.audio() && game.audio()->sources.is_playing(reload_sfx, 1);
        if (!actor_busy(game.kernel(), 1) && !game.combat().shot_under_way()) break;
    }
    CHECK(last_action(game) == 0xF && weapon->quality == 1 && game.shots_fired() == 0);
    CHECK(reload_heard);
    // NPC 6 (activity 0, default activity 2 = 5) stands on screen beside the
    // player, out of the line of fire: the shot wakes it (combat, target).
    const Item* player = w.get(1);
    w.move_item(6, player->x - 120, player->y + 120, player->z);
    CHECK(w.is_partly_on_screen(*w.get(6)) && !in_combat(w, 6) && w.npc_word(6, npc::kActivity) == 0);
    std::uint16_t rumble_low = 0;
    std::uint16_t rumble_high = 0;
    std::uint32_t rumble_duration = 0;
    int rumble_calls = 0;
    game.set_vibration_handler([&](std::uint16_t low, std::uint16_t high, std::uint32_t duration) {
        rumble_low = low;
        rumble_high = high;
        rumble_duration = duration;
        ++rumble_calls;
    });
    tap();  // shot (8 -> 0x36): one round
    CHECK(rumble_calls == 1 && rumble_low == 60000 && rumble_high == 48000 && rumble_duration == 130);
    std::printf("firing: NPC 6 after the shot: activity %u, in combat %d, target %u\n", w.npc_word(6, npc::kActivity),
                in_combat(w, 6), w.npc_word(6, npc::kTarget));
    CHECK(w.npc_word(6, npc::kActivity) == 5 && in_combat(w, 6) && w.npc_word(6, npc::kTarget) == 1);
    std::printf("firing: after the shot last action 0x%X, rounds %u, shot frames %d\n", last_action(game),
                weapon->quality, game.shots_fired());
    CHECK(last_action(game) == 0x36 && weapon->quality == 0 && game.shots_fired() >= 1);
    CHECK(game.audio() && game.audio()->sources.is_playing(shot_sfx, 1));
    tap();  // empty: reload from a clip (12 rounds), action 0xF
    CHECK(weapon->quality == 12 && clips->quality == 15 && last_action(game) == 0xF);
    CHECK(game.machine().faults().empty());
}

// The RP-22 and RP-32 are machine guns (fire type 1): a medium rumble a shot.
void test_machine_gun_vibration(const std::filesystem::path& data) {
    struct Expected {
        std::uint16_t shape;
        std::uint16_t low_frequency;
        std::uint16_t high_frequency;
        std::uint32_t duration_ms;
    };
    constexpr Expected profiles[] = {{0x333, 44000, 34000, 110}, {0x334, 44000, 34000, 110}};

    Game game(data, test_saves(data));
    game.load_save(13);
    settle(game);
    World& world = game.world();
    Item* weapon = world.get(world.active_weapon(1));
    CHECK(weapon != nullptr);
    if (!weapon) return;
    int calls = 0;
    std::uint16_t low_frequency = 0;
    std::uint16_t high_frequency = 0;
    std::uint32_t duration_ms = 0;
    game.set_vibration_handler([&](std::uint16_t low, std::uint16_t high, std::uint32_t duration) {
        ++calls;
        low_frequency = low;
        high_frequency = high;
        duration_ms = duration;
    });

    for (const Expected& profile : profiles) {
        weapon->shape = profile.shape;
        weapon->quality = 10;
        const int previous_calls = calls;
        const int previous_shots = game.shots_fired();
        press(game, 0x39, false, ' ');
        for (int frame = 0; frame < 180 && game.shots_fired() == previous_shots; ++frame) game.update(kFrameUs);
        CHECK(game.shots_fired() > previous_shots);
        CHECK(calls == previous_calls + 1);
        CHECK(low_frequency == profile.low_frequency && high_frequency == profile.high_frequency &&
              duration_ms == profile.duration_ms);
        settle(game);
    }
}

// Milestone 6.2: the inventory keys of World_on_key (player_commands.hpp).
// SAVEGAME.13: the trigger pad 3516 lies on the lift platform 3519 (ELEVPLAT)
// west of the player. Stepping on the platform sends the contact events
// (10E8:123A) to both; the pad (TRIGPAD) only reacts once the player stands
// completely on it, then its TRIGGER sends the lift up to z 88 (top 96).
// Walking off on the way up, the player falls into the shaft onto a death
// floor (DEATHFL3), whose event 6 kills him and plays the fall on a double.
// The player starts on the platform's northern edge (test_ledge_step).
// Stepping down while walking (10E8:162F), SAVEGAME.13: walking west from the
// start, the player steps down from the walkway (z 16) to z 8 on the floor
// 3074, whose strip under him is 4 units wide: only the north quarters of his
// footprint rest on it (support 9), the south ones on the floor 8 below, in
// the 9-unit slack of a walk. He walks on without tripping. The step down
// starts at the end of the step: from its start, it crossed the walkway and
// left him 8 above the strip, where he stumbled (action 0x3B) and slid into
// the lift shaft. The same made the player trip down stairs (TEST9) and off
// the joints of the gratings (TEST5).
void test_ledge_step(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    if (auto r = w.npc_record(1); !r.empty()) r[npc::kDirection] = 12;
    game.key(0x48, true, true, 0);
    bool on_strip = false, stumbled = false, fell = false;
    for (int f = 0; f < 120 && !w.is_dead(1); ++f) {
        game.update(kFrameUs);
        const Item& p = *w.get(1);
        if (p.z == 8 && p.y == 5675) on_strip = on_strip || support_mask(w, p.shape, p.x, p.y, p.z, false, 1) == 9;
        stumbled = stumbled || w.npc_word(1, npc::kLastAnim) == 0x3B;
        fell = fell || game.kernel().find_process(1, GravityProcess::kType) != nullptr;
    }
    game.key(0x48, true, false, 0);
    std::printf("ledge step: on the strip %d, stumbled %d, fell %d, dead %d\n", on_strip, stumbled, fell, w.is_dead(1));
    CHECK(on_strip && !stumbled && !fell && !w.is_dead(1));
}

// The same step down on the user's saves (docs/bugs.md): down the stairs of
// TEST9 (steps of 8, z 32 to 0) and east over the gratings of TEST5, whose
// joints stand 8 above the plates (z 88 / 96). The player walks down each
// step without falling or stumbling.
void test_walk_down_steps(const std::filesystem::path& data) {
    struct Case { const char* save; int dir; int frames; int end_z; };
    for (const Case c : {Case{"escalier-chute-en-descendant.sav", 4, 120, 0},
                         Case{"grilles-marche-entre-dalles.sav", 4, 140, -1}}) {
        Game game(data, test_saves(data));
        game.load_save_file(test_saves(data) / c.save);
        World& w = game.world();
        for (int i = 0; i < 10; ++i) game.update(kFrameUs);
        w.npc_record(1)[npc::kDirection] = static_cast<std::uint8_t>(c.dir);
        for (int i = 0; i < 10; ++i) game.update(kFrameUs);
        const int start_z = w.get(1)->z;
        game.key(0x48, true, true, 0);
        bool stumbled = false, fell = false;
        int downs = 0, last_z = start_z;
        for (int f = 0; f < c.frames; ++f) {
            game.update(kFrameUs);
            const int z = w.get(1)->z;
            if (z < last_z) ++downs;
            last_z = z;
            stumbled = stumbled || w.npc_word(1, npc::kLastAnim) == 0x3B;
            fell = fell || game.kernel().find_process(1, GravityProcess::kType) != nullptr;
        }
        game.key(0x48, true, false, 0);
        std::printf("walk down steps (%s): z %d -> %d, %d steps down, stumbled %d, fell %d\n", c.save, start_z,
                    w.get(1)->z, downs, stumbled, fell);
        CHECK(downs >= 2 && !stumbled && !fell);
        if (c.end_z >= 0) CHECK(w.get(1)->z == c.end_z);
    }
}

void test_floor_pad(const std::filesystem::path& data) {
    constexpr ObjId kPad = 3516, kLift = 3519;
    constexpr std::uint8_t kUp = 0x48;
    auto walk = [](Game& game, int dir, int frames) {
        if (auto r = game.world().npc_record(1); !r.empty()) r[npc::kDirection] = static_cast<std::uint8_t>(dir);
        game.key(kUp, true, true, 0);
        for (int f = 0; f < frames; ++f) game.update(kFrameUs);
        game.key(kUp, true, false, 0);
    };
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        CHECK(w.get(kPad) && w.get(kLift) && w.get(kLift)->z == 1);
        if (!w.get(kPad) || !w.get(kLift)) return;
        w.move_item(1, 5233, 5675, 9);  // on the platform, 28 units of 48 over it
        for (int f = 0; f < 30; ++f) game.update(kFrameUs);
        const Item& p = *w.get(1);
        std::printf("floor pad: player at (%d, %d, %d), pad frame %u\n", p.x, p.y, p.z, w.get(kPad)->frame);
        CHECK(p.z == 9 && w.get(kPad)->frame == 0);  // not completely on the pad
        for (int f = 0; f < 30; ++f) game.update(kFrameUs);
        walk(game, 8, 10);  // a step south, onto the pad
        CHECK(w.get(kPad)->frame == 1);
        for (int f = 0; f < 600; ++f) game.update(kFrameUs);
        std::printf("floor pad: lift at z %d, player at z %d\n", w.get(kLift)->z, w.get(1)->z);
        CHECK(w.get(kLift)->z == 88 && w.get(1)->z == 96 && !w.is_dead(1));
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
    {
        // Off the lift at the top: a fall of 96 units kills, and the death
        // shows the double (0x580). Walking off the rising lift used to get
        // there, but each walk cycle is tested before it starts (ANIMPRIM.C
        // 016F) and refused when it would end over the edge: whether a cycle
        // starts while the lift is still within 9 units of the floor next to
        // it depends on the frame, so the player is put past the edge, where
        // such a walk left it.
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        w.move_item(1, 5233, 5675, 9);
        for (int f = 0; f < 60; ++f) game.update(kFrameUs);
        walk(game, 8, 10);  // onto the pad
        for (int f = 0; f < 600 && w.get(kLift)->z != 88; ++f) game.update(kFrameUs);
        CHECK(w.get(kLift)->z == 88 && w.get(1)->z == 96);
        w.move_item(1, 5233, 5843, 96);
        fall(game.kernel(), w, 1);
        bool double_seen = false;
        for (int f = 0; f < 300; ++f) {
            game.update(kFrameUs);
            w.for_each([&](const Item& i) { double_seen = double_seen || i.shape == 0x580; });
        }
        std::printf("floor pad: fell, dead %d, double %d\n", w.is_dead(1), double_seen);
        CHECK(w.is_dead(1) && double_seen);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
}

void test_inventory_keys(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    // The running jump below holds Shift: the original behaviour, whatever
    // the player chose in CRUSADER.CFG ("togglerun", a port addition).
    game.config().toggle_run = false;
    World& w = game.world();
    settle(game);
    auto slot_shape = [&](int slot) {
        const Item* i = w.get(w.npc_equip(1, slot));
        return i ? i->shape : 0;
    };
    std::printf("inventory keys: weapons");
    for (int s = 1; s <= 5; ++s) std::printf(" %d:0x%X", s, slot_shape(s));
    std::printf(", items");
    for (int s = 11; s <= 25; ++s) {
        if (slot_shape(s)) std::printf(" %d:0x%X", s, slot_shape(s));
    }
    std::printf("; active weapon %u, item %u\n", w.npc_word(1, kNpcActiveWeapon), w.npc_word(1, kNpcActiveItem));

    // W then Q: the next weapon slot holding something, then back.
    std::vector<int> weapons;
    for (int s = 1; s <= 5; ++s) {
        if (slot_shape(s)) weapons.push_back(s);
    }
    const int weapon0 = w.npc_word(1, kNpcActiveWeapon);
    CHECK(weapons.size() >= 2 && weapon0 != 0);
    const auto at = std::find(weapons.begin(), weapons.end(), weapon0);
    const int next = at + 1 == weapons.end() ? weapons.front() : *(at + 1);
    press(game, 0x11, false, 'w');
    CHECK(w.npc_word(1, kNpcActiveWeapon) == next);
    press(game, 0x10, false, 'q');
    CHECK(w.npc_word(1, kNpcActiveWeapon) == weapon0);
    press(game, 0x37, false, '*');  // keypad *, through the Controller
    CHECK(w.npc_word(1, kNpcActiveWeapon) == next);
    press(game, 0x10, false, 'q');

    // O then I through the item slots.
    std::vector<int> items;
    for (int s = 11; s <= 24; ++s) {
        if (slot_shape(s)) items.push_back(s);
    }
    const int item0 = w.npc_word(1, kNpcActiveItem);
    if (items.size() >= 2 && item0 != 0) {
        const auto it = std::find(items.begin(), items.end(), item0);
        const int next_item = it == items.end() || it + 1 == items.end() ? items.front() : *(it + 1);
        press(game, 0x18, false, 'o');
        CHECK(w.npc_word(1, kNpcActiveItem) == next_item);
        press(game, 0x17, false, 'i');
        CHECK(w.npc_word(1, kNpcActiveItem) == item0);
        press(game, 0x4A, false, '-');  // keypad -
        CHECK(w.npc_word(1, kNpcActiveItem) == next_item);
        press(game, 0x17, false, 'i');
    }

    // M: a medikit (given if none is carried) heals 80 hit points and is
    // used up.
    ObjId medikit = 0;
    for (int s = 0; s < 30; ++s) {
        if (slot_shape(s) == 0x351) medikit = w.npc_equip(1, s);
    }
    if (!medikit) {
        medikit = w.create(0x351, 0);
        w.pop_to_coords(0, 0, 0);
        CHECK(add_item_cru(w, medikit, false));
    }
    const std::uint16_t count = w.get(medikit)->quality;
    w.set_npc_word(1, 0x00, 50);
    press(game, 0x32, false, 'm');
    for (int i = 0; i < 30; ++i) game.update(kFrameUs);
    std::printf("inventory keys: medikits %u, hit points 50 -> %u\n", count, w.npc_hp(1));
    CHECK(w.npc_hp(1) == 130);
    if (count > 1) CHECK(w.get(medikit) && w.get(medikit)->quality == count - 1);
    else CHECK(!w.get(medikit) && std::none_of(items.begin(), items.end(), [&](int s) { return w.npc_equip(1, s) == medikit; }));

    // Ctrl+D: the weapon goes to the player's feet, out of its slot; the
    // next weapon is active.
    const ObjId dropped = w.active_weapon(1);
    const int dropped_slot = w.npc_word(1, kNpcActiveWeapon);
    game.key(0x20, false, true, kCtrl, 4);
    game.key(0x20, false, false, kCtrl, 0);
    const Item* gun = w.get(dropped);
    const Item* player = w.get(1);
    CHECK(gun && gun->parent == 0 && !(gun->flags & (kItemEquipped | kItemContained)));
    CHECK(gun && gun->x == player->x && gun->y == player->y && gun->z == player->z);
    CHECK(w.npc_equip(1, dropped_slot) == 0 && w.npc_word(1, kNpcActiveWeapon) != dropped_slot);
    CHECK(std::find(player->contents.begin(), player->contents.end(), dropped) == player->contents.end());

    // In stasis nothing happens.
    w.state.avatar_in_stasis = true;
    const int weapon1 = w.npc_word(1, kNpcActiveWeapon);
    press(game, 0x11, false, 'w');
    CHECK(w.npc_word(1, kNpcActiveWeapon) == weapon1);
    w.state.avatar_in_stasis = false;

    // Tab: a jump (0x3A), after putting the weapon away when in a combat
    // stance; the player lands further on.
    for (int i = 0; i < 120 && actor_busy(game.kernel(), 1); ++i) game.update(kFrameUs);
    const Item* p = w.get(1);
    const int x0 = p->x, y0 = p->y;
    bool jumped = false;
    press(game, 0x0F, false, '\t');
    for (int i = 0; i < 240; ++i) {
        game.update(kFrameUs);
        jumped = jumped || last_action(game) == 0x3A;
    }
    const int moved = std::abs(p->x - x0) + std::abs(p->y - y0);
    std::printf("inventory keys: Tab jump %d, moved %d\n", jumped, moved);
    CHECK(jumped && moved >= 64);
    // J while running (Shift + up): the run gives way to a running jump (0x38).
    // Back the way the jump came: ahead of where it landed, the start of the
    // run (0x23) is blocked and, as in the original (ANIMPRIM.C 016F), the
    // player would stay standing.
    w.npc_record(1)[npc::kDirection] = static_cast<std::uint8_t>((w.npc_direction(1) + 8) & 0xF);
    game.key(0x48, true, true, kLeftShift);
    for (int i = 0; i < 90; ++i) game.update(kFrameUs);  // 0x22 / 0x23, then the run
    const std::uint16_t before = last_action(game);
    game.key(0x24, false, true, kLeftShift, 'J');
    game.key(0x24, false, false, kLeftShift, 0);
    bool running_jump = false;
    for (int i = 0; i < 20; ++i) {
        game.update(kFrameUs);
        running_jump = running_jump || last_action(game) == 0x38;
    }
    game.key(0x48, true, false, 0);
    for (int i = 0; i < 120; ++i) game.update(kFrameUs);
    std::printf("inventory keys: J after action 0x%X: running jump %d\n", before, running_jump);
    CHECK((before == 3 || before == 0x30 || before == 0x31) && running_jump);
    CHECK(game.machine().faults().empty());
    for (const auto& [n, calls] : game.machine().unbound_intrinsic_calls())
        std::printf("inventory keys: unbound intrinsic 0x%03X, %u calls\n", n, calls);
    CHECK(game.machine().unbound_intrinsic_calls().empty());
}

// Ctrl+D with a single weapon: the next weapon is the same slot, which the
// drop then empties; the weapon slot index stays. The Controller picks its
// run from the weapon itself (1138:12D6, Actor::getActiveWeapon): 3, not the
// armed run 0x30.
void test_run_after_last_weapon_dropped(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    game.config().toggle_run = false;
    World& w = game.world();
    settle(game);
    const int slot = w.npc_word(1, kNpcActiveWeapon);
    CHECK(slot != 0);
    for (int s = 1; s <= 5; ++s) {
        if (s != slot && w.npc_equip(1, s)) w.unequip(1, w.npc_equip(1, s));
    }
    game.key(0x20, false, true, kCtrl, 4);
    game.key(0x20, false, false, kCtrl, 0);
    for (int i = 0; i < 120 && actor_busy(game.kernel(), 1); ++i) game.update(kFrameUs);
    CHECK(w.npc_word(1, kNpcActiveWeapon) == slot && w.active_weapon(1) == 0);

    bool plain_run = false, armed_run = false;
    game.key(0x48, true, true, kLeftShift);
    for (int i = 0; i < 90; ++i) {
        game.update(kFrameUs);
        plain_run = plain_run || last_action(game) == 3;
        armed_run = armed_run || last_action(game) == 0x30;
    }
    game.key(0x48, true, false, 0);
    std::printf("run after the last weapon dropped: run 3 %d, run 0x30 %d\n", plain_run, armed_run);
    CHECK(plain_run && !armed_run);
}

// Running on the keypad 8, a turn to the 7 as the controller sends it (the 7
// down, then the 8 up). The original stops the run there (its release check
// skips the keypad diagonals), then the 7's repeat starts it again; with
// directional movement (AvatarMover::set_keypad_diagonals_held) the run goes on.
void test_run_turn_to_keypad_diagonal(const std::filesystem::path& data) {
    for (const bool directional : {false, true}) {
        Game game(data, test_saves(data));
        game.load_save(13);
        game.config().toggle_run = false;
        game.config().directional_move = directional;
        settle(game);
        bool running = false;
        game.key(0x48, false, true, kLeftShift);
        for (int i = 0; i < 60; ++i) {
            game.update(kFrameUs);
            const std::uint16_t a = last_action(game);
            running = running || a == 3 || a == 0x30 || a == 0x31;
        }
        game.key(0x47, false, true, kLeftShift);
        game.key(0x48, false, false, kLeftShift);
        bool stopped = false;
        for (int i = 0; i < 60; ++i) {
            game.update(kFrameUs);
            stopped = stopped || last_action(game) == 0x27 || last_action(game) == 0x19;  // the stop, armed
        }
        game.key(0x47, false, false, 0);
        std::printf("run turned to the keypad 7 (directional %d): running %d, stopped %d\n", directional, running,
                    stopped);
        CHECK(running);
        CHECK(stopped == !directional);
    }
}

// Issue #8: a run turned back on the keypad as the controller now sends it
// (the 2 down, then the 8 up: Gamepad::update_move) goes on the other way
// without its stop (0x27, armed 0x19). The release finds the 2 still down
// (1138:12CF), and the run's direction follows the 2.
void test_run_turn_back(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    game.config().toggle_run = false;
    game.config().directional_move = true;
    settle(game);
    game.key(0x48, false, true, kLeftShift);
    for (int i = 0; i < 60; ++i) game.update(kFrameUs);
    const std::int32_t y0 = game.world().get(1)->y;
    game.key(0x50, false, true, kLeftShift);
    game.key(0x48, false, false, kLeftShift);
    bool stopped = false, running = false;
    for (int i = 0; i < 60; ++i) {
        game.update(kFrameUs);
        const std::uint16_t a = last_action(game);
        stopped = stopped || a == 0x27 || a == 0x19;
        running = a == 3 || a == 0x30 || a == 0x31;
    }
    const int dir = game.world().npc_direction(1) & 0xF;
    const std::int32_t y1 = game.world().get(1)->y;
    game.key(0x50, false, false, 0);
    std::printf("run turned back: stopped %d, running %d, direction %d, y %d -> %d\n", stopped, running, dir, y0, y1);
    CHECK(!stopped && running && dir == 6);
    CHECK(game.machine().faults().empty());
}

// Milestone 6.2: the item selection mode (item_selection.hpp).
void test_item_selection(const std::filesystem::path& data) {
    {
        // SAVEGAME.1: one selectable item next to the player.
        Game game(data, test_saves(data));
        game.load_save(1);
        World& w = game.world();
        settle(game);
        press(game, 0x1F, false, 's');
        const auto& sel = w.state.selection;
        std::printf("selection (save 1): active %d,", sel.active);
        for (ObjId id : sel.items) std::printf(" %u:0x%X", id, w.get(id)->shape);
        std::printf("\n");
        CHECK(sel.active && sel.items.size() == 1 && selected_item(w) == sel.items[0]);
        press(game, 0x01, false, 0x1B);  // Escape ends the mode, the menu stays closed
        CHECK(!sel.active && !game.menu());
    }
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    settle(game);
    const auto& sel = w.state.selection;
    // Nothing near the player: the mode does not start.
    press(game, 0x1F, false, 's');
    CHECK(!sel.active && sel.items.empty());
    // Three items at 0x20, 0x40 and 0x90 (out of range) from the player's centre.
    const Item* player = w.get(1);
    auto drop = [&](std::uint16_t shape, int dx) {
        const ObjId id = w.create(shape, 0);
        w.pop_to_coords(player->x + dx, player->y, player->z);
        return id;
    };
    const ObjId far_item = drop(0x582, 0x40);
    const ObjId near_item = drop(0x351, 0x20);
    drop(0x351, 0x90);
    press(game, 0x1F, false, 's');
    CHECK(sel.active && sel.items.size() == 2 && selected_item(w) == near_item);
    press(game, 0x34, false, '.');
    CHECK(selected_item(w) == far_item);
    press(game, 0x34, false, '.');  // wraps
    CHECK(selected_item(w) == near_item);
    press(game, 0x33, false, ',');  // wraps backwards
    CHECK(selected_item(w) == far_item);
    // Enter picks the energy cube up.
    press(game, 0x1C, false, 0x0D);
    const Item* cube = w.get(far_item);
    CHECK(!sel.active && (!cube || cube->parent == 1));
    // An arrow ends the mode (the player turns, an animation: also ends it).
    press(game, 0x1F, false, 's');
    CHECK(sel.active);
    game.key(0x4B, true, true, 0);
    game.key(0x4B, true, false, 0);
    CHECK(!sel.active);
    // G picks everything in range, one per frame (the player carries 10
    // medikits, the most: down to 1 first).
    for (int s = 0; s < 30; ++s) {
        if (Item* i = w.get(w.npc_equip(1, s)); i && i->shape == 0x351) i->quality = 1;
    }
    const ObjId near2 = drop(0x351, 0x10);
    press(game, 0x22, false, 'g');
    CHECK(sel.active && sel.pick_all);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    const Item* m1 = w.get(near_item);
    const Item* m2 = w.get(near2);
    std::printf("selection: after G, active %d, medikits %d %d\n", sel.active, m1 ? static_cast<int>(m1->parent) : -1,
                m2 ? static_cast<int>(m2->parent) : -1);
    CHECK(!sel.active && (!m1 || m1->parent == 1) && (!m2 || m2->parent == 1));
    CHECK(game.machine().faults().empty());
}

// Milestone 6.2: R and the message box (message_gump.hpp).
void test_message_box(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    settle(game);
    const std::uint8_t level = game.kernel().level();
    CHECK(w.state.target_designation);  // DS:2889 is 1 at start-up
    press(game, 0x13, false, 'r');
    const MessageGump* box = game.message();
    CHECK(!w.state.target_designation && box && box->text() == "DESIGNATION DE CIBLE INACTIVE.");
    CHECK(game.kernel().level() == level + 1);
    if (!box) return;
    MessageGump::Rect r = box->box();
    MessageGump::Rect b = box->button();
    std::printf("message box: (%d, %d)-(%d, %d), button (%d, %d)-(%d, %d)\n", r.x0, r.y0, r.x1, r.y1, b.x0, b.y0, b.x1,
                b.y1);
    // Centred on the screen; the button centred across it, 4 pixels above its bottom.
    CHECK(r.x0 + ((r.x1 - r.x0 + 1) >> 1) == 320 && r.y0 + ((r.y1 - r.y0 + 1) >> 1) == 240);
    CHECK(b.y1 == r.y1 - 4 && b.x0 - r.x0 == ((r.x1 - r.x0 + 1) >> 1) - ((b.x1 - b.x0 + 1) >> 1));
    // The game is frozen: the player's position does not change.
    const Item* player = w.get(1);
    const auto x = player->x, y = player->y;
    game.key(0x48, true, true, 0);  // up arrow held: closes the box at once...
    game.key(0x48, true, false, 0);
    game.update(kFrameUs);
    CHECK(!game.message() && game.kernel().level() == level);
    // ...but a box opened again ignores the walk.
    press(game, 0x13, false, 'r');
    CHECK(w.state.target_designation && game.message() && game.message()->text() == "DESIGNATION DE CIBLE ACTIVE.");
    for (int i = 0; i < 30; ++i) game.update(kFrameUs);
    CHECK(player->x == x && player->y == y && game.message());
    if (!game.message()) return;
    r = game.message()->box();
    b = game.message()->button();
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    game.render(screen);
    // Outline 0, fill 0x14 inside the shade, light 0x16 along the bottom.
    CHECK(screen.row(r.y0)[r.x0] == 0 && screen.row(r.y1)[r.x1] == 0);
    CHECK(screen.row(r.y0 + 2)[r.x0 + 2] == MessageGump::kColour);
    CHECK(screen.row(r.y0 + 1)[r.x0 + 1] == MessageGump::kColour - 2);
    CHECK(screen.row(r.y1 - 1)[r.x0 + 2] == MessageGump::kColour + 2);
    CHECK(screen.row(b.y0 + 2)[b.x0 + 2] == MessageGump::kColour + 2);
    // The mouse: pressing then releasing outside "Ok" leaves it; inside closes it.
    game.mouse_button(true, b.x0 + 2, b.y0 + 2);
    game.mouse_button(false, r.x0 + 1, r.y0 + 1);
    CHECK(game.message());
    game.mouse_button(true, b.x0 + 2, b.y0 + 2);
    game.mouse_button(false, b.x0 + 3, b.y0 + 3);
    game.update(kFrameUs);
    CHECK(!game.message() && game.kernel().level() == level);
    // The flag outlives a load.
    press(game, 0x13, false, 'r');
    press(game, 0x1C, false, 0x0D);
    game.load_save(1);
    CHECK(!game.world().state.target_designation);
    CHECK(game.machine().faults().empty());
}

// The cheats (cheats.hpp): the typed code, F10, Ctrl+F10, ~, Ctrl+L.
void test_cheats(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    settle(game);
    const auto close_box = [&game] {
        press(game, 0x1C, false, 0x0D);
        game.update(kFrameUs);
    };
    const auto f10 = [&game](std::uint16_t flags) {
        game.key(0x44, false, true, flags, 0);
        game.key(0x44, false, false, flags, 0);
    };
    // Disabled: F10 and ~ do nothing.
    const std::uint16_t energy = game.world().npc_mana(1);
    game.world().set_npc_word(1, 0x57, 0);
    f10(0);
    press(game, 0x29, false, '~');
    CHECK(!game.message() && game.world().npc_mana(1) == 0 && !game.world().state.cheats.on);
    game.world().set_npc_word(1, 0x57, energy);
    // "JA" then "J": the code starts again at its second letter.
    const std::uint8_t typed[] = {0x24, 0x1E, 0x24, 0x1E, 0x1F, 0x1F, 0x17, 0x2E, 0x1E, 0x02, 0x07};
    const char ascii[] = "jajassica16";
    for (std::size_t i = 0; i < sizeof typed; ++i) {
        CHECK(!game.message());
        press(game, typed[i], false, static_cast<std::uint8_t>(ascii[i]));
    }
    const GameState::Cheats& c = game.world().state.cheats;
    CHECK(c.enabled && c.on && c.hack_mover_allowed && c.typed == 0);
    CHECK(game.message() && game.message()->text() == "Cheats are now active.");
    close_box();
    // The S of the code started the item selection (as in the original): Escape ends it.
    CHECK(game.world().state.selection.active);
    press(game, 0x01, false, 0x1B);
    CHECK(!game.world().state.selection.active && !game.menu());

    // F10: the player gets the weapons of the table, the stacks, the ammunition and full energy.
    World& w = game.world();
    std::vector<std::uint16_t> stacks_not_carried;
    for (const std::uint16_t shape : {std::uint16_t{0x560}, std::uint16_t{0x39A}, std::uint16_t{0x55F}, std::uint16_t{0x351}}) {
        if (carried(w, shape) == 0) stacks_not_carried.push_back(shape);
    }
    w.set_npc_word(1, 0x57, 0);
    f10(0);
    for (const int weapon : {0x386, 0x38A, 0x38E, 0x388, 0x334}) {
        CHECK(carried(w, static_cast<std::uint16_t>(weapon)) != 0 || free_equip_slot(w, 1, 5) == 0);
    }
    CHECK(w.npc_mana(1) == max_energy(w, 1) && w.npc_mana(1) != 0);
    for (const int shape : {0x560, 0x39A, 0x55F, 0x351, 0x33D, 0x33F, 0x340, 0x341, 0x33E}) {
        CHECK(carried(w, static_cast<std::uint16_t>(shape)) != 0);
    }
    const ObjId medikits = carried(w, 0x351);
    std::printf("cheats: F10 medikits %d, weapons", medikits ? static_cast<int>(w.get(medikits)->quality) : -1);
    for (int slot = 1; slot <= 5; ++slot) {
        const Item* item = w.get(w.npc_equip(1, slot));
        std::printf(" %03X", item ? item->shape : 0);
    }
    std::printf("\n");
    // Picked up until refused: 20 of 0x560, 10 of the others.
    CHECK(!stacks_not_carried.empty());
    for (const std::uint16_t shape : stacks_not_carried) {
        const ObjId stack = carried(w, shape);
        CHECK(stack && w.get(stack)->quality == (shape == 0x560 ? 20 : 10));
    }
    CHECK(!game.message());

    // Ctrl+F10: immortality on, then off.
    f10(kCtrl);
    CHECK((w.npc_record(1)[npc::kFlags13] & 0x02) && game.message() && game.message()->text() == "Immortality enabled.");
    close_box();
    f10(kCtrl);
    CHECK(!(w.npc_record(1)[npc::kFlags13] & 0x02) && game.message() &&
          game.message()->text() == "Immortality disabled.");
    close_box();

    // F10 brings a dead player back, with the hit points of his type and his shape 1.
    Item* player = w.get(1);
    w.set_broken(*player);
    w.npc_record(1)[npc::kFlags13] |= 0x01;
    w.set_npc_word(1, 0x00, 0);
    f10(0);
    CHECK(!w.is_dead(1) && !(player->flags & kItemBroken) && player->shape == 1 && w.npc_hp(1) != 0);
    std::printf("cheats: brought back with %d hit points\n", w.npc_hp(1));

    // Ctrl+L: the player's coordinates.
    char where[64];
    std::snprintf(where, sizeof where, "X: %u\nY: %u\nZ: %u", static_cast<unsigned>(player->x & 0xFFFF),
                  static_cast<unsigned>(player->y & 0xFFFF), static_cast<unsigned>(player->z & 0xFF));
    game.key(0x26, false, true, kCtrl, 0);
    game.key(0x26, false, false, kCtrl, 0);
    CHECK(game.message() && game.message()->text() == where);
    close_box();

    // ~ turns them off (F10 does nothing), and on again; they outlive a load.
    press(game, 0x29, false, '~');
    CHECK(!c.on && c.enabled && game.message() && game.message()->text() == "CHEATS OFF");
    close_box();
    w.set_npc_word(1, 0x57, 0);
    f10(0);
    CHECK(w.npc_mana(1) == 0);
    press(game, 0x29, false, '~');
    CHECK(c.on && game.message() && game.message()->text() == "CHEATS ON");
    close_box();
    game.load_save(1);
    CHECK(game.world().state.cheats.enabled && game.world().state.cheats.on);
    CHECK(game.machine().faults().empty());
}

// Milestone 6.2: the function keys of the game view (World_on_key).
void test_function_keys(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    settle(game);
    const std::uint8_t level = game.kernel().level();

    // F1: the help, over a cleared screen with HELP1.DAT's palette.
    press(game, 0x3B, false);
    const auto* help = dynamic_cast<const HelpScreen*>(game.screen());
    CHECK(help && help->page() == 0 && game.kernel().level() == level + 1);
    CHECK(!w.state.status_bar_visible && game.cursor().visible());
    if (!help) return;
    CHECK(game.palette() == *help->palette() && !(game.palette() == w.state.game_palette));
    press(game, 0x4D, true);  // right: page 1
    CHECK(game.screen() == help && help->page() == 1);
    press(game, 0x4B, true);  // left: page 0
    press(game, 0x4B, true);  // left again: stays on page 0
    CHECK(help->page() == 0);
    // The hidden page: e - - -; any key then goes back to page 0's number.
    press(game, 0x12, false, 'e');
    for (int i = 0; i < 3; ++i) press(game, 0x0C, false, '-');
    CHECK(help->page() == HelpScreen::kHiddenPage);
    press(game, 0x4D, true);  // right: 4 -> 0 -> 1
    CHECK(help->page() == 1);
    press(game, 0x31, false, 'n');  // next from page 1 closes
    CHECK(!game.screen() && game.kernel().level() == level);
    CHECK(w.state.status_bar_visible && !game.cursor().visible() && game.palette() == w.state.game_palette);
    // '?' opens it too; the buttons: "next", then "quit".
    press(game, 0x35, false, '?');
    help = dynamic_cast<const HelpScreen*>(game.screen());
    CHECK(help != nullptr);
    if (!help) return;
    const ShapeButton::Rect next = help->buttons()[0].rect;
    const ShapeButton::Rect quit = help->buttons()[1].rect;
    std::printf("help buttons: (%d, %d)-(%d, %d), (%d, %d)-(%d, %d)\n", next.x0, next.y0, next.x1, next.y1, quit.x0,
                quit.y0, quit.x1, quit.y1);
    auto click = [&](const ShapeButton& b) {
        // An opaque pixel of the button.
        for (int y = b.rect.y0; y <= b.rect.y1; ++y) {
            for (int x = b.rect.x0; x <= b.rect.x1; ++x) {
                if (b.contains(x, y)) {
                    game.mouse_button(true, x, y);
                    game.mouse_button(false, x, y);
                    return;
                }
            }
        }
    };
    click(help->buttons()[0]);
    CHECK(game.screen() == help && help->page() == 1);
    click(help->buttons()[1]);
    CHECK(!game.screen());

    // Alt+X: the question 0x15; 'n' leaves, 'o' quits.
    game.key(0x2D, false, true, kAlt, 'x');
    game.key(0x2D, false, false, kAlt, 0);
    const auto* ask = dynamic_cast<const ShapeDialog*>(game.screen());
    CHECK(ask && ask->shape() == ShapeDialog::kQuitQuestion);
    if (!ask) return;
    const ShapeButton::Rect d = ask->rect();
    std::printf("quit question: (%d, %d)-(%d, %d)\n", d.x0, d.y0, d.x1, d.y1);
    CHECK(ask->buttons().size() == 2 && ask->buttons()[0].rect.x0 == d.x0 + 57 && ask->buttons()[0].rect.y0 == d.y0 + 47);
    press(game, 0x31, false, 'n');
    CHECK(!game.screen() && !game.quit_requested() && w.state.status_bar_visible);
    game.key(0x2D, false, true, kAlt, 'x');
    press(game, 0x18, false, 'o');
    CHECK(!game.screen() && game.quit_requested());

    // F2: the options screen, Escape closes it.
    press(game, 0x3C, false);
    CHECK(game.screen() && !game.screen()->over_view());
    press(game, 0x01, false, 0x1B);
    CHECK(!game.screen() && game.palette() == w.state.game_palette);

    // F8: the load screen; Escape closes it, a slot chosen loads that game.
    // The colour cycling is paused meanwhile: MISC.PAL stays as it is.
    press(game, 0x42, false);
    CHECK(game.screen() && !game.screen()->over_view() && w.state.cycle_paused);
    const Palette load_palette = game.palette();
    for (int i = 0; i < 30; ++i) game.update(kFrameUs);
    CHECK(game.palette() == load_palette);
    press(game, 0x01, false, 0x1B);
    CHECK(!game.screen() && !w.state.cycle_paused);

    // In the item selection mode the function keys do nothing.
    press(game, 0x1F, false, 's');
    if (w.state.selection.active) {
        press(game, 0x3B, false);
        CHECK(!game.screen());
        press(game, 0x01, false, 0x1B);
    }

    // F4: load the quick save (SAVEGAME.13), then the notice.
    press(game, 0x3E, false);
    ask = dynamic_cast<const ShapeDialog*>(game.screen());
    CHECK(ask && ask->shape() == ShapeDialog::kQuickLoadQuestion);
    // Its buttons are not where the quit question has them (13E8:1ACD).
    if (ask) {
        const ShapeButton::Rect q = ask->rect();
        CHECK(ask->buttons().size() == 2 && ask->buttons()[0].rect.x0 == q.x0 + 49 &&
              ask->buttons()[1].rect.x0 == q.x0 + 121);
    }
    press(game, 0x15, false, 'y');
    CHECK(!game.screen());
    game.update(kFrameUs);
    ask = dynamic_cast<const ShapeDialog*>(game.screen());
    CHECK(ask && ask->shape() == ShapeDialog::kQuickLoadNotice && !game.world().state.status_bar_visible);
    press(game, 0x1C, false, 0x0D);
    CHECK(!game.screen() && game.world().state.status_bar_visible);
    CHECK(game.machine().faults().empty());
}

// Milestone 5: the healing and energy stations (CruHealer, BatteryCharger),
// as their scripts start them, with their sounds.
// Milestone 6.2: the mouse in play (world_mouse.hpp).
void test_mouse_in_play(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    settle(game);
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) game.update(kFrameUs);
    };
    const Item* player = w.get(1);
    CHECK(game.mouse_in_play() && !game.cursor().visible());
    GameConfig config = game.config();
    config.mouse_speed = 2;  // the default (data/CRUSADER.CFG has 3)
    game.set_config(config);

    // The item selection: motion does not put the mouse back; a left press
    // takes the selected item.
    press(game, 0x1F, false, 's');
    CHECK(w.state.selection.active);
    game.mouse_move(100, 100);
    run(1);
    CHECK(game.cursor().x() == 100);
    game.mouse_button(1, true, 100, 100);
    game.mouse_button(1, false, 100, 100);
    run(MouseQueue::kClickDelay + 1);
    CHECK(!w.state.selection.active && !game.world_mouse().firing());
    settle(game);

    // Motion: the mouse goes back to (320, 200); 20 pixels to the left at
    // speed 2, out of a combat stance, give a rate of 20 >> 3 = 2: a left
    // arrow, the player turns left.
    // Standing (save 1 ends on a shot, 0x8: a combat stance for 1138:0070).
    do_anim(game.kernel(), w, game.animations(), 1, 0, kDirCurrent);
    game.mouse_move(320, 200);
    run(1);
    settle(game);
    CHECK(!stance_not_moving(w, 1));
    const int dir = w.npc_direction(1) & 0xF;
    game.mouse_move(318, 200);  // rate 2 >> 3 = 0: nothing
    run(1);
    CHECK(game.cursor().x() == 320 && game.cursor().y() == 200);
    settle(game);
    CHECK((w.npc_direction(1) & 0xF) == dir);
    game.mouse_move(300, 200);
    run(1);
    CHECK(game.cursor().x() == 320 && !w.state.mouse_turn.active && w.state.mouse_turn.rate == 2);
    settle(game);
    const int turned = w.npc_direction(1) & 0xF;
    std::printf("mouse: direction %d -> %d\n", dir, turned);
    CHECK(turned == ((dir - 1) & 0xF) || turned == ((dir - 2) & 0xF));
    // To the right: back (the way ahead is clear there).
    game.mouse_move(340, 200);
    run(1);
    settle(game);
    CHECK((w.npc_direction(1) & 0xF) == dir);

    // Right button: after the double-click delay, the player walks forward
    // until the release.
    const std::int32_t x0 = player->x, y0 = player->y;
    game.mouse_button(2, true, 320, 200);
    run(MouseQueue::kClickDelay - 1);
    CHECK(!game.world_mouse().walking());
    run(2);
    CHECK(game.world_mouse().walking());
    run(60);
    std::printf("mouse: walked from (%d, %d) to (%d, %d), last action 0x%X\n", x0, y0, player->x, player->y,
                last_action(game));
    CHECK(player->x != x0 || player->y != y0);
    game.mouse_button(2, false, 320, 200);
    run(1);
    CHECK(!game.world_mouse().walking());
    settle(game);
    CHECK(last_action(game) == 0);
    const std::int32_t x1 = player->x, y1 = player->y;
    run(30);
    CHECK(player->x == x1 && player->y == y1);

    // A double click of the right button draws the weapon (7); in the
    // combat stance, another one holsters it (0xB).
    auto double_click = [&](int button) {
        game.mouse_button(button, true, 320, 200);
        game.mouse_button(button, false, 320, 200);
        run(3);
        game.mouse_button(button, true, 320, 200);
        run(1);
        game.mouse_button(button, false, 320, 200);
        run(1);
    };
    double_click(2);
    settle(game);
    const std::uint16_t drawn = last_action(game);
    std::printf("mouse: after a double click, last action 0x%X\n", drawn);
    CHECK(stance_not_moving(w, 1) && !game.world_mouse().walking());
    // In the combat stance, 40 pixels to the left: rate 40 >> 1 = 20, the
    // crosshair of the aiming mode turns by 10 degrees.
    const int angle = w.state.aim.angle;
    game.mouse_move(280, 200);
    run(1);
    std::printf("mouse: aiming mode %d, angle %d -> %d\n", w.state.aim.active, angle, w.state.aim.angle);
    CHECK(w.state.aim.active && w.state.mouse_turn.rate == 20 && w.state.aim.angle == angle - 10);
    settle(game);

    // Left button, in the combat stance: Space, a shot.
    const int shots = game.shots_fired();
    game.mouse_button(1, true, 320, 200);
    run(MouseQueue::kClickDelay + 1);
    CHECK(game.world_mouse().firing());
    run(60);
    game.mouse_button(1, false, 320, 200);
    run(1);
    CHECK(!game.world_mouse().firing());
    settle(game);
    std::printf("mouse: shots %d -> %d\n", shots, game.shots_fired());
    CHECK(game.shots_fired() > shots);

    double_click(2);
    settle(game);
    std::printf("mouse: after a second double click, last action 0x%X\n", last_action(game));
    CHECK(last_action(game) == 0xB || last_action(game) == 0x10);


    // The menu takes the mouse back.
    press(game, 0x01, false, 0x1B);
    CHECK(game.menu() && !game.mouse_in_play());
    CHECK(game.machine().faults().empty());
}

// Issue #22: the hack mover (H with the cheats, world_mouse.hpp): the mouse
// moves a cursor and the left button drags the item under it. Played on a
// 640 x 480 view, then on a larger one (native resolution), where the mouse's
// window is centred in the view.
void test_hack_mover(const std::filesystem::path& data) {
    for (const auto [width, height] : {std::pair{640, 480}, std::pair{1280, 800}}) {
        Game game(data, test_saves(data));
        game.load_save(1);
        World& w = game.world();
        settle(game);
        auto run = [&](int frames) {
            for (int i = 0; i < frames; ++i) game.update(kFrameUs);
        };
        Surface8 screen(width, height);
        game.render(screen);
        const auto close_box = [&] {
            press(game, 0x1C, false, 0x0D);
            run(1);
        };
        GameState::Cheats& c = w.state.cheats;

        // Without the typed code (-laurie only), H does nothing.
        c.enabled = c.on = true;
        c.hack_mover_allowed = false;
        press(game, 0x23, false, 'h');
        CHECK(!c.hack_mover && !game.message());
        c.hack_mover_allowed = true;
        press(game, 0x23, false, 'h');
        CHECK(c.hack_mover && game.message() && game.message()->text() == "HACK MOVER ON");
        close_box();
        CHECK(game.cursor().visible() && game.cursor().frame() == MouseCursor::kArrow && game.mouse_in_play());

        // An item that falls (not fixed, with a weight), not a stack nor an
        // NPC, drawn on top at a point of the mouse's window.
        const GameState& s = w.state;
        const auto list = build_display_list(w, game.shapes(), View{s.view_left, s.view_top}, s.screen_right + 1,
                                             s.screen_bottom + 1);
        ObjId id = 0;
        int mx = 0, my = 0;
        for (std::size_t i = list.size(); i-- > 0 && id == 0;) {
            const Item& item = *list[i].item;
            const ShapeInfo* info = w.shape_info(item.shape);
            if (item.id < 256 || !info || info->is(ShapeInfo::kFixed) || info->weight == 0 || info->family == 2 ||
                info->family == 9) {
                continue;
            }
            for (int y = list[i].top; y < list[i].bottom && id == 0; ++y) {
                for (int x = list[i].left; x < list[i].right && id == 0; ++x) {
                    const int cx = x - game.ui_x(), cy = y - game.ui_y();
                    if (cx < 60 || cx > 580 || cy < 60 || cy > 380) continue;
                    const auto hits = pick(list, x, y);
                    if (!hits.empty() && hits.back() == i) {
                        id = item.id;
                        mx = cx;
                        my = cy;
                    }
                }
            }
        }
        CHECK(id != 0);
        if (id == 0) return;
        const Item* item = w.get(id);
        std::printf("hack mover %dx%d: item %d shape 0x%X at (%d, %d, %d), mouse (%d, %d)\n", width, height, id,
                    item->shape, item->x, item->y, item->z, mx, my);

        // The cursor follows the mouse, the player does not turn.
        const int dir = w.npc_direction(1);
        game.mouse_move(mx, my);
        run(1);
        CHECK(game.cursor().x() == mx && game.cursor().y() == my && w.npc_direction(1) == dir);

        // The press takes it (after the double-click delay), without firing.
        const int shots = game.shots_fired();
        game.mouse_button(1, true, mx, my);
        run(MouseQueue::kClickDelay + 1);
        CHECK(game.world_mouse().dragging() && game.world_mouse().held() == id && !game.world_mouse().firing());
        // Each move of (dx, dy) on the screen moves it by (dx + 2 dy, 2 dy -
        // dx) on the map, when the place is free at the same height (within a
        // unit: over an item, the point goes down its front face, 1188:049E).
        int moves = 0;
        for (const auto [dx, dy] : {std::pair{16, 8}, std::pair{-16, 8}, std::pair{16, -8}, std::pair{-16, -8}}) {
            const std::int32_t x0 = item->x, y0 = item->y, z0 = item->z;
            mx += dx;
            my += dy;
            game.mouse_move(mx, my);
            run(1);
            CHECK(game.cursor().x() == mx && game.cursor().y() == my && w.npc_direction(1) == dir);
            std::printf("hack mover: mouse (%d, %d), item (%d, %d, %d) -> (%d, %d, %d)\n", mx, my, x0, y0, z0, item->x,
                        item->y, item->z);
            if (item->x == x0 && item->y == y0 && item->z == z0) continue;
            ++moves;
            if (item->z == z0) {
                CHECK(std::abs(item->x - x0 - (dx + 2 * dy)) <= 1 && std::abs(item->y - y0 - (2 * dy - dx)) <= 1);
            }
        }
        CHECK(moves > 0);
        CHECK(w.get(id) == item && item->parent == 0 && !(item->flags & kItemEthereal) && w.cells_consistent());

        // The release ends it.
        game.mouse_button(1, false, mx, my);
        run(1);
        CHECK(!game.world_mouse().dragging() && game.world_mouse().held() == 0);
        run(60);
        CHECK(game.shots_fired() == shots && w.pending_count() == 0);

        // H again: off, the cursor hidden; the mouse turns the player again.
        press(game, 0x23, false, 'h');
        CHECK(!c.hack_mover && game.message() && game.message()->text() == "HACK MOVER OFF");
        close_box();
        CHECK(!game.cursor().visible());
        game.mouse_move(mx, my);
        run(1);
        CHECK(game.cursor().x() == WorldMouse::kCentreX && game.cursor().y() == WorldMouse::kCentreY);

        // On again, the menu turns it off (13E8:0EF9).
        press(game, 0x23, false, 'h');
        close_box();
        CHECK(c.hack_mover);
        press(game, 0x01, false, 0x1B);
        CHECK(game.menu());
        press(game, 0x01, false, 0x1B);
        run(1);
        CHECK(!game.menu() && !w.state.cheats.hack_mover && !game.cursor().visible());
        CHECK(game.machine().faults().empty());
    }
}

// Using scenery on SAVEGAME.1: a chest opens and hands over the items of its
// content markers (0x476), a dead body is searched once.
void test_scenery_use(const std::filesystem::path& data) {
    const auto carried = [](World& w, std::uint16_t shape) -> const Item* {
        const Item* found = nullptr;
        w.for_each([&](const Item& i) {
            if (i.parent == 1 && i.shape == shape) found = &i;
        });
        return found;
    };
    // The first entry of the pickup list, and how many are used.
    const auto pickup = [](World& w) {
        int used = 0;
        for (const PickupEntry& e : w.state.pickups.entries()) used += e.name != nullptr;
        const PickupEntry& first = w.state.pickups.entries()[0];
        std::printf("scenery: pickups %d, first \"%s\" x%d (gump %u:%u)\n", used, first.name ? first.name : "",
                    first.count, first.icon_shape, first.icon_frame);
        return std::pair<int, PickupEntry>{used, first};
    };
    const auto use = [](Game& game, ObjId item) {
        use_item(game.world(), item);
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
    };
    {
        // CHEST_NS 3169, marker 3156 (map 0x3E, npc 0x60): a new 0x33E.
        Game game(data, test_saves(data));
        game.load_save(1);
        World& w = game.world();
        CHECK(!carried(w, 0x33E) && w.get(3156));
        use(game, 3169);
        const Item* chest = w.get(3169);
        std::printf("scenery: chest 3169 -> %04X:%u, marker %s, carried 0x33E %d\n", chest->shape, chest->frame,
                    w.get(3156) ? "kept" : "gone", carried(w, 0x33E) != nullptr);
        CHECK(chest->shape == 0x54A && chest->frame == 4 && !w.get(3156) && carried(w, 0x33E));
        const auto [used, first] = pickup(w);
        CHECK(used == 1 && std::string(first.name ? first.name : "") == "CHARGEUR SG-A1" && first.count == 1 &&
              first.icon_shape == 5 && first.icon_frame == 10);
        // Gone after 3 to 4 seconds.
        for (int f = 0; f < 4 * 60; ++f) game.update(kFrameUs);
        CHECK(pickup(w).first == 0);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
    {
        // CHEST_EW 3160, two markers of 0x33D: combined with the ammunition
        // the player carries.
        Game game(data, test_saves(data));
        game.load_save(1);
        World& w = game.world();
        const std::uint16_t ammo = carried(w, 0x33D)->quality;
        use(game, 3160);
        std::printf("scenery: chest 3160 -> %04X:%u, ammunition 0x33D %u -> %u\n", w.get(3160)->shape,
                    w.get(3160)->frame, ammo, carried(w, 0x33D)->quality);
        CHECK(w.get(3160)->shape == 0x54B && !w.get(3158) && !w.get(3159) && carried(w, 0x33D)->quality == ammo + 2);
        // Both in one entry.
        const auto [used, first] = pickup(w);
        CHECK(used == 1 && std::string(first.name ? first.name : "") == "CHARGEUR BA-41" && first.count == 2);
        // Opened: using it again does nothing.
        use(game, 3160);
        CHECK(carried(w, 0x33D)->quality == ammo + 2);
    }
    {
        // D_MAINT 8389 holds credits (0x4ED 8388): searched once.
        Game game(data, test_saves(data));
        game.load_save(1);
        World& w = game.world();
        const std::uint16_t credits = carried(w, 0x4ED)->quality;
        use(game, 8389);
        const std::uint16_t after = carried(w, 0x4ED)->quality;
        std::printf("scenery: dead body 8389: credits %u -> %u, status %04X\n", credits, after, w.get(8389)->flags);
        CHECK(!w.get(8388) && after > credits && (w.get(8389)->flags & 0x1000));
        // Credits count their quality.
        const auto [used, first] = pickup(w);
        CHECK(used == 1 && std::string(first.name ? first.name : "") == "CREDITS" && first.count == after - credits);
        CHECK(game.machine().unbound_intrinsic_calls().empty());
    }
}

// The computers: ComputerGump::I_readComputer (0x0FE) from a monitor's script.
void test_computer(const std::filesystem::path& data) {
    // 13A0:03C8: a space becomes a break at a character with 20 or more
    // counted since the last one.
    CHECK(ComputerGump::wrap("abcdefghij klmnopqrstu vw") == "abcdefghij^klmnopqrstu vw");
    CHECK(ComputerGump::wrap("abc^defghijklmnopqrstuvw x") == "abc^defghijklmnopqrstuvw^x");
    CHECK(ComputerGump::wrap("abcdefghijklmnopqrstuvwxyz") == "abcdefghijklmnopqrstuvwxyz");
    // MONITEW 5449 (save 2): a message through TEXTFILE, its code from
    // numToStr (0x0FF).
    Game game(data, test_saves(data));
    game.load_save(2);
    World& w = game.world();
    use_item(w, 5449);
    for (int f = 0; f < 3; ++f) game.update(kFrameUs);
    const auto* computer = dynamic_cast<const ComputerGump*>(game.screen());
    CHECK(computer != nullptr);
    if (!computer) return;
    std::printf("computer: \"%s\"\n", computer->text().c_str());
    CHECK(computer->text().rfind("De : DBEYER^", 0) == 0 &&
          computer->text().find("^962 est le code^") != std::string::npos);
    CHECK(!w.state.status_bar_visible && !game.cursor().visible());
    // One character per tick.
    const int shown = computer->shown();
    for (int f = 0; f < 10; ++f) game.update(kFrameUs);
    std::printf("computer: %d characters shown, then %d\n", shown, computer->shown());
    CHECK(computer->shown() > shown && computer->shown() <= shown + 10);
    // Escape shows the rest of the page; then a key per page, and Escape or
    // Enter closes once the text has ended.
    const auto escape = [&game] {
        game.key(0x01, false, true, 0, 0x1B);
        game.key(0x01, false, false, 0, 0x1B);
        game.update(kFrameUs);
    };
    int pages = 0;
    for (int i = 0; i < 20 && game.screen(); ++i) {
        escape();
        if (const auto* c = dynamic_cast<const ComputerGump*>(game.screen())) {
            CHECK(c->page_full() || c->ended());
            pages += c->page_full();
            if (c->ended()) {
                escape();
                break;
            }
            escape();  // the next page
        }
    }
    std::printf("computer: %d full pages, closed %d\n", pages, game.screen() == nullptr);
    CHECK(!game.screen() && w.state.status_bar_visible);
    for (int f = 0; f < 60; ++f) game.update(kFrameUs);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

void test_keypad(const std::filesystem::path& data) {
    // KEYPAD 5459 (save 2): the code of its VALBOX, 962, the one given by
    // monitor 5449.
    constexpr ObjId kKeypad = 5459;
    const auto open = [](Game& game) -> const KeypadGump* {
        use_item(game.world(), kKeypad);
        for (int f = 0; f < 3; ++f) game.update(kFrameUs);
        return dynamic_cast<const KeypadGump*>(game.screen());
    };
    const auto press = [](Game& game, std::uint8_t scancode, std::uint8_t ascii) {
        game.key(scancode, false, true, 0, ascii);
        game.key(scancode, false, false, 0, ascii);
        game.update(kFrameUs);
    };
    const auto press_arrow = [](Game& game, std::uint8_t scancode) {
        game.key(scancode, true, true, 0, 0);
        game.update(kFrameUs);
        game.key(scancode, true, false, 0, 0);
        game.update(kFrameUs);
    };
    const auto flags = [](Game& game) { return game.world().get(kKeypad)->flags; };
    const auto clean = [](Game& game) {
        return game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty();
    };

    // Escape: -1, the script only clears its busy flag (0x1000).
    {
        Game game(data, test_saves(data));
        game.load_save(2);
        const KeypadGump* keypad = open(game);
        CHECK(keypad != nullptr);
        if (!keypad) return;
        std::printf("keypad: code %d at (%d, %d)\n", keypad->code(), keypad->x(), keypad->y());
        CHECK(keypad->code() == 962 && game.cursor().visible() && !game.world().state.status_bar_visible);
        CHECK((flags(game) & 0x1000) != 0);
        press(game, 0x0A, '9');
        press(game, 0x02, '!');  // shifted 1
        CHECK(keypad->digit_count() == 2);
        press(game, 0x0E, 0x08);
        CHECK(keypad->digit_count() == 1);
        press(game, 0x01, 0x1B);
        for (int f = 0; f < 30; ++f) game.update(kFrameUs);
        std::printf("keypad: escape, closed %d, flags %04X\n", game.screen() == nullptr, flags(game));
        CHECK(!game.screen() && (flags(game) & 0x1004) == 0 && game.world().state.status_bar_visible);
        CHECK(clean(game));
    }
    // The buttons: 1 then enter, the wrong code.
    {
        Game game(data, test_saves(data));
        game.load_save(2);
        const KeypadGump* keypad = open(game);
        CHECK(keypad != nullptr);
        if (!keypad) return;
        const auto click = [&game, keypad](int button) {
            const KeypadGump::Rect& r = KeypadGump::kButtonRects[static_cast<std::size_t>(button)];
            const int x = keypad->x() + (r.x0 + r.x1) / 2;
            const int y = keypad->y() + (r.y0 + r.y1) / 2;
            game.mouse_button(true, x, y);
            game.update(kFrameUs);
            CHECK(game.screen() == keypad && keypad->pressed(button));
            game.mouse_button(false, x, y);
            game.update(kFrameUs);
        };
        click(0);
        CHECK(keypad->digit_count() == 1 && !keypad->pressed(0));
        CHECK(game.audio() && game.audio()->voices.handle_playing(KeypadGump::kSoundButton) != SoundManager::kNoHandle);
        click(KeypadGump::kEnter);
        CHECK(!game.screen());
        CHECK(game.audio() && game.audio()->voices.handle_playing(KeypadGump::kSoundWrong) != SoundManager::kNoHandle);
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        std::printf("keypad: wrong code, flags %04X\n", flags(game));
        CHECK((flags(game) & 0x1004) == 0);
        CHECK(clean(game));
    }
    // The keys: 962 then Enter, the door's code.
    {
        Game game(data, test_saves(data));
        game.load_save(2);
        CHECK(open(game) != nullptr);
        press(game, 0x0A, '9');
        press(game, 0x07, '6');
        press(game, 0x03, '2');
        press(game, 0x1C, 0x0D);
        CHECK(!game.screen());
        CHECK(game.audio() && game.audio()->voices.handle_playing(KeypadGump::kSoundRight) != SoundManager::kNoHandle);
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        std::printf("keypad: right code, flags %04X\n", flags(game));
        CHECK((flags(game) & 0x1000) == 0 && (flags(game) & 0x0004) != 0);
        CHECK(clean(game));
    }
    // Arrows navigate the on-screen keys; Enter activates the selected key.
    {
        Game game(data, test_saves(data));
        game.load_save(2);
        const KeypadGump* keypad = open(game);
        CHECK(keypad != nullptr);
        if (!keypad) return;
        press_arrow(game, 0x4D);  // right: select 2
        press(game, 0x1C, 0x0D);
        CHECK(game.screen() == keypad && keypad->digit_count() == 1);
        press_arrow(game, 0x50);  // down to 5, 8, 0
        press_arrow(game, 0x50);
        press_arrow(game, 0x50);
        press_arrow(game, 0x4D);  // select Enter
        press(game, 0x1C, 0x0D);
        CHECK(game.screen() == nullptr);
        CHECK(game.audio() && game.audio()->voices.handle_playing(KeypadGump::kSoundWrong) != SoundManager::kNoHandle);
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        CHECK((flags(game) & 0x1004) == 0);
        CHECK(clean(game));
    }
}

// Issue #18: the reporter's autosave (English 1.01, map 7), made just after
// keypad 5235's code. TRIGGER slides door 618 (RDOOR_N, 96 units towards -x)
// while holding its lock, status 0x1000; the autosave caught the lock before
// the slide, and with no process to release it once loaded, the keypad's
// TRIGGER waited for it forever: the door stayed shut. Once loaded the lock
// is gone and the keypad opens the door. And the autosave after a code now
// waits until the door is still.
void test_keypad_door_after_reload(const std::filesystem::path& data) {
    constexpr ObjId kKeypad = 5235, kDoor = 618;
    constexpr std::int32_t kShutX = 6959, kOpenX = 6863;
    const auto save = test_saves(data) / "issue18-digicode-porte-fermee-SAVEGAME.12.sav";
    {
        Game game(data, test_saves(data));
        game.load_save_file(save);
        World& w = game.world();
        CHECK(w.get(kDoor)->x == kShutX && !(w.get(kDoor)->flags & 0x1000) && (w.get(kKeypad)->flags & 0x0004));
        int linked_locked = 0;
        w.for_each([&](const Item& i) {
            linked_locked += !i.parent && (i.flags & 0x1000) && (i.quality & 0xFF) == 0x0B;
        });
        CHECK(linked_locked == 0);
        use_item(w, kKeypad);
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        std::printf("keypad door after reload: door x %d, flags %04X\n", w.get(kDoor)->x, w.get(kDoor)->flags);
        CHECK(w.get(kDoor)->x == kOpenX && !(w.get(kDoor)->flags & 0x1000));
        CHECK(game.machine().faults().empty());
    }
    // The code typed again (the keypad's "done" bit 4 cleared): the autosave
    // comes once the door has slid, its lock released.
    const auto dir = std::filesystem::temp_directory_path() / "crusader_keypad_door_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        GameConfig config = game.config();
        config.autosave = true;
        game.set_config(config);
        game.load_save_file(save);
        World& w = game.world();
        w.get(kKeypad)->flags &= static_cast<std::uint16_t>(~0x0004);
        use_item(w, kKeypad);
        for (int f = 0; f < 3; ++f) game.update(kFrameUs);
        const auto* keypad = dynamic_cast<const KeypadGump*>(game.screen());
        CHECK(keypad != nullptr);
        if (!keypad) return;
        const std::string digits = std::to_string(keypad->code());
        for (const char c : digits) {
            const auto scancode = static_cast<std::uint8_t>(c == '0' ? 0x0B : 0x02 + (c - '1'));
            game.key(scancode, false, true, 0, static_cast<std::uint8_t>(c));
            game.key(scancode, false, false, 0, static_cast<std::uint8_t>(c));
            game.update(kFrameUs);
        }
        game.key(0x1C, false, true, 0, 0x0D);
        game.key(0x1C, false, false, 0, 0x0D);
        game.update(kFrameUs);
        const auto autosave = dir / ("SAVEGAME." + std::to_string(Game::kAutoSave));
        int frames = 0;
        for (; frames < 1200 && !std::filesystem::exists(autosave); ++frames) game.update(kFrameUs);
        const Item* door = w.get(kDoor);
        std::printf("keypad door autosave: code %s, saved after %d frames, door x %d, flags %04X\n", digits.c_str(),
                    frames, door->x, door->flags);
        // Well before the 10 s limit: the 65 markers of the link were
        // unlocked on load too, their TRIGGER processes do not linger.
        CHECK(std::filesystem::exists(autosave) && frames < 300);
        CHECK(door->x == kOpenX && !(door->flags & 0x1000));
    }
}

// Issue #26: the reporter's save (map 40, the base) was made with the player
// in the range of TRIGEGG 7516 (55, by the teleporter room), its door 433
// (0x4D8) open, raised to z 114. TRIGGER toggles the door, on the egg's
// hatch (event 7) as on its way out (0x15). The original saves the
// EggHatcher with its "hatched" field (+0x50, 1098:0718); the port, which
// rebuilds the fast area instead, hatched the egg a second time on load:
// the door shut by the player, then opened as he went away. It stays open
// now, shuts when he leaves and opens when he comes back.
void test_egg_hatched_before_save(const std::filesystem::path& data) {
    constexpr ObjId kEgg = 7516, kDoor = 433;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "issue26-porte-teleporteur-inversee.sav");
    World& w = game.world();
    const Item* door = w.get(kDoor);
    const Item* player = w.get(1);
    CHECK(w.current_map() == 40 && w.get(kEgg) && door && door->shape == 0x4D8);
    if (!door || !player) return;
    const std::int32_t px = player->x, py = player->y, pz = player->z;
    auto run = [&](int frames) {
        for (int f = 0; f < frames; ++f) game.update(kFrameUs);
    };
    // Open: last frame, raised (saved on its way up, at z 114; 150 at the top).
    auto open = [&] { return door->frame == 9 && door->z >= 114; };
    run(120);
    const bool open_on_load = open();
    w.move_item(1, px - 400, py, pz);  // out of the egg's 160 units
    run(120);
    const bool shut_away = door->z == 16 && door->frame == 0;
    w.move_item(1, px, py, pz);
    run(120);
    const bool open_back = open();
    std::printf("egg hatched before the save: door open on load %d, shut away %d, open back %d (z %d)\n",
                open_on_load, shut_away, open_back, door->z);
    CHECK(open_on_load && shut_away && open_back);
    CHECK(game.machine().faults().empty());
}

// Issue #33: the reporter's save (German 1.07, mission 1) was made while the
// floating platform 819 (HOVER1) crossed the acid pool, on its way to
// waypoint 1 (QHi 0x21, status 0x1000). HOVER's event 0x21 moves it from
// waypoint to waypoint for good; its enterFastArea does not start it again
// (busy bit set, and QHi bit 0x20). The processes are not loaded: it stood
// still after the load. It travels again, and keeps on.
void test_hover_platform_after_load(const std::filesystem::path& data) {
    constexpr ObjId kPlatform = 819;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "issue33-plateforme-flottante-immobile.sav");
    World& w = game.world();
    const Item* platform = w.get(kPlatform);
    CHECK(w.current_map() == 1 && platform && platform->shape == 0x4F0);
    if (!platform) return;
    // Distinct places the platform went through, frame by frame.
    auto travel = [&](int frames) {
        std::set<std::pair<std::int32_t, std::int32_t>> places{{platform->x, platform->y}};
        for (int f = 0; f < frames; ++f) {
            game.update(kFrameUs);
            places.emplace(platform->x, platform->y);
        }
        return places.size();
    };
    const std::size_t first = travel(600);
    const std::size_t later = travel(600);
    std::printf("hover platform after load: %zu then %zu places, at (%d, %d), status 0x%04X\n", first, later,
                platform->x, platform->y, platform->flags);
    CHECK(first > 10 && later > 10);
    CHECK(platform->flags & 0x1000);
    CHECK(game.machine().faults().empty());
}

// Issue #33, the base: the reporter's save (map 40) caught the scenery guard
// 673 (REB_GAR) halfway through his move, frame 31 for a rest frame of 24
// (QLo). The rebuilt fast area made 31 his rest frame, and each move took
// him through frames 32..38 (32 empty, the others missing): he blinked out
// and back. His rest frame is 24 again; he only shows his frames 24..31, or
// stays away (frame 32) when his enterFastArea draws it.
void test_scenery_guard_after_load(const std::filesystem::path& data) {
    constexpr ObjId kGuard = 673;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "issue33-base-resistance-objets-clignotent.sav");
    World& w = game.world();
    const Item* guard = w.get(kGuard);
    CHECK(w.current_map() == 40 && guard && guard->shape == 0x4EC);
    if (!guard) return;
    const int rest = guard->quality & 0xFF;
    std::set<int> frames{guard->frame};
    for (int f = 0; f < 3000; ++f) {
        game.update(kFrameUs);
        frames.insert(guard->frame);
    }
    const bool away = frames == std::set<int>{32};
    const bool own = *frames.begin() >= 24 && *frames.rbegin() <= 31;
    std::printf("scenery guard after load: rest frame %d, %zu frames %d..%d%s\n", rest, frames.size(),
                *frames.begin(), *frames.rbegin(), away ? " (away)" : "");
    CHECK(rest == 24);
    CHECK(away || (own && frames.size() == 8));
    CHECK(game.machine().faults().empty());
}

// Status 0x1000 saved while a process of the item's class ran (the
// processes are not loaded): a running timer (TIMER) and a rolling roller
// (ROLLB_NS, ROLLB_EW) get their loop started again (events 0x20 and 0x21),
// which keeps the bit; without, nothing could ever start them. A one-shot
// timer (QHi bit 0 clear) clears it itself once its delay is over. Ported
// from No Regret (08d2749).
void test_saved_busy_items(const std::filesystem::path& data) {
    const auto dir = std::filesystem::temp_directory_path() / "crusader_saved_busy_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (int slot = 1; slot <= 7; ++slot) {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        game.load_save_file(test_saves(data) / ("SAVEGAME." + std::to_string(slot)));
        game.update(kFrameUs);
        World& w = game.world();
        const UsecodeFile& uc = game.machine().usecode();
        ObjId timer = 0, roller = 0;
        int delay = 0;
        w.for_each([&](const Item& i) {
            if (i.parent || (i.flags & 0x1000) || !uc.has_class(i.shape)) return;
            const std::string cls = uc.class_name(i.shape);
            // The delay: map array * 256 + NPC number (TIMER_00C0).
            const int d = i.map_number * 256 + i.npc_number;
            if (!timer && cls == "TIMER" && d > 0 && d < 600 && !((i.quality >> 8) & 1)) timer = i.id, delay = d;
            if (!roller && (cls == "ROLLB_NS" || cls == "ROLLB_EW")) roller = i.id;
        });
        if (!timer || !roller) continue;
        std::printf("saved busy: save %d, timer %u (delay %d), roller %u\n", slot, timer, delay, roller);
        for (ObjId id : {timer, roller}) w.get(id)->flags |= 0x1000;
        game.save_game(9);
        game.load_save(9);
        World& loaded = game.world();
        const auto runs = [&](ObjId id) {
            int n = 0;
            game.kernel().for_each_process([&](const Process& p) { n += p.owner() == id; });
            return n;
        };
        game.update(kFrameUs);
        CHECK(runs(roller) >= 1 && (loaded.get(roller)->flags & 0x1000));
        CHECK(loaded.get(timer)->flags & 0x1000);
        int frames = 0;
        for (; frames < 4 * delay + 60 && (loaded.get(timer)->flags & 0x1000); ++frames) game.update(kFrameUs);
        std::printf("saved busy: timer done after %d frames\n", frames);
        CHECK(!(loaded.get(timer)->flags & 0x1000));
        CHECK(game.machine().faults().empty());
        return;
    }
    std::printf("saved busy: no save with a one-shot timer and a roller\n");
    CHECK(false);
}

// The autosave (port addition): SAVEGAME.12 is written when a keypad's code
// matches, a keycard is picked up, a mission starts or a movie ends, only when
// GameConfig::autosave is on, and F6 loads it back. The keypad's waits for
// the end of its KEYPAD script: saved at once, the keypad kept its busy bit
// (0x1000) and the door was not open yet, for good once loaded. A keypad
// saved busy all the same (a save made during that script) is freed.
void test_autosave(const std::filesystem::path& data) {
    const auto dir = std::filesystem::temp_directory_path() / "crusader_autosave_test";
    const auto autosave = dir / ("SAVEGAME." + std::to_string(Game::kAutoSave));
    const auto fresh = [&] {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::filesystem::copy_file(test_saves(data) / "SAVEGAME.2", dir / "SAVEGAME.2");
    };
    const auto enable = [](Game& game) {
        GameConfig config = game.config();
        config.autosave = true;
        game.set_config(config);
    };
    const auto code = [&](Game& game, bool right) {
        constexpr ObjId kKeypad = 5459;
        use_item(game.world(), kKeypad);
        for (int f = 0; f < 3; ++f) game.update(kFrameUs);
        const char digits[] = {'9', '6', '2'};
        const std::uint8_t scancodes[] = {0x0A, 0x07, 0x03};
        for (int i = 0; i < (right ? 3 : 1); ++i) {
            game.key(scancodes[i], false, true, 0, static_cast<std::uint8_t>(digits[i]));
            game.key(scancodes[i], false, false, 0, static_cast<std::uint8_t>(digits[i]));
            game.update(kFrameUs);
        }
        game.key(0x1C, false, true, 0, 0x0D);
        game.key(0x1C, false, false, 0, 0x0D);
        game.update(kFrameUs);
    };

    // A wrong code saves nothing, the right one does, with the floppy disk.
    fresh();
    {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        enable(game);
        game.load_save(2);
        code(game, false);
        CHECK(!std::filesystem::exists(autosave) && !game.autosave_icon_shown());
    }
    {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        enable(game);
        game.load_save(2);
        code(game, true);
        CHECK(!std::filesystem::exists(autosave) && (game.world().get(5459)->flags & 0x1000));
        for (int f = 0; f < 600 && !std::filesystem::exists(autosave); ++f) game.update(kFrameUs);
        CHECK(std::filesystem::exists(autosave) && game.autosave_icon_shown());
        // The floppy disk goes after 2.5 s (150 frames), before the opened
        // door's keycard check saves again (I_hasKeycard, ~170 ticks on).
        for (int f = 0; f < 155; ++f) game.update(kFrameUs);
        CHECK(!game.autosave_icon_shown());
        // It loads like any save (F6 asks, Enter... here the slot directly).
        Game again(data, test_saves(data));
        again.set_save_dir(dir);
        again.load_save(Game::kAutoSave);
        CHECK(again.world().get(1) && again.world().get(1)->x == game.world().get(1)->x);
        CHECK(again.machine().faults().empty());
        const std::uint16_t keypad = again.world().get(5459)->flags;
        std::printf("autosave: keypad flags %04X once loaded\n", keypad);
        CHECK(!(keypad & 0x1000) && (keypad & 0x0004));
    }
    {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        game.load_save(2);
        game.world().get(5459)->flags |= 0x1000;
        game.save_game(Game::kAutoSave);
        game.load_save(Game::kAutoSave);
        CHECK(!(game.world().get(5459)->flags & 0x1000));
    }
    // Switched off: nothing.
    fresh();
    {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        enable(game);
        game.load_save(2);
        GameConfig config = game.config();
        config.autosave = false;
        game.set_config(config);
        code(game, true);
        CHECK(!std::filesystem::exists(autosave) && !game.autosave_icon_shown());
        CHECK(!GameConfig::parse(config.text()).autosave && GameConfig::parse(game.config().text()).autosave == false);
        CHECK(GameConfig::parse("").autosave);
    }
    // A keycard picked up.
    fresh();
    {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        enable(game);
        game.load_save(2);
        World& w = game.world();
        const ObjId card = w.create(0x111, 0);
        CHECK(card != 0);
        if (card) {
            w.get(card)->quality = 3;
            w.pop_to_coords(w.get(1)->x, w.get(1)->y, w.get(1)->z);
            CHECK(add_item_cru(w, card, false));
            CHECK((w.state.keycards & 8) != 0 && std::filesystem::exists(autosave));
        }
    }
    // A door opened with a keycard (CARD_NS 2415 of save 13), saved before it moves.
    fresh();
    std::filesystem::copy_file(test_saves(data) / "SAVEGAME.13", dir / "SAVEGAME.13");
    {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        enable(game);
        game.load_save(13);
        World& w = game.world();
        w.state.keycards = 0xFFFFFFFF;
        const Item* door = w.get(2415);
        CHECK(door != nullptr);
        if (door) {
            const std::uint16_t frame = door->frame;
            use_item(w, 2415);
            for (int f = 0; f < 5; ++f) game.update(kFrameUs);
            CHECK(std::filesystem::exists(autosave));
            for (int f = 0; f < 200; ++f) game.update(kFrameUs);
            std::fprintf(stderr, "autosave: card door frame %u -> %u\n", frame, w.get(2415)->frame);
        }
    }
    // A mission starts: once the player may act again.
    fresh();
    {
        Game game(data, test_saves(data));
        game.set_save_dir(dir);
        enable(game);
        game.new_game(3);
        int frames = 0;
        int movies = 0;
        for (; frames < 6000 && !std::filesystem::exists(autosave); ++frames) {
            // The arrival movie is skipped with Escape.
            if (game.movie()) {
                ++movies;
                game.key(0x01, false, true, 0, 0x1B);
                game.update(kFrameUs);
                game.key(0x01, false, false, 0);
            }
            game.update(kFrameUs);
        }
        std::printf("autosave: %d frames of movie\n", movies);
        std::printf("autosave: a mission started, saved after %d frames\n", frames);
        CHECK(std::filesystem::exists(autosave));
        Game again(data, test_saves(data));
        again.set_save_dir(dir);
        again.load_save(Game::kAutoSave);
        CHECK(again.world().get(1) != nullptr && again.machine().faults().empty());
    }
    std::filesystem::remove_all(dir);
}

// SURCAMNS 3224 (save 7, quality low 0xFE): the view of its EVENT egg, in
// grey, and the control of the NPC near it whose egg quality (record +0x63)
// is 0xFE, robot 10; Escape gives the control back. Without such an NPC, the
// view stays until a key is held (0x096).
void test_monitor(const std::filesystem::path& data) {
    constexpr ObjId kMonitor = 3224;
    constexpr ObjId kRobot = 10;
    const auto show = [](Game& game, const char* when) {
        const World& w = game.world();
        std::printf("monitor: %s, camera (%d, %d, %d), controlled %u, stasis %d, status bar %d\n", when,
                    w.state.camera_x, w.state.camera_y, w.state.camera_z, w.state.controlled_npc,
                    w.state.avatar_in_stasis, w.state.status_bar_visible);
    };
    const auto run = [](Game& game, int frames) {
        for (int f = 0; f < frames; ++f) game.update(kFrameUs);
    };
    // Back to the player: camera on him, status bar, no stasis.
    const auto restored = [](Game& game) {
        const World& w = game.world();
        const Item* p = w.get(1);
        return w.state.controlled_npc == 1 && !w.state.avatar_in_stasis && w.state.status_bar_visible &&
               w.state.camera_x == p->x && w.state.camera_y == p->y && !game.menu() && !game.screen();
    };
    const auto clean = [](Game& game) {
        return game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty();
    };

    // The robot.
    {
        Game game(data, test_saves(data));
        game.load_save(7);
        World& w = game.world();
        CHECK(w.npc_record(kRobot)[npc::kEggQuality] == 0xFE);
        use_item(w, kMonitor);
        run(game, 200);
        show(game, "robot");
        CHECK(w.state.controlled_npc == kRobot && w.state.avatar_in_stasis && !w.state.status_bar_visible);
        // The arrows drive the robot.
        const World::Point from = w.location(*w.get(kRobot));
        game.key(0x48, true, true, 0, 0);
        run(game, 60);
        game.key(0x48, true, false, 0, 0);
        run(game, 20);
        const World::Point to = w.location(*w.get(kRobot));
        std::printf("monitor: robot (%d, %d) -> (%d, %d)\n", from.x, from.y, to.x, to.y);
        CHECK(to.x != from.x || to.y != from.y);
        game.key(0x01, false, true, 0, 0x1B);
        game.key(0x01, false, false, 0, 0x1B);
        run(game, 120);
        show(game, "escape");
        CHECK(restored(game) && clean(game));
    }
    // No NPC to control: a key held ends the view.
    {
        Game game(data, test_saves(data));
        game.load_save(7);
        World& w = game.world();
        w.npc_record(kRobot)[npc::kEggQuality] = 0;
        const std::int32_t player_x = w.get(1)->x;
        use_item(w, kMonitor);
        run(game, 300);
        show(game, "view");
        CHECK(w.state.controlled_npc == 1 && w.state.avatar_in_stasis && !w.state.status_bar_visible);
        CHECK(w.state.camera_x != player_x);
        game.key(0x48, true, true, 0, 0);
        run(game, 5);
        game.key(0x48, true, false, 0, 0);
        run(game, 120);
        show(game, "key");
        CHECK(restored(game) && clean(game));
    }
}

// CAMERACO 6082 (save 2), once on screen, sweeps frames 0..4 and back; when
// it faces the controlled NPC and sees it (Item::I_getRangeIfVisible 0x044),
// it stops, beeps (0xE6), takes the status 0x1000 and raises the alarm.
void test_security_camera(const std::filesystem::path& data) {
    constexpr ObjId kCamera = 6082;
    for (const bool in_view : {false, true}) {
        Game game(data, test_saves(data));
        game.load_save(2);
        World& w = game.world();
        const Item* cam = w.get(kCamera);
        // Behind a wall, then in front of it.
        if (in_view) w.move_item(1, cam->x + 256, cam->y + 256, 0);
        else w.move_item(1, cam->x - 64, cam->y + 400, 0);
        const int range = range_if_visible(w, kCamera, 1);
        std::set<int> frames;
        bool beep = false;
        for (int f = 0; f < 600; ++f) {
            game.update(kFrameUs);
            frames.insert(cam->frame);
            beep = beep || game.audio()->sources.is_playing(0xE6, kCamera);
        }
        std::printf("camera: in view %d, range %d, frames %zu, beep %d, status %04X, alarm %d\n", in_view, range,
                    frames.size(), beep, cam->flags & 0x1000, w.state.alert_active);
        if (in_view) CHECK(range == 16 && beep && (cam->flags & 0x1000) && w.state.alert_active);
        else CHECK(range == 0 && frames.size() == 5 && !beep && !(cam->flags & 0x1000) && !w.state.alert_active);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
    // Native resolution, a 1720 x 720 view: the camera, 450 pixels right of
    // the camera point, is drawn in the band beside the original screen. It
    // sweeps as soon as it is seen, but only sees from the combat zone: the
    // player in front of it is not spotted.
    {
        Game game(data, test_saves(data));
        game.load_save(2);
        World& w = game.world();
        const Item* cam = w.get(kCamera);
        w.move_item(1, cam->x + 256, cam->y + 256, 0);
        w.set_view_size(1720, 720);
        w.set_camera(cam->x - 450, cam->y + 450, cam->z & 0xFF);
        const bool drawn = w.is_partly_on_screen(*cam), fights = w.is_partly_in_combat_zone(*cam);
        std::set<int> frames;
        bool beep = false;
        for (int f = 0; f < 600; ++f) {
            game.update(kFrameUs);
            frames.insert(cam->frame);
            beep = beep || game.audio()->sources.is_playing(0xE6, kCamera);
        }
        std::printf("camera: in the band, drawn %d, in zone %d, frames %zu, beep %d, status %04X, alarm %d\n", drawn,
                    fights, frames.size(), beep, cam->flags & 0x1000, w.state.alert_active);
        CHECK(drawn && !fights);
        CHECK(frames.size() == 5 && !beep && !(cam->flags & 0x1000) && !w.state.alert_active);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
}
// The report's save (camera-immobile-vue-native.sav): a 3440 x 1440 window
// with zoom x2 gives a 1720 x 684 view, camera following the player.
// CAMERANS 7645 is drawn 660 pixels left of him, outside the original screen:
// it stayed still while it waited to be in the combat zone. It sweeps now,
// without spotting him from there.
void test_camera_in_wide_view(const std::filesystem::path& data) {
    constexpr ObjId kCamera = 7645;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "camera-immobile-vue-native.sav");
    World& w = game.world();
    w.set_follow_player(true);
    w.set_view_size(1720, 684);
    const Item* cam = w.get(kCamera);
    CHECK(cam && cam->shape == 0x1B8);
    if (!cam) return;
    std::set<int> frames;
    for (int f = 0; f < 600; ++f) {
        game.update(kFrameUs);
        frames.insert(cam->frame);
    }
    const bool drawn = w.is_partly_on_screen(*cam), fights = w.is_partly_in_combat_zone(*cam);
    std::printf("camera in a wide view: drawn %d, in zone %d, frames %zu, status %04X, alarm %d\n", drawn, fights,
                frames.size(), cam->flags & 0x1000, w.state.alert_active);
    CHECK(drawn && !fights && frames.size() >= 5 && !(cam->flags & 0x1000));
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Switch 4595 of SAVEGAME.3 turns off the energy walls of quality 9: TRIGGER
// sends them event 0x13 (Item::I_avatarStoleSomething, their switch-off
// sound 0x3D) and turns them into markers 0x476 keeping the shape; used
// again, it turns them back on.
void test_energy_walls(const std::filesystem::path& data) {
    constexpr ObjId kSwitch = 4595;
    constexpr ObjId kWalls[] = {4593, 4598, 4623, 4644};
    Game game(data, test_saves(data));
    game.load_save(3);
    World& w = game.world();
    for (ObjId id : kWalls) {
        CHECK(w.get(id) && w.get(id)->shape == 0x564 && w.get(id)->frame == 1);
        if (!w.get(id)) return;
    }
    use_item(w, kSwitch);
    bool sound = false;
    for (int f = 0; f < 300; ++f) {
        game.update(kFrameUs);
        sound = sound || game.audio()->sources.is_playing(0x3D, kWalls[0]);
    }
    const Item* wall = w.get(kWalls[0]);
    if (!wall) return;
    std::printf("energy walls: off -> %04X:%u status %04X npc %02X map %02X, sound %d\n", wall->shape, wall->frame,
                wall->flags, wall->npc_number, wall->map_number, sound);
    CHECK(sound);
    for (ObjId id : kWalls) CHECK(w.get(id)->shape == 0x476 && (w.get(id)->flags & 0x10));
    // Turned back on, each marker is replaced by a new wall (FREE function
    // 0x863: create, copy the qualities, unequip, destroy the marker).
    use_item(w, kSwitch);
    for (int f = 0; f < 300; ++f) game.update(kFrameUs);
    int walls = 0, markers = 0;
    w.for_each([&](const Item& i) {
        walls += i.shape == 0x564 && i.frame == 1 && i.quality == 9 && !(i.flags & 0x10);
        markers += i.shape == 0x476 && i.quality == 9;
    });
    std::printf("energy walls: on again -> %d walls, %d markers\n", walls, markers);
    CHECK(walls == 4 && markers == 0);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}
void test_stations(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    Audio* audio = game.audio();
    CHECK(audio != nullptr);
    if (!audio) return;
    auto set_word = [&](std::size_t at, int v) {
        auto r = w.npc_record(1);
        r[at] = static_cast<std::uint8_t>(v);
        r[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    set_word(0, 100);
    start_healer(game.kernel(), w);
    CHECK(w.state.avatar_in_stasis && audio->sources.is_playing(0xDB, 1));
    game.update(kFrameUs);
    CHECK(audio->sources.is_playing(0xBA, 1));
    for (int i = 0; i < 80; ++i) game.update(kFrameUs);
    std::printf("stations: hit points 100 -> %u, healing sound %d\n", w.npc_hp(1), audio->sources.is_playing(0xDB, 1));
    CHECK(w.npc_hp(1) == 150 && !w.state.avatar_in_stasis && !audio->sources.is_playing(0xDB, 1));

    std::printf("stations: battery %u, max energy %d\n", w.state.battery, max_energy(w, 1));
    CHECK(w.state.battery == 1 && max_energy(w, 1) == 2500);
    set_word(0x57, 0);
    start_battery_charger(game.kernel(), w);
    for (int i = 0; i < 120; ++i) game.update(kFrameUs);
    CHECK(w.npc_mana(1) == 2500 && !audio->sources.is_playing(0xA4, 1));
}

// The player dropped from 48 units lands hard (1040:04BC): action 0x3B and
// a grunt.
void test_hard_landing(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    settle(game);
    const Item* player = w.get(1);
    const int ground = player->z & 0xFF;
    w.move_item(1, player->x, player->y, ground + 48);
    fall(game.kernel(), w, 1);
    bool stumbled = false, grunt = false;
    for (int i = 0; i < 200; ++i) {
        game.update(kFrameUs);
        stumbled = stumbled || last_action(game) == 0x3B;
        grunt = grunt || game.audio()->sources.is_playing(0x8F, 1);
    }
    std::printf("landing: z %d -> %d, stumbled %d, grunt %d\n", ground + 48, w.get(1)->z & 0xFF, stumbled, grunt);
    CHECK((w.get(1)->z & 0xFF) == ground && stumbled && grunt);
}

// Jalon 2: a step blocked by an obstacle (ANIMPRIM.C 1908). A box (0x81) is
// put on the walkway of SAVEGAME.13, south of the player. Walking into it
// ends the walk at the box. A roll (0x18) that does not fit is tried from
// 16, 32, 48 units ahead and behind (ANIMPRIM.C 016F): the player moves
// aside, rolls past the box and plays all its frames.
void test_blocked_step(const std::filesystem::path& data) {
    for (const std::uint16_t action : {std::uint16_t{1}, std::uint16_t{0x18}}) {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        settle(game);
        const Item* player = w.get(1);
        const int x0 = player->x, y0 = player->y + 64, z0 = player->z & 0xFF;
        w.move_item(1, x0, y0, z0);
        const ObjId box = w.create(0x81, 0);
        w.pop_to_coords(x0, y0 + 96, z0);
        // The roll goes sideways, 4 directions clockwise of the facing.
        const int facing = action == 1 ? 8 : 4;
        w.npc_record(1)[npc::kDirection] = static_cast<std::uint8_t>(facing);
        do_anim(game.kernel(), w, game.animations(), 1, action, facing);
        int frames = 0, last_frame = 0;
        for (; frames < 300 && game.kernel().count_processes(1, ActorAnimProcess::kType) != 0; ++frames) {
            game.update(kFrameUs);
            last_frame = w.npc_record(1)[npc::kAnimFrame];
        }
        const AnimAction* a = game.animations().action(player->shape, action);
        std::printf("blocked step: action 0x%X from %d %d to %d %d (box at y %d), frame %d of %zu, %d frames\n", action,
                    x0, y0, player->x, player->y, w.get(box)->y, last_frame, a ? a->frame_count() : 0, frames);
        CHECK(player->y > y0 && (player->z & 0xFF) == z0);
        if (action == 1) {
            CHECK(player->x == x0 && player->y < w.get(box)->y && last_frame < 8);
        } else {
            const int aside = std::abs(player->x - x0);
            CHECK(aside >= 16 && aside <= 48 && player->y > w.get(box)->y);
            CHECK(a && last_frame == static_cast<int>(a->frame_count()) - 1);
        }
        CHECK(game.machine().faults().empty());
    }
}

// Milestone 4: a pistol shot (fire type 11) at an NPC placed in front of the player.
std::size_t projectiles(Game& game) { return game.kernel().count_processes(0, ProjectileProcess::kType); }

// Fires as the shot frame of `shooter` facing `dir` would (action 8): from its
// muzzle offset, outside the shooter's box.
Pid fire_from_muzzle(Game& game, ObjId shooter, int dir, int fire_type, bool aim) {
    World& w = game.world();
    // A real shot leaves in the shooter's direction (the crosshair of the
    // aiming mode is placed from it).
    if (auto r = w.npc_record(shooter); !r.empty()) r[npc::kDirection] = static_cast<std::uint8_t>(dir);
    World::Point m{0, 0, 24};
    if (const AnimAction* a = game.animations().action(w.get(shooter)->shape, 8)) {
        for (std::size_t i = 0; i < a->frame_count(); ++i) {
            const AnimFrame* f = a->frame(dir, i);
            if (f && f->fires_weapon()) {
                m = {f->muzzle_a * 16, f->muzzle_b * 16, f->muzzle_z * 8};
                break;
            }
        }
    }
    return fire_weapon(game.kernel(), w, game.animations(), shooter, m.x, m.y, m.z, dir, fire_type, aim);
}

void test_shot_kills(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    const Item* player = w.get(1);
    ObjId victim = 0;
    for (ObjId id = 2; id < 256 && !victim; ++id) {
        const Item* it = w.get(id);
        if (it && !it->off_map && !w.is_dead(id) && (it->z & 0xFF) != 254 && w.npc_hp(id) > 0 && game.animations().action(it->shape, 0x12))
            victim = id;
    }
    CHECK(victim != 0);
    if (!victim) return;
    const int dir = w.npc_direction(1) & 0xE;
    w.move_item(victim, player->x + kDirectionDx[dir] * 96, player->y + kDirectionDy[dir] * 96, player->z);
    const int hp = w.npc_hp(victim);
    std::size_t sprites_before = 0;
    w.for_each([&](const Item& i) { sprites_before += i.shape == kProjectileShape; });

    fire_from_muzzle(game, 1, dir, 11, false);
    CHECK(projectiles(game) == 1);
    // A human's death cry (or a cry of pain if it lives), at the victim. The
    // scripts of this map keep the one-shot voices busy (see test_firing):
    // the cry is checked meanwhile.
    bool cry = false;
    const auto listen = [&] {
        for (int sfx : {0x88, 0x8C, 0x8F, 0xD8, 0x10}) cry = cry || game.audio()->sources.is_playing(sfx, victim);
    };
    int frames = 0;
    for (; frames < 300 && projectiles(game) != 0; ++frames) {
        game.update(kFrameUs);
        listen();
    }
    for (int i = 0; i < 10; ++i) {
        game.update(kFrameUs);  // the death animation starts
        listen();
    }
    const auto r = w.npc_record(victim);
    const std::uint16_t last = static_cast<std::uint16_t>(r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8);
    std::printf("shot: victim %u (shape %u) hp %d -> %u, dead %d, last action 0x%X, %d frames\n", victim,
                w.get(victim)->shape, hp, w.npc_hp(victim), w.is_dead(victim), last, frames);
    CHECK(projectiles(game) == 0);
    CHECK(w.npc_hp(victim) < hp);
    // 14..20 damage, x5 at difficulty 1: at least 70.
    CHECK(hp > 70 || (w.is_dead(victim) && w.npc_hp(victim) == 0 && (last == 0x12 || last == 0x14)));
    const std::uint16_t shape = w.get(victim)->shape;
    const bool robot = shape == 0x4C8 || shape == 0x338 || shape == 0x45D || shape == 0x2CB || shape == 0x4E6 ||
                       shape == 0x383 || shape == 0x385;
    std::printf("shot: victim's cry %d\n", cry);
    CHECK(robot || cry);
    std::size_t sprites_after = 0;
    w.for_each([&](const Item& i) { sprites_after += i.shape == kProjectileShape; });
    std::printf("shot: items of shape 0x1D9 before %zu, after %zu\n", sprites_before, sprites_after);
    CHECK(sprites_after == sprites_before);

    CHECK(game.machine().faults().empty());
}

// The last frame of an NPC's death (frame flag 0x4000) sends usecode event
// 0x0E (calledFromAnim, 10A8:2B01): the GUARD class starts NPCDEATH.
void test_death_called_from_anim(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    World& w = game.world();
    // A guard put near the player, where it stands on something, and away
    // from the floors that take no blood: NPC fn 32 (NPCDEATH 0CC2) finds
    // none of these shapes within 160 of the dead guard (CATWALK2 0x132 is
    // under the player).
    static constexpr std::uint8_t kNoBlood[] = {
        'N', 0x2F, 1, 0x65, 3, 0x66, 3, 0xCD, 4, 0x36, 3, 0x90, 2, 0xCD, 0,
        0x5F, 0, 0xD1, 0, 0x32, 1, 0x5F, 3, 0x62, 3, 0x5E, 3, 0x6C, 3, '$'};
    const ObjId victim = 2;
    const Item* player = w.get(1);
    CHECK(w.get(victim) && w.get(victim)->shape == 0x2FD && !w.is_dead(victim));
    bool placed = false;
    World::Point at{};
    for (int step = 1; step <= 6 && !placed; ++step) {
        for (int dir = 0; dir < 16 && !placed; dir += 2) {
            at = {player->x + kDirectionDx[dir] * 96 * step, player->y + kDirectionDy[dir] * 96 * step, player->z};
            w.move_item(victim, at.x, at.y, at.z);
            placed = (support_mask(w, 0x2FD, at.x, at.y, at.z, true, victim) & 0x10) &&
                     position_free(w, 0x2FD, at, nullptr, victim) && w.area_search(victim, 160, kNoBlood).empty();
        }
    }
    CHECK(placed);
    std::set<ObjId> ids_before;
    w.for_each([&](const Item& i) { ids_before.insert(i.id); });
    apply_damage(game.kernel(), w, game.animations(), victim, 1, w.npc_direction(victim), 250, 1);
    CHECK(w.is_dead(victim));
    for (int i = 0; i < 600; ++i) game.update(kFrameUs);

    // NPCDEATH: the guard is set aside (Item::I_push), a body D_GUARD (0x339)
    // lies in its place; it does not block the way.
    const Item* body = nullptr;
    w.for_each([&](const Item& i) {
        if (!ids_before.count(i.id) && i.shape == 0x339) body = &i;
    });
    std::printf("death: guard %s, body %u at (%d, %d, %d), placed at (%d, %d, %d)\n",
                w.get(victim) ? "still in the world" : "set aside", body ? body->id : 0, body ? body->x : -1,
                body ? body->y : -1, body ? body->z : -1, at.x, at.y, at.z);
    CHECK(!w.get(victim) && body);
    if (body) {
        const ShapeInfo* info = w.shape_info(body->shape);
        CHECK(info && !info->is(ShapeInfo::kSolid));
        CHECK(position_free(w, 1, {body->x, body->y, body->z}, nullptr, 1));
    }
    // With VIOLENCE (intrinsic 0x06D), a human leaves BLOOD (0x4B8): frames
    // 0..9 or 10..19 at random, played to the end.
    const Item* blood = nullptr;
    w.for_each([&](const Item& i) {
        if (!ids_before.count(i.id) && i.shape == 0x4B8) blood = &i;
    });
    std::printf("death: blood %u frame %d\n", blood ? blood->id : 0, blood ? blood->frame : -1);
    CHECK(blood && (blood->frame == 9 || blood->frame == 19));
    CHECK(game.machine().faults().empty());
}

// Milestone 4: aiming (TRACKER_1128_0158). An NPC placed 8.9 degrees off
// the direction of the shot is found and aimed at; a straight shot would pass
// beside it.
void test_aiming(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    World& w = game.world();

    // The tracked list: items of the fast area, not broken, trackable shapes.
    std::size_t listed = 0;
    bool consistent = true;
    for (ObjId id : w.tracked()) {
        if (id == 0) continue;
        ++listed;
        const Item* it = w.get(id);
        consistent = consistent && it && (it->flags & kItemFastArea) && !(it->flags & kItemBroken) &&
                     w.is_trackable(it->shape);
    }
    std::printf("aim: %zu tracked items\n", listed);
    CHECK(listed == w.tracked_count() && listed > 0 && consistent);

    const Item* player = w.get(1);
    ObjId victim = 0;
    for (ObjId id = 2; id < 256 && !victim; ++id) {
        const Item* it = w.get(id);
        if (it && !w.is_dead(id) && (it->z & 0xFF) != 254 && w.npc_hp(id) > 0 && w.is_trackable(it->shape))
            victim = id;
    }
    CHECK(victim != 0);
    if (!victim) return;
    // Every other tracked item out of the way: they would compete for the shot.
    std::vector<ObjId> others;
    for (ObjId id : w.tracked())
        if (id != 0 && id != victim) others.push_back(id);
    for (ObjId id : others) w.set_broken(*w.get(id));
    const int dir = 4;  // +x
    w.move_item(victim, player->x + 256, player->y - 40, player->z);
    w.get(victim)->off_map = false;  // an NPC of another map, brought here
    CHECK(find_target(w, 1, dir, true) == victim);
    CHECK(find_target(w, 1, (dir + 1) & 0xF, true) == 0);
    const World::Point aim = aim_point(w, 1, victim);
    const Item* v = w.get(victim);
    std::printf("aim: victim %u at (%d, %d, %d), aim point (%d, %d, %d)\n", victim, v->x, v->y, v->z & 0xFF, aim.x,
                aim.y, aim.z);
    CHECK(aim.x == v->x - 24 && aim.y == v->y - 24 && aim.z == ((v->z & 0xFF) + 64));

    // Without aiming the shot goes straight ahead and misses. It flies to
    // the crosshair with the pistol's spread (about 20 at this distance):
    // the victim is moved further off the line for it.
    const int hp = w.npc_hp(victim);
    w.move_item(victim, player->x + 256, player->y - 120, player->z);
    fire_from_muzzle(game, 1, dir, 11, false);
    for (int frames = 0; frames < 300 && projectiles(game) != 0; ++frames) game.update(kFrameUs);
    CHECK(w.npc_hp(victim) == hp);
    w.move_item(victim, player->x + 256, player->y - 40, player->z);
    w.get(victim)->off_map = false;  // an NPC of another map, brought here

    // In the game the last action is the shot (action 8, 16 directions).
    w.npc_record(1)[npc::kLastAnim] = 8;
    w.npc_record(1)[npc::kLastAnim + 1] = 0;
    const World::Point aim_now = aim_point(w, 1, victim);
    std::printf("aim: victim now at (%d, %d, %d), aim point (%d, %d, %d)\n", v->x, v->y, v->z & 0xFF, aim_now.x,
                aim_now.y, aim_now.z);
    const Pid pid = fire_from_muzzle(game, 1, dir, 11, true);
    const auto* p = dynamic_cast<const ProjectileProcess*>(game.kernel().get(pid));
    CHECK(p != nullptr);
    if (!p) return;
    // Pistol: spread = distance / 10 around the aim point.
    const int spread = std::max(std::abs(aim_now.x - p->position().x), std::abs(aim_now.y - p->position().y)) / 10;
    std::printf("aim: projectile towards (%d, %d, %d), spread %d\n", p->target().x, p->target().y, p->target().z, spread);
    CHECK(std::abs(p->target().x - aim_now.x) <= spread && std::abs(p->target().y - aim_now.y) <= spread);
    for (int frames = 0; frames < 300 && projectiles(game) != 0; ++frames) game.update(kFrameUs);
    std::printf("aim: victim hp %d -> %u\n", hp, w.npc_hp(victim));
    CHECK(w.npc_hp(victim) < hp);
    CHECK(game.machine().faults().empty());
}

// Port addition, precise aim (GameConfig::precise_aim): two NPCs in the
// direction of the shot, the nearer one a floor up, standing on a slab that
// cuts the line of fire. The original locks on the nearer one (the shot ends
// in the slab); with the option, on the one on the player's floor, which the
// shot hits.
void test_precise_aim(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    World& w = game.world();
    const AnimDat& anims = game.animations();
    const Item* player = w.get(1);
    std::vector<ObjId> victims;
    for (ObjId id = 2; id < 256 && victims.size() < 2; ++id) {
        const Item* it = w.get(id);
        if (it && !w.is_dead(id) && (it->z & 0xFF) != 254 && w.npc_hp(id) > 0 && w.is_trackable(it->shape))
            victims.push_back(id);
    }
    CHECK(victims.size() == 2);
    if (victims.size() != 2) return;
    const ObjId upstairs = victims[0], level = victims[1];
    std::vector<ObjId> others;
    for (ObjId id : w.tracked())
        if (id != 0 && id != upstairs && id != level) others.push_back(id);
    for (ObjId id : others) w.set_broken(*w.get(id));
    for (Pid pid = 1; pid < 400; ++pid)
        if (Process* p = game.kernel().get(pid); p && (p->owner() == upstairs || p->owner() == level)) p->fail();
    const int dir = 4;  // +x
    const std::int32_t px = player->x, py = player->y, pz = player->z & 0xFF;
    // NPCs of another map, brought here: put in the map's cells by the move.
    w.get(upstairs)->off_map = w.get(level)->off_map = false;
    w.move_item(upstairs, px + 256, py - 40, pz + 96);
    w.move_item(level, px + 320, py + 24, pz);
    w.npc_record(1)[npc::kDirection] = static_cast<std::uint8_t>(dir);
    w.npc_record(1)[npc::kLastAnim] = 8;
    w.npc_record(1)[npc::kLastAnim + 1] = 0;
    CHECK(find_target(w, 1, dir, true) == upstairs);  // the nearer (Chebyshev 256 against 320)

    // The upper floor: a slab (0x2E, 128 x 32 x 16) at the height of the
    // upper NPC's feet, placed where it cuts the line of fire to it only (the
    // muzzle is 64 above the player's feet: the line to the other NPC stays
    // under the slab).
    const ObjId slab = w.create(0x2E, 0);
    w.pop_to_coords(px, py, pz + 80);
    bool placed = false;
    for (int x = 64; x <= 192 && !placed; x += 16) {
        for (int y = -32; y <= 8 && !placed; y += 8) {
            w.move_item(slab, px + x, py + y, pz + 80);
            placed = fire_distance(w, anims, 1, upstairs, dir) == 0 && fire_distance(w, anims, 1, level, dir) != 0;
        }
    }
    const Item* si = w.get(slab);
    std::printf("precise aim: slab %u at (%d, %d, +80), cuts only the line to the upper NPC %d\n", slab,
                si ? si->x - px : 0, si ? si->y - py : 0, placed);
    CHECK(placed);
    if (!placed) return;
    // Another slab under the upper NPC's feet, or it falls down to the
    // player's floor (and becomes a fair target there).
    w.create(0x2E, 0);
    w.pop_to_coords(px + 256, py - 40, pz + 80);
    CHECK(fire_distance(w, anims, 1, level, dir) != 0);

    // Off (whatever CRUSADER.CFG says): the original choice, the nearer one.
    game.config().precise_aim = false;
    game.update(kFrameUs);
    CHECK(!w.precise_aim());
    CHECK(find_target(w, 1, dir, true, &anims) == upstairs);
    // On: the one in sight; the shot hits it, not the other.
    game.config().precise_aim = true;
    game.update(kFrameUs);
    CHECK(w.precise_aim());
    CHECK(find_target(w, 1, dir, true, &anims) == level);
    CHECK(find_target(w, 1, dir, true) == upstairs);  // without the line of fire check
    const int up_hp = w.npc_hp(upstairs), level_hp = w.npc_hp(level);
    bool hit = false;
    for (int shot = 0; shot < 5 && !hit; ++shot) {  // the pistol's spread may miss
        fire_from_muzzle(game, 1, dir, 11, true);
        for (int frames = 0; frames < 300 && projectiles(game) != 0; ++frames) game.update(kFrameUs);
        hit = w.npc_hp(level) < level_hp;
    }
    std::printf("precise aim: NPC on the floor hp %d -> %u, upper NPC hp %d -> %u at z %d\n", level_hp,
                w.npc_hp(level), up_hp, w.npc_hp(upstairs), w.get(upstairs)->z & 0xFF);
    CHECK(hit && w.npc_hp(upstairs) == up_hp);
    CHECK(game.machine().faults().empty());
}

// Milestone 6: the aiming mode and target designation (aim_mode.hpp), on
// SAVEGAME.13.
void test_aim_mode(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) game.update(kFrameUs);
    };
    run(10);
    World& w = game.world();
    const GameState::Aim& aim = w.state.aim;
    const Item* player = w.get(1);
    // One NPC that can be marked; the other tracked items out of the way.
    ObjId victim = 0;
    for (ObjId id = 2; id < 256 && !victim; ++id) {
        const Item* it = w.get(id);
        const ShapeInfo* info = it ? w.shape_info(it->shape) : nullptr;
        if (it && !it->off_map && !w.is_dead(id) && (it->z & 0xFF) != 254 && w.is_trackable(it->shape) && info &&
            info->is(ShapeInfo::kOccludes))
            victim = id;
    }
    CHECK(victim != 0);
    if (!victim) return;
    std::vector<ObjId> others;
    for (ObjId id : w.tracked())
        if (id != 0 && id != victim) others.push_back(id);
    for (ObjId id : others) w.set_broken(*w.get(id));
    for (Pid pid = 1; pid < 400; ++pid)
        if (Process* p = game.kernel().get(pid); p && p->owner() == victim) p->fail();  // keep it still
    w.move_item(victim, player->x, player->y + 600, player->z);  // behind the player

    // 1. The combat stance enters the mode: the crosshair, 200 units ahead.
    w.npc_record(1)[npc::kDirection] = 4;
    do_anim(game.kernel(), w, game.animations(), 1, 4, kDirCurrent);
    const ShapeInfo* pinfo = w.shape_info(player->shape);
    const int cx = player->x - pinfo->footpad_x * 8, cy = player->y - pinfo->footpad_y * 8;
    const ScreenSprite* cross = w.state.screen_sprites.get(aim.crosshair);
    CHECK(aim.active && cross && cross->shape == kCrosshairShape && aim.angle == 3);
    if (!cross) return;
    const int ex = cx + static_cast<int>((std::int64_t{w.trig().cos_deg(3)} * 200) >> 16);
    const int ey = cy + static_cast<int>((std::int64_t{w.trig().sin_deg(3)} * 200) >> 16);
    std::printf("aim mode: crosshair at (%d, %d, %d), player centre (%d, %d)\n", cross->x, cross->y, cross->z, cx, cy);
    CHECK(cross->x == ex && cross->y == ey && cross->x == cx + 199 && cross->y == cy + 9);  // entry 2: 2.8 degrees
    run(30);

    // 2. The right arrow turns the crosshair by 2 degrees (no Shift); the
    // player turns to face it (docs/bugs.md: "impossible de tourner").
    constexpr std::uint8_t kRight = 0x4D;
    for (int i = 0; i < 12; ++i) {
        game.key(kRight, true, true, 0);
        run(2);
        game.key(kRight, true, false, 0);
        run(2);
    }
    run(30);
    cross = w.state.screen_sprites.get(aim.crosshair);
    CHECK(aim.active && cross && aim.angle == 27);
    if (!cross) return;
    const int facing = direction_16(cx, cy, cross->x, cross->y);
    std::printf("aim mode: angle %d, crosshair (%d, %d), player direction %d (crosshair's %d)\n", aim.angle, cross->x,
                cross->y, w.npc_direction(1), facing);
    CHECK(facing == 5 && w.npc_direction(1) == 5);

    // 3. A target in that direction is marked within a second (DumbTimer).
    const int dir = 5;
    // Still on the floor: farther, the ground ends and it would fall (support
    // is checked at every ground frame, even standing).
    w.move_item(victim, player->x + kDirectionDx[dir] * 40, player->y + kDirectionDy[dir] * 40, player->z);
    CHECK(find_target(w, 1, dir, true) == victim);
    run(70);
    const Item* v = w.get(victim);
    const ScreenSprite* marker = w.state.screen_sprites.get(aim.marker);
    std::printf("aim mode: target %u at (%d, %d, %d), marker %s (%d, %d, %d) frame %d\n", aim.target, v->x, v->y,
                v->z & 0xFF, marker ? "at" : "missing", marker ? marker->x : 0, marker ? marker->y : 0,
                marker ? marker->z : 0, marker ? marker->frame : 0);
    CHECK(aim.target == victim && marker && marker->shape == SpriteProcess::kTargetMarker);
    if (!marker) return;
    // NPC footprint 3 x 3 x 10: height 10 / 2 + 1 = 6, square, so (x, y, z + 48).
    CHECK(marker->x == v->x && marker->y == v->y && marker->z == (v->z & 0xFF) + 48);
    run(70);  // frames 0..5, 10 ticks each, then it stays on frame 5
    CHECK(marker->frame == 5 && aim.marker_pid == 0 && w.state.screen_sprites.get(aim.marker) == marker);

    // 4. The marker follows its target.
    w.move_item(victim, v->x + 16, v->y + 8, v->z);
    CHECK(aim.target == victim && marker->x == v->x && marker->y == v->y);

    // 5. Destroyed, it is no longer marked.
    const int slot = aim.marker;
    w.destroy(victim);
    CHECK(aim.target == 0 && aim.marker == -1 && w.state.screen_sprites.get(slot) == nullptr);

    // 6. No target: the shot flies towards the crosshair.
    const Pid pid = fire_from_muzzle(game, 1, dir, 11, true);
    const auto* p = dynamic_cast<const ProjectileProcess*>(game.kernel().get(pid));
    cross = w.state.screen_sprites.get(aim.crosshair);
    CHECK(p != nullptr && cross != nullptr);
    if (!p || !cross) return;
    std::printf("aim mode: shot towards (%d, %d, %d), crosshair (%d, %d)\n", p->target().x, p->target().y,
                p->target().z, cross->x, cross->y);
    CHECK(std::abs(p->target().x - cross->x) <= 20 && std::abs(p->target().y - cross->y) <= 20);
    run(60);

    // 7. Walking leaves the mode: no crosshair.
    do_anim(game.kernel(), w, game.animations(), 1, 1, kDirCurrent);
    bool crosshair_left = false;
    for (const ScreenSprite& s : w.state.screen_sprites.all()) crosshair_left |= s.shape == kCrosshairShape;
    CHECK(!aim.active && aim.crosshair == -1 && !crosshair_left);
    CHECK(game.machine().faults().empty());
}

// Port addition: the right stick aims where it points on screen
// (aim_mode.cpp, aim_at_angle). The bug's save: the Silencer on a catwalk,
// soldier 110 on the floor below (88 lower), a camera (8430) in the
// neighbouring direction behind a pillar. The stick pushed at the soldier as
// the screen shows him used to give that neighbouring direction (a mere
// rotation of the screen, and the soldier's height left out): the shot went
// to the camera, or with precise aim to the crosshair, over his head.
void test_stick_aim_floor_below(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "visee-etage-inferieur.sav");
    GameConfig config = game.config();
    config.precise_aim = false;
    config.draw_on_aim = true;
    game.set_config(config);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    World& w = game.world();
    const AnimDat& anims = game.animations();
    const GameState::Aim& aim = w.state.aim;
    constexpr ObjId kSoldier = 110, kCamera = 8430;
    const Item* soldier = w.get(kSoldier);
    CHECK(soldier && soldier->shape == 0x371 && !w.is_dead(kSoldier) && w.get(kCamera));
    if (!soldier || !w.get(kCamera)) return;
    // Held still: the aim is checked, not the fight.
    for (Pid pid = 1; pid < 400; ++pid)
        if (Process* p = game.kernel().get(pid); p && p->owner() == kSoldier) p->fail();
    const Item* player = w.get(1);
    const ShapeInfo* info = w.shape_info(player->shape);
    const int cx = static_cast<std::uint16_t>(player->x - info->footpad_x * 8);
    const int cy = static_cast<std::uint16_t>(player->y - info->footpad_y * 8);
    // A screen angle of the offset (dx, dy, dz) from the crosshair's centre.
    auto screen_deg = [](double dx, double dy, double dz) {
        return std::atan2(-((dx + dy) / 4 - dz), (dx - dy) / 2) * 180.0 / 3.14159265358979;
    };

    // The world's x axis shows at 26.6 degrees below screen east: the stick
    // there puts the crosshair on it (a rotation gave 342 degrees).
    game.set_aim_stick(true, screen_deg(1, 0, 0));
    settle(game);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    const ScreenSprite* cross = w.state.screen_sprites.get(aim.crosshair);
    std::printf("stick aim: x axis -> angle %d, crosshair offset (%d, %d), direction %d\n", aim.angle,
                cross ? cross->x - cx : 0, cross ? cross->y - cy : 0, w.npc_direction(1) & 0xF);
    CHECK(aim.active && aim.angle == 0 && aim.stick_target == 0);
    CHECK(cross && std::abs(cross->y - cy) <= 1 && (w.npc_direction(1) & 0xF) == 4);
    CHECK(world_angle_of_screen(0) == 315 && world_angle_of_screen(-90) == 45 && world_angle_of_screen(90) == 225);

    // The soldier as the screen shows him, from the crosshair's height.
    const World::Point a = aim_point(w, 1, kSoldier);
    const double at = screen_deg(static_cast<std::int16_t>(a.x - cx), static_cast<std::int16_t>(a.y - cy),
                                 a.z - (static_cast<int>(cross ? cross->z : player->z)));
    const int soldier_dir = closest_direction_in_range(player->x, player->y, soldier->x, soldier->y, 16, 0, 15);
    // Precise aim: the stick takes him, the Silencer faces him, the shot and
    // the marker take him.
    config.precise_aim = true;
    game.set_config(config);
    game.set_aim_stick(true, at + 12);  // within the 20 degrees of the stick
    settle(game);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    const int dir = w.npc_direction(1) & 0xF;
    std::printf("stick aim: soldier at %.1f degrees on screen, direction %d (his %d), stick target %u, shot target %u\n",
                at, dir, soldier_dir, aim.stick_target, find_target(w, 1, dir, true, &anims));
    CHECK(aim.stick_target == kSoldier && dir == soldier_dir);
    CHECK(find_target(w, 1, dir, true, &anims) == kSoldier);
    const int hp = w.npc_hp(kSoldier);
    bool hit = false;
    for (int shot = 0; shot < 5 && !hit; ++shot) {  // the spread may miss
        fire_from_muzzle(game, 1, dir, 11, true);
        for (int frames = 0; frames < 300 && projectiles(game) != 0; ++frames) game.update(kFrameUs);
        hit = w.npc_hp(kSoldier) < hp;
    }
    std::printf("stick aim: soldier hp %d -> %u\n", hp, w.npc_hp(kSoldier));
    CHECK(hit);
    // The camera, in the next direction, stays out of reach behind its pillar:
    // the stick pointed at it takes nothing.
    game.set_aim_stick(true, at - 45);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    std::printf("stick aim: 45 degrees off, stick target %u\n", aim.stick_target);
    CHECK(aim.stick_target != kCamera && aim.stick_target != kSoldier);
    game.set_aim_stick(false, 0);
    settle(game);
    CHECK(game.machine().faults().empty());
}

// Port addition: pushing the aim stick of directional aiming draws the weapon
// by itself (aim_mode.cpp, aim_at_angle), without the fire key first.
void test_aim_stick_draws(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    GameConfig config = game.config();
    // SAVEGAME.1 ends on a shot (0x8, a combat stance): stand him up first so
    // that the weapon is holstered.
    auto holster = [&] {
        do_anim(game.kernel(), w, game.animations(), 1, 0, kDirCurrent);
        settle(game);
    };
    holster();
    const GameState::Aim& aim = w.state.aim;
    CHECK(!aim.active);
    CHECK(last_action(game) == 0);

    // Switched off: the stick leaves the holstered weapon alone.
    config.draw_on_aim = false;
    game.set_config(config);
    game.set_aim_stick(true, 0);  // screen east: a direction, no trigger
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    std::printf("draw on aim off: last action 0x%X, aim mode %d\n", last_action(game), aim.active);
    CHECK(!aim.active && last_action(game) == 0);

    // Switched on: pushing the stick draws it (action 7, or 0xC remapped).
    game.set_aim_stick(false, 0);
    config.draw_on_aim = true;
    game.set_config(config);
    game.set_aim_stick(true, 0);
    settle(game);
    const std::uint16_t raised = last_action(game);
    std::printf("draw on aim on: last action 0x%X, aim mode %d\n", raised, aim.active);
    CHECK(aim.active);
    // Action 7, left as is by a weapon that keeps the actions, else 0xC.
    CHECK(raised == 7 || raised == 0xC);

    // Moving while the stick aims (a strafe towards the crosshair, 0x24)
    // carries the crosshair along: it stays 200 units ahead of him, and he
    // keeps his direction instead of turning round where it was.
    auto offset = [&] {
        const Item* p = w.get(1);
        const ShapeInfo* info = w.shape_info(p->shape);
        const ScreenSprite* cross = w.state.screen_sprites.get(aim.crosshair);
        const int cx = static_cast<std::uint16_t>(p->x - info->footpad_x * 8);
        const int cy = static_cast<std::uint16_t>(p->y - info->footpad_y * 8);
        return std::pair<int, int>{cross->x - cx, cross->y - cy};
    };
    const auto before = offset();
    const int dir_before = w.npc_direction(1) & 0xF;
    const std::int32_t x0 = w.get(1)->x, y0 = w.get(1)->y;
    for (int step = 0; step < 4; ++step) {
        do_anim(game.kernel(), w, game.animations(), 1, 0x24, kDirCurrent);
        settle(game);
    }
    const auto after = offset();
    std::printf("aim stick while moving: moved (%d, %d), offset (%d, %d) -> (%d, %d), direction %d -> %d\n",
                w.get(1)->x - x0, w.get(1)->y - y0, before.first, before.second, after.first, after.second,
                dir_before, w.npc_direction(1) & 0xF);
    CHECK(w.get(1)->x != x0 || w.get(1)->y != y0);
    CHECK(std::abs(after.first - before.first) <= 1 && std::abs(after.second - before.second) <= 1);
    CHECK((w.npc_direction(1) & 0xF) == dir_before);

    // A side step (the strafe sideways, 0x1C) leaves the aiming mode until it
    // ends, but the crosshair stays shown, still ahead of him, at every frame.
    const std::int32_t x1 = w.get(1)->x, y1 = w.get(1)->y;
    int frames = 0, hidden = 0, off = 0;
    bool left_mode = false;
    for (int step = 0; step < 3; ++step) {
        do_anim(game.kernel(), w, game.animations(), 1, 0x1C, kDirCurrent);
        for (int i = 0; i < 400 && actor_busy(game.kernel(), 1); ++i) {
            const std::int32_t px = w.get(1)->x, py = w.get(1)->y;
            game.update(kFrameUs);
            ++frames;
            left_mode |= !aim.active;
            if (!w.state.screen_sprites.get(aim.crosshair)) {
                ++hidden;
                continue;
            }
            // Placed before the frame's slice moves him: on time, or one
            // frame behind, by the step he just took (up to 20 units a
            // side in this side step's frames).
            const auto o = offset();
            const int mx = w.get(1)->x - px, my = w.get(1)->y - py;
            const bool on_time = std::abs(o.first - before.first) <= 1 && std::abs(o.second - before.second) <= 1;
            const bool behind =
                std::abs(o.first + mx - before.first) <= 1 && std::abs(o.second + my - before.second) <= 1;
            if (!on_time && !behind) ++off;
        }
    }
    std::printf("aim stick side steps: moved (%d, %d), %d frames, crosshair hidden %d, off %d, mode left %d\n",
                w.get(1)->x - x1, w.get(1)->y - y1, frames, hidden, off, left_mode);
    CHECK(w.get(1)->x != x1 || w.get(1)->y != y1);
    CHECK(left_mode && hidden == 0 && off == 0);
    // Released, the stick leaves no crosshair out of the mode.
    do_anim(game.kernel(), w, game.animations(), 1, 0x1C, kDirCurrent);
    game.update(kFrameUs);
    game.update(kFrameUs);
    game.set_aim_stick(false, 0);
    game.update(kFrameUs);
    std::printf("aim stick released in a side step: aim mode %d, crosshair %d\n", aim.active, aim.crosshair);
    CHECK(aim.active == (aim.crosshair != -1));
    settle(game);
    CHECK(game.machine().faults().empty());
}

// A shot towards the crosshair flies level with the muzzle, upstairs too:
// Item::I_fireWeapon passes the shooter's z + the muzzle offset to
// SPRITE_1140_0e88 (docs/bugs.md: "tirer sur les caisses", the shot went
// into the floor at z 96). SAVEGAME.3: the player stands at z 96.
void test_crosshair_shot_height(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(3);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    World& w = game.world();
    const Item* player = w.get(1);
    CHECK((player->z & 0xFF) == 96);
    for (ObjId id : w.tracked())
        if (id != 0) w.set_broken(*w.get(id));  // nothing to aim at
    const int dir = w.npc_direction(1);
    do_anim(game.kernel(), w, game.animations(), 1, 4, kDirCurrent);
    CHECK(w.state.aim.active);
    const Pid pid = fire_from_muzzle(game, 1, dir, 11, true);
    const auto* p = dynamic_cast<const ProjectileProcess*>(game.kernel().get(pid));
    CHECK(p != nullptr);
    if (!p) return;
    std::printf("crosshair shot upstairs: from z %d towards z %d (player z %d)\n", p->position().z, p->target().z,
                player->z & 0xFF);
    CHECK(p->position().z > 96);
    CHECK(std::abs(p->target().z - p->position().z) <= 8);  // spread: Chebyshev 200 / 10 / 3
    CHECK(game.machine().faults().empty());
}

// Port addition: while the right stick aims with the weapon out, the
// controller's left stick strafes with the original's Alt moves (the gamepad
// layer's mapping is in input_tests.cpp). This checks the actions Alt + arrows
// give the player, and that the forward one keeps the aiming mode.
void test_weapon_out_movement(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    settle(game);
    CHECK(game.weapon_out());
    auto hold = [&](std::uint8_t scancode) {
        game.key(kScAlt, false, true, 0);
        game.key(scancode, true, true, kLeftAlt | kAlt);
        for (int i = 0; i < 12; ++i) game.update(kFrameUs);
        const std::uint16_t action = last_action(game);
        game.key(scancode, true, false, kLeftAlt | kAlt);
        game.key(kScAlt, false, false, 0);
        settle(game);
        return action;
    };
    // SAVEGAME.13's weapon does not keep the actions, so 0x24 becomes 0x2C and
    // 2 becomes 0x2D; a weapon that keeps them would play 0x24 and 2.
    const std::uint16_t forward = hold(0x48);
    const std::uint16_t backward = hold(0x50);
    const std::uint16_t left = hold(0x4B);
    const std::uint16_t right = hold(0x4D);
    std::printf("strafe: forward 0x%X, backward 0x%X, left 0x%X, right 0x%X\n", forward, backward, left, right);
    CHECK(forward == 0x24 || forward == 0x2C);
    CHECK(backward == 2 || backward == 0x2D);
    CHECK(left == 0x1C);
    CHECK(right == 0x1D);

    // The forward strafe is an aiming action: the aim mode stays on.
    do_anim(game.kernel(), game.world(), game.animations(), 1, 4, kDirCurrent);
    settle(game);
    CHECK(game.world().state.aim.active);
    game.key(kScAlt, false, true, 0);
    game.key(0x48, true, true, kLeftAlt | kAlt);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    CHECK(game.world().state.aim.active);
    game.key(0x48, true, false, kLeftAlt | kAlt);
    game.key(kScAlt, false, false, 0);
    settle(game);
    CHECK(game.machine().faults().empty());
}

// The save of the bug "left stick up, right stick down": the player aims
// (action 4, in combat) with the stance bit 0x02 clear, which only an NPC's
// AttackProcess sets. Game::weapon_out() read that bit, so the controller
// sent the keypad's Up (walk north) instead of the strafe while the stick
// aimed south: he turned north and back every few frames without moving.
// The weapon is out, and the backward strafe (Alt + Down) moves him north
// facing the aim.
void test_weapon_out_with_stance_bit_clear(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "pad-recul-en-visant-vers-le-bas.sav");
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    World& w = game.world();
    CHECK(!weapon_drawn(w, 1) && in_combat(w, 1));
    CHECK(game.weapon_out());
    const std::int32_t y0 = w.get(1)->y;
    game.key(kScAlt, false, true, 0);
    game.key(0x50, true, true, kLeftAlt | kAlt);
    int turned = 0;
    for (int i = 0; i < 60; ++i) {
        game.set_aim_stick(true, -90);
        game.update(kFrameUs);
        turned += (w.npc_direction(1) & 0xF) != 6;
    }
    game.key(0x50, true, false, kLeftAlt | kAlt);
    game.key(kScAlt, false, false, 0);
    std::printf("backward while aiming south: last action 0x%X, moved y %d, frames turned away %d\n", last_action(game),
                w.get(1)->y - y0, turned);
    CHECK(last_action(game) == 2 || last_action(game) == 0x2D);
    CHECK(w.get(1)->y < y0 - 32);
    CHECK(turned == 0);
    CHECK(game.machine().faults().empty());
}

// ActorAnim_init (1158:016F): the controlled NPC is in combat while it plays
// a combat action, which shows its weapon (WPNOVLAY.DAT).
void test_player_combat_flag(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(3);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    World& w = game.world();
    w.npc_record(1)[npc::kCombatFlags] &= static_cast<std::uint8_t>(~0x08);
    do_anim(game.kernel(), w, game.animations(), 1, 4, kDirCurrent);
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    CHECK(in_combat(w, 1));
    do_anim(game.kernel(), w, game.animations(), 1, 0, kDirCurrent);
    for (int i = 0; i < 10; ++i) game.update(kFrameUs);
    CHECK(!in_combat(w, 1));
    CHECK(game.machine().faults().empty());
}

// Issue #16 (mission 5, English version): the contact who gives the keycard
// played his movie but stayed unseen. MISS5 (egg type 0x11) destroys its egg,
// then looks around it for the item 0x21E that leads to the door the contact
// (NPC 82) comes out by. The original reads the centre's x/y in the item
// arrays, which a free slot keeps; the port found nothing around a destroyed
// item, so the door stayed shut and the contact walked into it, behind the
// wall. The save comes from the English 1.01, whose usecode globals are not
// laid out as the French ones: the global MISS5 reads the player from
// (0x3C) is set back.
void test_mission5_contact_comes_out(const std::filesystem::path& data) {
    constexpr ObjId kEgg = 6703, kContact = 82;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "contact-invisible-mission5-issue16.sav");
    World& w = game.world();
    const Item* egg = w.get(kEgg);
    const Item* contact = w.get(kContact);
    CHECK(egg && egg->quality == 0x11 && contact && contact->shape == 0x4E0);
    if (!egg || !contact) return;
    const std::int32_t hidden_y = contact->y;
    game.machine().globals()[0x3C] = 1;
    game.machine().globals()[0x3D] = 0;
    w.teleport(1, egg->x, egg->y, egg->z, w.current_map());
    int frames = 0;
    while (!game.movie() && frames < 900) {
        game.update(kFrameUs);
        ++frames;
    }
    contact = w.get(kContact);
    const Item* player = w.get(1);
    std::printf("mission 5 contact: movie after %d frames, egg %s, contact y %d -> %d (player y %d)\n", frames,
                w.get(kEgg) ? "kept" : "gone", hidden_y, contact ? contact->y : 0, player->y);
    CHECK(game.movie() != nullptr && !w.get(kEgg));
    CHECK(contact && !contact->off_map && (contact->flags & kItemFastArea));
    CHECK(contact && contact->y > hidden_y + 200 && std::abs(contact->y - player->y) < 64);
}

// Issue #8: with a weapon, the run is armed (3 -> 0x31) and so is its stop
// (0x27 -> 0x19, Actor_remap_action 1130:2105). The original, seen under
// DOSBox (SEB3, holstered, Shift + Up then Up released), ends the stop in its
// combat stance with the weapon out, whether it was out before the run or
// not: nothing holsters it.
void test_run_stop_stays_armed(const std::filesystem::path& data) {
    for (const bool drawn : {false, true}) {
        Game game(data, test_saves(data));
        game.load_save(3);
        game.config().toggle_run = false;
        World& w = game.world();
        for (int i = 0; i < 5; ++i) game.update(kFrameUs);
        do_anim(game.kernel(), w, game.animations(), 1, drawn ? 4 : 0, kDirCurrent);
        settle(game);
        CHECK(in_combat(w, 1) == drawn);
        game.key(kScShift, false, true, kLeftShift);
        game.key(0x48, true, true, kLeftShift);
        for (int i = 0; i < 40; ++i) game.update(kFrameUs);
        game.key(0x48, true, false, kLeftShift);  // the stop, run button still held
        settle(game);
        std::printf("run stop, weapon out before %d: last action 0x%X, in combat %d\n", drawn, last_action(game),
                    in_combat(w, 1));
        CHECK(last_action(game) == 0x19);
        CHECK(in_combat(w, 1));
        game.key(kScShift, false, false, 0);
        CHECK(game.machine().faults().empty());
    }
}

// ITEM.C 2970: before a move, what rests on the mover falls and what it rests
// on gets event 9 ("release"). Telepad 3319 of SAVEGAME.1 (TELEPAD) clears
// the "teleporting" flag in it once nobody stands on it any more.
void test_leave_surface(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) game.update(kFrameUs);
    };
    run(10);
    const Item& pad = *w.get(3319);
    CHECK(pad.shape == 0x1DB);
    const ShapeInfo* pi = w.shape_info(pad.shape);
    const ShapeInfo* ai = w.shape_info(w.get(1)->shape);
    const std::int32_t px = pad.x - (pi->footpad_x - ai->footpad_x) * 8;
    const std::int32_t py = pad.y - (pi->footpad_y - ai->footpad_y) * 8;
    w.move_item(1, px, py, pad.z + pi->footpad_z * 8);
    w.state.crusader_teleporting = true;
    run(30);
    CHECK(w.state.crusader_teleporting);  // standing still: no release

    // A step on the pad: released, but the player is still on it.
    do_anim(game.kernel(), w, game.animations(), 1, 1, 2);
    run(4);
    const bool kept = w.state.crusader_teleporting;
    for (int i = 0; i < 90; ++i) {
        if (!actor_busy(game.kernel(), 1)) do_anim(game.kernel(), w, game.animations(), 1, 1, 2);
        game.update(kFrameUs);
    }
    const Item& p = *w.get(1);
    std::printf("leave surface: player (%d, %d) -> (%d, %d), flag on the pad %d, after %d\n", px, py, p.x, p.y, kept,
                w.state.crusader_teleporting);
    CHECK(kept);
    CHECK(!w.state.crusader_teleporting);

    // An item put on the player's head falls when the player moves.
    ObjId loose = 0;
    w.for_each([&](const Item& i) {
        const ShapeInfo* info = w.shape_info(i.shape);
        if (!loose && !is_npc_id(i.id) && (i.flags & kItemFastArea) && !(i.flags & kItemContained) && info &&
            !info->is(ShapeInfo::kFixed) && info->weight != 0 && info->family != 4 && info->family != 8) {
            loose = i.id;
        }
    });
    CHECK(loose != 0);
    w.move_item(loose, p.x, p.y, p.z + ai->footpad_z * 8);
    leave_surface(game.kernel(), w, 1);
    CHECK(game.kernel().find_process(loose, GravityProcess::kType) != nullptr);
    CHECK(game.machine().faults().empty());
}

// Milestone 4: enemies fight back (ATTACK.C), on SAVEGAME.13.
std::vector<ObjId> attackers(Game& game) {
    std::vector<ObjId> ids;
    game.kernel().for_each_process([&](const Process& p) {
        if (p.type() == AttackProcess::kType) ids.push_back(p.owner());
    });
    return ids;
}

void test_enemies(const std::filesystem::path& data) {
    // 1. NPC 2 guards (activity 9: in combat, weapon drawn, stays put). Put
    // 80 direction steps in front of the player, it enters the fast area, gets its
    // attack process, turns to the player and fires.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        const int dir = w.npc_direction(1) & 0xE;
        w.move_item(2, player->x + kDirectionDx[dir] * 80, player->y + kDirectionDy[dir] * 80, player->z);
        const auto ids = attackers(game);
        CHECK(ids.size() == 1 && ids[0] == 2 && in_combat(w, 2) && w.npc_word(2, npc::kTarget) == 1);
        const int hp = w.npc_hp(1);
        const int shots = game.shots_fired();
        int first_shot = -1;
        for (int f = 0; f < 600; ++f) {
            game.update(kFrameUs);
            if (first_shot < 0 && game.shots_fired() > shots) first_shot = f;
        }
        const int facing = w.npc_direction(2);
        const Item* guard = w.get(2);
        const int towards = closest_direction_in_range(guard->x, guard->y, player->x, player->y, 16, 0, 15);
        std::printf("enemies: guard faces %d (player at %d), first shot after %d frames, %d shots in 10 s, "
                    "player hp %d -> %u\n", facing, towards, first_shot, game.shots_fired() - shots, hp, w.npc_hp(1));
        CHECK(facing == towards);
        CHECK(first_shot >= 0 && game.shots_fired() - shots >= 3);
        CHECK(w.npc_hp(1) < hp);
        CHECK(game.machine().faults().empty());
    }
    // 2. NPC 6 stands (activity 0, default activities 1, 5, 5). Hit by the
    // player, it takes its default activity 2 (5: combat) and targets the
    // player.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        w.state.difficulty = 0;  // no x5: the NPC survives the hit
        const Item* player = w.get(1);
        const int dir = w.npc_direction(1) & 0xE;
        w.move_item(6, player->x + kDirectionDx[dir] * 64, player->y + kDirectionDy[dir] * 64, player->z);
        CHECK(attackers(game).empty() && !in_combat(w, 6));
        apply_damage(game.kernel(), w, game.animations(), 6, 1, dir, 10, 11);
        const auto ids = attackers(game);
        std::printf("enemies: hit NPC 6: activity %u, in combat %d, target %u, hp %u\n", w.npc_word(6, npc::kActivity),
                    in_combat(w, 6), w.npc_word(6, npc::kTarget), w.npc_hp(6));
        CHECK(ids.size() == 1 && ids[0] == 6);
        CHECK(w.npc_word(6, npc::kActivity) == 5 && in_combat(w, 6) && w.npc_word(6, npc::kTarget) == 1);
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        CHECK(game.machine().faults().empty());
    }
    // 4. NPC 16 (activity 5, armed) put 750 units east of the player, off
    // screen: it walks towards the player (path finder), stops when it can
    // shoot, and fires.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        w.move_item(16, player->x + 750, player->y, player->z);
        const Item* npc = w.get(16);
        const int shots = game.shots_fired();
        const int start = std::abs(npc->x - player->x);
        bool walked = false;
        int first_shot = -1;
        for (int f = 0; f < 8 * 60; ++f) {
            game.update(kFrameUs);
            walked = walked || game.kernel().count_processes(16, PathfinderProcess::kType) != 0;
            if (first_shot < 0 && game.shots_fired() > shots) first_shot = f;
        }
        const int end = std::abs(npc->x - player->x);
        std::printf("enemies: walker from %d to %d units, first shot after %d frames, %d shots, player hp %u\n",
                    start, end, first_shot, game.shots_fired() - shots, w.npc_hp(1));
        CHECK(walked && end < start - 64);
        // Whether a shot hits depends on the draws: with the generator state
        // saved in SAVEGAME.13, the 16 shots of these 8 s miss (the other
        // states tried all hit the player).
        CHECK(first_shot > 0 && game.shots_fired() - shots >= 3);
        CHECK(game.machine().faults().empty());
    }
    // 5. Items (DAMAGE.FLX): barrel 2370 (shape 0x1FD, 50 hit points) loses
    // 20, then breaks: it explodes and is replaced by shape 0x568 frame 1.
    // Item 2427 (0x2A5, 10 hit points) explodes with splash damage and shows
    // its next frame.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        w.state.difficulty = 0;  // damage as given
        Item* barrel = w.get(2370);
        CHECK(barrel && barrel->shape == 0x1FD && barrel->hit_points == 50);
        if (!barrel) return;
        const World::Point at{barrel->x, barrel->y, barrel->z};
        apply_damage(game.kernel(), w, game.animations(), 2370, 1, 0, 20, 11);
        CHECK(w.get(2370) && w.get(2370)->hit_points == 30);
        apply_damage(game.kernel(), w, game.animations(), 2370, 1, 0, 40, 11);
        ObjId replacement = 0;
        for (ObjId id : w.rect_search(at.x, at.y, at.x, at.y))
            if (w.get(id)->shape == 0x568) replacement = id;
        std::printf("items: barrel replaced %d, replacement %u frame %u\n", replacement != 0, replacement,
                    replacement ? w.get(replacement)->frame : 0);
        // (The replacement may reuse the freed id 2370.)
        CHECK(replacement != 0 && w.get(replacement)->frame == 1 && (w.get(replacement)->flags & 0x0100) == 0);

        Item* box = w.get(2427);
        CHECK(box && box->shape == 0x2A5 && box->hit_points == 10);
        if (!box) return;
        std::vector<std::pair<ObjId, std::uint8_t>> around;
        for (ObjId id : w.rect_search(box->x - 80, box->y - 80, box->x + 80, box->y + 80))
            if (id != 2427 && !w.damage_record(w.get(id)->shape).empty()) around.emplace_back(id, w.get(id)->hit_points);
        const std::uint16_t frame = box->frame;
        apply_damage(game.kernel(), w, game.animations(), 2427, 1, 0, 20, 11);
        int hurt = 0;
        for (auto [id, hp] : around)
            if (!w.get(id) || w.get(id)->hit_points != hp || (w.get(id)->flags & 0x100)) ++hurt;
        std::printf("items: box frame %u -> %u, flags 0x%04X, %d of %zu items around damaged\n", frame, box->frame,
                    box->flags, hurt, around.size());
        CHECK(box->frame == frame + 1 && (box->flags & 0x0140) == 0x0140);
        // Explosion sprites: running, then gone with their items.
        const std::size_t sprites = game.kernel().count_processes(0, SpriteProcess::kType);
        std::size_t sprite_items = 0;
        w.for_each([&](const Item& i) { sprite_items += i.sprite_only; });
        for (int f = 0; f < 180; ++f) game.update(kFrameUs);
        std::size_t left = 0;
        w.for_each([&](const Item& i) { left += i.sprite_only; });
        std::printf("items: %zu sprites (%zu items), %zu sprite items left after 3 s\n", sprites, sprite_items, left);
        CHECK(sprites >= 2 && sprite_items >= 2 && game.kernel().count_processes(0, SpriteProcess::kType) == 0 &&
              left == 0);
        CHECK(game.machine().faults().empty());
    }
    // 6. Weapon overlay: the player of SAVEGAME.13 (in combat, shotgun, last
    // action 0x19) is drawn with a piece of shape 0x33B (class 4).
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const WeaponOverlay overlay = WeaponOverlay::load(data / "STATIC/WPNOVLAY.DAT");
        const auto r = w.npc_record(1);
        const std::uint16_t last = static_cast<std::uint16_t>(r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8);
        const Item* weapon = w.get(w.active_weapon(1));
        const AnimAction* header = game.animations().action(1, last);
        CHECK(weapon && header && in_combat(w, 1));
        if (weapon && header) {
            const auto piece = overlay.piece(weapon->shape, last, *header, w.npc_direction(1), r[npc::kAnimFrame]);
            std::printf("overlay: weapon %u action 0x%X dir %u frame %u -> shape 0x%X frame %u offset (%d, %d)\n",
                        weapon->shape, last, w.npc_direction(1), r[npc::kAnimFrame], piece ? piece->shape : 0,
                        piece ? piece->frame : 0, piece ? piece->dx : 0, piece ? piece->dy : 0);
            CHECK(piece && piece->shape == 0x33B);
        }
    }
    // 7. Guided missile (fire type 9) shot by NPC 2 two directions off: it
    // turns towards the player (its target) and hits.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        w.move_item(2, player->x + 160, player->y, player->z);  // east of the player
        for (Pid pid = 1; pid < 200; ++pid)
            if (Process* p = game.kernel().get(pid); p && p->owner() == 2) p->fail();  // keep NPC 2 still
        const int hp = w.npc_hp(1);
        const int west = 12, off = (west + 2) & 0xF;
        const Pid pid = fire_from_muzzle(game, 2, off, 9, true);
        const auto* missile = dynamic_cast<const ProjectileProcess*>(game.kernel().get(pid));
        CHECK(missile != nullptr);
        int min_distance = 1 << 20, frames = 0;
        for (; frames < 600 && game.kernel().get(pid); ++frames) {
            if (const auto* m = dynamic_cast<const ProjectileProcess*>(game.kernel().get(pid)))
                min_distance = std::min(min_distance, std::max(std::abs(m->position().x - player->x),
                                                               std::abs(m->position().y - player->y)));
            game.update(kFrameUs);
        }
        std::printf("missile: closest %d units, ended after %d frames, player hp %d -> %u\n", min_distance, frames, hp,
                    w.npc_hp(1));
        CHECK(min_distance < 64 && w.npc_hp(1) < hp);
    }
    // 8. Ricochet (fire type 5) off a shape with TYPEFLAG byte 1 bit 2
    // (0x2E) put east of the player: the flight turns back west.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        const ObjId wall = w.create(0x2E, 0);
        w.pop_to_coords(player->x + 160, player->y + 16, player->z);
        const Pid pid = fire_from_muzzle(game, 1, 4, 5, false);
        const auto* p = dynamic_cast<const ProjectileProcess*>(game.kernel().get(pid));
        CHECK(p != nullptr && p->target().x > player->x);
        bool turned = false;
        for (int f = 0; f < 120 && game.kernel().get(pid); ++f) {
            game.update(kFrameUs);
            if (const auto* m = dynamic_cast<const ProjectileProcess*>(game.kernel().get(pid)))
                turned = turned || m->target().x < m->position().x;
        }
        std::printf("ricochet: wall %u, flight turned back %d\n", wall, turned);
        CHECK(wall != 0 && turned);
    }
    // 9. Special deaths of NPC 2 (shape 765): fire (type 6) -> burnt shape
    // 0x58F; explosive (type 4, 250) -> blown apart, shape 0x576; electric
    // (type 15) -> shape 0x59C.
    for (const auto [type, shape] : {std::pair{6, 0x58F}, std::pair{4, 0x576}, std::pair{15, 0x59C}}) {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        w.move_item(2, player->x + 128, player->y, player->z);
        apply_damage(game.kernel(), w, game.animations(), 2, 1, 12, 250, type);
        // The shape as long as the NPC lies there: at the end of the death
        // NPCDEATH sets it aside (test_death_called_from_anim).
        std::uint16_t seen = w.get(2)->shape;
        for (int f = 0; f < 600 && game.kernel().count_processes(2, ActorAnimProcess::kType) != 0; ++f) {
            game.update(kFrameUs);
            if (const Item* it = w.get(2)) seen = it->shape;
        }
        std::printf("death: type %d -> shape 0x%X, dead %d, last action 0x%X\n", type, seen, w.is_dead(2),
                    w.npc_word(2, npc::kLastAnim));
        CHECK(w.is_dead(2) && seen == shape);
        CHECK(game.machine().faults().empty());
    }
    // 10. An explosive hit (type 4) throws an item with a weight (a pistol,
    // shape 0x32E) along the hit's direction (east, 4): its gravity process
    // gets vx = 2 x 10..25, vy = 0.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        const ObjId gun = w.create(0x32E, 0);
        w.pop_to_coords(player->x + 96, player->y, player->z);
        apply_damage(game.kernel(), w, game.animations(), gun, 0, 4, 5, 4);
        const auto* g = dynamic_cast<const GravityProcess*>(game.kernel().find_process(gun, GravityProcess::kType));
        std::printf("thrown: velocity (%d, %d, %d)\n", g ? g->vx : 0, g ? g->vy : 0, g ? g->vz : 0);
        CHECK(g && g->vx >= 20 && g->vx <= 50 && g->vy == 0 && g->vz == 0);
    }
    // 11. Activities outside combat: NPC 8 loiters (activity 2), NPC 5 walks
    // straight on (activity 3, facing east), both east of the player. The
    // loiter process checks each second (its 1 Hz timer, TYPE_1108_0d3e
    // event 2, also while it walks) whether it sees the player: NPC 8 does
    // and takes its default activity 1. The port never looked while
    // loitering.
    for (const auto [id, facing] : {std::pair{ObjId{8}, -1}, std::pair{ObjId{5}, 4}}) {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        if (facing >= 0) w.npc_record(id)[npc::kDirection] = static_cast<std::uint8_t>(facing);
        // The player stands on a walkway (z 16) running south over a shaft
        // with a death floor; NPC 8 loiters on it.
        if (facing < 0) w.move_item(id, player->x + 48, player->y + 192, player->z);
        else w.move_item(id, player->x + 256, player->y - 40, player->z);
        const Item* npc = w.get(id);
        const int x0 = npc->x, y0 = npc->y;
        int moved = 0;
        for (int f = 0; f < 600; ++f) {
            game.update(kFrameUs);
            moved = std::max(moved, std::max(std::abs(npc->x - x0), std::abs(npc->y - y0)));
        }
        std::printf("activity: NPC %u moved up to %d units, now activity %u\n", id, moved, w.npc_word(id, npc::kActivity));
        if (id == 8) CHECK(w.npc_word(id, npc::kActivity) == w.npc_word(id, npc::kDefaultActivity + 2) && moved > 0);
        else CHECK(moved > 64);
        CHECK(game.machine().faults().empty());
    }
    // 12. A guard (activity 8) facing the player sees it and attacks (5).
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        auto r = w.npc_record(12);
        r[npc::kActivity] = 8;
        r[npc::kActivity + 1] = 0;
        r[npc::kDirection] = 12;  // west, towards the player
        w.move_item(12, player->x + 256, player->y - 40, player->z);
        const std::uint16_t before = w.npc_word(12, npc::kActivity);
        for (int f = 0; f < 240; ++f) game.update(kFrameUs);
        std::printf("activity: guard 12 activity %u -> %u, in combat %d\n", before, w.npc_word(12, npc::kActivity),
                    in_combat(w, 12));
        CHECK(before == 8 && w.npc_word(12, npc::kActivity) == 5 && in_combat(w, 12));
    }
    // 13. Actor::I_createActorCru: an NPC of the type of shape 765 made at a
    // spawner east of the player, facing 4, with its default activities,
    // hit points and an active weapon.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        const auto types = dtable::NpcTypes::load(data / "STATIC/DTABLE.FLX");
        const int type = types.index(765);
        const ObjId egg = w.create(0x1, 0);  // any item will do as a spawner
        w.pop_to_coords(player->x + 256, player->y - 40, player->z);
        Item* e = w.get(egg);
        e->quality = 0x0107;      // default activity 1 = 1, record +0x63 = 7
        e->map_number = 0;        // from difficulty 1, first weapon
        e->npc_number = 0x32;     // facing 4, record +0x12 = 3
        const ObjId kind = w.create(0x1, 0);
        w.pop_to_coords(0, 0, 0);
        w.get(kind)->npc_number = static_cast<std::uint8_t>(type);
        w.get(kind)->quality = 0x0800;  // default activity 0 = 8
        w.get(kind)->map_number = 5;    // default activity 2 = 5
        const ObjId npc = create_actor(game.kernel(), w, game.animations(), types, egg, kind);
        const Item* n = w.get(npc);
        std::printf("create: type %d -> NPC %u shape %u at (%d, %d), dir %u, hp %u, activities %u %u %u, weapon %u\n",
                    type, npc, n ? n->shape : 0, n ? n->x : 0, n ? n->y : 0, w.npc_direction(npc), w.npc_hp(npc),
                    w.npc_word(npc, 6), w.npc_word(npc, 8), w.npc_word(npc, 10), w.active_weapon(npc));
        CHECK(n && n->shape == 765 && n->x == e->x && w.npc_direction(npc) == 4 && w.npc_hp(npc) == types.max_hp(type));
        CHECK(w.npc_word(npc, 6) == 8 && w.npc_word(npc, 8) == 1 && w.npc_word(npc, 10) == 5 && w.active_weapon(npc) != 0);
        for (int f = 0; f < 60; ++f) game.update(kFrameUs);
        CHECK(game.machine().faults().empty());

        // The hit points come from the first record of the NPC's shape
        // (NPC_10f0_2610, DTABLE_1120_010a): type 32 shares shape 0x528 with
        // type 22 but has 0 hit points of its own.
        const ObjId kind32 = w.create(0x1, 0);
        w.pop_to_coords(0, 0, 0);
        w.get(kind32)->npc_number = 32;
        const ObjId npc32 = create_actor(game.kernel(), w, game.animations(), types, egg, kind32);
        const int first = types.index(0x528);
        std::printf("create: type 32 -> shape 0x%X, hp %u (record %d: %u)\n", w.get(npc32) ? w.get(npc32)->shape : 0,
                    w.npc_hp(npc32), first, types.max_hp(first));
        CHECK(w.get(npc32) && w.get(npc32)->shape == 0x528 && first != 32 && types.max_hp(32) == 0);
        CHECK(w.npc_hp(npc32) == types.max_hp(first) && w.npc_hp(npc32) != 0);
    }
    // 14. A monster egg (shape 0x24F, family 7) brought into the fast area
    // makes its NPC once: type of shape 765 (template inside the egg),
    // default activity 0 = 8 (guard), facing 2.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        const auto types = dtable::NpcTypes::load(data / "STATIC/DTABLE.FLX");
        const ObjId egg = w.create(0x24F, 0);
        w.pop_to_coords(200, 200, 0);  // far from the fast area
        Item* e = w.get(egg);
        e->npc_number = 0x01;  // facing 2
        e->map_number = 0;
        const ObjId kind = w.create(0x1, 0);
        w.pop_to_end(egg);
        w.get(kind)->npc_number = static_cast<std::uint8_t>(types.index(765));
        w.get(kind)->quality = 0x0801;  // default activities 0 = 8, 1 = 1
        int before = 0;
        for (ObjId id = 2; id < 256; ++id) before += w.get(id) != nullptr;
        w.move_item(egg, player->x + 256, player->y - 40, player->z);
        ObjId made = 0;
        for (ObjId id = 2; id < 256; ++id)
            if (const Item* i = w.get(id); i && i->x == e->x && i->y == e->y && i->shape == 765) made = id;
        std::printf("egg: frame %u, NPC %u, activity %u, weapon %u, guard process %d\n", e->frame, made,
                    w.npc_word(made, npc::kActivity), w.active_weapon(made),
                    game.kernel().find_process(made, 0x25E) != nullptr);
        CHECK(e->frame == 1 && made != 0 && w.npc_word(made, npc::kActivity) == 8 && w.active_weapon(made) != 0);
        w.move_item(egg, 200, 200, 0);
        w.move_item(egg, player->x + 256, player->y - 40, player->z);  // a spent egg makes nothing
        int after = 0;
        for (ObjId id = 2; id < 256; ++id) after += w.get(id) != nullptr;
        CHECK(after == before + 1);
        for (int f = 0; f < 60; ++f) game.update(kFrameUs);
        CHECK(game.machine().faults().empty());
    }
    // 16. World::I_setControlledNPCNum: NPC 2 (a guard) takes the player's
    // commands (an arrow key moves it, not the player), the camera follows
    // it; then the player gets them back.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        const Item* player = w.get(1);
        w.move_item(2, player->x + 256, player->y - 40, player->z);
        const Item start2 = *w.get(2), start1 = *player;
        set_controlled_npc(game.kernel(), w, game.animations(), 2);
        CHECK(w.state.controlled_npc == 2 && w.state.camera_target == 2);
        game.key(0x48, true, true, 0);
        for (int f = 0; f < 90; ++f) game.update(kFrameUs);
        game.key(0x48, true, false, 0);
        for (int f = 0; f < 30; ++f) game.update(kFrameUs);
        const Item& now2 = *w.get(2);
        std::printf("control: NPC 2 (%d, %d) -> (%d, %d), player moved %d\n", start2.x, start2.y, now2.x, now2.y,
                    std::abs(player->x - start1.x) + std::abs(player->y - start1.y));
        CHECK(now2.x != start2.x || now2.y != start2.y);
        CHECK(player->x == start1.x && player->y == start1.y);
        // Killed while controlled: no NPC is controlled any more.
        apply_damage(game.kernel(), w, game.animations(), 2, 1, 0, 250, 11);
        CHECK(w.is_dead(2) && w.state.controlled_npc == 0);
        set_controlled_npc(game.kernel(), w, game.animations(), 1);
        CHECK(w.state.controlled_npc == 1 && w.state.camera_target == 1);
        CHECK(game.machine().faults().empty());
    }
    // 15. Vargas's shield: NPC 6 made Vargas (0x3AC) loses no hit points, the
    // shield flashes (action 0x21) and loses the damage; a hit during the
    // flash does nothing; with the shield empty, hits hurt.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        w.state.difficulty = 0;
        w.set_shape(*w.get(6), 0x3AC);
        const int hp = w.npc_hp(6);
        apply_damage(game.kernel(), w, game.animations(), 6, 1, 0, 30, 11);
        const auto* anim =
            dynamic_cast<const ActorAnimProcess*>(game.kernel().find_process(6, ActorAnimProcess::kType));
        const int after_first = w.state.vargas_shield;
        apply_damage(game.kernel(), w, game.animations(), 6, 1, 0, 30, 11);
        std::printf("vargas: hp %d -> %u, shield 5000 -> %d -> %d, action 0x%X\n", hp, w.npc_hp(6), after_first,
                    w.state.vargas_shield, anim ? anim->action() : 0);
        CHECK(w.npc_hp(6) == hp && after_first == 5000 - 30 && w.state.vargas_shield == after_first);
        CHECK(anim && anim->action() == 0x21);
        w.state.vargas_shield = 0;
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        apply_damage(game.kernel(), w, game.animations(), 6, 1, 0, 30, 11);
        CHECK(w.npc_hp(6) == hp - 30);
    }
    // 3. Shields: with shield type 1 (record +0x66), a pistol shot (fire type
    // 11: cost 80, types 7) takes 80 energy instead of hit points; without
    // enough energy it hurts. Immortality (+0x13 bit 1) stops everything.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        int player_damage_events = 0;
        int last_player_damage = 0;
        WorldHooks hooks = w.hooks();
        hooks.player_damaged = [&](int damage) {
            ++player_damage_events;
            last_player_damage = damage;
        };
        w.set_hooks(std::move(hooks));
        auto r = w.npc_record(1);
        r[0x66] = 1;
        r[0x67] = 0;
        const int hp = w.npc_hp(1), energy = w.npc_mana(1);
        apply_damage(game.kernel(), w, game.animations(), 1, 2, 0, 20, 11);
        std::printf("enemies: shield: hp %d -> %u, energy %d -> %u\n", hp, w.npc_hp(1), energy, w.npc_mana(1));
        CHECK(w.npc_hp(1) == hp && w.npc_mana(1) == energy - 80 && player_damage_events == 0);
        apply_damage(game.kernel(), w, game.animations(), 1, 2, 0, 20, 11);  // 50 left: not enough
        CHECK(w.npc_hp(1) == hp - 4 && w.npc_mana(1) == energy - 80 && player_damage_events == 1 &&
              last_player_damage == 4);   // 20 / 5 at difficulty 1
        r[0x66] = 0;
        r[npc::kFlags13] |= 0x02;
        apply_damage(game.kernel(), w, game.animations(), 1, 2, 0, 20, 11);
        CHECK(w.npc_hp(1) == hp - 4 && player_damage_events == 1);
    }
}

// The alert (World::I_setAlertActive): using an alarm box (ALARMBOX, shape
// 0x477) on a map raises the alert through the scripts (FREE event 0x3B);
// alarm lights and boxes on the map change frame with it.
void test_alert(const std::filesystem::path& data) {
    for (int save = 1; save <= 13; ++save) {
        Game game(data, test_saves(data));
        game.load_save(save);
        World& w = game.world();
        ObjId box = 0;
        int lights = 0;
        w.for_each([&](const Item& i) {
            if (i.parent != 0) return;
            if (i.shape == 0x477 && box == 0 && i.frame < 2) box = i.id;
            if (i.shape == 0x21 || i.shape == 0x49 || i.shape == 0x174 || i.shape == 0x271) lights += i.frame == 0;
        });
        if (box == 0 || lights == 0 || w.state.alert_active) continue;
        const int box_frame = w.get(box)->frame;
        const std::uint8_t raise[2] = {1, 0};  // equip(1): raise the alert
        game.machine().start_event(0x477, 0x0A, box, raise);
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        int lit = 0;
        w.for_each([&](const Item& i) {
            if (i.parent == 0 && (i.shape == 0x21 || i.shape == 0x49 || i.shape == 0x174 || i.shape == 0x271))
                lit += i.frame == 1;
        });
        std::printf("alert: save %d, box %u frame %u -> %u, alert %d, %d lights off, %d lit, %zu faults\n", save, box,
                    box_frame, w.get(box)->frame, w.state.alert_active, lights, lit, game.machine().faults().size());
        for (const auto& [n, c] : game.machine().unbound_intrinsic_calls()) std::printf("  unbound 0x%03X x%u\n", n, c);
        CHECK(w.state.alert_active && w.get(box)->frame == box_frame + 2 && lit >= lights);
        game.machine().start_event(0x477, 0x01, box);  // use(): switch it off
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        int still = 0;
        w.for_each([&](const Item& i) {
            if (i.parent == 0 && (i.shape == 0x21 || i.shape == 0x49 || i.shape == 0x174 || i.shape == 0x271))
                still += i.frame == 1;
        });
        std::printf("alert: switched off: alert %d, box frame %u, %d lit\n", w.state.alert_active, w.get(box)->frame,
                    still);
        CHECK(!w.state.alert_active && w.get(box)->frame == box_frame && still == 0);
        CHECK(game.machine().faults().empty());
        return;
    }
    CHECK(!"no save with an alarm box");
}

// Wall lasers (LAZERNS, 0x19B): "equip" (event 0x0A) with 1 starts their
// loop (event 0x20), which fires south (direction 8) through
// Item::I_fireWeapon (intrinsic 0x01E) every (5 - difficulty) x 20 ticks.
void test_wall_lasers(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    ObjId laser = 0;
    w.for_each([&](const Item& i) {
        if (!laser && i.shape == 0x19B && i.frame < 4 && !i.parent) laser = i.id;
    });
    CHECK(laser != 0);
    if (!laser) return;
    const std::uint8_t one[] = {1, 0, 0, 0};
    CHECK(game.machine().start_event(0x19B, 0x0A, laser, one) != 0);
    // A shot is a projectile process that was not there on the frame before.
    std::set<Pid> previous;
    int shots = 0;
    bool southwards = true;
    const Item* l = w.get(laser);
    const std::uint32_t start = game.kernel().ticks();
    for (int f = 0; f < 900; ++f) {
        game.update(kFrameUs);
        std::set<Pid> now;
        game.kernel().for_each_process([&](const Process& p) {
            if (p.type() != ProjectileProcess::kType) return;
            now.insert(p.pid());
            shots += !previous.count(p.pid());
            const auto& shot = static_cast<const ProjectileProcess&>(p);
            if (shot.target().y <= l->y) southwards = false;
        });
        previous = std::move(now);
    }
    std::printf("lasers: laser %u (frame %u) fired %d shots in %u ticks, difficulty %d\n", laser, l->frame, shots,
                game.kernel().ticks() - start, w.state.difficulty);
    CHECK(shots >= 2 && southwards);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Port addition (barrel_leaks.hpp): the explosive barrel 5070 of
// SAVEGAME.3 (BARRELUP 0xAA) blows up at 100 damage; with the option a
// bullet does it 3/5 of its damage. Each bullet before the last pierces it
// (the shot lands on it: WorldHooks::shot_landed) and its fuel runs out into
// a pool; when it blows up it throws burning drops and its pool catches
// fire; whoever stands in the fire burns; the pool leaves a burnt stain.
void test_barrel_leaks(const std::filesystem::path& data) {
    constexpr ObjId kBarrel = 5070;
    CHECK(barrel_bullet_damage(0xAA, 30, 2) == 18 && barrel_bullet_damage(0xAA, 30, 4) == 30);
    Game game(data, test_saves(data));
    game.config().barrel_leaks = true;
    game.load_save(3);
    World& w = game.world();
    w.state.difficulty = 3;
    const Item* barrel = w.get(kBarrel);
    CHECK(barrel && barrel->shape == 0xAA);
    if (!barrel) return;
    const World::Point spot = w.location(*barrel);
    // The Silencer out of the blast's reach.
    World::Point away = w.location(*w.get(1));
    bool far = false;
    for (int ring = 720; ring <= 1600 && !far; ring += 32) {
        for (int k = 0; k < 16 && !far; ++k) {
            const World::Point at{spot.x + kDirectionDx[k] * ring / 2, spot.y + kDirectionDy[k] * ring / 2, spot.z};
            far = position_free(w, w.get(1)->shape, at, nullptr, 1);
            if (far) away = at;
        }
    }
    CHECK(far);
    w.move_item(1, away.x, away.y, away.z);
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    const auto run = [&](int frames) {
        for (int f = 0; f < frames; ++f) {
            game.update(kFrameUs);
            game.render(screen);
        }
    };
    // A bullet (fire type 2, 30 damage) in the barrel's side, from (dx, dy) at height dz.
    const auto hit = [&](int dx, int dy, int dz) {
        apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 30, 2);
        if (w.hooks().shot_landed) w.hooks().shot_landed(kBarrel, spot.x + dx, spot.y + dy, spot.z + dz, 2);
    };
    const BarrelLeaks& leaks = game.barrel_leaks();
    hit(8, 8, 20);
    run(90);
    std::printf("barrel leaks: after a hit, damage byte %u, %zu barrel(s), %zu pool(s)\n",
                w.get(kBarrel) ? w.get(kBarrel)->npc_number : 0, leaks.barrels().size(), leaks.pools().size());
    CHECK(w.get(kBarrel) && w.get(kBarrel)->npc_number == 18);
    CHECK(leaks.barrels().size() == 1 && leaks.pools().size() == 1);
    if (leaks.barrels().size() != 1 || leaks.pools().size() != 1) return;
    CHECK(leaks.barrels()[0].holes.size() == 1 && leaks.barrels()[0].level < leaks.barrels()[0].height * 0.9);
    hit(20, 2, 10);
    run(300);
    double extent = 0;
    for (const BarrelLeaks::Lobe& l : leaks.pools()[0].lobes) extent = std::max(extent, l.r);
    std::printf("barrel leaks: 2 holes, level %.1f, pool %.1f\n", leaks.barrels()[0].level, extent);
    CHECK(extent > 20 && !leaks.pools()[0].burning);
    // 18 a bullet: 6 to blow it up (4 without the option).
    int hits = 2;
    for (; hits < 12 && w.get(kBarrel) && w.get(kBarrel)->shape == 0xAA; ++hits) {
        hit(8, 8, 20);
        run(10);
    }
    CHECK(!w.get(kBarrel) || w.get(kBarrel)->shape != 0xAA);
    std::size_t most_drops = leaks.drops().size();
    for (int f = 0; f < 40; ++f) {
        run(1);
        most_drops = std::max(most_drops, leaks.drops().size());
    }
    std::printf("barrel leaks: blown up after %d bullets, up to %zu drops in flight, %zu pools, %zu fires on walls\n",
                hits, most_drops, leaks.pools().size(), leaks.wall_fires().size());
    CHECK(hits >= 6 && most_drops >= 10 && !leaks.pools().empty() && leaks.pools()[0].burning);
    // The Silencer walks into the fire: he catches fire and burns, and goes
    // out a second after he left it.
    // A free place where the pool burns well (not on the barrel's debris).
    const BarrelLeaks::Pool& pool = leaks.pools()[0];
    const ShapeInfo* body = w.shape_info(w.get(1)->shape);
    const int half_x = body ? body->footpad_x * 8 : 0, half_y = body ? body->footpad_y * 8 : 0;
    World::Point in_fire{};
    bool burning = false;
    for (int r = 0; r <= 160 && !burning; r += 8) {
        for (int k = 0; k < 16 && !burning; ++k) {
            const World::Point at{spot.x + kDirectionDx[k] * r / 2, spot.y + kDirectionDy[k] * r / 2, w.get(1)->z};
            burning = leaks.fire_at(pool, at.x - half_x, at.y - half_y, game.kernel().ticks()) > 0.5 &&
                      position_free(w, w.get(1)->shape, {at.x, at.y, spot.z}, nullptr, 1);
            if (burning) in_fire = {at.x, at.y, spot.z};
        }
    }
    CHECK(burning);
    const int hp = w.npc_hp(1);
    w.move_item(1, in_fire.x, in_fire.y, in_fire.z);
    run(45);
    const int burnt_hp = w.npc_hp(1);
    std::printf("barrel leaks: the Silencer in the fire, %s, hit points %d -> %d\n",
                leaks.on_fire(1) ? "on fire" : "not on fire", hp, burnt_hp);
    CHECK(leaks.on_fire(1) && burnt_hp < hp);
    w.move_item(1, away.x, away.y, away.z);
    run(BarrelLeaks::kAfterBurn + 5);
    CHECK(!leaks.on_fire(1));
    run(900);
    CHECK(!leaks.pools().empty() && leaks.pools()[0].burnt);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Port addition (barrel_leaks.hpp): barrel 5070 of SAVEGAME.3, pierced
// twice, its pool set alight (a blast's reach, the hook only: no damage).
// The barrel stands in the fire: it heats up and blows up after kCookMin to
// kCookMin + kCookMore ticks in it. TRAIL_SHOTS=<folder>: pictures of it.
void test_barrel_cook_off(const std::filesystem::path& data) {
    constexpr ObjId kBarrel = 5070;
    Game game(data, test_saves(data));
    game.config().barrel_leaks = true;
    game.load_save(3);
    World& w = game.world();
    w.state.difficulty = 3;
    const Item* barrel = w.get(kBarrel);
    CHECK(barrel && barrel->shape == 0xAA);
    if (!barrel) return;
    const ShapeInfo* info = w.shape_info(barrel->shape);
    CHECK(info != nullptr);
    if (!info) return;
    // Out on the open floor a little way off the Silencer, in sight.
    const World::Point me = w.location(*w.get(1));
    World::Point spot{};
    bool open = false;
    for (int ring = 256; ring <= 1024 && !open; ring += 32) {
        for (int k = 0; k < 16 && !open; ++k) {
            const World::Point at{me.x + kDirectionDx[k] * ring / 2, me.y + kDirectionDy[k] * ring / 2, me.z};
            open = position_free(w, barrel->shape, at, nullptr, kBarrel) &&
                   !position_free(w, barrel->shape, {at.x, at.y, at.z - 1}, nullptr, kBarrel);
            for (int j = 0; j < 16 && open; j += 2) {
                for (int d = 32; d <= 128 && open; d += 32) {
                    const World::Point by{at.x + kDirectionDx[j] * d / 2, at.y + kDirectionDy[j] * d / 2, at.z};
                    open = position_free(w, barrel->shape, by, nullptr, kBarrel) &&
                           !position_free(w, barrel->shape, {by.x, by.y, by.z - 1}, nullptr, kBarrel);
                }
            }
            if (open) spot = at;
        }
    }
    CHECK(open);
    if (!open) return;
    w.move_item(kBarrel, spot.x, spot.y, spot.z);
    const double cx = spot.x - info->footpad_x * 8.0, cy = spot.y - info->footpad_y * 8.0;
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    const char* shots = std::getenv("TRAIL_SHOTS");
    const auto run = [&](int frames) {
        for (int f = 0; f < frames; ++f) {
            game.update(kFrameUs);
            game.render(screen);
        }
    };
    const auto hit = [&](int dx, int dy, int dz) {
        apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 30, 2);
        if (w.hooks().shot_landed) w.hooks().shot_landed(kBarrel, spot.x + dx, spot.y + dy, spot.z + dz, 2);
    };
    const BarrelLeaks& leaks = game.barrel_leaks();
    hit(8, 8, 20);
    run(90);
    hit(20, 2, 10);
    run(300);
    CHECK(leaks.pools().size() == 1 && leaks.heating().empty());
    if (leaks.pools().size() != 1) return;
    // Lit at its lobe farthest from the barrel.
    const BarrelLeaks::Pool& pool = leaks.pools()[0];
    const BarrelLeaks::Lobe* lit = nullptr;
    for (const BarrelLeaks::Lobe& l : pool.lobes)
        if (l.r > 0 && (!lit || std::hypot(l.x - cx, l.y - cy) > std::hypot(lit->x - cx, lit->y - cy))) lit = &l;
    CHECK(lit != nullptr);
    if (!lit) return;
    w.hooks().shot_landed(0, static_cast<std::int32_t>(lit->x), static_cast<std::int32_t>(lit->y), pool.z, 4);
    const std::uint32_t lit_at = game.kernel().ticks();
    CHECK(leaks.pools()[0].burning);
    std::uint32_t heated = 0, blown = 0;
    float most = 0;
    bool shot = false;
    for (int f = 0; f < 2400 && !blown; ++f) {
        run(1);
        const std::uint32_t now = game.kernel().ticks();
        for (const BarrelLeaks::Heating& h : leaks.heating()) {
            if (h.id != kBarrel) continue;
            if (!heated) heated = now;
            most = std::max(most, h.heat / h.fuse);
            if (shots && !shot && h.heat > 0.8f * h.fuse) {
                write_png(std::filesystem::path(shots) / "cook-off-heating.png", screen, game.palette());
                shot = true;
            }
        }
        const Item* b = w.get(kBarrel);
        if (!b || b->shape != 0xAA) blown = now;
    }
    if (shots) {
        run(6);
        write_png(std::filesystem::path(shots) / "cook-off-blast.png", screen, game.palette());
    }
    std::printf("barrel cook-off: lit at tick %u, the barrel heating from %u, blown up at %u (%u ticks in the fire, "
                "%.0f%% of its fuse seen)\n",
                lit_at, heated, blown, blown && heated ? blown - heated : 0, most * 100);
    CHECK(heated && blown && blown - heated >= BarrelLeaks::kCookMin &&
          blown - heated <= BarrelLeaks::kCookMin + BarrelLeaks::kCookMore + 30);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Port addition (barrel_leaks.hpp): barrel 5070 of SAVEGAME.3, whole,
// blown up at once. Part of its fuel runs out where it stood, in a pool
// that spreads out over kSpreadTicks (no longer at its full size at once);
// the rest (kThrown) flies all round and splashes, burning, a few metres
// away. TRAIL_SHOTS=<folder>: pictures of the blast.
void test_barrel_splash(const std::filesystem::path& data) {
    constexpr ObjId kBarrel = 5070;
    Game game(data, test_saves(data));
    game.config().barrel_leaks = true;
    game.load_save(3);
    World& w = game.world();
    w.state.difficulty = 3;
    const Item* barrel = w.get(kBarrel);
    CHECK(barrel && barrel->shape == 0xAA);
    if (!barrel) return;
    const std::uint16_t shape = barrel->shape;
    const ShapeInfo* info = w.shape_info(shape);
    CHECK(info != nullptr);
    if (!info) return;
    // Out on the open floor a little way off the Silencer, in sight: free
    // and held up there, free all round it.
    const World::Point me = w.location(*w.get(1));
    World::Point spot{};
    bool open = false;
    for (int ring = 256; ring <= 1024 && !open; ring += 32) {
        for (int k = 0; k < 16 && !open; ++k) {
            const World::Point at{me.x + kDirectionDx[k] * ring / 2, me.y + kDirectionDy[k] * ring / 2, me.z};
            open = position_free(w, shape, at, nullptr, kBarrel) &&
                   !position_free(w, shape, {at.x, at.y, at.z - 1}, nullptr, kBarrel);
            for (int j = 0; j < 16 && open; j += 2) {
                for (int d = 32; d <= 160 && open; d += 32) {
                    const World::Point by{at.x + kDirectionDx[j] * d / 2, at.y + kDirectionDy[j] * d / 2, at.z};
                    open = position_free(w, shape, by, nullptr, kBarrel) &&
                           !position_free(w, shape, {by.x, by.y, by.z - 1}, nullptr, kBarrel);
                }
            }
            if (open) spot = at;
        }
    }
    CHECK(open);
    if (!open) return;
    std::printf("barrel splash: the barrel at (%d, %d, %d), %d units off the Silencer\n", spot.x, spot.y, spot.z,
                static_cast<int>(std::hypot(spot.x - me.x, spot.y - me.y)));
    w.move_item(kBarrel, spot.x, spot.y, spot.z);
    const double cx = spot.x - info->footpad_x * 8.0, cy = spot.y - info->footpad_y * 8.0;
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    const char* shots = std::getenv("TRAIL_SHOTS");
    // On to `ticks` game ticks later.
    const auto run = [&](std::uint32_t ticks, const char* name = nullptr) {
        const std::uint32_t until = game.kernel().ticks() + ticks;
        for (int f = 0; f < 2000 && game.kernel().ticks() < until; ++f) {
            game.update(kFrameUs);
            game.render(screen);
        }
        if (shots && name) write_png(std::filesystem::path(shots) / name, screen, game.palette());
    };
    run(5, "splash-before.png");
    const BarrelLeaks& leaks = game.barrel_leaks();
    CHECK(leaks.pools().empty());
    // A blast (fire type 4): it blows up whole.
    apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 250, 4);
    for (int f = 0; f < 10 && leaks.spreads().empty(); ++f) run(1);
    CHECK(leaks.spreads().size() == 1);
    if (leaks.spreads().size() != 1) return;
    const BarrelLeaks::Spread s = leaks.spreads()[0];
    const BarrelLeaks::Pool* pool = leaks.pool(s.pool);
    CHECK(pool && s.lobe < pool->lobes.size());
    if (!pool || s.lobe >= pool->lobes.size()) return;
    const double first = pool->lobes[s.lobe].r;
    // What it held, all of it at once before.
    const double area = info->footpad_x * 16.0 * info->footpad_y * 16.0 / 3.14159265358979;
    const double whole = std::min(160.0, 0.6 * std::sqrt(area * info->footpad_z * 8.0 * 0.9 / 0.45));
    run(4, "splash-0.png");
    run(BarrelLeaks::kSpreadTicks);
    pool = leaks.pool(s.pool);
    CHECK(pool != nullptr);
    if (!pool) return;
    const double spread = pool->lobes[s.lobe].r;
    std::printf("barrel splash: its pool %.1f at first, %.1f spread out (to %.1f; %.1f all at once before)\n", first,
                spread, s.to, whole);
    CHECK(first < 0.4 * s.to && leaks.spreads().empty() && std::abs(spread - s.to) < 0.01 && s.to < 0.8 * whole &&
          pool->burning);
    // The drops land: splashes all round, burning, a few metres off.
    run(15, "splash-1.png");
    run(30, "splash-2.png");
    int splashes = 0, sectors = 0;
    double farthest = 0;
    bool sector[8] = {};
    for (const BarrelLeaks::Pool& p : leaks.pools()) {
        if (p.serial == s.pool || p.lobes.empty()) continue;
        const double dx = p.lobes[0].x - cx, dy = p.lobes[0].y - cy, d = std::hypot(dx, dy);
        if (d < 24) continue;
        ++splashes;
        farthest = std::max(farthest, d);
        const int k = static_cast<int>(std::floor((std::atan2(dy, dx) + 3.14159265358979) / (2 * 3.14159265358979) * 8)) & 7;
        if (!sector[k]) ++sectors;
        sector[k] = true;
    }
    std::printf("barrel splash: %d splashes in %d of 8 directions, the farthest %.0f units off, %zu wall fires\n",
                splashes, sectors, farthest, leaks.wall_fires().size());
    CHECK(splashes >= 8 && sectors >= 5 && farthest > 60);
    run(90, "splash-3.png");
    run(400, "splash-4.png");
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Port addition (barrel_leaks.hpp): barrel 5070 of SAVEGAME.3 (baril2)
// blown up on the floor of its room just behind the room's east wall
// (7939, 7940, shape 9, x 30703..30719, the side the camera sees drawn
// black): its fire and the drops' burning behind the wall are hidden by it
// (a patch of fire 32 units a side poked into the wall and came in front of
// it, its flames up the black wall), and no fuel runs on past the wall (a
// pool's field went under it, a drop splashing against it streaked through
// it, and burnt on the far side). TRAIL_SHOTS=<folder>: a picture of the
// worst frame, if any fire shows on the wall.
void test_fire_behind_wall(const std::filesystem::path& data) {
    constexpr ObjId kBarrel = 5070;
    constexpr std::int32_t kWallX = 30703;  // the wall's x_left
    Game game(data, test_saves(data));
    game.config().barrel_leaks = true;
    game.load_save(3);
    World& w = game.world();
    w.state.difficulty = 3;
    const Item* barrel = w.get(kBarrel);
    CHECK(barrel && barrel->shape == 0xAA);
    if (!barrel) return;
    const std::uint16_t shape = barrel->shape;
    const auto is_wall = [&](ObjId id) { return id == 7939 || id == 7940; };
    for (ObjId id : {7939, 7940}) {
        const Item* wall = w.get(static_cast<ObjId>(id));
        CHECK(wall && wall->shape == 9 && wall->x == kWallX + 16);
        if (!wall) return;
    }
    // On the floor as near the wall as it is free (66 units off).
    World::Point spot{};
    bool found = false;
    for (std::int32_t x = kWallX - 2; x >= kWallX - 96 && !found; x -= 2) {
        for (std::int32_t y = 20000; y <= 20220 && !found; y += 4) {
            const World::Point at{x, y, 96};
            found = position_free(w, shape, at, nullptr, kBarrel) &&
                    !position_free(w, shape, {at.x, at.y, at.z - 1}, nullptr, kBarrel);
            if (found) spot = at;
        }
    }
    CHECK(found);
    if (!found) return;
    w.move_item(kBarrel, spot.x, spot.y, spot.z);
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    const BarrelLeaks& leaks = game.barrel_leaks();
    const auto frame = [&] {
        game.update(kFrameUs);
        game.render(screen);
    };
    for (int f = 0; f < 10; ++f) frame();
    apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 250, 4);
    // The fire drawn over the wall, once the drops have landed (those in
    // flight are drawn over everything) while the pools burn.
    int over = 0, burning = 0, frames = 0, worst = 0;
    const char* shots = std::getenv("TRAIL_SHOTS");
    const std::uint32_t until = game.kernel().ticks() + 240;
    for (int f = 0; f < 4000 && game.kernel().ticks() < until; ++f) {
        frame();
        if (!leaks.drops().empty()) continue;
        const GameState& gs = w.state;
        const auto list = build_display_list(w, game.shapes(), View{gs.view_left, gs.view_top}, screen.width,
                                             screen.height);
        int here = 0;
        std::vector<std::uint8_t> seen(static_cast<std::size_t>(screen.width) * screen.height, 0);
        for (const auto& [index, colour] : leaks.glowing()) {
            if (seen[index]) continue;  // drawn over by more fire
            seen[index] = 1;
            const int x = static_cast<int>(index % static_cast<std::uint32_t>(screen.width));
            const int y = static_cast<int>(index / static_cast<std::uint32_t>(screen.width));
            if (screen.row(y)[x] != colour) continue;  // drawn over by what stands in front
            const auto hits = pick(list, x, y);
            if (!hits.empty() && is_wall(list[hits.back()].item->id)) ++here;
        }
        ++frames;
        over += here;
        if (here > worst) {
            worst = here;
            if (shots) write_png(std::filesystem::path(shots) / "fire-behind-wall.png", screen, game.palette());
        }
        for (const BarrelLeaks::Pool& p : leaks.pools()) burning += p.burning;
    }
    // Every pool's fuel behind the wall: none on its other side.
    int pools = 0, behind = 0;
    for (const BarrelLeaks::Pool& p : leaks.pools()) {
        ++pools;
        bool all = true;
        for (int j = 0; j < p.grid_h; ++j)
            for (int i = 0; i < p.grid_w; ++i)
                if (p.fuel[static_cast<std::size_t>(j) * p.grid_w + i] >= 0)
                    all = all && p.grid_x + (i + 0.5) * BarrelLeaks::kGrid < kWallX;
        behind += all;
    }
    std::printf("fire behind wall: barrel at (%d, %d, %d), %d pools (%d behind the wall), %d frames, %d fire pixels "
                "over the wall (%d at most in a frame)\n",
                spot.x, spot.y, spot.z, pools, behind, frames, over, worst);
    CHECK(pools > 0 && behind == pools && frames > 100 && burning > 0);
    CHECK(over == 0);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Leaking barrels: barrel 5070 of SAVEGAME.3, pierced twice, then thrown
// along the floor (as a blast throws it, Item::I_hurl, gravity 2) lays a
// trail of fuel in its pool from where it stood to where it stops; it blows
// up there, and the fire runs back along the trail.
void test_barrel_trail(const std::filesystem::path& data) {
    constexpr ObjId kBarrel = 5070;
    Game game(data, test_saves(data));
    game.config().barrel_leaks = true;
    game.load_save(3);
    World& w = game.world();
    w.state.difficulty = 3;
    const Item* barrel = w.get(kBarrel);
    CHECK(barrel && barrel->shape == 0xAA);
    if (!barrel) return;
    const World::Point spot = w.location(*barrel);
    const std::uint16_t shape = barrel->shape;
    World::Point away = w.location(*w.get(1));
    bool far = false;
    for (int ring = 720; ring <= 1600 && !far; ring += 32) {
        for (int k = 0; k < 16 && !far; ++k) {
            const World::Point at{spot.x + kDirectionDx[k] * ring / 2, spot.y + kDirectionDy[k] * ring / 2, spot.z};
            far = position_free(w, w.get(1)->shape, at, nullptr, 1);
            if (far) away = at;
        }
    }
    w.move_item(1, away.x, away.y, away.z);
    // A way along the floor: free and held up all along, away from the
    // camera (a grenade from the player throws it so), away from its jets.
    int dir = -1;
    for (int k : {12, 13, 14, 11, 15, 10, 0, 9, 1}) {
        if (dir >= 0) break;
        bool clear = true;
        for (int d = 8; d <= 128 && clear; d += 8) {
            const World::Point at{spot.x + kDirectionDx[k] * d / 2, spot.y + kDirectionDy[k] * d / 2, spot.z};
            clear = position_free(w, shape, at, nullptr, kBarrel) &&
                    !position_free(w, shape, {at.x, at.y, at.z - 1}, nullptr, kBarrel);
        }
        if (clear) dir = k;
    }
    CHECK(dir >= 0);
    if (dir < 0) return;
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    const auto run = [&](int frames) {
        for (int f = 0; f < frames; ++f) {
            game.update(kFrameUs);
            game.render(screen);
        }
    };
    const auto hit = [&](int dx, int dy, int dz) {
        apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 30, 2);
        if (w.hooks().shot_landed) w.hooks().shot_landed(kBarrel, spot.x + dx, spot.y + dy, spot.z + dz, 2);
    };
    const BarrelLeaks& leaks = game.barrel_leaks();
    // Pierced, then a blast throws it as hard as it can (DS:26F8 x 25: 50
    // units a step), before its pool has spread far: the gravity process
    // waits 20 ticks before it moves it.
    hit(8, 8, 20);
    hit(20, 2, 10);
    const int step = 50 / std::max(std::abs(kDirectionDx[dir]), std::abs(kDirectionDy[dir]));
    hurl(game.kernel(), w, kBarrel, kDirectionDx[dir] * step, kDirectionDy[dir] * step, 0, 2);
    run(10);
    CHECK(leaks.barrels().size() == 1 && leaks.pool(leaks.barrels()[0].pool));
    if (leaks.barrels().size() != 1 || !leaks.pool(leaks.barrels()[0].pool)) return;
    const std::uint32_t serial = leaks.barrels()[0].pool;
    // Till it comes to rest (its jets then start a pool there, over the
    // trail's end as it grows).
    bool rolled_on = false;
    for (int f = 0; f < 200 && leaks.barrels().size() == 1; ++f) {
        run(1);
        if (leaks.barrels()[0].rolling) rolled_on = true;
        else if (rolled_on) break;
    }
    CHECK(rolled_on);
    const Item* moved = w.get(kBarrel);
    CHECK(moved && moved->shape == 0xAA);
    if (!moved || leaks.barrels().size() != 1) return;
    const BarrelLeaks::Barrel& b = leaks.barrels()[0];
    const double dx = moved->x - spot.x, dy = moved->y - spot.y;
    const double rolled = std::sqrt(dx * dx + dy * dy);
    const BarrelLeaks::Pool* pool = leaks.pool(serial);
    CHECK(pool != nullptr);
    if (!pool) return;
    std::size_t drops = 0;
    for (const BarrelLeaks::Lobe& l : pool->lobes) drops += l.trail;
    // The floor at the foot of its first hole, a tenth of the way at a time:
    // the points wet by the trail only (dry without its drops).
    BarrelLeaks::Pool dry;
    dry.seed = pool->seed;
    for (const BarrelLeaks::Lobe& l : pool->lobes)
        if (!l.trail) dry.lobes.push_back(l);
    const BarrelLeaks::Hole& first = b.holes.front();
    const double foot_x = first.x + first.ux * 3, foot_y = first.y + first.uy * 3;
    std::vector<std::pair<double, double>> laid;
    for (int t = 1; t <= 9; ++t) {
        const double x = spot.x + dx * t / 10 + foot_x, y = spot.y + dy * t / 10 + foot_y;
        if (BarrelLeaks::field(dry, x, y) < 1 && BarrelLeaks::field_at(*pool, x, y) >= 1) laid.emplace_back(x, y);
    }
    std::printf("barrel trail: thrown %.0f units (direction %d), %zu drops in pool %u, %zu of 9 points of its way "
                "wet by the trail only, %s, same pool %s\n",
                rolled, dir, drops, serial, laid.size(), b.rolling ? "rolling" : "at rest",
                b.pool == serial ? "yes" : "no");
    CHECK(rolled >= 48 && drops >= 5 && laid.size() >= 3 && !b.rolling && b.pool == serial);
    // At rest, its jets make a pool where it lies.
    CHECK(pool->lobes.size() > drops + 10);
    // It blows up where it stopped; the trail burns back to where it stood.
    for (int hits = 2; hits < 12 && w.get(kBarrel) && w.get(kBarrel)->shape == 0xAA; ++hits) {
        apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 30, 2);
        run(10);
    }
    CHECK(!w.get(kBarrel) || w.get(kBarrel)->shape != 0xAA);
    std::vector<double> hottest(laid.size(), 0.0);
    for (int f = 0; f < 150; ++f) {
        run(1);
        if (const BarrelLeaks::Pool* p = leaks.pool(serial))
            for (std::size_t k = 0; k < laid.size(); ++k)
                hottest[k] = std::max(hottest[k], leaks.fire_at(*p, laid[k].first, laid[k].second, game.kernel().ticks()));
    }
    std::size_t burnt = 0;
    std::printf("barrel trail: blown up, the trail burns");
    for (double h : hottest) {
        std::printf(" %.2f", h);
        burnt += h > 0.2;
    }
    std::printf("\n");
    // Up to where it stood (none under its debris, at the far end).
    CHECK(!laid.empty() && hottest.front() > 0.2 && burnt * 2 >= laid.size());
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Leaking barrels: a rolling barrel (ROLLB_NS 0x586, 6960, the one lying by
// the upright barrel 6959 in save baril-roule-sans-trainee.sav) shot with
// the Silencer's BA-40 rolls along its way (its class, event 0x21) and,
// pierced, leaves a trail of fuel behind it; it blows up where it stops and
// the fire runs back along the trail. TRAIL_SHOTS=<folder>: pictures of the
// roll and of the fire there.
void test_rolling_barrel_trail(const std::filesystem::path& data) {
    constexpr ObjId kBarrel = 6960;
    Game game(data, test_saves(data));
    game.config().barrel_leaks = true;
    game.load_save_file(test_saves(data) / "baril-roule-sans-trainee.sav");
    World& w = game.world();
    const Item* barrel = w.get(kBarrel);
    CHECK(barrel && barrel->shape == 0x586);
    if (!barrel) return;
    const World::Point start = w.location(*barrel);
    const Item* me = w.get(1);
    w.npc_record(1)[npc::kDirection] = direction_16(me->x, me->y, barrel->x, barrel->y);
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    const char* shots = std::getenv("TRAIL_SHOTS");
    const auto shot = [&](const char* name) {
        if (shots) write_png(std::filesystem::path(shots) / name, screen, game.palette());
    };
    const auto frame = [&] {
        game.update(kFrameUs);
        game.render(screen);
    };
    const BarrelLeaks& leaks = game.barrel_leaks();
    // One shot.
    game.key(0x39, false, true, 0, ' ');
    for (int f = 0; f < 20; ++f) frame();
    game.key(0x39, false, false, 0, 0);
    // It rolls till it blows up.
    World::Point last = start;
    bool rolled = false, pierced = false;
    int f = 0;
    for (; f < 900; ++f) {
        frame();
        const Item* b = w.get(kBarrel);
        if (!b || b->shape != 0x586) break;
        rolled = rolled || b->x != start.x || b->y != start.y;
        pierced = pierced || (!leaks.barrels().empty() && !leaks.barrels()[0].holes.empty());
        last = w.location(*b);
        if (f == 40) shot("rolling-1.png");
        if (f == 80) shot("rolling-2.png");
    }
    shot("rolling-3-end.png");
    const double dx = last.x - start.x, dy = last.y - start.y;
    const double way = std::sqrt(dx * dx + dy * dy);
    // The trail's drops, in whatever pool, along the way it rolled.
    std::vector<std::pair<double, double>> drops;
    for (const BarrelLeaks::Pool& p : leaks.pools())
        for (const BarrelLeaks::Lobe& l : p.lobes)
            if (l.trail) drops.emplace_back(l.x, l.y);
    double nearest = 1e9, farthest = 0;
    for (const auto& [x, y] : drops) {
        const double along = ((x - start.x) * dx + (y - start.y) * dy) / std::max(way, 1.0);
        nearest = std::min(nearest, along);
        farthest = std::max(farthest, along);
    }
    std::printf("rolling barrel: rolled %s %.0f units in %d frames, pierced %s, %zu drops from %.0f to %.0f along it, "
                "%zu pools\n",
                rolled ? "yes" : "no", way, f, pierced ? "yes" : "no", drops.size(), nearest, farthest,
                leaks.pools().size());
    CHECK(rolled && pierced && way > 200 && drops.size() >= 20 && nearest < 0.25 * way && farthest > 0.75 * way);
    if (drops.empty()) return;
    // The fire runs back along it: four drops a quarter of the way apart,
    // from where it stopped.
    std::vector<std::pair<double, double>> marks;
    std::sort(drops.begin(), drops.end(), [&](const auto& a, const auto& b) {
        return (a.first - start.x) * dx + (a.second - start.y) * dy < (b.first - start.x) * dx + (b.second - start.y) * dy;
    });
    for (int k = 0; k < 4; ++k) marks.push_back(drops[(drops.size() - 1) * k / 3]);
    std::vector<double> hottest(marks.size(), 0.0);
    std::vector<int> first(marks.size(), -1);
    for (int g = 0; g < 900; ++g) {
        frame();
        if (g == 60) shot("rolling-4-burning.png");
        if (g == 240) shot("rolling-5-burning.png");
        for (std::size_t k = 0; k < marks.size(); ++k) {
            for (const BarrelLeaks::Pool& p : leaks.pools()) {
                const double heat = leaks.fire_at(p, marks[k].first, marks[k].second, game.kernel().ticks());
                if (heat > 0.2 && first[k] < 0) first[k] = g;
                hottest[k] = std::max(hottest[k], heat);
            }
        }
    }
    std::printf("rolling barrel: the trail burns, from where it rolled from:");
    for (std::size_t k = 0; k < marks.size(); ++k) std::printf(" %.2f (frame %d)", hottest[k], first[k]);
    std::printf("\n");
    // All of it, the far end last.
    CHECK(std::all_of(hottest.begin(), hottest.end(), [](double h) { return h > 0.2; }));
    CHECK(first.front() >= first.back());
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Leaking barrels: two barrels stacked one on the other (save
// baril-empile-flaque-en-l-air.sav), the upper one pierced: its fuel runs
// down the lower one to the floor, its pool lies there (it lay on the lower
// barrel's top, where no floor is: none showed). TRAIL_SHOTS=<folder>: a
// picture of it.
void test_stacked_barrel_leak(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.config().barrel_leaks = true;
    game.load_save_file(test_saves(data) / "baril-empile-flaque-en-l-air.sav");
    World& w = game.world();
    const Item* me = w.get(1);
    CHECK(me != nullptr);
    if (!me) return;
    // The barrel nearest the player standing on another.
    ObjId upper = 0, lower = 0;
    double best = 1e18;
    for (ObjId id : w.rect_search(me->x - 1024, me->y - 1024, me->x + 1024, me->y + 1024)) {
        const Item* a = w.get(id);
        if (!a || !is_fuel_barrel(a->shape) || a->parent != 0) continue;
        for (ObjId other : w.rect_search(a->x - 64, a->y - 64, a->x + 64, a->y + 64)) {
            const Item* b = w.get(other);
            if (!b || other == id || !is_fuel_barrel(b->shape) || b->parent != 0) continue;
            const ShapeInfo* info = w.shape_info(b->shape);
            if (!info || a->z != b->z + info->footpad_z * 8 || std::abs(a->x - b->x) > 16 || std::abs(a->y - b->y) > 16)
                continue;
            const double dx = a->x - me->x, dy = a->y - me->y;
            if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, upper = id, lower = other;
        }
    }
    CHECK(upper != 0);
    if (!upper) return;
    const Item* top = w.get(upper);
    const Item* under = w.get(lower);
    const World::Point spot = w.location(*top);
    std::printf("stacked barrels: %u (shape 0x%X at %d,%d,%d) on %u (shape 0x%X at %d,%d,%d)\n", upper, top->shape,
                top->x, top->y, top->z, lower, under->shape, under->x, under->y, under->z);
    const std::int32_t ground = under->z;
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    const auto run = [&](int frames) {
        for (int f = 0; f < frames; ++f) {
            game.update(kFrameUs);
            game.render(screen);
        }
    };
    // A bullet in its side facing the camera, halfway up.
    const ShapeInfo* info = w.shape_info(top->shape);
    apply_damage(game.kernel(), w, game.animations(), upper, 1, 0, 30, 2);
    if (w.hooks().shot_landed) w.hooks().shot_landed(upper, spot.x + 2, spot.y + 2, spot.z + info->footpad_z * 4, 2);
    run(120);
    if (const char* shots = std::getenv("TRAIL_SHOTS"))
        write_png(std::filesystem::path(shots) / "stacked-leak.png", screen, game.palette());
    const BarrelLeaks& leaks = game.barrel_leaks();
    CHECK(leaks.barrels().size() == 1);
    if (leaks.barrels().size() != 1) return;
    const BarrelLeaks::Barrel& b = leaks.barrels()[0];
    const BarrelLeaks::Pool* pool = leaks.pool(b.pool);
    CHECK(pool != nullptr);
    if (!pool) return;
    std::printf("stacked barrels: the upper one's floor %d, its pool at %d (the floor at %d), %zu lobes, level %.1f "
                "of %.1f\n",
                b.floor, pool->z, ground, pool->lobes.size(), b.level, b.height);
    // On the floor under both (the pools are drawn on the floors at their level).
    CHECK(b.floor == ground && std::abs(pool->z - ground) <= 2 && b.level < b.height * 0.9);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Timing of the leaking barrels (not a check): barrel 5070 of SAVEGAME.3
// leaking, then blown up and burning, with the 3D lighting and its smoke
// (CPU shading), on a `width` x `height` screen. Prints the mean and worst
// milliseconds of update, render and shade over spans of frames, and the
// smoke puffs and lights shaded. BENCH_NOSMOKE set: without the smoke.
void bench_barrel_leaks(const std::filesystem::path& data, int width, int height) {
    constexpr ObjId kBarrel = 5070;
    Game game(data, test_saves(data));
    game.config().barrel_leaks = true;
    game.config().lighting = true;
    game.config().volumetric_smoke = std::getenv("BENCH_NOSMOKE") == nullptr;
    game.load_save(3);
    World& w = game.world();
    w.state.difficulty = 3;
    const Item* barrel = w.get(kBarrel);
    if (!barrel) return;
    const World::Point spot = w.location(*barrel);
    World::Point away = w.location(*w.get(1));
    bool far = false;
    for (int ring = 720; ring <= 1600 && !far; ring += 32) {
        for (int k = 0; k < 16 && !far; ++k) {
            const World::Point at{spot.x + kDirectionDx[k] * ring / 2, spot.y + kDirectionDy[k] * ring / 2, spot.z};
            far = position_free(w, w.get(1)->shape, at, nullptr, 1);
            if (far) away = at;
        }
    }
    w.move_item(1, away.x, away.y, away.z);
    Surface8 screen(width, height);
    std::vector<std::uint32_t> argb;
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const auto span = [&](const char* what, int frames) {
        double sum[3] = {}, worst[3] = {};
        for (int f = 0; f < frames; ++f) {
            const auto t0 = Clock::now();
            game.update(kFrameUs);
            const auto t1 = Clock::now();
            game.render(screen);
            const auto t2 = Clock::now();
            game.render_lit(screen, argb);
            const auto t3 = Clock::now();
            const double d[3] = {ms(t1 - t0), ms(t2 - t1), ms(t3 - t2)};
            for (int k = 0; k < 3; ++k) sum[k] += d[k], worst[k] = std::max(worst[k], d[k]);
        }
        const auto& leaks = game.barrel_leaks();
        std::size_t burning = 0;
        for (const auto& p : leaks.pools()) burning += p.burning;
        const WorldLighting* lit = game.lighting();
        std::printf("%-18s update %5.2f (%5.2f)  render %5.2f (%5.2f)  shade %5.2f (%5.2f) ms  pools %zu (%zu burning) "
                    "drops %zu walls %zu puffs %zu lights %zu\n",
                    what, sum[0] / frames, worst[0], sum[1] / frames, worst[1], sum[2] / frames, worst[2],
                    leaks.pools().size(), burning, leaks.drops().size(), leaks.wall_fires().size(),
                    lit ? lit->puffs_shown().size() : 0, lit ? lit->lights().size() : 0);
    };
    const auto hit = [&](int dx, int dy, int dz) {
        apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 30, 2);
        if (w.hooks().shot_landed) w.hooks().shot_landed(kBarrel, spot.x + dx, spot.y + dy, spot.z + dz, 2);
    };
    std::printf("barrel bench %dx%d\n", width, height);
    span("quiet", 30);
    hit(8, 8, 20);
    hit(20, 2, 10);
    span("leaking", 300);
    for (int hits = 2; hits < 12 && w.get(kBarrel) && w.get(kBarrel)->shape == 0xAA; ++hits) {
        hit(8, 8, 20);
        span("shot", 10);
    }
    span("blown up 0-0.5 s", 15);
    span("burning 0.5-2 s", 45);
    span("burning 2-5 s", 90);
    span("burning 5-10 s", 150);
    span("burnt", 300);
}

// Explosive barrels (BARRELUP 0xAA, SAVEGAME.3): their gotHit spawns BARREL,
// which counts the damage in the npc number byte (ITEM_0574, a process that
// ends at once: spawn hands its result over, INTER_1418_038b 0x57) and at 100
// explodes through Item::I_explode (0x017), which takes any item.
void test_barrels(const std::filesystem::path& data) {
    constexpr ObjId kBarrel = 5070;
    Game game(data, test_saves(data));
    game.config().barrel_leaks = false;  // the game's own damage (data/CRUSADER.CFG may set it)
    game.load_save(3);
    World& w = game.world();
    const Item* barrel = w.get(kBarrel);
    CHECK(barrel && barrel->shape == 0xAA);
    if (!barrel) return;
    const World::Point at = w.location(*barrel);
    const auto run = [&](int frames) {
        for (int f = 0; f < frames; ++f) game.update(kFrameUs);
    };
    // Difficulty 3: 30 damage of fire type 2, kept in the byte.
    w.state.difficulty = 3;
    apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 30, 2);
    run(10);
    std::printf("barrels: after a hit, damage byte %u\n", w.get(kBarrel) ? w.get(kBarrel)->npc_number : 0);
    CHECK(w.get(kBarrel) && w.get(kBarrel)->npc_number == 30);
    for (int k = 0; k < 10 && w.get(kBarrel) && w.get(kBarrel)->shape == 0xAA; ++k) {
        apply_damage(game.kernel(), w, game.animations(), kBarrel, 1, 0, 30, 2);
        run(10);
    }
    run(60);
    int fires = 0, debris = 0;
    w.for_each([&](const Item& i) {
        const bool here = std::abs(i.x - at.x) < 64 && std::abs(i.y - at.y) < 64;
        fires += here && i.shape == 0x509;
        debris += here && i.shape == 0x22B;
    });
    const Item* left = w.get(kBarrel);
    std::printf("barrels: barrel %s, fires %d, debris %d\n", left ? "left" : "gone", fires, debris);
    CHECK((!left || left->shape != 0xAA) && fires >= 1);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());

    // The energy station RECHAGNS 5689 of SAVEGAME.2 counts its damage the
    // same way; at 100 it blows up (ITEM_05D6) with its twin, its pad and
    // its panels (frames + 4 and + 1).
    Game station_game(data, test_saves(data));
    station_game.load_save(2);
    World& sw = station_game.world();
    sw.state.difficulty = 3;
    for (int k = 0; k < 4; ++k) {
        apply_damage(station_game.kernel(), sw, station_game.animations(), 5689, 1, 0, 30, 2);
        for (int f = 0; f < 10; ++f) station_game.update(kFrameUs);
    }
    for (int f = 0; f < 120; ++f) station_game.update(kFrameUs);
    std::printf("barrels: station frames %u, %u, pad %u\n", sw.get(5689)->frame, sw.get(5691)->frame,
                sw.get(5693)->frame);
    CHECK(sw.get(5689)->frame == 4 && sw.get(5691)->frame == 5 && sw.get(5693)->frame == 1);
    CHECK(station_game.machine().faults().empty() && station_game.machine().unbound_intrinsic_calls().empty());
}

// Floor mine FLORMINE 6035 (SAVEGAME.2): walked onto, its gotHit (contact
// of the move) finds the player on it (Item::I_isOn), explodes and breaks.
void test_mines(const std::filesystem::path& data) {
    constexpr ObjId kMine = 6035;
    Game game(data, test_saves(data));
    game.load_save(2);
    World& w = game.world();
    const Item* mine = w.get(kMine);
    CHECK(mine && mine->shape == 0x29F && mine->frame == 0);
    if (!mine) return;
    w.move_item(1, mine->x + 96, mine->y, mine->z);
    const int hp = w.npc_hp(1);
    if (auto r = w.npc_record(1); !r.empty()) r[npc::kDirection] = 12;  // west
    game.key(0x48, true, true, 0);
    for (int f = 0; f < 90; ++f) game.update(kFrameUs);
    game.key(0x48, true, false, 0);
    for (int f = 0; f < 60; ++f) game.update(kFrameUs);
    std::printf("mines: mine frame %u status %04X, player hit points %d -> %u\n", mine->frame, mine->flags, hp,
                w.npc_hp(1));
    CHECK(mine->frame >= 2 && (mine->flags & 0x40) && w.npc_hp(1) < hp);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// The same mine shot from 160 east, by a shot whose spread takes it down
// to z 0 under the mine: the step that ends at z 0 is swept in parts of 16
// (SPRITE_1140_1098), meets the mine (1 high, 1 above the floor) on the way
// down and blows it up. The port ended such a step on nothing, unswept.
void test_shot_mine(const std::filesystem::path& data) {
    constexpr ObjId kMine = 6035;
    Game game(data, test_saves(data));
    game.load_save(2);
    World& w = game.world();
    const Item* mine = w.get(kMine);
    CHECK(mine && mine->shape == 0x29F && mine->frame == 0);
    if (!mine) return;
    w.move_item(1, mine->x + 160, mine->y, mine->z);
    game.update(kFrameUs);
    const Item* player = w.get(1);
    const World::Point start{player->x, player->y, player->z + 40};
    World::Point target = aim_point(w, 1, kMine);
    target.z = 0;
    game.kernel().add(std::make_unique<ProjectileProcess>(w, game.animations(), 1, start, target, 2, 20, 0));
    for (int f = 0; f < 120 && projectiles(game) != 0; ++f) game.update(kFrameUs);
    for (int f = 0; f < 60; ++f) game.update(kFrameUs);
    std::printf("shot mine: mine frame %u status %04X\n", mine->frame, mine->flags);
    CHECK(projectiles(game) == 0);
    CHECK(mine->frame >= 2 && (mine->flags & 0x40));
    CHECK(game.machine().faults().empty());
}

// The station pads of SAVEGAME.2, walked onto from the south: RECHFLNS 5693
// (energy) and SICBFLNS 5514 (health). Their gotHit, a contact of the move
// from the player whose centre is on the pad, holds him in stasis, places
// him, then starts BatteryChargerProcess (0x0EE) or CruHealerProcess (0x0F6).
void test_station_pads(const std::filesystem::path& data) {
    for (const ObjId pad_id : {ObjId{5693}, ObjId{5514}}) {
        Game game(data, test_saves(data));
        game.load_save(2);
        World& w = game.world();
        const Item* pad = w.get(pad_id);
        CHECK(pad != nullptr);
        if (!pad) return;
        const ShapeInfo* info = w.shape_info(pad->shape);
        const ShapeInfo* me = w.shape_info(w.get(1)->shape);
        if (auto r = w.npc_record(1); !r.empty()) {
            r[0] = 50;  // hit points
            r[1] = 0;
            r[npc::kDirection] = 0;  // north
        }
        w.npc_record(1)[0x57] = 0;  // energy
        w.npc_record(1)[0x58] = 0;
        w.move_item(1, pad->x - info->footpad_x * 8 + me->footpad_x * 8, pad->y + 100, pad->z);
        game.key(0x48, true, true, 0);
        int stasis_frames = 0;
        for (int f = 0; f < 600; ++f) {
            if (f == 60) game.key(0x48, true, false, 0);
            game.update(kFrameUs);
            stasis_frames += w.state.avatar_in_stasis;
        }
        std::printf("station pads: %04X, hit points 50 -> %u, energy 0 -> %u, %d frames in stasis\n", pad->shape,
                    w.npc_hp(1), w.npc_mana(1), stasis_frames);
        if (pad_id == 5693) CHECK(w.npc_mana(1) == 2500 && w.npc_hp(1) == 50);
        else CHECK(w.npc_hp(1) == 150 && w.npc_mana(1) == 0);
        CHECK(stasis_frames > 0 && !w.state.avatar_in_stasis);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
}

// The turret of a surveillance monitor (issue #15): the monitor gives the
// player the control of a BASEGUN NPC, which fires through the player's
// fire key with the weapon of its slot 1. The turret is not a container
// (family 1), so pop_into leaves that weapon on the map, equipped (0x200)
// but not contained: a saved game then lost the slot at its load
// (World::forget_lost_equipment) and the turret turned without firing.
// - SAVEGAME.1 of the report, new scene: control, fire, back to the player,
//   save, load, control again, fire.
// - SAVEGAME.13 of the report, saved with the slot already lost: given back
//   at the load (the weapon item is still there, equipped).
void test_player_turret(const std::filesystem::path& data) {
    constexpr ObjId kMonitor = 2119;  // SURCAMEW, low quality 0xFA
    // Uses the monitor, dismisses its texts, and returns the NPC it gave.
    const auto take_control = [](Game& game) {
        World& w = game.world();
        use_item(w, kMonitor);
        for (int f = 0; f < 1500 && w.state.controlled_npc == 1; ++f) {
            game.update(kFrameUs);
            if (game.screen() && f % 20 == 0) {
                game.key(0x1C, false, true, 0, 13);  // Enter
                game.key(0x1C, false, false, 0, 0);
            }
        }
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        return w.state.controlled_npc;
    };
    // Presses the fire key three times; returns the shots they gave.
    const auto fire = [](Game& game) {
        const int before = game.shots_fired();
        for (int press = 0; press < 3; ++press) {
            game.key(0x39, false, true, 0, ' ');  // Space
            for (int f = 0; f < 60; ++f) game.update(kFrameUs);
            game.key(0x39, false, false, 0, 0);
            for (int f = 0; f < 60; ++f) game.update(kFrameUs);
        }
        return game.shots_fired() - before;
    };
    const auto weapon_shape = [](const World& w, ObjId npc) {
        const Item* weapon = w.get(w.active_weapon(npc));
        return weapon ? weapon->shape : 0;
    };

    const auto scratch = std::filesystem::temp_directory_path() / "cr_player_turret";
    std::filesystem::create_directories(scratch);
    {
        Game game(data, scratch);
        game.load_save_file(test_saves(data) / "issue15-tourelle-ne-tire-pas-SAVEGAME.1.sav");
        for (int f = 0; f < 30; ++f) game.update(kFrameUs);
        const ObjId turret = take_control(game);
        const int shots = fire(game);
        std::printf("player turret: new scene, NPC %u (shape %u), weapon shape %u, %d shots\n", turret,
                    game.world().get(turret) ? game.world().get(turret)->shape : 0, weapon_shape(game.world(), turret), shots);
        CHECK(turret > 1 && game.world().get(turret) && game.world().get(turret)->shape == 0x4C8);
        CHECK(shots >= 3);
        // The right stick turns it to each of its 16 directions (it never
        // aims: no crosshair), and it fires that way.
        std::set<int> faced;
        for (int step = 0; step < 64; ++step) {
            game.set_aim_stick(true, step * 360.0 / 64);
            for (int f = 0; f < 20; ++f) game.update(kFrameUs);
            faced.insert(game.world().npc_direction(turret) & 0xF);
        }
        const int stick_shots = fire(game);
        game.set_aim_stick(false, 0);
        game.update(kFrameUs);
        std::printf("player turret: stick, %zu directions, crosshair %d, %d shots\n", faced.size(),
                    game.world().state.aim.crosshair, stick_shots);
        CHECK(faced.size() == 16 && game.world().state.aim.crosshair == -1 && stick_shots >= 3);
        game.key(0x01, false, true, 0, 0x1B);  // Escape gives the control back
        game.key(0x01, false, false, 0, 0);
        for (int f = 0; f < 400; ++f) game.update(kFrameUs);
        CHECK(game.world().state.controlled_npc == 1);

        game.save_game(5);
        Game again(data, scratch);
        again.load_save(5);
        std::printf("player turret: after save and load, weapon shape %u\n", weapon_shape(again.world(), turret));
        CHECK(weapon_shape(again.world(), turret) == 909);
        const ObjId second = take_control(again);
        const int more = fire(again);
        std::printf("player turret: after load, NPC %u, %d shots\n", second, more);
        CHECK(second == turret && more >= 3);
        CHECK(again.machine().faults().empty() && again.machine().unbound_intrinsic_calls().empty());
    }
    {
        // The save of the report: NPC 24, its slot lost, its weapon 8736 on the map.
        Game game(data, scratch);
        game.load_save_file(test_saves(data) / "issue15-tourelle-ne-tire-pas-SAVEGAME.13.sav");
        for (int f = 0; f < 30; ++f) game.update(kFrameUs);
        std::printf("player turret: SAVEGAME.13 of the report, NPC 24 weapon shape %u\n", weapon_shape(game.world(), 24));
        CHECK(game.world().npc_equip(24, 1) == 8736 && weapon_shape(game.world(), 24) == 909);
        const ObjId turret = take_control(game);
        const int shots = fire(game);
        std::printf("player turret: SAVEGAME.13, NPC %u, %d shots\n", turret, shots);
        CHECK(turret == 24 && shots >= 3);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
    std::filesystem::remove_all(scratch);
}

// Wall guns (WALLGUN 0xA1A). The search for a target (WALLGUN_0A5F) is a
// spawned process whose result decides the attack: it often ends at once,
// so spawn must hand that result over.
// - Mission 1 (new game), GATGUNEW 2080 sleeps in mode 6 until a trigger
//   sends equip(mode); with mode 0 it sweeps, sees the player 200 units
//   north and shoots him.
// - SAVEGAME.13, GATGUNEW 3369 (mode 0): the save keeps its status bit 4
//   (its process runs) but not the process, which the load restarts.
void test_turrets(const std::filesystem::path& data) {
    const auto count_shots = [](Game& game, int frames) {
        std::set<Pid> previous;
        int shots = 0;
        for (int f = 0; f < frames; ++f) {
            game.update(kFrameUs);
            std::set<Pid> now;
            game.kernel().for_each_process([&](const Process& p) {
                if (p.type() != ProjectileProcess::kType) return;
                now.insert(p.pid());
                shots += !previous.count(p.pid());
            });
            previous = std::move(now);
        }
        return shots;
    };
    {
        Game game(data, test_saves(data));
        game.new_game(3);
        game.update(kFrameUs);
        game.key(0x01, false, true, 0, 0x1B);
        for (int f = 0; game.movie() && f < 200; ++f) game.update(kFrameUs);
        game.key(0x01, false, false, 0);
        for (int f = 0; f < 600; ++f) game.update(kFrameUs);
        World& w = game.world();
        constexpr ObjId kGun = 2080;
        const Item* gun = w.get(kGun);
        CHECK(gun && gun->shape == 0x113 && gun->npc_number == 6);
        if (!gun) return;
        w.move_item(1, gun->x, gun->y - 200, gun->z);
        CHECK(count_shots(game, 120) == 0);
        const std::uint8_t mode[] = {0, 0};
        game.machine().start_event(gun->shape, 0x0A, kGun, mode);
        const int shots = count_shots(game, 900);
        std::printf("turrets: mission 1 gun %u, %d shots, player hit points %u\n", kGun, shots, w.npc_hp(1));
        CHECK(shots >= 5 && w.npc_hp(1) < 150);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        constexpr ObjId kGun = 3369;
        const Item* gun = w.get(kGun);
        CHECK(gun && gun->shape == 0x113);
        if (!gun) return;
        w.move_item(1, gun->x, gun->y - 200, gun->z);
        const int hp = w.npc_hp(1);
        const int shots = count_shots(game, 900);
        std::printf("turrets: SAVEGAME.13 gun %u, %d shots, player hit points %d -> %u\n", kGun, shots, hp, w.npc_hp(1));
        CHECK(shots >= 5 && w.npc_hp(1) < hp);
        // Shot down by the player: gotHit -> WALLGUN_3C01 -> WALLGUN_3A77,
        // whose damage count (ITEM_0574, threshold 100) ends without a
        // pop_res of its own; its result is the 1 the child left in the
        // shared result register. Broken frame 27, two pieces hurled.
        int hits = 0;
        for (; hits < 40 && w.get(kGun) && w.get(kGun)->frame != 27; ++hits) {
            apply_damage(game.kernel(), w, game.animations(), kGun, 1, 0, 5, 2);
            for (int f = 0; f < 10; ++f) game.update(kFrameUs);
        }
        std::printf("turrets: SAVEGAME.13 gun %u broken after %d hits\n", kGun, hits);
        CHECK(w.get(kGun) && w.get(kGun)->frame == 27 && hits >= 2);
        CHECK(count_shots(game, 300) == 0);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
    // Native resolution, a 1720 x 720 view: the same gun, 450 pixels right
    // of the camera point, is drawn in the band beside the original screen.
    // It sweeps, but only finds and shoots the player from the combat zone.
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        constexpr ObjId kGun = 3369;
        const Item* gun = w.get(kGun);
        CHECK(gun != nullptr);
        if (!gun) return;
        w.move_item(1, gun->x, gun->y - 200, gun->z);
        w.set_view_size(1720, 720);
        w.set_camera(gun->x - 450, gun->y + 450, gun->z & 0xFF);
        const bool drawn = w.is_partly_on_screen(*gun), fights = w.is_partly_in_combat_zone(*gun);
        const int shots = count_shots(game, 600);
        std::set<int> frames;
        for (int f = 0; f < 300; ++f) {
            game.update(kFrameUs);
            frames.insert(gun->frame);
        }
        std::printf("turrets: in the band, drawn %d, in zone %d, %d shots, %zu frames\n", drawn, fights, shots,
                    frames.size());
        CHECK(drawn && !fights && shots == 0 && frames.size() > 2);
        // Back in the combat zone, it shoots: its first shot within 30 s
        // (its sweep and waits are random: 8 to 12 s from this save), then
        // at least 5 in the next 15 s.
        w.set_camera(gun->x, gun->y, gun->z & 0xFF);
        int first = -1;
        for (int f = 0; f < 1800 && first < 0; ++f) {
            if (count_shots(game, 1) > 0) first = f;
        }
        const int burst = first < 0 ? 0 : count_shots(game, 900);  // the first one still flies: counted
        std::printf("turrets: back in the zone, first shot after %d frames, %d shots in 15 s\n", first, burst);
        CHECK(first >= 0 && burst >= 5);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
}

// Mission 1's surveillance monitors (new game), whose camera goes far from
// the player: the monitor leaves the fast area and its processes are failed
// (10A8:2DAD). The script of event 0x21 has no owner (set_info 0) and must
// keep none: implies (KERNEL_11d8_1327) does not pass the waiter's owner on,
// unlike Process_wait_for, or the view stayed black with the player in
// stasis.
// - 5459 (SURCAMNS, quality low 0xEE): the simple view in grey, until a key
//   is held; then the camera, the status bar and the player are back.
// - 2136 (SURCAMEW, 0xFA): the control of NPC 8 near the egg (event 0x22),
//   until Escape gives it back.
void test_monitors(const std::filesystem::path& data) {
    const auto start = [](Game& game) {
        game.new_game(3);
        game.update(kFrameUs);
        game.key(0x01, false, true, 0, 0x1B);
        for (int f = 0; game.movie() && f < 200; ++f) game.update(kFrameUs);
        game.key(0x01, false, false, 0);
        for (int f = 0; f < 600; ++f) game.update(kFrameUs);
    };
    const auto run = [](Game& game, int frames) {
        for (int f = 0; f < frames; ++f) game.update(kFrameUs);
    };
    // Uses the monitor and closes its texts.
    const auto watch = [&](Game& game, ObjId monitor) {
        World& w = game.world();
        const Item* m = w.get(monitor);
        w.move_item(1, m->x + 64, m->y + 64, m->z);
        run(game, 30);
        use_item(w, monitor);
        run(game, 60);
        for (int k = 0; k < 20 && game.screen(); ++k) {
            press(game, 0x01, false, 0x1B);
            run(game, 30);
        }
        run(game, 240);
    };
    const auto grey = [](const Game& game) {
        const Palette& p = game.palette();
        return p.colors[200].r != 0 && p.colors[200].r == p.colors[200].g && p.colors[200].g == p.colors[200].b;
    };
    const auto restored = [](Game& game) {
        const World& w = game.world();
        return w.state.controlled_npc == 1 && !w.state.avatar_in_stasis && !w.state.cru_stasis &&
               w.state.status_bar_visible && w.state.camera_x == w.get(1)->x && w.state.camera_y == w.get(1)->y;
    };
    {
        Game game(data, test_saves(data));
        start(game);
        World& w = game.world();
        watch(game, 5459);
        std::printf("monitors: 5459 camera %+d, %+d from the player, stasis %d, grey %d\n",
                    w.state.camera_x - w.get(1)->x, w.state.camera_y - w.get(1)->y, w.state.avatar_in_stasis, grey(game));
        CHECK(w.state.avatar_in_stasis && grey(game) && !w.state.status_bar_visible && w.state.camera_y - w.get(1)->y > 5000);
        game.key(0x48, true, true, 0, 0);
        run(game, 10);
        game.key(0x48, true, false, 0, 0);
        run(game, 120);
        CHECK(restored(game) && !grey(game));
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
    {
        Game game(data, test_saves(data));
        start(game);
        World& w = game.world();
        watch(game, 2136);
        std::printf("monitors: 2136 controlled NPC %u (shape %04X), grey %d\n", w.state.controlled_npc,
                    w.get(w.state.controlled_npc)->shape, grey(game));
        CHECK(w.state.controlled_npc == 8 && grey(game));
        press(game, 0x01, false, 0x1B);
        run(game, 120);
        CHECK(restored(game) && !game.menu());
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
}

// Animated scenery (DLIST_1198_162B): the valve 4600 of SAVEGAME.3 lets out
// steam (STEAM2 0x511, TYPEFLAG animation type 3, speed 3), which cycles its
// frames while on screen, one step every third pass of the game process.
void test_scenery_animation(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(3);
    World& w = game.world();
    constexpr ObjId kValve = 4600;
    const Item* valve = w.get(kValve);
    CHECK(valve && valve->shape == 0x30);
    if (!valve) return;
    w.move_item(1, valve->x + 64, valve->y + 64, valve->z);
    for (int f = 0; f < 10; ++f) game.update(kFrameUs);
    use_item(w, kValve);
    for (int f = 0; f < 300; ++f) game.update(kFrameUs);
    ObjId steam = 0;
    w.for_each([&](const Item& i) {
        if (i.shape == 0x511 && !i.parent && w.is_partly_on_screen(i)) steam = i.id;
    });
    CHECK(steam != 0);
    if (!steam) return;
    std::set<int> frames;
    int changes = 0, last = w.get(steam)->frame;
    for (int f = 0; f < 60; ++f) {
        game.update(kFrameUs);
        const int now = w.get(steam)->frame;
        frames.insert(now);
        changes += now != last;
        last = now;
    }
    std::printf("scenery animation: steam %u, %zu frames, %d changes in 60 passes\n", steam, frames.size(), changes);
    CHECK(frames.size() > 2 && changes >= 15 && changes <= 21);
    CHECK(game.machine().faults().empty());
}

// The valve 6689 of SAVEGAME.1 shuts off the steam 6533 (STEAM2 0x511):
// STEAMBOX_0327 swaps it for 0x512 (status 0x1000) and, at its last frame,
// for the marker 0x3A9 frame 1. A game saved in between loses that process;
// loading finishes the swap, and the valve opens the steam again.
void test_steam_valve_save(const std::filesystem::path& data) {
    const auto dir = std::filesystem::temp_directory_path() / "crusader_steam_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(test_saves(data) / "SAVEGAME.1", dir / "SAVEGAME.1");
    constexpr ObjId kValve = 6689;
    constexpr std::int32_t kJetX = 27887, kJetY = 28255;
    auto jet = [&](World& w) {
        const Item* found = nullptr;
        w.for_each([&](const Item& i) {
            if (!i.parent && i.x == kJetX && i.y == kJetY && (i.quality & 0xFF) == 0x1C) found = &i;
        });
        return found;
    };
    Game game(data, test_saves(data));
    game.set_save_dir(dir);
    game.load_save(1);
    World& w = game.world();
    CHECK(w.get(kValve) && w.get(kValve)->shape == 0x30 && jet(w) && jet(w)->shape == 0x511);
    if (!w.get(kValve) || !jet(w)) return;
    w.move_item(1, 27879, 28689, 0);
    for (int f = 0; f < 10; ++f) game.update(kFrameUs);
    use_item(w, kValve);
    for (int f = 0; f < 300 && jet(w) && jet(w)->shape != 0x512; ++f) game.update(kFrameUs);
    // The steam starts shutting while the valve is still turning (frame 2);
    // save once the valve is fully shut, the steam still closing. A valve
    // saved half turned would, used again, shut a bit more.
    for (int f = 0; f < 120 && jet(w) && jet(w)->shape == 0x512 && w.get(kValve)->frame != 4; ++f)
        game.update(kFrameUs);
    const Item* closing = jet(w);
    CHECK(closing && closing->shape == 0x512 && (closing->flags & 0x1000) && w.get(kValve)->frame == 4);
    game.save_game(2);

    Game again(data, test_saves(data));
    again.set_save_dir(dir);
    again.load_save(2);
    World& v = again.world();
    const Item* marker = jet(v);
    CHECK(marker && marker->shape == 0x3A9 && marker->frame == 1 && !(marker->flags & 0x1000));
    for (int f = 0; f < 10; ++f) again.update(kFrameUs);
    use_item(v, kValve);
    for (int f = 0; f < 400; ++f) again.update(kFrameUs);
    const Item* reopened = jet(v);
    std::printf("steam valve: saved while closing, reloaded as 0x%03X frame %u, reopened as 0x%03X\n",
                marker ? marker->shape : 0, marker ? marker->frame : 0, reopened ? reopened->shape : 0);
    CHECK(reopened && reopened->shape == 0x511);
    CHECK(game.machine().faults().empty() && again.machine().faults().empty());
    std::filesystem::remove_all(dir);
}

// Laser barrier L_NS-16 6406 of mission 1 (new game): crossed, its gotHit
// (contact of the move, the item is not solid) sees the controlled NPC,
// spawns LAZER and its TRIGGER; LAZER's NPC_0A1D zaps the player
// (Item::I_receiveHit, fire type 7).
void test_laser_barrier(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.new_game(3);
    game.update(kFrameUs);
    game.key(0x01, false, true, 0, 0x1B);
    for (int f = 0; game.movie() && f < 200; ++f) game.update(kFrameUs);
    game.key(0x01, false, false, 0);
    for (int f = 0; f < 600; ++f) game.update(kFrameUs);
    World& w = game.world();
    constexpr ObjId kLaser = 6406;
    const Item* laser = w.get(kLaser);
    CHECK(laser && laser->shape == 0x1CF && laser->frame == 0);
    if (!laser) return;
    const ShapeInfo* info = w.shape_info(laser->shape);
    w.move_item(1, laser->x - info->footpad_x * 8 + 24, laser->y + 80, laser->z);
    if (auto r = w.npc_record(1); !r.empty()) r[npc::kDirection] = 0;  // north, across the beam
    const int hp = w.npc_hp(1);
    game.key(0x48, true, true, 0);
    for (int f = 0; f < 150; ++f) game.update(kFrameUs);
    game.key(0x48, true, false, 0);
    for (int f = 0; f < 30; ++f) game.update(kFrameUs);
    std::printf("laser barrier: player %+d past the beam, hit points %d -> %u\n", laser->y - w.get(1)->y, hp,
                w.npc_hp(1));
    CHECK(w.get(1)->y <= laser->y && w.npc_hp(1) < hp);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Aiming while kneeling (SAVEGAME.13, shotgun): keypad 5 in the combat
// stance kneels (0x28). Actor::I_doAnim (10F0:0245) keeps the player
// kneeling for the actions of DS:135F (after the weapon's remapping) and
// stands him up for any other (NPC_10f0_221b / 226d: record +0x13 bit 2,
// footpad height 6 / 10). Turning the crosshair then plays the kneeling
// stance (0x2E, 0x2F with the shotgun) and the shot is a kneeling one
// (0x2A, 0x32 with the shotgun); Up stands him up (0x29, then a pose).
void test_kneeling_aim(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) game.update(kFrameUs);
    };
    auto tap = [&](std::uint8_t scancode, bool extended) {
        game.key(scancode, extended, true, 0);
        run(2);
        game.key(scancode, extended, false, 0);
        run(2);
    };
    run(10);
    World& w = game.world();
    const GameState::Aim& aim = w.state.aim;
    const auto kneeling = [&] { return (w.npc_record(1)[npc::kFlags13] & 0x04) != 0; };
    const auto height = [&] { return w.shape_info(w.get(1)->shape)->footpad_z; };
    w.npc_record(1)[npc::kDirection] = 4;
    do_anim(game.kernel(), w, game.animations(), 1, 4, kDirCurrent);
    run(30);
    CHECK(aim.active && !kneeling() && height() == 10);
    tap(0x4C, false);  // keypad 5
    run(60);
    CHECK(last_action(game) == 0x28 && kneeling() && height() == 6);
    for (int i = 0; i < 12; ++i) tap(0x4D, true);  // right arrow
    run(40);
    std::printf("kneeling aim: angle %d, direction %d, last action 0x%X, kneeling %d\n", aim.angle, w.npc_direction(1),
                last_action(game), kneeling());
    CHECK(aim.angle == 27 && w.npc_direction(1) == 5 && last_action(game) == 0x2F && kneeling());
    press(game, 0x39, false, ' ');
    run(60);
    CHECK(last_action(game) == 0x32 && game.shots_fired() == 1 && kneeling());
    tap(0x48, true);  // up arrow
    run(60);
    std::printf("kneeling aim: after Up, last action 0x%X, kneeling %d, height %d\n", last_action(game), kneeling(),
                height());
    CHECK(!kneeling() && height() == 10);
    CHECK(game.machine().faults().empty());
}

// Hazard stripes (shape 0x10C, flat, 32 x 256) on the floor tiles (0xCC,
// flat, 256 x 256) of SAVEGAME.2: each stripe is drawn after every tile it
// overlaps, so that the next tile does not cut it (large square flats first,
// render.cpp).
void test_floor_stripes(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(2);
    game.update(kFrameUs);
    const World& w = game.world();
    const FlexArchive flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    ShapeCache shapes(flex, ShapeLayout::World);
    const auto list = build_display_list(w, shapes, View{w.state.view_left, w.state.view_top}, 640, 480);
    int stripes = 0, checked = 0, wrong = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const DrawItem& s = list[i];
        if (s.item->shape != 0x10C) continue;
        ++stripes;
        for (std::size_t j = 0; j < list.size(); ++j) {
            const DrawItem& t = list[j];
            if (t.item->shape != 0xCC || t.z != s.z) continue;
            if (t.x <= s.x_left || s.x <= t.x_left || t.y <= s.y_far || s.y <= t.y_far) continue;
            ++checked;
            wrong += j > i;
        }
    }
    std::printf("floor stripes: %d stripes on screen, %d overlaps with tiles, %d drawn under a tile\n", stripes, checked,
                wrong);
    CHECK(stripes > 0 && checked > stripes && wrong == 0);
}

// TEST11 (docs/bugs.md): guard 8 was saved as its death began (dead, action
// 0x12 on its first frame). The animation processes are not loaded from
// saves; loading plays the death on, NPCDEATH then lays its body (D_GUARD
// 0x339). Before, the guard stayed standing, dead, for good. The event
// runs at once (Item::callUsecodeEvent): the body may be laid by the load.
void test_saved_death_plays_on(const std::filesystem::path& data) {
    constexpr ObjId kGuard = 8;
    const auto file = test_saves(data) / "ordinateur-robot-face-garde-mort-debout.sav";
    std::int32_t gx = 0, gy = 0;
    {
        World saved(load_typeflags(data / "STATIC/TYPEFLAG.DAT"));
        saved.load_saved_items(FlexArchive::open(file));
        const Item* g = saved.get(kGuard);
        CHECK(g && g->frame == 231 && saved.npc_word(kGuard, npc::kLastAnim) == 0x12 && saved.is_dead(kGuard));
        if (!g) return;
        gx = g->x;
        gy = g->y;
    }
    Game game(data, test_saves(data));
    game.load_save_file(file);
    World& w = game.world();
    for (int f = 0; f < 600; ++f) game.update(kFrameUs);
    bool body = false;
    w.for_each([&](const Item& i) {
        body = body || (i.shape == 0x339 && std::abs(i.x - gx) < 64 && std::abs(i.y - gy) < 64);
    });
    std::printf("saved death: guard %s, body %d\n", w.get(kGuard) ? "still standing" : "set aside", body);
    CHECK(!w.get(kGuard) && body);
    CHECK(game.machine().faults().empty());
}

// Robot floating in the void (docs/bugs.md): NPC 31 belongs to map 9 and sits in
// no cell of map 11; it was drawn all the same. An NPC of another map is off the map.
void test_other_map_npc_hidden(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "robot-affiche-dans-le-vide.sav");
    const Item* robot = game.world().get(31);
    CHECK(robot && robot->map_number == 9 && robot->off_map);
}

// SEB25, shot dead by an invisible enemy: NPCs 36 and 40, guards of map 13,
// stand off the map at coordinates that map 15 (the current one) also has.
// Rebuilding the fast area on loading, and any later move of the camera,
// took them in by their coordinates: their activity 5 attacked the player,
// who could not see them. The original enters only the items linked to the
// cells reached (FastArea_update, US 10B0:13DD).
void test_other_map_npc_out_of_fast_area(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "ennemi-invisible-tire.sav");
    World& w = game.world();
    CHECK(w.current_map() == 15);
    int awake = 0;
    w.for_each([&](const Item& i) {
        if (is_npc_id(i.id) && i.off_map && (i.flags & kItemFastArea)) ++awake;
    });
    int processes = 0;
    game.kernel().for_each_process([&](const Process& p) { processes += p.owner() == 36 || p.owner() == 40; });
    const int hp = w.npc_hp(1);
    for (int f = 0; f < 300; ++f) game.update(kFrameUs);
    std::printf("other map NPCs: %d in the fast area, %d processes, player hp %d -> %d\n", awake, processes, hp,
                w.npc_hp(1));
    CHECK(awake == 0 && processes == 0);
    CHECK(w.npc_hp(1) == hp);
}

// TEST15 (docs/bugs.md, "un garde mort qui est toujours debout"): a guard
// killed out of the fast area. Its death waits there (ANIMPRIM.C 1908) and
// plays once the camera brings it in. Before, it played at once with no
// floor under the guard (glob items only exist in the fast area): the first
// frame stumbled, the death was cut and the guard stayed standing.
// ANIMPRIM.C 2E66: the start of a run (0x22) ends with the half-stride bit
// of the record (+0x59 bit 2) cleared, so the run that follows starts from
// the middle of its cycle. The port left it set, as at the end of any action.
// ANIMPRIM.C 2D09: an action flagged in ANIM.DAT (byte 3 bit 7: shapes 1423
// and 1436, standing and deaths) destroys the actor at its end.
void test_anim_end(const std::filesystem::path& data) {
    {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        settle(game);
        const Pid pid = do_anim(game.kernel(), w, game.animations(), 1, 0x22, kDirCurrent, 10000, true);
        CHECK(pid != 0);
        int frames = 0;
        while (game.kernel().get(pid) && !game.kernel().get(pid)->terminated() && frames < 300) {
            game.update(kFrameUs);
            ++frames;
        }
        const bool half = w.npc_record(1)[npc::kAnimFlags] & 0x04;
        std::printf("anim end: start of a run over after %d frames, half-stride bit %d\n", frames, half);
        CHECK(frames < 300 && !half);
    }
    {
        constexpr ObjId kGuard = 4;
        Game game(data, test_saves(data));
        game.load_save_file(test_saves(data) / "garde-mort-debout.sav");
        World& w = game.world();
        Item* guard = w.get(kGuard);
        CHECK(guard != nullptr);
        if (!guard) return;
        w.set_camera(guard->x, guard->y, guard->z);
        for (int f = 0; f < 10; ++f) game.update(kFrameUs);
        guard = w.get(kGuard);
        CHECK(guard && (guard->flags & kItemFastArea));
        if (!guard) return;
        w.set_shape(*guard, 1423);
        const AnimAction* a = game.animations().action(1423, 0);
        CHECK(a && a->destroys_actor());
        CHECK(do_anim(game.kernel(), w, game.animations(), kGuard, 0, kDirCurrent, 10000, true) != 0);
        for (int f = 0; f < 120; ++f) game.update(kFrameUs);
        std::printf("anim end: shape 1423 after its action 0: %s\n", w.get(kGuard) ? "still there" : "destroyed");
        CHECK(!w.get(kGuard));
    }
}

// WPNCACHE_1160_01f0, the frames flagged 0x1000 in ANIM.DAT, which the port
// ignored: the robot 0x385 explodes on its death (action 0x12), with the
// large explosion, and stays; the burning man (0x576) screams as he runs.
void test_special_frames(const std::filesystem::path& data) {
    constexpr ObjId kGuard = 4;
    for (const std::uint16_t shape : {std::uint16_t{0x385}, std::uint16_t{0x576}}) {
        Game game(data, test_saves(data));
        game.load_save_file(test_saves(data) / "garde-mort-debout.sav");
        World& w = game.world();
        Item* guard = w.get(kGuard);
        CHECK(guard != nullptr);
        if (!guard) return;
        w.set_camera(guard->x, guard->y, guard->z);
        for (int f = 0; f < 10; ++f) game.update(kFrameUs);
        guard = w.get(kGuard);
        if (!guard) return;
        w.set_shape(*guard, shape);
        const std::uint16_t action = shape == 0x385 ? 0x12 : 1;
        CHECK(do_anim(game.kernel(), w, game.animations(), kGuard, action, kDirCurrent, 10000, true) != 0);
        bool blast = false, scream = false;
        for (int f = 0; f < 180; ++f) {
            game.update(kFrameUs);
            w.for_each([&](const Item& i) { blast = blast || (i.shape >= 0x323 && i.shape <= 0x325); });
            for (const int s : {101, 102, 103, 11, 10}) scream = scream || game.audio()->sources.is_playing(s, kGuard);
        }
        std::printf("special frames: shape 0x%X action 0x%X, large explosion %d, scream %d, still there %d\n",
                    shape, action, blast, scream, w.get(kGuard) != nullptr);
        if (shape == 0x385) CHECK(blast && w.get(kGuard));
        else CHECK(scream);
    }
}

// ANIMPRIM.C 1908: in combat, the actions flagged 0x80 (shape 0x33C) go one
// unit up or down per step towards the target's height. The port kept the
// height of the frames.
void test_combat_height(const std::filesystem::path& data) {
    constexpr ObjId kGuard = 4;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "garde-mort-debout.sav");
    World& w = game.world();
    Item* guard = w.get(kGuard);
    CHECK(guard != nullptr);
    if (!guard) return;
    w.set_camera(guard->x, guard->y, guard->z);
    for (int f = 0; f < 10; ++f) game.update(kFrameUs);
    guard = w.get(kGuard);
    if (!guard) return;
    w.set_shape(*guard, 0x33C);
    const AnimAction* a = game.animations().action(0x33C, 1);
    CHECK(a && (a->flags() & 0x80));
    const int z0 = guard->z & 0xFF;
    const ObjId mark = w.create(0x81, 0);
    w.pop_to_coords(guard->x - 512, guard->y, z0 + 40);
    auto r = w.npc_record(kGuard);
    r[npc::kCombatFlags] |= 0x08;
    r[npc::kTarget] = static_cast<std::uint8_t>(mark);
    r[npc::kTarget + 1] = static_cast<std::uint8_t>(mark >> 8);
    CHECK(do_anim(game.kernel(), w, game.animations(), kGuard, 1, kDirCurrent, 10000, true) != 0);
    int top = z0;
    for (int f = 0; f < 60; ++f) {
        game.update(kFrameUs);
        if (const Item* g = w.get(kGuard)) top = std::max(top, g->z & 0xFF);
    }
    std::printf("combat height: shape 0x33C from z %d, target at %d, up to %d\n", z0, z0 + 40, top);
    CHECK(top > z0 && top <= z0 + 40);
}

// ANIMPRIM.C 1906: a non-controlled actor's second animation step advances
// from frame 0 to frame 2 when frameskip is enabled.
void test_animation_frameskip(const std::filesystem::path& data) {
    constexpr ObjId kGuard = 4;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "garde-mort-debout.sav");
    World& w = game.world();
    Item* guard = w.get(kGuard);
    CHECK(guard != nullptr);
    if (!guard) return;
    w.set_camera(guard->x, guard->y, guard->z);
    for (int f = 0; f < 10; ++f) game.update(kFrameUs);
    guard = w.get(kGuard);
    if (!guard) return;
    GameConfig config = game.config();
    config.frameskip = 3;
    game.set_config(config);
    std::uint16_t action_number = 0;
    std::uint16_t animation_shape = 0;
    const AnimAction* action = nullptr;
    for (std::uint16_t shape = 1; shape < 0x800 && !action; ++shape) {
        for (action_number = 0; action_number < AnimDat::kActions; ++action_number) {
            const AnimAction* candidate = game.animations().action(shape, action_number);
            if (!candidate || candidate->frame_count() < 5 || (candidate->flags() & 0x01) ||
                candidate->destroys_actor()) continue;
            bool simple = true;
            for (std::size_t frame = 0; frame < candidate->frame_count(); ++frame) {
                const AnimFrame* f = candidate->frame(8, frame);
                if (!f || f->special || f->event || f->fires_weapon()) simple = false;
            }
            if (simple) {
                action = candidate;
                animation_shape = shape;
                break;
            }
        }
    }
    CHECK(action != nullptr);
    if (!action) return;
    w.set_shape(*guard, animation_shape);
    w.npc_record(kGuard)[npc::kLastAnim] = 0xFF;
    w.npc_record(kGuard)[npc::kLastAnim + 1] = 0xFF;
    CHECK(do_anim(game.kernel(), w, game.animations(), kGuard, action_number, kDirCurrent, 10000, true) != 0);
    for (int f = 0; f < 120 && w.npc_record(kGuard)[npc::kAnimFrame] != 2; ++f) game.update(kFrameUs);
    CHECK(w.npc_record(kGuard)[npc::kAnimFrame] == 2);
}

// ATTACK_1110_2259 (the end of a shot of a burst): the burst ends when the
// target is dead or when the shot animation listened to failed (its flag
// 0x20). The port only looked at the target: a shot cut short (an animation
// replacing it) was followed by the rest of the burst. Guard 4 of the save
// attacks the player; its weapon is made a burst weapon (0x332), its shot is
// failed in the middle of the burst.
void test_burst_cut(const std::filesystem::path& data) {
    constexpr ObjId kGuard = 4;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "garde-mort-debout.sav");
    World& w = game.world();
    const Item* guard = w.get(kGuard);
    CHECK(guard != nullptr);
    if (!guard) return;
    w.set_camera(guard->x, guard->y, guard->z);
    // The player in front of the guard, on screen.
    w.move_item(1, guard->x, guard->y + 160, guard->z);
    w.npc_record(kGuard)[npc::kDirection] = 8;
    Item* weapon = w.get(w.active_weapon(kGuard));
    std::uint16_t burst_shape = 0;
    for (std::uint16_t s = 0x320; s < 0x3A0 && burst_shape == 0; ++s) {
        if (dtable::weapon(s).burst() >= 3) burst_shape = s;
    }
    std::printf("burst cut: weapon 0x%X burst %d, first burst weapon 0x%X\n", weapon ? weapon->shape : 0,
                weapon ? dtable::weapon(weapon->shape).burst() : 0, burst_shape);
    CHECK(weapon && burst_shape != 0);
    if (!weapon || burst_shape == 0) return;
    weapon->shape = burst_shape;
    AttackProcess* attack = nullptr;
    int frames = 0;
    for (; frames < 1200; ++frames) {
        game.update(kFrameUs);
        attack = dynamic_cast<AttackProcess*>(game.kernel().find_process(kGuard, AttackProcess::kType));
        if (attack && attack->listening() != 0 && attack->burst_left() > 1) break;
    }
    CHECK(attack && attack->listening() != 0);
    if (!attack || attack->listening() == 0) return;
    const int left = attack->burst_left();
    game.kernel().fail_processes(kGuard, ActorAnimProcess::kType, 1);
    game.update(kFrameUs);
    attack = dynamic_cast<AttackProcess*>(game.kernel().find_process(kGuard, AttackProcess::kType));
    const int after = attack ? attack->burst_left() : -1;
    std::printf("burst cut: burst under way after %d frames, %d shots left, %d after the shot failed\n", frames, left,
                after);
    CHECK(after == 0);
}

// CRUSADER_1130_000e: DeathSilence, started by the player's death, is a 1 Hz
// timer that plays sound 9 at its third period. The port waited 3 ticks.
void test_death_silence(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    settle(game);
    for (int i = 0; i < 20 && !w.is_dead(1); ++i) apply_damage(game.kernel(), w, game.animations(), 1, 0, 0, 250, 1);
    const auto alive = [&] { return game.kernel().find_process(0, 0x261) != nullptr; };
    const bool started = alive();
    int frames = 0;
    while (alive() && frames < 600) {
        game.update(kFrameUs);
        ++frames;
    }
    std::printf("death silence: dead %d, started %d, over after %d frames\n", w.is_dead(1), started, frames);
    CHECK(started && frames >= 170 && frames <= 200);
}

// A spot far enough for the player's cell to leave the fast area.
World::Point far_from(const Item& p) {
    return {p.x < 16384 ? p.x + 4096 : p.x - 4096, p.y < 16384 ? p.y + 4096 : p.y - 4096, p.z};
}

// CurrentMap::unsetChunkFast (10B0:0386, 1040:11FD): a falling item whose
// cell leaves the fast area drops to the ground and its fall ends. The port
// left it in the air, its fall waiting for the camera to come back.
// Item::leaveFastArea (10A8:2DAD): a shape with the sound flag (TYPEFLAG
// byte 6 bit 3) falls silent. The port let its sound play on.
void test_leave_fast_area(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    settle(game);
    const Item* p = w.get(1);
    CHECK(p != nullptr);
    if (!p) return;
    const World::Point at{p->x, p->y, p->z & 0xFF}, away = far_from(*p);
    std::uint16_t shape = 0, noisy = 0;
    for (std::uint16_t s = 0x100; s < 0x800 && (shape == 0 || noisy == 0); ++s) {
        const ShapeInfo* info = w.shape_info(s);
        if (!info) continue;
        if (noisy == 0 && info->is(ShapeInfo::kSound)) noisy = s;
        if (shape == 0 && !info->is(ShapeInfo::kFixed) && info->is(ShapeInfo::kSolid) && info->weight != 0 &&
            info->family == 0 && info->footpad_x == 1 && info->footpad_y == 1 && info->footpad_z == 1) {
            shape = s;
        }
    }
    const World::Point high{at.x + 64, at.y, at.z + 120};
    CHECK(shape != 0 && noisy != 0 && position_free(w, shape, high));
    if (shape == 0 || noisy == 0) return;
    const ObjId item = w.create(shape, 0);
    w.pop_to_coords(high.x, high.y, high.z);
    CHECK(fall(game.kernel(), w, item) != 0);
    const ObjId sounding = w.create(noisy, 0);
    w.pop_to_coords(at.x, at.y + 64, at.z);
    game.audio()->sources.play(w, 9, sounding);
    const bool played = game.audio()->sources.is_playing(9, sounding);
    w.set_camera(away.x, away.y, away.z);
    const Item* i = w.get(item);
    const bool falling = game.kernel().find_process(item, GravityProcess::kType) != nullptr;
    const bool still = game.audio()->sources.is_playing(9, sounding);
    std::printf("leave fast area: item 0x%X from z %d to %d, still falling %d; shape 0x%X sound %d then %d\n", shape,
                high.z, i ? i->z & 0xFF : -1, falling, noisy, played, still);
    CHECK(i && !(i->flags & kItemFastArea) && (i->z & 0xFF) < high.z && !falling);
    CHECK(played && !still);
}

// GLOB.C 039C: a glob egg near the drawn area places its items one by one,
// and they enter the fast area (tracked, events 0x0F); far from it they are
// loaded into the cell without entering it. The port never let them enter.
void test_glob_fast_area(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    int entered = 0, loaded = 0;
    w.for_each([&](const Item& i) {
        if ((i.flags & (kItemDisposable | 0x0002)) != (kItemDisposable | 0x0002) || i.parent != 0) return;
        ++((i.flags & kItemFastArea) ? entered : loaded);
    });
    std::printf("glob fast area: %d glob items entered, %d loaded\n", entered, loaded);
    CHECK(entered > 0);
}

// CRUSADER_1130_026b: a shot stopped by the player's shield shows its sprite,
// 0x5A9 at the point hit for shields 1 and 2, 0x52B on the player for the
// large one, which follows him (ITEMCACH.C 1D29) until it ends. The port
// showed nothing.
void test_shield_sprites(const std::filesystem::path& data) {
    for (const int shield : {1, 3}) {
        Game game(data, test_saves(data));
        game.load_save(13);
        World& w = game.world();
        settle(game);
        const int bit = shield == 3 ? 4 : shield;
        int type = 0;
        for (int t = 1; t < 16 && type == 0; ++t) {
            const dtable::FireType* ft = dtable::fire_type(t);
            if (ft && (ft->shield_mask() & bit)) type = t;
        }
        CHECK(type != 0);
        if (type == 0) return;
        w.set_npc_word(1, 0x66, static_cast<std::uint16_t>(shield));
        w.set_npc_word(1, 0x57, 30000);
        const Item* p = w.get(1);
        const World::Point at{p->x - 16, p->y - 16, (p->z & 0xFF) + 20};
        const std::uint16_t hp = w.npc_hp(1);
        apply_damage(game.kernel(), w, game.animations(), 1, 0, 0, 10, type, &at);
        const std::uint16_t shape = shield == 3 ? 0x52B : 0x5A9;
        ObjId sprite = 0;
        w.for_each([&](const Item& i) {
            if (i.sprite_only && i.shape == shape) sprite = i.id;
        });
        CHECK(sprite != 0 && w.npc_hp(1) == hp);
        if (shield != 3 || sprite == 0) {
            std::printf("shield sprites: shield %d, fire type %d, sprite 0x%X %s\n", shield, type, shape,
                        sprite ? "shown" : "missing");
            continue;
        }
        CHECK(w.state.shield_sprite != 0 && w.state.shield_sprite_item == sprite);
        const std::int32_t sx = w.get(sprite)->x;
        p = w.get(1);
        w.move_item(1, p->x + 32, p->y, p->z);
        const std::int32_t moved = w.get(sprite) ? w.get(sprite)->x - sx : -1;
        int frames = 0;
        while (w.state.shield_sprite != 0 && frames < 600) {
            game.update(kFrameUs);
            ++frames;
        }
        std::printf("shield sprites: large shield, fire type %d, sprite moved by %d, gone after %d frames\n", type,
                    moved, frames);
        CHECK(moved == 32 && w.state.shield_sprite == 0 && frames < 600);
    }
}

// Item::I_fireWeapon (1130:11DA) with the muzzle already inside an item: after
// the hit, the fire type's splash around the muzzle (the controlled shooter
// not spared) and its impact sprite. The port stopped at the hit.
void test_muzzle_inside(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    settle(game);
    int type = 0;
    for (int t = 1; t < 16 && type == 0; ++t) {
        const dtable::FireType* ft = dtable::fire_type(t);
        if (ft && ft->splash() > 0 && ft->impact_sprite()) type = t;
    }
    std::uint16_t shape = 0;
    for (std::uint16_t s = 0x100; s < 0x800 && shape == 0; ++s) {
        const ShapeInfo* info = w.shape_info(s);
        if (info && info->is(ShapeInfo::kSolid) && info->family == 0 && info->footpad_x == 1 &&
            info->footpad_y == 1 && info->footpad_z == 1) {
            shape = s;
        }
    }
    CHECK(type != 0 && shape != 0);
    if (type == 0 || shape == 0) return;
    w.npc_record(1)[npc::kFlags13] &= static_cast<std::uint8_t>(~0x02);
    w.set_npc_word(1, 0x66, 0);
    const Item* p = w.get(1);
    const World::Point at{p->x + 48, p->y, p->z & 0xFF};
    w.create(shape, 0);
    w.pop_to_coords(at.x, at.y, at.z);
    auto sprites = [&] {
        int n = 0;
        w.for_each([&](const Item& i) { n += i.sprite_only; });
        return n;
    };
    const int before = sprites();
    const std::uint16_t hp = w.npc_hp(1);
    fire_weapon(game.kernel(), w, game.animations(), 1, 40, -8, 4, 0, type, false);
    std::printf("muzzle inside: fire type %d, sprites %d -> %d, player hp %d -> %d\n", type, before, sprites(), hp,
                w.npc_hp(1));
    CHECK(sprites() > before && w.npc_hp(1) < hp);
}

void test_death_out_of_fast_area(const std::filesystem::path& data) {
    constexpr ObjId kGuard = 4;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "garde-mort-debout.sav");
    World& w = game.world();
    const Item* guard = w.get(kGuard);
    CHECK(guard && guard->shape == 0x2FD && !w.is_dead(kGuard) && !(guard->flags & kItemFastArea));
    if (!guard) return;
    const std::int32_t gx = guard->x, gy = guard->y, gz = guard->z;
    const std::uint16_t standing = guard->frame;
    apply_damage(game.kernel(), w, game.animations(), kGuard, 1, 0, 250, 1);
    for (int f = 0; f < 120; ++f) game.update(kFrameUs);
    guard = w.get(kGuard);
    const bool waited = guard && guard->frame == standing && !(w.npc_record(kGuard)[npc::kAnimFlags] & 0x02);
    w.set_camera(gx, gy, gz);
    for (int f = 0; f < 600; ++f) game.update(kFrameUs);
    bool body = false;
    w.for_each([&](const Item& i) {
        body = body || (i.shape == 0x339 && std::abs(i.x - gx) < 64 && std::abs(i.y - gy) < 64);
    });
    std::printf("death out of the fast area: waited %d, guard %s, body %d\n", waited,
                w.get(kGuard) ? "still standing" : "set aside", body);
    CHECK(waited && !w.get(kGuard) && body);
    CHECK(game.machine().faults().empty());
}

// The force field floor (FFFLOOR 0x135, flat, 64 x 64; frame 0 its outline)
// of TEST10 (docs/bugs.md, "un piège ne s'affiche pas correctement") lies on
// floor tiles (0x14, flat, 256 x 256) at the same height: both are large
// square flats, the larger is drawn first. Before, the tile it lay on was
// drawn over it and only bits of its outline showed.
void test_force_field_floor(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "piege-sprite-mal-affiche.sav");
    World& w = game.world();
    w.move_item(1, 30450, 5500, 96);
    for (int i = 0; i < 5; ++i) game.update(kFrameUs);
    const FlexArchive flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    ShapeCache shapes(flex, ShapeLayout::World);
    const auto list = build_display_list(w, shapes, View{w.state.view_left, w.state.view_top}, 640, 480);
    int fields = 0, checked = 0, wrong = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const DrawItem& f = list[i];
        if (f.item->shape != 0x135 || f.item->frame != 0) continue;
        ++fields;
        for (std::size_t j = 0; j < list.size(); ++j) {
            const DrawItem& t = list[j];
            if (t.item->shape != 0x14 || t.z != f.z) continue;
            if (t.x <= f.x_left || f.x <= t.x_left || t.y <= f.y_far || f.y <= t.y_far) continue;
            ++checked;
            wrong += j > i;
        }
    }
    std::printf("force field floor: %d outlines on screen, %d on tiles, %d drawn under their tile\n", fields, checked,
                wrong);
    CHECK(fields > 0 && checked >= fields && wrong == 0);
}

// "PB SPRITE" (triangle-noir-machine.sav, map 13): a black panel (0x175,
// 32 x 512 x 96, it masks the void beyond the map) crosses the end of the
// machine wall (0x127 frame 3, 512 x 32 x 96, TYPEFLAG roof bit). The
// original's comparator (DLIST 11A0:0000) orders such intersecting boxes by
// their far bottom corners: the panel, further back, is drawn first. The
// port's former rules drew roofs first, and the panel's top left corner
// covered the machine as a black triangle.
void test_void_panel_behind_wall(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "triangle-noir-machine.sav");
    game.update(kFrameUs);
    const World& w = game.world();
    const FlexArchive flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    ShapeCache shapes(flex, ShapeLayout::World);
    const auto list = build_display_list(w, shapes, View{w.state.view_left, w.state.view_top}, 640, 480);
    int panel = -1, wall = -1;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const DrawItem& d = list[i];
        if (d.item->shape == 0x175 && d.x == 54334 && d.y == 54302) panel = static_cast<int>(i);
        if (d.item->shape == 0x127 && d.item->frame == 3 && d.x == 54782 && d.y == 54302) wall = static_cast<int>(i);
    }
    std::printf("void panel: drawn %d, machine wall drawn %d\n", panel, wall);
    CHECK(panel >= 0 && wall >= 0 && panel < wall);
}

// Mission 7 (map 13): its egg MISS7 (class 0x913) gives the player the
// fusion pack (FUSPAC 0x39C) that blows up the reservoir, from its cachein
// event (4) when the map loads. The egg lies in a corner of the map, near
// (0, 0), where the teleport first takes the player to load the map: it
// enters the fast area there, then leaves it when the player reaches the
// start egg, and that fails its processes. The original's events run at
// once (Item::callUsecodeEvent, US 0086:0E3A), so the pack is given before;
// queued for the next pass, the event was failed first and the pack never
// came (saves "PB SPRITE", "DECORSCLIGNOTE": no FUSPAC anywhere).
void test_mission7_fusion_pack(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "carte11-vers-mission7-bombe-fusion.sav");
    World& w = game.world();
    CHECK(w.current_map() == 11);
    start_switch_map(game.kernel(), w, 13);
    for (int f = 0; f < 120; ++f) game.update(kFrameUs);
    int packs = 0, carried = 0;
    w.for_each([&](const Item& i) {
        if (i.shape != 0x39C) return;
        ++packs;
        carried += i.parent == 1;
    });
    std::printf("mission 7: map %d, %d fusion pack(s), %d carried by the player\n", w.current_map(), packs, carried);
    CHECK(w.current_map() == 13 && packs == 1 && carried == 1);
    CHECK(game.machine().faults().empty());
}

// Lot 6.4: the end of the game, EVENT (class 0x361) equip(88): movie O01,
// then Game::I_playCredits: the credits, the start-up movies T01 and T02,
// and the menu with no game to go back to.
void test_end_credits(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    const std::uint8_t ending[] = {88, 0};
    CHECK(game.machine().start_event(0x361, 0x0A, 1, ending) != 0);
    // The movies chain: the next one may be on as soon as one is skipped.
    auto skip_movie = [&](const char* name) {
        int f = 0;
        for (; f < 600 && !game.movie(); ++f) game.update(kFrameUs);
        std::printf("end: movie %s after %d frames\n", name, f);
        CHECK(game.movie() != nullptr);
        game.key(0x01, false, true, 0, 0x1B);
        game.update(kFrameUs);
        game.key(0x01, false, false, 0);
    };
    auto credits = [&] { return game.menu() && const_cast<MainMenu*>(game.menu())->credits(); };
    skip_movie("O01");
    for (int f = 0; f < 120 && !credits(); ++f) game.update(kFrameUs);
    for (int f = 0; f < 30; ++f) game.update(kFrameUs);
    CHECK(credits() && game.audio()->music.current() == Music::kCredits);
    CHECK(!game.movie());
    press(game, 0x39, false, ' ');
    skip_movie("T01");
    skip_movie("T02");
    CHECK(!game.movie());
    game.update(kFrameUs);
    CHECK(game.menu() && !credits() && game.audio()->music.current() == Music::kMenu);
    // No game to go back to: Escape leaves the menu open.
    press(game, 0x01, false, 0x1B);
    game.update(kFrameUs);
    CHECK(game.menu() && !game.menu()->result());
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Normal launch (Game_start): T01, T02, then the menu with no game, from
// which "load" leads into a game.
void test_start_up(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.start_up();
    for (const char* name : {"T01", "T02"}) {
        int f = 0;
        for (; f < 600 && !game.movie(); ++f) game.update(kFrameUs);
        std::printf("start-up: movie %s after %d frames\n", name, f);
        CHECK(game.movie() != nullptr);
        game.key(0x01, false, true, 0, 0x1B);
        game.update(kFrameUs);
        game.key(0x01, false, false, 0);
    }
    CHECK(!game.movie());
    game.update(kFrameUs);
    CHECK(game.menu() && game.audio()->music.current() == Music::kMenu);
    // The colour cycling stays paused, or it flickers over the menu palette.
    CHECK(game.world().state.cycle_paused);
    press(game, 0x01, false, 0x1B);
    game.update(kFrameUs);
    CHECK(game.menu() && !game.menu()->result());
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

// Weasel's shop (WeaselGump::I_showWeaselGump, 0x12D): save 5 has the booth
// (WEA_BOOT 7233) and global 0x1F = 2.
void test_weasel(const std::filesystem::path& data) {
    constexpr ObjId kBooth = 7233;
    Game game(data, test_saves(data));
    game.load_save(5);
    World& w = game.world();
    const auto credits_carried = [&w] { return carried(w, WeaselGump::kCredits) ? w.get(carried(w, 0x4ED))->quality : 0; };
    const int before = credits_carried();
    CHECK(game.machine().start_event(0x45B, 0x0A, kBooth) != 0);
    const auto gump = [&game] { return dynamic_cast<const WeaselGump*>(game.screen()); };
    int f = 0;
    for (; f < 900 && !gump(); ++f) game.update(kFrameUs);
    std::printf("weasel: open after %d frames, level %d\n", f, gump() ? gump()->level() : -1);
    CHECK(gump() != nullptr);
    if (!gump()) return;
    // The first step came at the same pass: the gump stays hidden behind its movie.
    CHECK(gump()->level() == 2 && gump()->x() == 214 && gump()->y() == 6);
    CHECK(!game.world().state.status_bar_visible && game.cursor().visible());
    // A step at each pass, with its movie; Escape skips the movie.
    const auto skip_movie = [&](const char* name) {
        int n = 0;
        for (; n < 10 && !game.movie(); ++n) game.update(kFrameUs);
        std::printf("weasel: movie %s after %d frames\n", name, n);
        CHECK(game.movie() != nullptr);
        game.key(0x01, false, true, 0, 0x1B);
        for (n = 0; n < 600 && game.movie(); ++n) game.update(kFrameUs);
        game.key(0x01, false, false, 0);
        CHECK(!game.movie());
    };
    skip_movie("17A");
    CHECK(gump()->state() == WeaselGump::kBrowsing && gump()->shown());
    CHECK(gump()->credits() == before && gump()->items().size() == 6 && gump()->current() == 0);
    CHECK(gump()->button_shown(WeaselGump::kBuy) && gump()->button_shown(WeaselGump::kAmmo) &&
          !gump()->button_shown(WeaselGump::kWeapons) && !gump()->button_shown(WeaselGump::kYes));
    std::printf("weasel: credits %d, items", gump()->credits());
    for (const auto& i : gump()->items()) std::printf(" 0x%X:%u", i.shape, i.price);
    std::printf("\n");
    // The last item: previous from the first goes round.
    press(game, 0x4B, true);
    CHECK(gump()->current() == 5 && gump()->items()[5].shape == 0x39A);
    const int owned_before = carried(w, 0x39A) ? w.get(carried(w, 0x39A))->quality : 0;
    press(game, 0x1C, false, 0x0D);  // buy
    press(game, 0x39, false, ' ');   // buy again
    CHECK(gump()->credits() == before - 400 && gump()->cart().size() == 2);
    // The ammunition: 0x341 and 0x340 left out at level 2.
    press(game, 0x0F, false, '\t');
    CHECK(gump()->level() == 1 && gump()->button_shown(WeaselGump::kWeapons) &&
          !gump()->button_shown(WeaselGump::kAmmo));
    int ammo = 0;
    for (const auto& i : gump()->items()) ammo += i.shape != 0;
    std::printf("weasel: ammunition, %d of %zu items\n", ammo, gump()->items().size());
    CHECK(ammo == 4);
    // Exit: back to the weapons, the confirmation with its movie.
    press(game, 0x01, false, 0x1B);
    CHECK(gump()->level() == 2 && gump()->state() == WeaselGump::kConfirm);
    skip_movie("21A/21B");
    CHECK(gump()->state() == WeaselGump::kConfirm && gump()->button_shown(WeaselGump::kYes) &&
          !gump()->button_shown(WeaselGump::kBuy));
    // Refused: the credits back, the cart emptied.
    press(game, 0x31, false, 'n');
    CHECK(gump()->credits() == before && gump()->cart().empty() && gump()->state() == WeaselGump::kRefused);
    skip_movie("21E/21F");
    CHECK(gump()->state() == WeaselGump::kQuestion);
    // Yes: browsing again; buy with the mouse on "buy", then confirm.
    // The weapons read again when leaving: the first item.
    press(game, 0x18, false, 'o');
    CHECK(gump()->state() == WeaselGump::kBrowsing && gump()->current() == 0);
    press(game, 0x4B, true);
    const auto click = [&](int button) {
        const WeaselGump::Rect r = gump()->button_rect(button);
        const int x = (r.x0 + r.x1) / 2, y = (r.y0 + r.y1) / 2;
        game.mouse_move(x, y);
        game.mouse_button(true, x, y);
        game.mouse_button(false, x, y);
    };
    click(WeaselGump::kBuy);
    CHECK(gump()->credits() == before - 200 && gump()->cart().size() == 1);
    click(WeaselGump::kExit);
    skip_movie("21A/21B");
    press(game, 0x15, false, 'y');
    CHECK(gump()->state() == WeaselGump::kBought);
    const int owned_after = carried(w, 0x39A) ? w.get(carried(w, 0x39A))->quality : 0;
    std::printf("weasel: bought, credits %d -> %d, 0x39A %d -> %d\n", before, credits_carried(), owned_before,
                owned_after);
    CHECK(credits_carried() == before - 200 && owned_after == owned_before + 1);
    skip_movie("21C/21D");
    CHECK(gump()->state() == WeaselGump::kQuestion);
    // No, after buying: closed at the next step.
    press(game, 0x31, false, 'n');
    CHECK(gump() && gump()->state() == WeaselGump::kClosing);
    game.update(kFrameUs);
    CHECK(!game.screen() && game.world().state.status_bar_visible);
    for (int n = 0; n < 300; ++n) game.update(kFrameUs);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    // The first movie only once.
    game.show_weasel(2);
    skip_movie("18A/18B/18C");
    CHECK(gump() && gump()->state() == WeaselGump::kBrowsing);
    press(game, 0x01, false, 0x1B);
    skip_movie("19C/19D");
    press(game, 0x01, false, 0x1B);
    CHECK(!game.screen());
}

// Issue #13: Weasel's shop, drawn at a native resolution larger than 640 x
// 480. The snapshot under each of Weasel's movies (640 x 480) resized the
// view, so the fast area: items entering it again started their usecode at
// the shop's raised kernel level, the booth's endless idle animation (type
// 0x21B) among them. That level stayed up after the shop, the game below never
// ran again and the Silencer stayed in stasis. The user's TEST23: in front of
// the booth at the base, the shop opened by the selection (S, Enter).
void test_weasel_native_view(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "weasel-silencer-fige-en-resolution-native.sav");
    World& w = game.world();
    Surface8 screen(1280, 720);
    const auto frame = [&] {
        game.update(kFrameUs);
        game.render(screen);
    };
    const auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) frame();
    };
    const auto gump = [&game] { return dynamic_cast<const WeaselGump*>(game.screen()); };
    // Enter, held after the movie's fade in, skips it.
    const auto skip_movie = [&] {
        for (int n = 0; n < 10 && !game.movie(); ++n) frame();
        CHECK(game.movie() != nullptr);
        frames(90);
        game.key(0x1C, false, true, 0, 0x0D);
        frames(8);
        game.key(0x1C, false, false, 0);
        frames(3);
        CHECK(!game.movie());
    };
    frames(60);
    press(game, 0x1F, false, 's');
    press(game, 0x1C, false, 0x0D);
    for (int f = 0; f < 300 && !gump(); ++f) frame();
    CHECK(gump() != nullptr);
    if (!gump()) return;
    skip_movie();
    press(game, 0x01, false, 0x1B);  // exit, nothing bought
    skip_movie();
    press(game, 0x31, false, 'n');  // nothing else
    frames(120);
    std::printf("weasel native view: screen %d, stasis %d, kernel level %u\n", game.screen() != nullptr,
                static_cast<int>(w.state.avatar_in_stasis), game.kernel().level());
    CHECK(!game.screen() && !w.state.avatar_in_stasis && game.kernel().level() == 1);
    // Facing east (free there), the Silencer walks.
    const Item start = *w.get(1);
    if (auto r = w.npc_record(1); !r.empty()) r[npc::kDirection] = 4;
    game.key(0x48, true, true, 0);
    frames(120);
    game.key(0x48, true, false, 0);
    const Item& now = *w.get(1);
    std::printf("weasel native view: walked from (%d, %d) to (%d, %d)\n", start.x, start.y, now.x, now.y);
    CHECK(now.x != start.x || now.y != start.y);
    CHECK(game.machine().faults().empty());
}

// Milestone 7: saving with F9 (the save screen and its name), F5 (quick
// save) and the menu's "save game", in a copy of the saves, then loading back.
void test_save_game(const std::filesystem::path& data) {
    const auto dir = std::filesystem::temp_directory_path() / "crusader_save_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (const char* n : {"SAVEGAME.1", "SAVEGAME.13"}) std::filesystem::copy_file(test_saves(data) / n, dir / n);
    Game game(data, test_saves(data));
    game.set_save_dir(dir);
    CHECK(game.save_names()[0] == "SEB" && game.save_names()[2].empty());
    game.load_save(1);
    settle(game);
    World& w = game.world();
    auto type = [&](const std::string& text) {
        for (char c : text) press(game, 0x10, false, static_cast<std::uint8_t>(c));
    };
    auto name_of = [](const FlexArchive& f) {
        const auto e = f.entry(9);
        return std::string(e.begin(), std::find(e.begin(), e.end(), std::uint8_t{0}));
    };

    // F9, down twice: slot 2; Enter edits its (empty) name. Lower case
    // becomes upper case, Backspace removes, Enter saves SAVEGAME.3.
    press(game, 0x43, false);
    CHECK(game.screen() && w.state.cycle_paused);
    press(game, 0x50, true);
    press(game, 0x50, true);
    press(game, 0x1C, false, 0x0D);
    type("sauvx");
    press(game, 0x0E, false, 8);
    type("e 1");
    CHECK(game.screen() && !std::filesystem::exists(dir / "SAVEGAME.3"));
    press(game, 0x1C, false, 0x0D);
    CHECK(!game.screen() && !w.state.cycle_paused);
    CHECK(game.save_names()[2] == "SAUVE 1");
    CHECK(std::filesystem::exists(dir / "SAVEGAME.3"));
    if (!std::filesystem::exists(dir / "SAVEGAME.3")) return;
    {
        const auto saved = FlexArchive::open(dir / "SAVEGAME.3");
        CHECK(name_of(saved) == "SAUVE 1");
        const auto table = w.saved_item_table();
        CHECK(std::ranges::equal(saved.entry(0), table));
        CHECK(saved.entry(20).size() == 0x15 && saved.entry(60).size() == 0x42 && saved.entry(52).size() == 200);
        CHECK(saved.entry(100).empty());  // the kernel is not written
    }
    CHECK(LoadScreen::read_names(dir)[2] == "SAUVE 1");

    // Loading it back: the player, the globals, the difficulty and the items.
    const Item before = *w.get(1);
    const auto globals = game.machine().globals();
    const std::size_t items = w.item_count();
    game.load_save(3);
    World& w2 = game.world();
    const Item* player = w2.get(1);
    CHECK(player && player->x == before.x && player->y == before.y && player->z == before.z &&
          player->frame == before.frame);
    CHECK(std::equal(globals.begin(), globals.begin() + 0x42, game.machine().globals().begin()));
    CHECK(w2.item_count() == items);
    std::printf("save 3: %zu items, player (%d, %d, %d)\n", w2.item_count(), player ? player->x : 0,
                player ? player->y : 0, player ? player->z : 0);
    settle(game);

    // F5, yes: SAVEGAME.13 under its (empty) name, then the notice.
    const auto quick_before = std::filesystem::last_write_time(dir / "SAVEGAME.13");
    press(game, 0x3F, false);
    const auto* ask = dynamic_cast<const ShapeDialog*>(game.screen());
    CHECK(ask && ask->shape() == ShapeDialog::kQuickSaveQuestion);
    press(game, 0x15, false, 'y');
    ask = dynamic_cast<const ShapeDialog*>(game.screen());
    CHECK(ask && ask->shape() == ShapeDialog::kQuickSaveNotice);
    press(game, 0x1C, false, 0x0D);
    CHECK(!game.screen());
    CHECK(std::filesystem::last_write_time(dir / "SAVEGAME.13") != quick_before);
    CHECK(name_of(FlexArchive::open(dir / "SAVEGAME.13")).empty());

    // The menu's "save game": the same screen, slot 2 still selected; its
    // name edited to "SAUVE 2", the menu closes and the game is saved.
    press(game, 0x01, false, 0x1B);
    CHECK(game.menu());
    if (!game.menu()) return;
    const auto b = game.menu()->button(MainMenu::kSave);
    game.mouse_button(true, b.x0 + 5, b.y0 + 5);
    game.mouse_button(false, b.x0 + 5, b.y0 + 5);
    CHECK(game.menu() && game.menu()->load_screen() &&
          game.menu()->load_screen()->mode() == LoadScreen::Mode::kSave);
    press(game, 0x1C, false, 0x0D);
    CHECK(game.menu() && game.menu()->load_screen() && game.menu()->load_screen()->editing() &&
          game.menu()->load_screen()->name() == "SAUVE 1");
    press(game, 0x0E, false, 8);
    type("2");
    press(game, 0x1C, false, 0x0D);
    CHECK(!game.menu());
    CHECK(name_of(FlexArchive::open(dir / "SAVEGAME.3")) == "SAUVE 2");
    CHECK(game.machine().faults().empty());
    std::filesystem::remove_all(dir);
}

// Port addition, for consoles without a keyboard (--auto-save-names, the
// PortMaster launcher): choosing a slot to save proposes the date and time,
// Enter (the pad's A) saves under it; Escape (B) leaves the slot as it was.
void test_auto_save_names(const std::filesystem::path& data) {
    std::tm t{};
    t.tm_year = 2026 - 1900, t.tm_mon = 10, t.tm_mday = 12, t.tm_hour = 9, t.tm_min = 5;
    CHECK(LoadScreen::dated_name(t) == "12/11/2026 09:05");
    std::tm unset{};
    unset.tm_year = 70;
    CHECK(LoadScreen::dated_name(unset) == "SAUVEGARDE");

    const auto dir = std::filesystem::temp_directory_path() / "crusader_auto_save_name_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(test_saves(data) / "SAVEGAME.1", dir / "SAVEGAME.1");
    Game game(data, test_saves(data));
    game.set_save_dir(dir);
    game.set_auto_save_names(true);
    game.load_save(1);
    settle(game);
    // Every character of the proposed name has a glyph in the slots' font, and
    // the name with '_' fits a slot as a typed one must.
    const TextWriter writer(game.fonts(), LoadScreen::kFont);
    for (char c : std::string("0123456789/: ")) CHECK(writer.char_width(static_cast<unsigned char>(c)) > 0);
    CHECK(writer.measure("28/12/2026 23:59_").width <= 314 - 4);

    // F9, down: slot 1 (empty); Escape leaves it, Enter again proposes the
    // date and saves under it.
    press(game, 0x43, false);
    press(game, 0x50, true);
    press(game, 0x1C, false, 0x0D);
    press(game, 0x01, false, 0x1B);
    CHECK(game.screen() && game.save_names()[1].empty());
    press(game, 0x1C, false, 0x0D);
    press(game, 0x1C, false, 0x0D);
    CHECK(!game.screen());
    const std::string name = game.save_names()[1];
    CHECK(name.size() == 16 && name[2] == '/' && name[5] == '/' && name[13] == ':');
    CHECK(LoadScreen::read_names(dir)[1] == name);

    // The menu's "save game" on slot 0 ("SEB"): the date replaces its name.
    press(game, 0x01, false, 0x1B);
    CHECK(game.menu());
    if (!game.menu()) return;
    const auto b = game.menu()->button(MainMenu::kSave);
    game.mouse_button(true, b.x0 + 5, b.y0 + 5);
    game.mouse_button(false, b.x0 + 5, b.y0 + 5);
    press(game, 0x48, true);
    press(game, 0x1C, false, 0x0D);
    CHECK(game.menu() && game.menu()->load_screen() && game.menu()->load_screen()->editing() &&
          game.menu()->load_screen()->name().size() == 16);
    press(game, 0x1C, false, 0x0D);
    CHECK(!game.menu());
    CHECK(LoadScreen::read_names(dir)[0].size() == 16);
    CHECK(game.machine().faults().empty());
    std::filesystem::remove_all(dir);
}

// Item::I_destroy (10A8:1365): what rests on a crate falls when the crate is
// destroyed. For items carried by crates in the saved games, every crate
// under the item is destroyed on a fresh load; the item must end lower (an
// item on several crates stays up while one of them remains).
void test_crate_destroyed(const std::filesystem::path& data) {
    constexpr std::uint16_t kCrates[] = {0x432, 0x5A0, 0x08F, 0x59F, 0x2B3, 0x2CA, 0x1FD, 0x1FE};
    static constexpr std::uint8_t kAll[] = {'$'};
    auto is_crate = [&](const Item& i) {
        return i.parent == 0 && std::find(std::begin(kCrates), std::end(kCrates), i.shape) != std::end(kCrates);
    };
    int tried = 0, fell = 0, stayed = 0;
    for (int slot : {1, 2, 3, 4, 5, 6, 7, 8, 9, 13}) {
        if (!std::filesystem::exists(test_saves(data) / ("SAVEGAME." + std::to_string(slot)))) continue;
        // Carried item -> the crates under it.
        std::map<ObjId, std::vector<ObjId>> under;
        {
            Game game(data, test_saves(data));
            game.load_save(slot);
            World& w = game.world();
            w.for_each([&](const Item& i) {
                if (!is_crate(i)) return;
                for (ObjId above : w.surface_search(i.id, 1, kAll)) {
                    if (above != i.id) under[above].push_back(i.id);
                }
            });
        }
        int n = 0;
        for (const auto& [top, crates] : under) {
            if (++n > 6) break;
            Game game(data, test_saves(data));
            game.load_save(slot);
            World& w = game.world();
            const Item* item = w.get(top);
            if (!item) continue;
            const int z = item->z;
            ++tried;
            for (ObjId crate : crates) destroy_item(game.kernel(), w, crate);
            for (int f = 0; f < 120; ++f) game.update(kFrameUs);
            item = w.get(top);
            if (item && item->z < z) ++fell;
            else ++stayed;
            CHECK(game.machine().faults().empty());
        }
    }
    std::printf("crates destroyed: %d carried items tried, %d fell, %d stayed\n", tried, fell, stayed);
    CHECK(tried > 0 && fell > 0);
}

// Item::I_create (10A8:11E7): a weapon created by a script (family 10) holds
// the rounds of a full clip (DTABLE_1120_0180); the NPCs' weapons are made
// this way, and carried the loot of a dead NPC with them.
void test_created_weapon_has_a_clip(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(1);
    World& w = game.world();
    int checked = 0, empty = 0;
    for (int i = 1; i < 64; ++i) {
        const std::uint16_t shape = dtable::weapon_at(i).shape();
        if (shape == 0 || w.family_of(shape) != 10 || dtable::clip_size(shape) == 0) continue;
        const ObjId id = w.create(shape, 0);
        if (id == 0) continue;
        ++checked;
        empty += w.get(id)->quality != dtable::clip_size(shape);
        w.destroy(id);
    }
    std::printf("created weapons: %d checked, %d without a full clip\n", checked, empty);
    CHECK(checked > 0 && empty == 0);
}

// Issue #17: the BA-40 (0x32E) fires forever at difficulty 1 only. Setting the
// difficulty (difficulty screen 13D0:2975 / 29B3, game load 1058:02F1) also
// sets its ammo type DS:2172: none at level 1 (no ammo counter), else the
// BA-41 clips (0x33D, 20 rounds).
void test_pistol_ammo_by_difficulty(const std::filesystem::path& data) {
    constexpr std::uint16_t kPistol = 0x32E, kClip = 0x33D;
    for (int level : {1, 3, 1}) {
        Game game(data, test_saves(data));
        game.new_game(level);
        World& w = game.world();
        const bool unlimited = level == 1;
        CHECK(dtable::ammo_shape(kPistol) == (unlimited ? 0 : kClip));
        CHECK(dtable::clip_size(kPistol) == (unlimited ? 0 : 20));
        // A pistol created (as MISS1EGG gives it) comes loaded with a clip.
        const ObjId id = w.create(kPistol, 0);
        CHECK(id && w.get(id)->quality == (unlimited ? 0 : 20));
    }
    // From the difficulty screen: Enter on the level below the selected one
    // (so above 1), after a level 1 game.
    {
        Game g(data, test_saves(data));
        g.load_save(1);
        dtable::set_difficulty(1);
        press(g, 0x01, false, 0x1B);
        press(g, 0x1C, false, 0x0D);
        press(g, 0x50, true);  // down
        press(g, 0x1C, false, 0x0D);
        g.update(kFrameUs);
        CHECK(!g.menu() && g.world().state.difficulty >= 2 && dtable::ammo_shape(kPistol) == kClip);
    }
    // Loading a game sets it from the saved difficulty, whatever came before:
    // SAVEGAME.1 is at level 1, garde-mort-debout.sav at level 3.
    for (int saved : {1, 3}) {
        const auto save = test_saves(data) / (saved == 1 ? "SAVEGAME.1" : "garde-mort-debout.sav");
        if (!std::filesystem::exists(save)) continue;
        Game game(data, test_saves(data));
        dtable::set_difficulty(saved == 1 ? 3 : 1);
        game.load_save_file(save);
        CHECK(game.world().state.difficulty == saved);
        CHECK(dtable::ammo_shape(kPistol) == (saved == 1 ? 0 : kClip));
    }
}

// PBM2: walk forward to the mission's last scene. The avatar must get through
// the door before 2h.AVI, the NPC runs away, and the prison shot stays visible
// until the script returns to the room and gives back control.
void test_mission2_final_scene(const std::filesystem::path& data) {
    const auto save = test_saves(data) / "fin-mission2-deplacement-camera.sav";
    if (!std::filesystem::exists(save)) return;
    for (bool follow : {false, true}) {
        Game game(data, test_saves(data));
        auto config = game.config();
        config.follow_player = follow;
        config.autosave = false;
        game.set_config(config);
        game.load_save_file(save);
        World& w = game.world();
        CHECK(w.current_map() == 3);
        game.key(0x48, true, true, 0);
        bool started = false, movie = false, returned = false, escaped = false;
        ObjId npc = 0;
        World::Point npc_start{};
        int prison_frames = 0;
        for (int f = 0; f < 3600; ++f) {
            game.update(kFrameUs);
            if (w.state.avatar_in_stasis) {
                started = true;
                game.key(0x48, true, false, 0);
            }
            if (f == 600) game.key(0x48, true, false, 0);
            if (game.movie() && !movie) {
                movie = true;
                const Item* player = w.get(1);
                CHECK(player && std::abs(player->x - 22031) < 96 && std::abs(player->y - 15135) < 96);
                w.for_each([&](const Item& i) {
                    if (is_npc_id(i.id) && i.shape == 0x4D9) {
                        npc = i.id;
                        npc_start = w.location(i);
                        CHECK(w.is_partly_on_screen(i));
                    }
                });
                CHECK(npc != 0);
            }
            game.key(0x01, false, game.movie() != nullptr, 0, 0x1B);
            if (const Item* i = w.get(npc)) escaped |= std::abs(i->x - npc_start.x) > 64;
            if (w.state.camera_x == 21023 && w.state.camera_y == 16799) ++prison_frames;
            if (prison_frames > 0 && w.state.camera_x == 22295 && w.state.camera_y == 15041) returned = true;
            if (started && !w.state.avatar_in_stasis && !game.movie()) break;
        }
        CHECK(started && movie && escaped && returned && prison_frames > 1000);
        CHECK(!w.state.avatar_in_stasis && w.state.camera_target == 1);
        CHECK(w.is_partly_on_screen(*w.get(1)));
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
        // Normal input and smooth following work again after the scene.
        const Item before = *w.get(1);
        game.key(0x48, true, true, 0);
        for (int f = 0; f < 30; ++f) game.update(kFrameUs);
        game.key(0x48, true, false, 0);
        CHECK(w.get(1)->x != before.x || w.get(1)->y != before.y);
        if (follow) CHECK(w.state.camera_x != 22295 || w.state.camera_y != 15041);
        std::printf("mission 2 scene: follow %d, room (%d, %d), prison %d frames, escaped %d, returned %d\n",
                    follow, before.x, before.y, prison_frames, escaped, returned);
    }
}

// Updated PBM2: open the door, watch 2j.AVI, let the NPC teleport, then move.
// A newly created NPC with map 0 used to leave its freed id in a nearby cell;
// the next walk's collision check then dereferenced the missing item.
void test_mission2_npc_teleport(const std::filesystem::path& data) {
    const auto save = test_saves(data) / "fin-mission2-teleportation-plantage.sav";
    if (!std::filesystem::exists(save)) return;
    for (bool follow : {false, true}) {
        Game game(data, test_saves(data));
        auto config = game.config();
        config.follow_player = follow;
        config.autosave = true;
        game.set_config(config);
        game.load_save_file(save);
        World& w = game.world();
        Surface8 screen(960, 720);
        w.set_view_size(960, 720);
        CHECK(w.current_map() == 3);
        CHECK(w.get(5517) && w.get(5517)->shape == 0xA1);
        use_item(w, 5517);  // the door's wall switch
        for (int f = 0; f < 30; ++f) game.update(kFrameUs);
        game.key(0x48, true, true, 0);
        bool started = false, movie = false, teleporting = false;
        ObjId npc = 0;
        for (int f = 0; f < 3000; ++f) {
            game.update(kFrameUs);
            if (w.state.avatar_in_stasis) {
                started = true;
                game.key(0x48, true, false, 0);
            }
            if (f == 600) game.key(0x48, true, false, 0);
            if (game.movie() && !movie) {
                movie = true;
                w.for_each([&](const Item& i) {
                    if (is_npc_id(i.id) && i.shape == 0x3AC) {
                        npc = i.id;
                        CHECK(i.map_number == 3);
                    }
                });
            }
            game.key(0x01, false, game.movie() != nullptr, 0, 0x1B);
            if (w.get(npc)) teleporting |= w.npc_word(npc, npc::kLastAnim) == 0x21;
            if (started && !w.state.avatar_in_stasis && !game.movie()) break;
        }
        CHECK(started && movie && teleporting && npc != 0 && !w.get(npc));
        CHECK(!w.state.avatar_in_stasis && w.state.controlled_npc == 1);
        // Check the actual grid even on builds whose allocation order happens
        // to keep the dangling cell outside the next movement's sweep.
        for (int x = 0; x < World::kGridSize; ++x) {
            for (int y = 0; y < World::kGridSize; ++y) {
                const auto& cell = w.cell(x, y);
                CHECK(std::find(cell.begin(), cell.end(), npc) == cell.end());
            }
        }
        const Item before = *w.get(1);
        game.key(0x48, true, true, 0);
        for (int f = 0; f < 120; ++f) {
            game.update(kFrameUs);
            game.render(screen);
        }
        game.key(0x48, true, false, 0);
        CHECK(w.get(1)->x != before.x || w.get(1)->y != before.y);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
        std::printf("mission 2 teleport: follow %d, NPC %u removed, player (%d, %d) -> (%d, %d)\n",
                    follow, npc, before.x, before.y, w.get(1)->x, w.get(1)->y);
    }
}

// TEST16: CONVFLR starts CONVEYOR when the player steps onto the belt.
// The script pushes each passenger before legalMoveToPoint. A successful
// move must pop it back onto the map, or subsequent walks animate in place
// while the player is absent from rendering, collisions and surface searches.
void test_conveyor_passenger(const std::filesystem::path& data) {
    for (int dir : {4, 6}) {
        Game game(data, test_saves(data));
        game.load_save_file(test_saves(data) / "tapis-roulants-traverses.sav");
        World& w = game.world();
        const Item start = *w.get(1);
        const auto pending = w.pending_count();
        w.npc_record(1)[npc::kDirection] = static_cast<std::uint8_t>(dir);
        bool ethereal = false, on_belt = false, occupied = false, unlinked = false;
        game.key(0x48, true, true, 0);
        for (int f = 0; f < 90; ++f) {
            game.update(kFrameUs);
            const Item& p = *w.get(1);
            ethereal |= (p.flags & kItemEthereal) != 0;
            const auto cell = w.cell(p.x >> World::kCellShift, p.y >> World::kCellShift);
            unlinked |= std::count(cell.begin(), cell.end(), ObjId{1}) != 1;
            occupied |= !position_free(w, p.shape, w.location(p), nullptr, 1);
            static constexpr std::uint8_t kAll[] = {'$'};
            for (ObjId id : w.surface_search(1, 2, kAll)) {
                if (w.get(id)->shape == 0x22C) on_belt = true;
            }
        }
        game.key(0x48, true, false, 0);
        const Item& end = *w.get(1);
        std::printf("conveyor: dir %d, (%d,%d,%d) -> (%d,%d,%d), on belt %d, ethereal %d\n",
                    dir, start.x, start.y, start.z, end.x, end.y, end.z, on_belt, ethereal);
        CHECK(on_belt && !ethereal && !unlinked && !occupied && !w.is_dead(1));
        CHECK(end.x > start.x + 100);
        if (dir == 4) CHECK(end.y < start.y);  // the belt carries him north while he walks east
        CHECK(w.pending_count() == pending);
        CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
    }
}

// Décors qui clignotent (docs/bugs.md, save DECORSCLIGNOTE): after a while,
// walls, floors and bodies flickered between the frames of their shape.
// World::destroy freed the slot of a glob item gone with its cell but left
// its processes running; the next item created took the slot (head of the
// free list) and those processes went on with it, setting its frame.
// Item_forget (10B0:1AB3) ends with Kernel_fail_processes(id, any type).
// The camera sweeps the map around the player for ten minutes (one second
// per place, 7 x 7 places 1024 units apart) to load and unload cells.
void test_processes_end_with_their_item(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "decors-clignotent.sav");
    World& w = game.world();
    Kernel& k = game.kernel();
    const Item* player = w.get(1);
    CHECK(player != nullptr);
    if (!player) return;
    const std::int32_t px = player->x, py = player->y, pz = player->z;
    // (pid, owner, type) of a live process -> its owner's shape when first seen.
    std::map<std::tuple<Pid, ObjId, std::uint16_t>, std::uint16_t> owner_shape;
    int moved = 0;
    for (int f = 0; f < 36000; ++f) {
        if (f % 60 == 0) {
            const int step = f / 60;
            const int dx = (step % 7 - 3) * 1024, dy = (step / 7 % 7 - 3) * 1024;
            w.set_camera(std::max(0, px + dx), std::max(0, py + dy), pz);
        }
        game.update(kFrameUs);
        std::map<std::tuple<Pid, ObjId, std::uint16_t>, std::uint16_t> live;
        k.for_each_process([&](const Process& p) {
            if (p.owner() < 256) return;
            const Item* i = w.get(p.owner());
            const std::uint16_t shape = i ? i->shape : 0xFFFF;
            const auto key = std::make_tuple(p.pid(), p.owner(), p.type());
            const auto it = owner_shape.find(key);
            if (it != owner_shape.end() && it->second != shape) ++moved;
            live[key] = shape;
        });
        owner_shape = std::move(live);
    }
    // Then a still camera: an item with no animation type changes its frame
    // only when one of its own processes does it.
    int flips = 0;
    std::map<ObjId, std::pair<std::uint16_t, std::uint16_t>> last;
    for (int f = 0; f < 600; ++f) {
        game.update(kFrameUs);
        std::set<ObjId> owners;
        k.for_each_process([&](const Process& p) { owners.insert(p.owner()); });
        w.for_each([&](const Item& i) {
            if (i.id < 256) return;
            const ShapeInfo* s = w.shape_info(i.shape);
            if (s && s->anim_type) return;
            auto [it, fresh] = last.try_emplace(i.id, i.shape, i.frame);
            if (!fresh && it->second.first == i.shape && it->second.second != i.frame && !owners.count(i.id)) ++flips;
            it->second = {i.shape, i.frame};
        });
    }
    std::printf("blinking scenery: %d processes moved to another item, %d frame changes, %zu processes\n", moved,
                flips, k.process_count());
    CHECK(moved == 0 && flips == 0);
    CHECK(game.machine().faults().empty());
}

// A jump down to a lower floor (SAVEGAME.13: the player stands at z 16, the
// floor ahead is at z 0). The landing frame's support check makes the player
// fall; as in the original (ANIMPRIM.C 1908, US 1158:1906), the jump waits
// for the landing and plays its last frames (2109 to 2111, then 0): the
// player ends standing, not frozen on the landing frame 2108
// (docs/KNOWN_BUGS.md, "jump stance when landing").
void test_jump_down_ends_standing(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    for (int i = 0; i < 120 && actor_busy(game.kernel(), 1); ++i) game.update(kFrameUs);
    const Item* p = w.get(1);
    const int z0 = p->z;
    press(game, 0x0F, false, '\t');
    bool fell = false;
    for (int i = 0; i < 300; ++i) {
        game.update(kFrameUs);
        fell = fell || (w.npc_record(1)[npc::kAnimFlags] & 0x02);
        if (fell && !actor_busy(game.kernel(), 1) && !(w.npc_record(1)[npc::kAnimFlags] & 0x02)) break;
    }
    const AnimAction* stand = game.animations().action(p->shape, 0);
    const int standing = stand ? stand->frame(w.npc_direction(1), 0)->frame : -1;
    std::printf("jump down: z %d -> %d, fell %d, action 0x%X, frame %d (standing %d)\n", z0, p->z, fell,
                last_action(game), p->frame, standing);
    CHECK(fell && p->z < z0);
    CHECK(last_action(game) == 0x3A && p->frame == standing);
    CHECK(!actor_busy(game.kernel(), 1));
}

// SEB88: the chemist NPC 128 (CHEMSUIT, map 29), off screen, behind the laser
// barrier. Off screen an animation plays two frames at once unless its current
// frame is one of the last two (US 1158:1906), so it never passes the last
// frame. The port tested the frame after the current one: the 11-frame death
// went 8, 9, then 11 and ended without frame 10 (220) and its calledFromAnim
// event. No NPCDEATH: the chemist stayed lying as an NPC and his DEATHBOX 4951
// never stopped the TIMER 2105 that re-arms the barrier (SEB91, "barriere
// laser qui se referme").
void test_offscreen_death_reaches_last_frame(const std::filesystem::path& data) {
    constexpr ObjId kChemist = 128;
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "chimiste-128-vivant-avant-barriere.sav");
    World& w = game.world();
    for (int f = 0; f < 30; ++f) game.update(kFrameUs);
    const Item* chemist = w.get(kChemist);
    CHECK(chemist && chemist->shape == 758 && !w.is_dead(kChemist));
    if (!chemist) return;
    CHECK(!w.is_partly_on_screen(*chemist));
    std::set<ObjId> ids_before;
    w.for_each([&](const Item& i) { ids_before.insert(i.id); });
    apply_damage(game.kernel(), w, game.animations(), kChemist, 1, 0, 250, 1);
    CHECK(w.is_dead(kChemist));
    std::set<int> frames;
    for (int f = 0; f < 300 && w.get(kChemist); ++f) {
        game.update(kFrameUs);
        if (const Item* c = w.get(kChemist); c && c->parent == 0 && !c->off_map) frames.insert(c->frame);
    }
    int bodies = 0;
    w.for_each([&](const Item& i) { bodies += !ids_before.count(i.id) && i.parent == 0 && i.shape != 758; });
    std::printf("off-screen death: frames %d..%d, chemist %s, %d new items\n", frames.empty() ? -1 : *frames.begin(),
                frames.empty() ? -1 : *frames.rbegin(), w.get(kChemist) ? "still there" : "set aside", bodies);
    CHECK(frames.count(220) == 1);
    CHECK(w.get(kChemist) == nullptr || w.get(kChemist)->parent != 0 || w.get(kChemist)->z == 254);
    CHECK(game.machine().faults().empty());
}


// A clip destroyed (spent, or by the usecode) leaves the player's ammunition
// slot it held: Item_forget pushes it out of the world (10B0:1C21), which
// empties the NPC's equipment slots naming it. SAVEGAME.13 carries ammo 2351.
void test_destroyed_clip_leaves_its_slot(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save(13);
    World& w = game.world();
    constexpr ObjId kClip = 2351;
    int slot = 0;
    for (int s = 6; s < 11; ++s) {
        if (w.npc_equip(ObjId{1}, s) == kClip) slot = s;
    }
    CHECK(slot != 0);
    w.destroy(kClip);
    int left = 0;
    for (int s = 0; s < 30; ++s) left += w.npc_equip(ObjId{1}, s) == kClip;
    std::printf("destroyed clip: slot %d, slots still naming it %d\n", slot, left);
    CHECK(left == 0);
}

// SEB22, saved by the port before that fix: four of the five ammunition
// slots named spent clips, so Weasel refused the GL-303 clips (a refusal
// sound). They are emptied on loading; the clip still carried stays.
void test_weasel_buys_ammo_after_spent_clips(const std::filesystem::path& data) {
    Game game(data, test_saves(data));
    game.load_save_file(test_saves(data) / "weasel-munitions-gl303-refusees.sav");
    const World& w = game.world();
    int lost = 0;
    for (int s = 1; s < 30; ++s) {
        const ObjId id = w.npc_equip(ObjId{1}, s);
        if (id != 0 && (!w.get(id) || w.get(id)->parent != ObjId{1})) ++lost;
    }
    const ObjId clip = w.npc_equip(ObjId{1}, 7);
    int refused = 0;
    for (std::uint16_t shape = 0x33D; shape <= 0x341; ++shape) refused += !may_add_item(w, shape);
    std::printf("weasel ammo: lost slots %d, slot 7 shape 0x%X, ammo kinds refused %d\n", lost,
                clip && w.get(clip) ? unsigned{w.get(clip)->shape} : 0u, refused);
    CHECK(lost == 0);
    CHECK(clip != 0 && w.get(clip) && w.get(clip)->shape == 0x33E);
    CHECK(refused == 0);
}

// Mission 10, SEB40 (before the teleporter to the chemist's room): arriving
// at teleport egg 13 makes the courier (0x45D), the chemist and two guards,
// all of group 19, that of the green keycard 9 (egg 0x4E7 7453). NPCDEATH
// gives the card to the first of them to die, and empties the courier, which
// leaves no body (Container::I_destroyContents): killing it first loses the
// card, in the original too (checked under DOSBox). With the original bug
// fixes, the card is put down where the courier stood.
void test_courier_keycard(const std::filesystem::path& data) {
    constexpr ObjId kCardEgg = 7453, kShelfCard = 6486;
    for (const bool fix : {false, true}) {
        Game game(data, test_saves(data));
        game.load_save_file(test_saves(data) / "mission10-chimiste-vivant.sav");
        game.config().fix_original_bugs = fix;
        World& w = game.world();
        start_teleport_to_egg(game.kernel(), w, 19, 13, false, false);
        const Item* egg = w.get(kCardEgg);
        CHECK(egg && egg->quality == 0x0313);
        if (!egg) return;
        ObjId courier = 0;
        for (int f = 0; f < 10 && !courier; ++f) {
            game.update(kFrameUs);
            for (ObjId id = 2; id < 256 && !courier; ++id) {
                const Item* a = w.get(id);
                if (a && a->shape == 0x45D && !a->parent && std::abs(a->x - egg->x) < 1500 &&
                    std::abs(a->y - egg->y) < 1500)
                    courier = id;
            }
        }
        CHECK(courier != 0 && w.npc_record(courier)[npc::kEggQuality] == 19);
        if (!courier) return;
        const std::int32_t cx = w.get(courier)->x, cy = w.get(courier)->y;
        apply_damage(game.kernel(), w, game.animations(), courier, 1, 0, 500, 2);
        for (int f = 0; f < 600; ++f) game.update(kFrameUs);
        const Item* card = nullptr;
        w.for_each([&](const Item& i) {
            if (i.shape == 0x111 && (i.quality & 0xFF) == 9 && i.id != kShelfCard) card = &i;
        });
        std::printf("courier keycard (fix %d): courier %u %s, egg 0x%04X, card %u parent %u at %d %d\n", fix,
                    courier, w.get(courier) && !w.is_dead(courier) ? "alive" : "dead", w.get(kCardEgg)->quality,
                    card ? card->id : 0u, card ? card->parent : 0u, card ? card->x - cx : 0, card ? card->y - cy : 0);
        CHECK(w.get(kCardEgg)->quality == 0x0013);  // the card was given
        if (!fix) {
            CHECK(card == nullptr);
        } else {
            CHECK(card != nullptr && card->parent == 0 && card->frame == 2);
            CHECK(card && std::abs(card->x - cx) < 256 && std::abs(card->y - cy) < 256);
        }
        CHECK(game.machine().faults().empty());
    }
}

// Glass debris (GameConfig::glass_debris) on the glass walls of MISSION 6:
// a bullet breaks wall 4261 (GLASS_EW) and its pile's pieces fly west, the
// way the shot went, and come to rest scattered there, the pile in their
// middle; a blast beside them throws them away; an explosive shot throws
// another wall's further. GLASS_SHOTS=<folder>: pictures.
void test_glass_debris(const std::filesystem::path& data) {
    constexpr ObjId kWall = 4261;
    Game game(data, test_saves(data));
    game.config().glass_debris = true;
    game.load_save_file(test_saves(data) / "mission6-murs-de-verre.sav");
    World& w = game.world();
    const Item* wall = w.get(kWall);
    CHECK(wall && wall->shape == 0x27B);
    if (!wall) return;
    const World::Point at = w.location(*wall);
    // The Silencer east of the wall.
    const Item* me = w.get(1);
    std::int32_t me_x = at.x;
    for (int d = 160; d <= 480; d += 16) {
        const World::Point spot{at.x + d, at.y - 32, at.z};
        if (position_free(w, me->shape, spot, nullptr, 1)) {
            w.move_item(1, spot.x, spot.y, spot.z);
            me_x = spot.x;
            break;
        }
    }
    const char* shots = std::getenv("GLASS_SHOTS");
    Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
    const auto run = [&](int frames) {
        for (int f = 0; f < frames; ++f) {
            game.update(kFrameUs);
            game.render(screen);
        }
    };
    const auto shot = [&](const char* name) {
        if (shots) write_png(std::filesystem::path(shots) / name, screen, game.palette());
    };
    const GlassDebris& glass = game.glass_debris();
    CHECK(glass.chunk_count(0x27A) >= 30 && glass.chunk_count(0x349) >= 30);
    const auto moving = [&] {
        std::size_t n = 0;
        for (const GlassDebris::Piece& p : glass.pieces()) n += !p.resting;
        return n;
    };
    // The middle of a pile's pieces, and how far they spread (largest distance from it).
    const auto spread = [&](ObjId pile, double& mx, double& my) {
        mx = my = 0;
        int n = 0;
        for (const GlassDebris::Piece& p : glass.pieces())
            if (p.pile == pile) {
                mx += p.x;
                my += p.y;
                ++n;
            }
        if (!n) return 0.0;
        mx /= n;
        my /= n;
        double far = 0;
        for (const GlassDebris::Piece& p : glass.pieces())
            if (p.pile == pile) far = std::max(far, std::hypot(p.x - mx, p.y - my));
        return far;
    };
    run(2);
    // The Silencer fires a bullet (fire type 2) west (direction 12): it
    // breaks the wall.
    fire_from_muzzle(game, 1, 12, 2, false);
    for (int f = 0; f < 60 && glass.watches().empty(); ++f) run(1);
    CHECK(glass.watches().size() == 1);
    std::size_t most_moving = 0;
    bool high = false;
    for (int f = 0; f < 60; ++f) {
        run(1);
        most_moving = std::max(most_moving, moving());
        for (const GlassDebris::Piece& p : glass.pieces()) high |= p.z > at.z + 20;
        if (f == 30) shot("glass-1-breaking.png");
    }
    run(300);
    shot("glass-2-broken.png");
    CHECK(w.get(kWall)->shape == 0x348);
    CHECK(glass.shattered().size() == 1);
    if (glass.shattered().size() != 1) return;
    const ObjId pile = glass.shattered()[0].pile;
    double mx = 0, my = 0;
    const double far = spread(pile, mx, my);
    const double wall_cx = at.x - 8.0;
    const Item* p = w.get(pile);
    const double pile_cx = p->x - 5 * 8.0, pile_cy = p->y - 6 * 8.0;
    std::printf("glass debris: bullet: %zu pieces, %zu in flight at most, at rest %zu; their middle %.0f west of the "
                "wall, spread over %.0f; pile %u at its middle %.0f, %.0f\n",
                glass.pieces().size(), most_moving, glass.pieces().size() - moving(), wall_cx - mx, far, pile,
                pile_cx - mx, pile_cy - my);
    CHECK(most_moving >= 30 && high && moving() == 0);
    CHECK(wall_cx - mx > 30 && far > 40);
    CHECK(std::abs(pile_cx - mx) < 2 && std::abs(pile_cy - my) < 2);
    // A blast (fire type 4) 50 units north of their middle throws them south.
    const World::Point blast{static_cast<std::int32_t>(mx), static_cast<std::int32_t>(my) - 50, p->z & 0xFF};
    const int before_y = p->y;
    splash_damage(game.kernel(), w, game.animations(), blast, 10, 40, 4, 0, 0);
    const std::size_t blown = moving();
    run(8);
    shot("glass-3-blast.png");
    run(300);
    shot("glass-4-blown.png");
    p = w.get(pile);
    spread(pile, mx, my);
    std::printf("glass debris: blast: %zu pieces blown, their middle now %.0f south of the blast, pile moved %d\n",
                blown, my - blast.y, p->y - before_y);
    CHECK(blown >= 20 && moving() == 0);
    CHECK(p->y - before_y > 30);
    // An explosive shot (fire type 0xE, splash 4) flying west hits wall 4474
    // further south, as ProjectileProcess::impact deals it: it breaks and
    // its pieces fly further.
    constexpr ObjId kWall2 = 4474;
    const Item* wall2 = w.get(kWall2);
    CHECK(wall2 && wall2->shape == 0x27B);
    if (!wall2) return;
    const World::Point at2 = w.location(*wall2);
    w.move_item(1, at2.x + (me_x - at.x), at2.y - 32, at2.z);
    const std::size_t piles_before = glass.shattered().size();
    const World::Point face{at2.x + 1, at2.y - 32, at2.z + 40};
    w.hooks().item_hit({kWall2, 0xE, false, face.x + 300, face.y, face.z, face.x, face.y, face.z});
    apply_damage(game.kernel(), w, game.animations(), kWall2, 1, 3, 250, 0xE, &face);
    splash_damage(game.kernel(), w, game.animations(), face, 4, 250, 0xE, kWall2, 1);
    // (The blast breaks the walls beside it as well.)
    CHECK(std::any_of(glass.watches().begin(), glass.watches().end(),
                      [](const GlassDebris::Watch& g) { return g.wall == kWall2; }));
    run(40);
    shot("glass-5-rocket.png");
    run(300);
    shot("glass-6-rocket-after.png");
    CHECK(w.get(kWall2)->shape == 0x348);
    double furthest = 0;
    for (std::size_t k = piles_before; k < glass.shattered().size(); ++k) {
        double x = 0, y = 0;
        spread(glass.shattered()[k].pile, x, y);
        if (std::abs(y - (at2.y - 32)) < 80) furthest = std::max(furthest, (at2.x - 8.0) - x);
    }
    std::printf("glass debris: explosive: pieces %.0f west of the wall in the middle, %zu piles shattered\n", furthest,
                glass.shattered().size());
    CHECK(furthest > 60);
    CHECK(game.machine().faults().empty() && game.machine().unbound_intrinsic_calls().empty());
}

int main(int argc, char** argv) {
    set_languages_dir(CRUSADER_LANGUAGES_DIR);  // the French texts (languages/fr/port.txt)
    const std::filesystem::path data = argc > 1 ? argv[1] : "data";
    if (!std::filesystem::exists(data / "STATIC/FIXED.DAT")) {
        std::printf("no game data, skipped\n");
        return 0;
    }
    if (!have_test_saves(data)) return 1;
    if (argc > 2 && std::string(argv[2]) == "--weapon-out") {
        test_weapon_out_movement(data);
        test_weapon_out_with_stance_bit_clear(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--player-turret") {
        test_player_turret(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--combat-zone") {
        test_combat_zone(data);
        test_security_camera(data);
        test_camera_in_wide_view(data);
        test_turrets(data);
        test_player_turret(data);
        std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--weasel-native-view") {
        test_weasel_native_view(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--weasel-ammo") {
        test_destroyed_clip_leaves_its_slot(data);
        test_weasel_buys_ammo_after_spent_clips(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--editor-mod") {
        test_editor_mod(data);
        std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--keypad-door") {
        test_keypad_door_after_reload(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--egg-hatched") {
        test_egg_hatched_before_save(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--scenery-guard") {
        test_scenery_guard_after_load(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--hover-platform") {
        test_hover_platform_after_load(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--saved-busy") {
        test_saved_busy_items(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--rolling-barrel") {
        test_rolling_barrel_trail(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--glass-debris") {
        test_glass_debris(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--barrel-cook-off") {
        test_barrel_cook_off(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--barrel-splash") {
        test_barrel_splash(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--fire-behind-wall") {
        test_fire_behind_wall(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--barrel-trail") {
        test_barrel_trail(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--stacked-barrel") {
        test_stacked_barrel_leak(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--barrels") {
        test_barrels(data);
        test_barrel_leaks(data);
        test_barrel_splash(data);
        test_fire_behind_wall(data);
        test_barrel_cook_off(data);
        test_barrel_trail(data);
        test_rolling_barrel_trail(data);
        test_stacked_barrel_leak(data);
        std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--barrel-bench") {
        bench_barrel_leaks(data, Game::kScreenWidth, Game::kScreenHeight);
        bench_barrel_leaks(data, 1920, 1080);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--camera-smoke") {
        test_camera_smoke(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--held-use-key-movie") {
        test_movie_ignores_held_use_key(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--courier-keycard") {
        test_courier_keycard(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--blinking-scenery") {
        test_processes_end_with_their_item(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--mission7-bomb") {
        test_mission7_fusion_pack(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--draw-order") {
        test_floor_stripes(data);
        test_force_field_floor(data);
        test_void_panel_behind_wall(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--offscreen-death") {
        test_offscreen_death_reaches_last_frame(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--jump-down") {
        test_jump_down_ends_standing(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--precise-aim") {
        test_aiming(data);
        test_precise_aim(data);
        test_stick_aim_floor_below(data);
        test_aim_mode(data);
        std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--conveyor") {
        test_conveyor_passenger(data);
        return failures ? 1 : 0;
    }
    // Isolate this crash regression when running under a memory checker.
    if (argc > 2 && std::string(argv[2]) == "--mission2-teleport") {
        test_mission2_npc_teleport(data);
        std::error_code ignored;
        std::filesystem::remove(test_saves(data) / ("SAVEGAME." + std::to_string(Game::kAutoSave)), ignored);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--death-out-of-fast-area") {
        test_death_out_of_fast_area(data);
        test_other_map_npc_hidden(data);
        test_other_map_npc_out_of_fast_area(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--mission5-contact") {
        test_mission5_contact_comes_out(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--inventory-keys") {
        test_inventory_keys(data);
        test_run_after_last_weapon_dropped(data);
    test_run_turn_to_keypad_diagonal(data);
    test_run_turn_back(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--enemies") {
        test_enemies(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--cheats") {
        test_cheats(data);
        test_hack_mover(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--keypad") {
        test_keypad(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--camera-follow") {
        test_new_game(data);
        test_camera_follows(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--animation-frameskip") {
        test_animation_frameskip(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--run-stop") {
        test_run_stop_stays_armed(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--floor-pad") {
        test_floor_pad(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--pistol-ammo") {
        test_pistol_ammo_by_difficulty(data);
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--fades") {
        test_fades(data);
        return failures ? 1 : 0;
    }
    test_load_and_run(data);
    test_alert(data);
    test_player_walks(data);
    test_camera_follows(data);
    test_camera_sweep(data);
    test_status_bar(data);
    test_keys_and_cursor(data);
    test_menu(data);
    test_load_from_menu(data);
    test_save_game(data);
    test_auto_save_names(data);
    test_crate_destroyed(data);
    test_created_weapon_has_a_clip(data);
    test_pistol_ammo_by_difficulty(data);

    test_fades(data);
    test_credits(data);
    test_options(data);
    test_firing(data);
    test_remains_loot(data);
    test_combat_zone(data);
    test_machine_gun_vibration(data);
    test_auto_firer(data);
    test_anim_end(data);
    test_special_frames(data);
    test_combat_height(data);
    test_animation_frameskip(data);
    test_burst_cut(data);
    test_death_silence(data);
    test_leave_fast_area(data);
    test_glob_fast_area(data);
    test_shield_sprites(data);
    test_muzzle_inside(data);
    test_stations(data);
    test_scenery_use(data);
    test_computer(data);
    test_keypad(data);
    test_autosave(data);
    test_keypad_door_after_reload(data);
    test_egg_hatched_before_save(data);
    test_saved_busy_items(data);
    test_hover_platform_after_load(data);
    test_scenery_guard_after_load(data);
    test_monitor(data);
    test_security_camera(data);
    test_camera_in_wide_view(data);
    test_energy_walls(data);
    test_floor_pad(data);
    test_ledge_step(data);
    test_walk_down_steps(data);
    test_conveyor_passenger(data);
    test_inventory_keys(data);
    test_run_after_last_weapon_dropped(data);
    test_run_turn_to_keypad_diagonal(data);
    test_run_turn_back(data);
    test_item_selection(data);
    test_message_box(data);
    test_cheats(data);
    test_function_keys(data);
    test_mouse_in_play(data);
    test_hack_mover(data);
    test_hard_landing(data);
    test_jump_down_ends_standing(data);
    test_offscreen_death_reaches_last_frame(data);
    test_blocked_step(data);
    test_shot_kills(data);
    test_death_called_from_anim(data);
    test_wall_lasers(data);
    test_end_credits(data);
    test_start_up(data);
    test_weasel(data);
    test_weasel_native_view(data);
    test_destroyed_clip_leaves_its_slot(data);
    test_weasel_buys_ammo_after_spent_clips(data);
    test_courier_keycard(data);
    test_barrels(data);
    test_barrel_leaks(data);
    test_barrel_splash(data);
    test_fire_behind_wall(data);
    test_barrel_cook_off(data);
    test_barrel_trail(data);
    test_rolling_barrel_trail(data);
    test_stacked_barrel_leak(data);
    test_glass_debris(data);
    test_mines(data);
    test_shot_mine(data);
    test_station_pads(data);
    test_turrets(data);
    test_player_turret(data);
    test_monitors(data);
    test_scenery_animation(data);
    test_steam_valve_save(data);
    test_laser_barrier(data);
    test_kneeling_aim(data);
    test_floor_stripes(data);
    test_force_field_floor(data);
    test_void_panel_behind_wall(data);
    test_mission7_fusion_pack(data);
    test_saved_death_plays_on(data);
    test_death_out_of_fast_area(data);
    test_other_map_npc_hidden(data);
    test_other_map_npc_out_of_fast_area(data);
    test_mission5_contact_comes_out(data);
    test_processes_end_with_their_item(data);
    test_aiming(data);
    test_precise_aim(data);
    test_aim_mode(data);
    test_aim_stick_draws(data);
    test_stick_aim_floor_below(data);
    test_crosshair_shot_height(data);
    test_player_combat_flag(data);
    test_run_stop_stays_armed(data);
    test_weapon_out_movement(data);
    test_weapon_out_with_stance_bit_clear(data);
    test_leave_surface(data);
    test_enemies(data);
    test_new_game(data);
    test_editor_mod(data);
    test_movies(data);
    test_back_to_base(data);
    test_movie_ignores_held_use_key(data);
    test_transformers_broken_together(data);
    test_camera_smoke(data);
    test_mission2_final_scene(data);
    test_mission2_npc_teleport(data);
    // The tests that play on the test saves autosave into them (Game::kAutoSave): not kept.
    std::error_code ignored;
    std::filesystem::remove(test_saves(data) / ("SAVEGAME." + std::to_string(Game::kAutoSave)), ignored);
    std::printf(failures ? "%d failure(s)\n" : "all game checks passed\n", failures);
    return failures ? 1 : 0;
}
