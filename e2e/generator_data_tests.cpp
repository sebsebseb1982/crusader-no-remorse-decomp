// Procedural level generator on the original data (docs/GENERATEUR.md): the
// biomes it learns from the globs, and generated levels played in the game,
// without a window:
//  - physical check: from the start, a search over the 16-unit grid moves
//    the player's box with the engine's walking move (try_move: support,
//    steps, collisions; doors taken out, as open) and must reach every room
//    and the exit pad;
//  - a door opens when the player comes near it (its DOOREGG);
//  - the exit teleporter leads to the next level's start.
// Usage: generator_data_tests <data_dir> [first_seed] [seeds] [--shots DIR]

#include "core/language.hpp"
#include "core/png_writer.hpp"
#include "core/surface.hpp"
#include "core/flex.hpp"
#include "editor/generator/campaign.hpp"
#include "editor/level_tools.hpp"
#include "editor/project.hpp"
#include "engine/usecode/usecode_file.hpp"
#include "engine/ui/computer_gump.hpp"
#include "engine/ui/keypad_gump.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/inventory.hpp"
#include "game/game.hpp"
#include "test_saves.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <tuple>

using namespace crusader;
using namespace crusader::editor;

namespace {

int failures = 0;
// --shots DIR: a picture of each lock and its key, in the game (port review).
std::filesystem::path shots_dir;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

constexpr std::uint32_t kFrameUs = 1'000'000 / 60;
constexpr std::uint16_t kDoorShapes[] = {0x0A9, 0x046};

bool is_door(std::uint16_t shape) { return shape == kDoorShapes[0] || shape == kDoorShapes[1]; }

void check_biomes(const gen::GameData& game) {
    std::printf("%zu biomes\n", game.biomes.size());
    CHECK(game.biomes.size() >= 4);
    for (const gen::Biome& b : game.biomes) {
        std::printf("  %s, %d globs\n", b.name().c_str(), b.globs);
        CHECK(b.floor.valid() && !b.floor_variants.empty());
        CHECK(game.shapes[b.floor.shape].footpad_x == 16 && !game.scripted[b.floor.shape]);
        for (int side = 0; side < 2; ++side) {
            for (int axis = 0; axis < 2; ++axis) {
                for (int units : {1, 5, 7, 23, 30, 46}) CHECK(b.can_build(side, axis, units));
            }
        }
    }
}

// Positions reached from the player's by walking moves of 16 units.
std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> walk_area(World& w) {
    const Item* p = w.get(1);
    w.set_fast_area(FastArea::around(p->x, p->y, 200, 200));
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> seen{{p->x, p->y, p->z}};
    std::deque<World::Point> queue{{p->x, p->y, p->z}};
    const int dirs[4][2] = {{16, 0}, {-16, 0}, {0, 16}, {0, -16}};
    while (!queue.empty() && seen.size() < 400000) {
        const World::Point at = queue.front();
        queue.pop_front();
        for (const auto& d : dirs) {
            const World::Point to{at.x + d[0], at.y + d[1], at.z};
            const SweepResult r = try_move(w, 1, at, to, kSweepSupportOnly | kSweepStepUp);
            if (!r.ok || r.end.x != to.x || r.end.y != to.y) continue;
            if (seen.insert({r.end.x, r.end.y, r.end.z}).second) queue.push_back(r.end);
        }
    }
    return seen;
}

bool reached_in(const std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>>& seen, std::int32_t x0,
                std::int32_t y0, std::int32_t x1, std::int32_t y1) {
    for (const auto& [x, y, z] : seen) {
        // The player's box (48 units) entirely inside.
        if (x - 47 >= x0 && x < x1 && y - 47 >= y0 && y < y1) return true;
    }
    return false;
}

void run_frames(Game& game, int frames) {
    for (int f = 0; f < frames; ++f) game.update(kFrameUs);
}

void check_seed(const std::filesystem::path& data, const gen::GameData& game_data, std::uint32_t seed) {
    const auto mod = std::filesystem::temp_directory_path() / ("crusader_generator_tests_" + std::to_string(seed));
    std::filesystem::remove_all(mod);
    std::vector<gen::GeneratedLevel> levels;
    {
        Project project(data, mod);
        std::string error;
        levels = gen::generate_levels(project, game_data, seed, 2, -1, error);
        CHECK(levels.size() == 2);
        if (levels.size() != 2) {
            std::fprintf(stderr, "seed %u: %s\n", seed, error.c_str());
            return;
        }
        project.save();
    }
    const gen::Result& level = levels[0].result;
    std::printf("map %d, %s\n", levels[0].map, level.notes.front().c_str());

    // The editor's level check finds nothing wrong.
    {
        static const UsecodeFile usecode = UsecodeFile::from_flex(FlexArchive::open(data / "USECODE/EUSECODE.FLX"));
        CheckContext context;
        context.info.family = [&](std::uint16_t s) {
            return s < game_data.shapes.size() ? game_data.shapes[s].family : std::uint8_t{0};
        };
        context.info.has_class = [&](std::uint16_t cid) { return usecode.has_class(cid); };
        context.map = levels[0].map;
        context.glob_pieces = [&](std::uint16_t g) {
            return g < game_data.globs.size() ? game_data.globs[g].pieces.size() : std::size_t{0};
        };
        context.map_exists = [&](int m) { return m == levels[0].map || m == levels[1].map; };
        context.arrivals_on = [&](int m) {
            return arrival_ids(m == levels[1].map ? levels[1].result.level : level.level, context.info);
        };
        for (const Problem& p : check_level(level.level, context)) {
            std::fprintf(stderr, "seed %u: level check: %s\n", seed, p.text.c_str());
            CHECK(p.severity == Severity::Note);
        }
    }

    // Each check on a new game: the walking search widens the fast area, and
    // an opened door changes shape.
    auto play = [&](const char* what) {
        auto game = std::make_unique<Game>(data, test_saves(data), mod);
        game->set_start(levels[0].map, 0x1E);
        game->new_game(3);
        run_frames(*game, 120);
        const World& w = game->world();
        const Item* player = w.get(1);
        const bool ok = w.current_map() == levels[0].map && player && player->x == level.start.x &&
                        player->y == level.start.y;
        if (!ok) std::fprintf(stderr, "seed %u, %s: the game does not start on the level's start\n", seed, what);
        CHECK(ok);
        return game;
    };

    // The automatic doors' links: those doors are taken out (open) for the
    // walking search; locked doors and energy walls stay.
    std::set<int> auto_links;
    for (const gen::Link& l : level.links) {
        if (l.door_a) auto_links.insert(l.door_a);
        if (l.door_b) auto_links.insert(l.door_b);
    }
    auto is_auto_door = [&](const Item& i) {
        return i.parent == 0 && is_door(i.shape) && auto_links.count(i.quality & 0xFF);
    };

    // Walking, and the locks opened one by one with their keys, in the order
    // the solver opens them: each lock bars its room until its key is used.
    {
        auto game = play("walking");
        World& w = game->world();
        std::vector<ObjId> doors;
        w.for_each([&](const Item& i) {
            if (is_auto_door(i)) doors.push_back(i.id);
        });
        CHECK(doors.size() == auto_links.size());
        for (ObjId d : doors) w.destroy(d);
        auto find = [&](std::uint16_t shape, const gen::Spot& at) {
            ObjId id = 0;
            w.for_each([&](const Item& i) {
                if (!id && i.parent == 0 && i.shape == shape && i.x == at.x && i.y == at.y) id = i.id;
            });
            return id;
        };
        // A spot of the walk within `range` of a point (x and y).
        auto near = [](const auto& seen, const gen::Spot& at, std::int32_t range) {
            for (const auto& [x, y, z] : seen) {
                if (std::abs(x - at.x) <= range && std::abs(y - at.y) <= range) return true;
            }
            return false;
        };
        std::vector<int> order;
        CHECK(gen::check_solvable(level, &order).empty());
        CHECK(order.size() == level.locks.size());
        for (int k : order) {
            const gen::Lock& lock = level.locks[static_cast<std::size_t>(k)];
            const gen::Room& behind = level.rooms[static_cast<std::size_t>(lock.room)];
            w.teleport(1, level.start.x, level.start.y, level.start.z, w.current_map());
            auto seen = walk_area(w);
            const bool barred = !reached_in(seen, behind.x0, behind.y0, behind.x1, behind.y1);
            // A monitor stands on a desk: used from in front of it.
            const std::int32_t key_range = lock.kind == gen::LockKind::kKeypadDoor ? 128 : 64;
            const bool key_reached =
                near(seen, lock.key, key_range) && (!lock.reader_shape || near(seen, lock.reader, 64));
            std::printf("  %s %u: %s, key %s\n", gen::lock_name(lock.kind), lock.number,
                        barred ? "bars its room" : "DOES NOT BAR ITS ROOM", key_reached ? "reached" : "NOT REACHED");
            CHECK(barred && key_reached);
            if (!key_reached) {
                std::fprintf(stderr, "seed %u: key at (%d, %d) %s, reader at (%d, %d) %s; rooms reached:", seed,
                             lock.key.x, lock.key.y, near(seen, lock.key, 64) ? "reached" : "not reached",
                             lock.reader.x, lock.reader.y, near(seen, lock.reader, 64) ? "reached" : "not reached");
                for (std::size_t r = 0; r < level.rooms.size(); ++r) {
                    const gen::Room& room = level.rooms[r];
                    if (reached_in(seen, room.x0, room.y0, room.x1, room.y1)) std::fprintf(stderr, " %zu", r);
                }
                std::fprintf(stderr, "\n");
            }
            // Using the key.
            const ObjId key = find(lock.key_shape, lock.key);
            CHECK(key != 0);
            if (!key) break;
            if (lock.kind == gen::LockKind::kCardDoor) {
                w.teleport(1, lock.key.x, lock.key.y, lock.key.z, w.current_map());
                run_frames(*game, 2);
                // Picked up as the selection mode does (S, then Enter).
                // (families 10 to 13 are taken, start_selection).
                const ShapeInfo* info = w.shape_info(lock.key_shape);
                CHECK(info && info->family >= 10 && info->family <= 13);
                CHECK(add_item_cru(w, key, false));
                run_frames(*game, 30);
                CHECK((w.state.keycards >> lock.number) & 1);
                const ObjId reader = find(lock.reader_shape, lock.reader);
                CHECK(reader != 0);
                if (!reader) break;
                use_item(w, reader);
            } else if (lock.kind == gen::LockKind::kKeypadDoor) {
                // The monitor shows the code its VALUEBOX drew when the map loaded.
                const Item* box = w.get(find(0x251, lock.code_box));
                CHECK(box != nullptr);
                if (!box) break;
                const int code = box->npc_number + ((box->map_number & 3) << 8);
                CHECK(code >= 100 && code <= 999);
                use_item(w, key);
                run_frames(*game, 3);
                const auto* computer = dynamic_cast<const ComputerGump*>(game->screen());
                CHECK(computer && computer->text().find(std::to_string(code)) != std::string::npos);
                if (computer) std::printf("    monitor: \"%s\"\n", computer->text().c_str());
                for (int i = 0; i < 30 && game->screen(); ++i) {
                    game->key(0x01, false, true, 0, 0x1B);
                    game->key(0x01, false, false, 0, 0x1B);
                    run_frames(*game, 2);
                }
                CHECK(!game->screen());
                // The code typed on the keypad.
                const ObjId keypad = find(lock.reader_shape, lock.reader);
                CHECK(keypad != 0);
                if (!keypad) break;
                use_item(w, keypad);
                run_frames(*game, 3);
                const auto* pad = dynamic_cast<const KeypadGump*>(game->screen());
                CHECK(pad && pad->code() == code);
                for (const char c : std::to_string(code)) {
                    const auto scancode = static_cast<std::uint8_t>(c == '0' ? 0x0B : 0x02 + (c - '1'));
                    game->key(scancode, false, true, 0, static_cast<std::uint8_t>(c));
                    game->key(scancode, false, false, 0, static_cast<std::uint8_t>(c));
                    game->update(kFrameUs);
                }
                game->key(0x1C, false, true, 0, 0x0D);
                game->key(0x1C, false, false, 0, 0x0D);
                game->update(kFrameUs);
            } else {
                use_item(w, key);
            }
            run_frames(*game, 180);
            w.teleport(1, level.start.x, level.start.y, level.start.z, w.current_map());
            seen = walk_area(w);
            const bool open = reached_in(seen, behind.x0, behind.y0, behind.x1, behind.y1);
            if (!open) {
                const ObjId barrier = find(lock.barrier_shape, lock.barrier);
                const Item* bi = w.get(barrier);
                std::fprintf(stderr, "seed %u: %s %u still shut: barrier %u shape 0x%X frame %u quality 0x%X\n", seed,
                             gen::lock_name(lock.kind), lock.number, barrier, bi ? bi->shape : 0,
                             bi ? bi->frame : 0, bi ? bi->quality : 0);
            }
            CHECK(open);
        }
        w.teleport(1, level.start.x, level.start.y, level.start.z, w.current_map());
        const auto seen = walk_area(w);
        for (std::size_t r = 0; r < level.rooms.size(); ++r) {
            const gen::Room& room = level.rooms[r];
            const bool ok = reached_in(seen, room.x0, room.y0, room.x1, room.y1);
            if (!ok) std::fprintf(stderr, "seed %u: room %zu not reached on foot\n", seed, r);
            CHECK(ok);
        }
        CHECK(reached_in(seen, level.exit.x - 64, level.exit.y - 64, level.exit.x + 64, level.exit.y + 64));
        CHECK(game->machine().faults().empty());
    }

    // A door opens when the player comes near.
    {
        auto game = play("door");
        World& w = game->world();
        ObjId door = 0;
        w.for_each([&](const Item& i) {
            if (!door && is_auto_door(i)) door = i.id;
        });
        CHECK(door != 0);
        if (door) {
            const Item before = *w.get(door);
            const bool along_x = before.shape == kDoorShapes[0];
            // 80 units in front of the door's centre, inside its egg's area.
            const std::int32_t x = along_x ? before.x - 24 : before.x + 80;
            const std::int32_t y = along_x ? before.y + 80 : before.y - 24;
            w.teleport(1, x, y, before.z, w.current_map());
            run_frames(*game, 90);
            const Item* after = w.get(door);
            CHECK(!after || after->shape != before.shape || after->frame != before.frame || after->z != before.z);
            // Open: the player walks through, to 80 units behind it.
            run_frames(*game, 60);
            const World::Point from{x, y, w.get(1)->z};
            const World::Point to{along_x ? x : before.x - 96, along_x ? before.y - 96 : y, from.z};
            const SweepResult r = try_move(w, 1, from, to, kSweepSupportOnly | kSweepStepUp);
            const Item* blocker = w.get(r.blocker);
            if (!r.ok) {
                std::fprintf(stderr, "seed %u: through the open door: stopped at (%d, %d, %d) by shape 0x%X z %d\n",
                             seed, r.end.x, r.end.y, r.end.z, blocker ? blocker->shape : 0, blocker ? blocker->z : 0);
            }
            CHECK(r.ok);
        }
        CHECK(game->machine().faults().empty());
    }

    // Pictures of the locks and keys (--shots): the player beside each.
    if (!shots_dir.empty()) {
        auto game = play("shots");
        World& w = game->world();
        Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
        auto shoot = [&](const gen::Spot& at, std::int32_t dx, std::int32_t dy, const std::string& name) {
            w.teleport(1, at.x + dx, at.y + dy, 0, w.current_map());
            run_frames(*game, 40);
            game->render(screen);
            write_png(shots_dir / ("seed" + std::to_string(seed) + "-" + name + ".png"), screen, game->palette());
        };
        for (std::size_t k = 0; k < level.locks.size(); ++k) {
            const gen::Lock& lock = level.locks[k];
            const std::string n = std::to_string(k) + "-" + std::to_string(static_cast<int>(lock.kind));
            shoot(lock.key, 40, 80, n + "-key");
            shoot(lock.kind == gen::LockKind::kCardDoor ? lock.reader : lock.barrier, 60, 60, n + "-lock");
        }
    }

    // The exit teleporter: to the next level's start.
    {
        auto game = play("exit");
        World& w = game->world();
        w.teleport(1, level.exit.x, level.exit.y, level.exit.z, w.current_map());
        for (int f = 0; w.current_map() != levels[1].map && f < 600; ++f) game->update(kFrameUs);
        run_frames(*game, 30);
        CHECK(w.current_map() == levels[1].map);
        const Item* player = w.get(1);
        CHECK(player && player->x == levels[1].result.start.x && player->y == levels[1].result.start.y);
        CHECK(game->machine().faults().empty());
    }
    std::filesystem::remove_all(mod);
}

}  // namespace

int main(int argc, char** argv) {
    set_languages_dir(CRUSADER_LANGUAGES_DIR);
    const std::filesystem::path data = argc > 1 ? argv[1] : "data";
    if (!std::filesystem::exists(data / "STATIC/FIXED.DAT")) {
        std::printf("no game data, skipped\n");
        return 0;
    }
    if (!have_test_saves(data)) return 1;
    const std::uint32_t first = argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 1;
    const std::uint32_t count = argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 6;
    if (argc > 5 && std::string(argv[4]) == "--shots") {
        shots_dir = argv[5];
        std::filesystem::create_directories(shots_dir);
    }
    const gen::GameData game_data = gen::GameData::load(data);
    check_biomes(game_data);
    for (std::uint32_t s = first; s < first + count; ++s) check_seed(data, game_data, s);
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
