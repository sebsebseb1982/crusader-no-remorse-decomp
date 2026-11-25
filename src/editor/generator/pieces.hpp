#pragma once
// Crusader: No Limit, set pieces: the rooms of the original maps, cut out
// with their floors, walls, furniture, scenery and NPC posts, to build new
// levels from the designers' own rooms (docs/GENERATEUR.md, "Set pieces").
//
// A map is flattened (its glob eggs replaced by their pieces), then read as
// a grid of 16-unit cells at one storey: a cell is walkable when a solid
// flat piece (a floor, not a black void tile) bears it and nothing solid
// stands over it up to the player's height; doors are not walkable. The
// walkable cells, eroded by 64 units so that doorways and passages narrower
// than 128 units fall away, make one component per room; every walkable
// cell then goes back to the nearest room. A room's items, at its storey,
// are what stands within its inside (its cells and what its walls enclose
// with them), the walls that border it, what hangs on their face that the
// camera sees from it, and the floor tiles under all that; things that
// touch (a heap of crates) go whole or stay whole (docs/GENERATEUR.md).
// Its openings are the runs of its border where it meets another room or
// a door.

#include "core/typeflags.hpp"
#include "editor/level.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace crusader::editor::gen {

struct GameData;

enum PieceSide { kNorth = 0, kEast = 1, kSouth = 2, kWest = 3 };

// A run of a room's border that leads out of it.
struct Opening {
    int side = kNorth;
    std::int32_t from = 0, to = 0;  // along the border (x for north and south, y for east and west)
    std::int32_t edge = 0;          // the border: the room's first cell (north, west) or one past its last (south, east)
    bool door = false;              // a door stands in it on the original map
    bool cut = false;               // no opening on the original map: a straight wall piece where a door can be cut
    std::int32_t centre() const { return (from + to) / 2; }
    std::int32_t width() const { return to - from; }
};

// What a room holds, by kind (scripted objects and scenery counted by class).
enum PieceTag { kStorage = 0, kOffice, kMachines, kSecurity, kTagCount };
const char* tag_name(int tag);

struct RoomPiece {
    int map = 0;
    std::int32_t z = 0;                  // the storey's floor
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounding box of its cells (x1, y1 excluded)
    int cols = 0, rows = 0;              // cells of 16 units from (x0, y0)
    std::vector<std::uint8_t> cells;     // walkable cells of the room, rows * cols
    // Its outline, rows * cols: its inside (cells and what its walls enclose
    // with them) closed by 48 units, so that a dent narrower than a doorway
    // (a passage left behind) is taken in; the walls that close the room
    // follow it, and its floor covers it.
    std::vector<std::uint8_t> outline;
    std::vector<LevelItem> items;        // at their positions on the original map
    std::vector<Opening> openings;
    std::array<int, kTagCount> tags{};
    std::vector<std::uint8_t> local_links;  // quality lows whose scripted objects are all in the room
    int npcs = 0;                        // MONSTER pairs
    int furniture = 0;                   // items with a height that are neither walls nor doors
    // Its border cells, and those that open on nothing on the original map:
    // neither a wall (64 high or more), a door, nor another room (an open
    // side, a balcony behind a railing). A room cut out with many of those
    // is a fragment of a larger space, not a room.
    int border = 0, open_border = 0;
    int low_border = 0;  // border cells closed only by something low (a railing, under 64): a balcony
    int area = 0;                        // walkable cells
    // Cells of its floor that run past it on the side the camera sees
    // (south, east): over the void once the room stands alone.
    int overhang = 0;
    int unfloored = 0;  // cells of its outline that no floor of it bears
    bool cell(std::int32_t x, std::int32_t y) const;  // a walkable cell of the room at (x, y)
    std::string describe() const;
};

// The rooms of `map` at storey `z` (0 or 96) whose sides are between
// `min_side` and `max_side` units, with at least one opening.
std::vector<RoomPiece> extract_rooms(const GameData& data, int map, std::int32_t z = 0, std::int32_t min_side = 256,
                                     std::int32_t max_side = 1100);

// The cells of 16 units at storey `z0`: walkable (a solid flat piece, not
// a black void tile nor a death floor, bears it within 9 units, nothing
// solid over it lower than 80, the player's height), and the cells of doors (not walkable).
struct WalkGrid {
    int W = 0, H = 0;  // from (0, 0)
    std::vector<std::uint8_t> walk, door, support;  // support: a floor bears the cell (whatever stands on it)
    bool walkable(std::int32_t x, std::int32_t y) const;
};
WalkGrid walk_grid(const std::vector<LevelItem>& items, const GameData& data, std::int32_t z0);

// A map's items with its glob eggs replaced by their pieces (at their place).
std::vector<LevelItem> flatten(const Level& level, const GameData& data);

}  // namespace crusader::editor::gen
