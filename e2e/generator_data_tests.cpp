// Procedural level generator on the original data (docs/GENERATEUR.md): the
// biomes it learns from the globs, and generated levels played in the game,
// without a window:
//  - physical check: from the start, a search over the 16-unit grid moves
//    the player's box with the engine's walking move (try_move: support,
//    steps, collisions; doors taken out, as open) and must reach every room
//    and the exit pad;
//  - a door opens when the player comes near it (its DOOREGG);
//  - the exit teleporter leads to the next level's start.
// Usage: generator_data_tests <data_dir> [first_seed] [seeds]

#include "core/language.hpp"
#include "core/flex.hpp"
#include "editor/generator/campaign.hpp"
#include "editor/level_tools.hpp"
#include "editor/project.hpp"
#include "engine/usecode/usecode_file.hpp"
#include "engine/world/collision.hpp"
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

    // Walking: every room and the exit, the doors taken out (open).
    {
        auto game = play("walking");
        World& w = game->world();
        std::vector<ObjId> doors;
        w.for_each([&](const Item& i) {
            if (i.parent == 0 && is_door(i.shape)) doors.push_back(i.id);
        });
        CHECK(doors.size() == 2 * level.links.size());
        for (ObjId d : doors) w.destroy(d);
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
            if (!door && i.parent == 0 && is_door(i.shape)) door = i.id;
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
    const gen::GameData game_data = gen::GameData::load(data);
    check_biomes(game_data);
    for (std::uint32_t s = first; s < first + count; ++s) check_seed(data, game_data, s);
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
