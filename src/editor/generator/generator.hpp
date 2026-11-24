#pragma once
// Procedural level generator (docs/GENERATEUR.md): a mission graph of rooms,
// laid out on a grid and built from the pieces of a biome, with the eggs and
// scripted objects of the original maps.
//
// Milestone 1: one storey of rooms joined by corridors through automatic
// doors (a door and its DOOREGG, as the original maps make them), the
// mission start (arrival egg 0x1E) in the first room and, in the room
// farthest from it, a teleporter pad whose departure egg leads to the next
// level.

#include "editor/generator/biome.hpp"
#include "editor/level.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crusader::editor::gen {

// Deterministic on every platform (the standard distributions are not).
class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed * 2654435761u + 0x9E3779B9u) { next(); }
    std::uint32_t next();
    int below(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
    int range(int lo, int hi) { return lo + below(hi - lo + 1); }  // lo..hi
    bool chance(int percent) { return below(100) < percent; }

private:
    std::uint64_t state_;
};

struct Options {
    std::uint32_t seed = 1;
    int biome = -1;           // index in the biomes; -1: drawn from the seed
    int rooms = 0;            // 0: drawn (6 to 10)
    int map = 0;              // the level's own map number
    int next_map = 0;         // where the exit teleporter leads (0: no exit teleport)
    std::uint8_t next_egg = 0x1E;
};

// A room: a rectangle of whole floor tiles (usecode units, x1 / y1 excluded).
struct Room {
    int slot_x = 0, slot_y = 0;
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, z = 0;
    int depth = 0;  // links from the start room
    std::int32_t centre_x() const { return (x0 + x1) / 2; }
    std::int32_t centre_y() const { return (y0 + y1) / 2; }
};

// A passage between two rooms: a corridor with a door at each end.
struct Link {
    int a = 0, b = 0;
    std::uint8_t door_a = 0, door_b = 0;  // the doors' links (quality low, DOOREGG map number)
};

struct Result {
    Level level;
    int biome = 0;
    std::vector<Room> rooms;
    std::vector<Link> links;
    int start_room = 0, exit_room = 0;
    struct Point {
        std::int32_t x = 0, y = 0, z = 0;
    } start, exit;  // the arrival egg; the exit pad's centre
    std::vector<std::string> notes;
};

Result generate(const std::vector<Biome>& biomes, const Options& options);

// Logical check: the exit room is reached from the start room through the
// links. Empty when it is, else what is wrong.
std::string check_solvable(const Result& result);

}  // namespace crusader::editor::gen
