// Procedural level generator on the original data (docs/GENERATEUR.md): the
// biomes it learns from the globs, and generated levels played in the game,
// without a window:
//  - physical check: from the start, a search over the 16-unit grid moves
//    the player's box with the engine's walking move (try_move: support,
//    steps, collisions; doors taken out, as open) and must reach every room
//    and the exit pad;
//  - a door opens when the player comes near it (its DOOREGG);
//  - each lift carries the player up, and its call boxes send it up and down;
//  - the guards appear in their rooms, from their difficulty, and shoot;
//  - the exit teleporter leads to the next level's start.
// Usage: generator_data_tests <data_dir> [first_seed] [seeds] [--shots DIR] [--mission MAP]

#include "core/dtable.hpp"
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
#include "engine/world/actor_anim.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/inventory.hpp"
#include "game/game.hpp"
#include "test_saves.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
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
// --mission MAP: the levels are missions built from the rooms of that map.
int mission_source = 0;

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

std::filesystem::path data_dir;
const dtable::NpcTypes& npc_types() {
    static const dtable::NpcTypes types = dtable::NpcTypes::load(data_dir / "STATIC/DTABLE.FLX");
    return types;
}

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

// Positions reached from the player's by walking moves of 16 units. A lift
// joins its two ends: standing on its platform at the bottom leads to the
// upper floor in front of it, and that spot back to the bottom (the ride
// itself is checked on its own, check_lift).
std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> walk_area(World& w,
                                                                          const std::vector<gen::Lift>& lifts = {}) {
    const Item* p = w.get(1);
    w.set_fast_area(FastArea::around(p->x, p->y, 200, 200));
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> seen{{p->x, p->y, p->z}};
    std::deque<World::Point> queue{{p->x, p->y, p->z}};
    auto visit = [&](const World::Point& at) {
        if (seen.insert({at.x, at.y, at.z}).second) queue.push_back(at);
    };
    std::vector<bool> up(lifts.size()), down(lifts.size());
    const int dirs[4][2] = {{16, 0}, {-16, 0}, {0, 16}, {0, -16}};
    while (!queue.empty() && seen.size() < 400000) {
        const World::Point at = queue.front();
        queue.pop_front();
        for (std::size_t k = 0; k < lifts.size(); ++k) {
            const gen::Lift& l = lifts[k];
            const bool on_platform = at.x - 47 >= l.x0 && at.x < l.x1 && at.y - 47 >= l.y0 && at.y < l.y1 &&
                                     at.z == l.platform.z + 8;
            if (on_platform && !up[k]) up[k] = true, visit({l.top.x, l.top.y, l.top.z});
            const bool at_top = std::abs(at.x - l.top.x) <= 16 && std::abs(at.y - l.top.y) <= 16 && at.z == l.top.z;
            if (at_top && !down[k]) down[k] = true, visit({l.bottom.x, l.bottom.y, l.bottom.z});
        }
        for (const auto& d : dirs) {
            const World::Point to{at.x + d[0], at.y + d[1], at.z};
            const SweepResult r = try_move(w, 1, at, to, kSweepSupportOnly | kSweepStepUp);
            if (!r.ok || r.end.x != to.x || r.end.y != to.y) continue;
            visit(r.end);
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

// Walks the player (the arrow key held) towards (dx, dy) until `done` or
// `frames` frames. Directions: 0 north (-y), 4 east (+x), 8 south, 12 west.
template <typename Done>
bool walk_until(Game& game, std::int32_t dx, std::int32_t dy, int frames, Done done) {
    World& w = game.world();
    const int dir = std::abs(dx) > std::abs(dy) ? (dx > 0 ? 4 : 12) : (dy > 0 ? 8 : 0);
    if (auto r = w.npc_record(1); !r.empty()) r[npc::kDirection] = static_cast<std::uint8_t>(dir);
    game.key(0x48, true, true, 0);
    bool ok = false;
    for (int f = 0; f < frames && !(ok = done()); ++f) game.update(kFrameUs);
    game.key(0x48, true, false, 0);
    return ok;
}

// A lift ridden as the player does: from the bottom, onto the platform (its
// pad sends it up), off it onto the upper floor; each call box sends it
// down and then up (a box acts in both its states).
void check_lift(Game& game, const gen::Lift& lift, std::uint32_t seed) {
    World& w = game.world();
    ObjId platform = 0, pad = 0, below = 0, above = 0;
    w.for_each([&](const Item& i) {
        if (i.parent != 0 || (i.quality & 0xFF) != lift.number) return;
        if (i.shape == 0x12F) platform = i.id;
        if (i.shape == 0x4CD) pad = i.id;
        if (i.shape == lift.box_shape && i.z == lift.bottom_box.z) below = i.id;
        if (i.shape == lift.box_shape && i.z == lift.top_box.z) above = i.id;
    });
    CHECK(platform && pad && below && above);
    if (!platform || !pad || !below || !above) return;
    auto wait_platform = [&](int z) {
        for (int f = 0; f < 900 && w.get(platform)->z != z; ++f) game.update(kFrameUs);
        run_frames(game, 10);
        return w.get(platform)->z == z;
    };
    w.teleport(1, lift.bottom.x, lift.bottom.y, lift.bottom.z, w.current_map());
    run_frames(game, 30);
    const std::int32_t dx = lift.top.x - lift.bottom.x, dy = lift.top.y - lift.bottom.y;
    // Onto the platform, until the player stands completely on it.
    const bool on = walk_until(game, dx, dy, 240, [&] {
        const Item& p = *w.get(1);
        return p.x - 47 >= lift.x0 + 8 && p.x < lift.x1 - 8 && p.y - 47 >= lift.y0 + 8 && p.y < lift.y1 - 8;
    });
    const bool rose = wait_platform(88);
    std::printf("  lift %u: on the platform %d, platform z %d, player z %d\n", lift.number, on,
                w.get(platform)->z, w.get(1)->z);
    CHECK(on && rose && w.get(1)->z == 96 && !w.is_dead(1));
    // Off the platform, onto the upper floor in front of it.
    const bool off = walk_until(game, dx, dy, 240, [&] {
        const Item& q = *w.get(1);
        return std::abs(q.x - lift.top.x) <= 8 && std::abs(q.y - lift.top.y) <= 8;
    });
    run_frames(game, 30);
    std::printf("  lift %u: off at the top %d, player (%d, %d, %d), dead %d\n", lift.number, off, w.get(1)->x,
                w.get(1)->y, w.get(1)->z, w.is_dead(1));
    CHECK(off && w.get(1)->z == 96 && !w.is_dead(1));
    // The call boxes.
    use_item(w, above);
    const bool down = wait_platform(1);
    use_item(w, above);
    const bool up_again = wait_platform(88);
    use_item(w, below);
    const bool down_again = wait_platform(1);
    use_item(w, below);
    const bool up_last = wait_platform(88);
    std::printf("  lift %u: call box above: down %d, up %d; below: down %d, up %d\n", lift.number, down, up_again,
                down_again, up_last);
    CHECK(down && up_again && down_again && up_last);
    if (!(on && rose && off && down && up_again && down_again && up_last)) {
        std::fprintf(stderr, "seed %u: lift %u fails\n", seed, lift.number);
    }
}

void check_seed(const std::filesystem::path& data, const gen::GameData& game_data, std::uint32_t seed) {
    const auto mod = std::filesystem::temp_directory_path() / ("crusader_generator_tests_" + std::to_string(seed));
    std::filesystem::remove_all(mod);
    std::vector<gen::GeneratedLevel> levels;
    {
        Project project(data, mod);
        std::string error;
        levels = mission_source ? gen::generate_mission_levels(project, game_data, seed, mission_source, error)
                                : gen::generate_levels(project, game_data, seed, 2, -1, error);
        const std::size_t wanted = mission_source ? 1 : 2;
        CHECK(levels.size() == wanted);
        if (levels.size() != wanted) {
            std::fprintf(stderr, "seed %u: %s\n", seed, error.c_str());
            return;
        }
        project.save();
    }
    // The same levels without their guards (MONSTER pairs), for the checks
    // of the mechanisms: the guards are checked on their own.
    const auto calm = std::filesystem::temp_directory_path() / ("crusader_generator_tests_calm_" + std::to_string(seed));
    std::filesystem::remove_all(calm);
    {
        Project project(data, calm);
        for (const gen::GeneratedLevel& g : levels) {
            Level without = g.result.level;
            std::erase_if(without.items, [](const LevelItem& i) { return i.shape == 0x4D0; });
            project.store_map(static_cast<std::size_t>(g.map), without);
        }
        project.save();
    }
    const gen::Result& level = levels[0].result;
    // Where the exit leads: the next level (the first one after the last).
    const gen::GeneratedLevel& next = levels[1 % levels.size()];
    std::printf("map %d, %s\n", levels[0].map, level.notes.front().c_str());
    if (mission_source) {
        for (std::size_t k = 1; k < level.notes.size(); ++k) std::printf("%s\n", level.notes[k].c_str());
    }

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
        context.map_exists = [&](int m) { return m == levels[0].map || m == next.map; };
        context.arrivals_on = [&](int m) {
            return arrival_ids(m == next.map ? next.result.level : level.level, context.info);
        };
        for (const Problem& p : check_level(level.level, context)) {
            std::fprintf(stderr, "seed %u: level check: %s\n", seed, p.text.c_str());
            CHECK(p.severity == Severity::Note);
        }
    }

    // Each check on a new game: the walking search widens the fast area, and
    // an opened door changes shape.
    auto play = [&](const char* what, bool guards = false, int difficulty = 3) {
        auto game = std::make_unique<Game>(data, test_saves(data), guards ? mod : calm);
        game->set_start(levels[0].map, 0x1E);
        game->new_game(difficulty);
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
        // A room reached: the player's box inside it; for a mission, whose
        // rooms touch (an L-shaped room's box takes in some of its
        // neighbour), at its entrance.
        auto room_reached = [&](const auto& seen, int room_index) {
            const gen::Room& rm = level.rooms[static_cast<std::size_t>(room_index)];
            if (mission_source && room_index > 0) return near(seen, gen::entry(level, room_index), 32);
            return reached_in(seen, rm.x0, rm.y0, rm.x1, rm.y1);
        };
        std::vector<int> order;
        CHECK(gen::check_solvable(level, &order).empty());
        CHECK(order.size() == level.locks.size());
        for (int k : order) {
            const gen::Lock& lock = level.locks[static_cast<std::size_t>(k)];
            w.teleport(1, level.start.x, level.start.y, level.start.z, w.current_map());
            auto seen = walk_area(w, level.lifts);
            const bool barred = !room_reached(seen, lock.room);
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
            seen = walk_area(w, level.lifts);
            const bool open = room_reached(seen, lock.room);
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
        const auto seen = walk_area(w, level.lifts);
        for (std::size_t r = 0; r < level.rooms.size(); ++r) {
            const bool ok = room_reached(seen, static_cast<int>(r));
            if (!ok) std::fprintf(stderr, "seed %u: room %zu not reached on foot\n", seed, r);
            CHECK(ok);
        }
        CHECK(reached_in(seen, level.exit.x - 64, level.exit.y - 64, level.exit.x + 64, level.exit.y + 64));
        CHECK(game->machine().faults().empty());
    }

    // The lifts, ridden.
    for (const gen::Lift& lift : level.lifts) {
        auto game = play("lift");
        check_lift(*game, lift, seed);
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
                // What the player stood on.
                w.for_each([&](const Item& i) {
                    if (i.id == 1 || i.parent != 0 || std::abs(i.x - from.x) > 300 || std::abs(i.y - from.y) > 300 || i.z > 16) return;
                    std::fprintf(stderr, "  near: shape 0x%X frame %d at (%d, %d, %d)\n", i.shape, i.frame, i.x, i.y, i.z);
                });
                std::fprintf(stderr, "  from (%d, %d, %d)\n", from.x, from.y, from.z);
            }
            CHECK(r.ok);
        }
        CHECK(game->machine().faults().empty());
    }

    // Pictures of a mission's rooms (--shots): from where the player comes
    // in, with the guards, on a large view.
    if (!shots_dir.empty() && mission_source) {
        auto game = play("room shots", true, 1);
        World& w = game->world();
        Surface8 view(1280, 960);
        Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
        for (std::size_t k = 0; k < level.rooms.size(); ++k) {
            const gen::Spot in = k == 0 ? level.start : gen::entry(level, static_cast<int>(k));
            w.teleport(1, in.x, in.y, in.z, w.current_map());
            run_frames(*game, 45);
            game->render(view);
            const std::string name = "mission" + std::to_string(seed) + "-room" + std::to_string(k);
            write_png(shots_dir / (name + ".png"), view, game->palette());
            // As the player sees it, at the game's own resolution.
            game->render(screen);
            write_png(shots_dir / (name + "-screen.png"), screen, game->palette());
        }
    }

    // Pictures of the locks and keys (--shots): the player beside each.
    if (!shots_dir.empty()) {
        auto game = play("shots");
        World& w = game->world();
        Surface8 screen(Game::kScreenWidth, Game::kScreenHeight);
        auto shoot = [&](const gen::Spot& at, std::int32_t dx, std::int32_t dy, const std::string& name) {
            w.teleport(1, at.x + dx, at.y + dy, at.z >= 96 ? 96 : 0, w.current_map());
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
        for (std::size_t k = 0; k < level.lifts.size(); ++k) {
            const gen::Lift& lift = level.lifts[k];
            shoot(lift.bottom, 0, 0, "lift" + std::to_string(k) + "-bottom");
            shoot(lift.top, 0, 0, "lift" + std::to_string(k) + "-top");
        }
    }

    // The guards: each one appears when the player comes into its room, of
    // its type, from its lowest difficulty only, and attacks the player. In
    // a mission, the designers' posts are kept as they stand: one now and
    // then never fires (open question, docs/GENERATEUR.md); 4 rooms in 5 must
    // fire, and the silent ones are listed.
    int guarded_rooms = 0, silent_rooms = 0;
    for (const int difficulty : {1, 3}) {
        std::set<int> rooms;
        for (const gen::Guard& g : level.guards) {
            if (g.room != level.exit_room) rooms.insert(g.room);
        }
        for (const int room_index : rooms) {
            auto game = std::make_unique<Game>(data, test_saves(data), mod);
            game->set_start(levels[0].map, 0x1E);
            game->new_game(difficulty);
            World& w = game->world();
            // Each NPC where it is first seen: where it appeared.
            std::map<ObjId, std::tuple<std::uint16_t, std::int32_t, std::int32_t, std::int32_t>> born;
            auto step = [&](int frames) {
                for (int f = 0; f < frames; ++f) {
                    game->update(kFrameUs);
                    for (ObjId id = 2; id <= 0xFF; ++id) {
                        const Item* a = w.get(id);
                        if (a && !born.count(id)) born[id] = {a->shape, a->x, a->y, a->z};
                    }
                }
            };
            step(120);
            const gen::Room& room = level.rooms[static_cast<std::size_t>(room_index)];
            const gen::Spot in = gen::entry(level, room_index);
            w.teleport(1, in.x, in.y, in.z, w.current_map());
            const int hp = w.npc_hp(1);
            step(240);
            // A post far in a large room is out of the fast area from the
            // doorway: the player walks towards it (to 192 units, on the
            // doorway's side), as in play, then comes back.
            {
                bool moved = false;
                for (const gen::Guard& g : level.guards) {
                    if (g.room != room_index || g.min_difficulty > difficulty) continue;
                    const double dx = in.x - g.place.x, dy = in.y - g.place.y, d = std::max(1.0, std::hypot(dx, dy));
                    if (d <= 400) continue;
                    w.teleport(1, g.place.x + static_cast<std::int32_t>(dx * 192 / d),
                               g.place.y + static_cast<std::int32_t>(dy * 192 / d), in.z, w.current_map());
                    step(60);
                    moved = true;
                }
                if (moved) {
                    w.teleport(1, in.x, in.y, in.z, w.current_map());
                    step(30);
                }
            }
            int expected = 0, appeared = 0;
            std::vector<ObjId> actors;
            for (const gen::Guard& g : level.guards) {
                if (g.room != room_index) continue;
                bool seen = false;
                for (const auto& [id, at] : born) {
                    const auto& [s, x, y, z] = at;
                    // At its post, whatever its shape: its type's, or that
                    // of another of its stances (a guard kneeling to shoot).
                    (void)s;
                    if (std::abs(x - g.place.x) <= 24 && std::abs(y - g.place.y) <= 24 &&
                        std::abs(z - g.place.z) <= 24) {
                        seen = true;
                        actors.push_back(id);
                    }
                }
                const bool wanted = g.min_difficulty <= difficulty;
                expected += wanted;
                appeared += seen;
                if (seen != wanted) {
                    std::fprintf(stderr, "seed %u: guard %u (type %d, from difficulty %d) %s at difficulty %d\n",
                                 seed, g.link, g.npc_type, g.min_difficulty, seen ? "appears" : "does not appear",
                                 difficulty);
                    std::fprintf(stderr, "  post (%d, %d, %d)\n", g.place.x, g.place.y, g.place.z);
                    for (const auto& [id, at] : born) {
                        const auto& [s, x, y, z] = at;
                        if (std::abs(x - g.place.x) <= 200 && std::abs(y - g.place.y) <= 200) {
                            std::fprintf(stderr, "  born near: %u shape 0x%X at (%d, %d, %d)\n", id, s, x, y, z);
                        }
                    }
                }
                CHECK(seen == wanted);
            }
            // Shot at from the doorway, or else on walking up to a guard (to
            // 160 units of it, on the doorway's side).
            bool hurt = false, closer = false;
            auto wait_hurt = [&](int frames) {
                for (int f = 0; f < frames && !hurt; ++f) {
                    game->update(kFrameUs);
                    hurt = w.is_dead(1) || w.npc_hp(1) < hp;
                }
            };
            if (expected > 0) wait_hurt(600);
            // Walking up to a guard: from the doorway's side, then round it
            // (furniture may hide the player from one side).
            if (expected > 0 && !hurt && !actors.empty() && w.get(actors.front())) {
                const Item a = *w.get(actors.front());
                const double dx = in.x - a.x, dy = in.y - a.y, d = std::max(1.0, std::hypot(dx, dy));
                const std::int32_t sides[5][2] = {{static_cast<std::int32_t>(dx * 160 / d), static_cast<std::int32_t>(dy * 160 / d)},
                                                  {160, 0}, {-160, 0}, {0, 160}, {0, -160}};
                closer = true;
                for (const auto& s : sides) {
                    if (hurt) break;
                    w.teleport(1, a.x + s[0], a.y + s[1], in.z, w.current_map());
                    wait_hurt(400);
                }
            }
            std::printf("  difficulty %d, room %d: %d guard(s) of %d appear%s%s\n", difficulty, room_index, appeared,
                        expected, expected ? (hurt ? ", the player is shot at" : ", THE PLAYER IS NOT SHOT AT") : "",
                        closer ? " (walking up)" : "");
            if (expected > 0 && !hurt) {
                const Item& p = *w.get(1);
                std::fprintf(stderr, "seed %u, room %d (%d..%d, %d..%d, z %d): player (%d, %d, %d)", seed, room_index,
                             room.x0, room.x1, room.y0, room.y1, room.z, p.x, p.y, p.z);
                for (ObjId id : actors) {
                    const Item* a = w.get(id);
                    if (a) {
                        std::fprintf(stderr, "; NPC %u shape 0x%X at (%d, %d, %d) dead %d", id, a->shape, a->x, a->y,
                                     a->z, w.is_dead(id));
                    }
                }
                std::fprintf(stderr, "\n");
            }
            if (expected > 0) {
                ++guarded_rooms;
                if (!hurt) ++silent_rooms;
                if (!mission_source) CHECK(hurt);
            }
            CHECK(game->machine().faults().empty());
        }
    }
    if (mission_source) {
        if (silent_rooms) std::printf("  %d guarded room(s) of %d silent\n", silent_rooms, guarded_rooms);
        CHECK(silent_rooms * 5 <= guarded_rooms);
    }

    // The exit teleporter: to the next level's start.
    {
        auto game = play("exit");
        World& w = game->world();
        w.teleport(1, level.exit.x, level.exit.y, level.exit.z, w.current_map());
        // A level that leads back to itself: the player is put back on its
        // start (the map is loaded again).
        bool arrived = false;
        for (int f = 0; !arrived && f < 600; ++f) {
            game->update(kFrameUs);
            const Item* p = w.get(1);
            arrived = w.current_map() == next.map && p && p->x == next.result.start.x && p->y == next.result.start.y;
        }
        run_frames(*game, 30);
        CHECK(w.current_map() == next.map);
        const Item* player = w.get(1);
        CHECK(arrived && player && player->x == next.result.start.x && player->y == next.result.start.y);
        CHECK(game->machine().faults().empty());
    }
    std::filesystem::remove_all(mod);
    std::filesystem::remove_all(calm);
}

}  // namespace

int main(int argc, char** argv) {
    set_languages_dir(CRUSADER_LANGUAGES_DIR);
    const std::filesystem::path data = argc > 1 ? argv[1] : "data";
    data_dir = data;
    if (!std::filesystem::exists(data / "STATIC/FIXED.DAT")) {
        std::printf("no game data, skipped\n");
        return 0;
    }
    if (!have_test_saves(data)) return 1;
    const std::uint32_t first = argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 1;
    const std::uint32_t count = argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 6;
    for (int i = 4; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--shots") {
            shots_dir = argv[i + 1];
            std::filesystem::create_directories(shots_dir);
        }
        if (std::string(argv[i]) == "--mission") mission_source = std::stoi(argv[i + 1]);
    }
    const gen::GameData game_data = gen::GameData::load(data);
    check_biomes(game_data);
    for (std::uint32_t s = first; s < first + count; ++s) check_seed(data, game_data, s);
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
