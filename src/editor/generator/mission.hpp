#pragma once
// Crusader: No Limit, missions built from set pieces (docs/GENERATEUR.md,
// "Missions"): a level is told before it is built. A mission is a run of
// steps, each with a purpose (arrive, first contact, the room that holds
// the key, the locked door, the climax, the way out) and an intensity; each
// step takes a room of the original maps that suits it (pieces.hpp), with
// its furniture, scenery and the designers' own guard posts, kept or
// thinned out to the step's intensity. Rooms meet as on the original maps:
// their openings face each other across one line of wall, with a door in
// it; their other openings are walled up. Where a room has no opening on
// the right side, a door is cut in one of its straight walls.
//
// All the rooms of a level come from the source map and the maps that look
// like it, so that the level keeps one look.

#include "editor/generator/generator.hpp"
#include "editor/generator/pieces.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crusader::editor::gen {

struct GameData;

// What a step of the mission is for.
enum class Purpose {
    kArrival,     // where the player comes in: quiet, a storage room
    kContact,     // first guards, a room to cross
    kKeyRoom,     // a dead end off the way: an office, where the key card lies at the back
    kLocked,      // behind the card door: a guarded control or security room
    kTransit,     // machines, a few guards
    kClimax,      // the largest room, fully guarded
    kExtraction,  // a small dead end with the exit teleporter, quiet
};
const char* purpose_name(Purpose p);

struct MissionStep {
    Purpose purpose = Purpose::kArrival;
    int parent = -1;    // the step it opens from (-1: the first one)
    bool locked = false;  // the passage from its parent is a card door
    int max_guards = 0;   // of the room's own guard posts
    int alt_parent = -1;  // another step it may open from, when the layout fails from `parent`
    int min_guards = 0;   // guards added when the room's own posts are fewer
    int key = -1;         // a locked step: the step whose room holds its card (-1: the first key room)
};

// The source map and the maps that look like it (floors and walls shared
// for three quarters at least; 4 at most, the closest first): the rooms of a
// mission come from them, so that it keeps one look.
std::vector<int> look_alike_maps(const GameData& data, int source_map);

// The demo mission: arrival, contact, the key office off it, the locked
// control room, machines, the climax, extraction.
std::vector<MissionStep> demo_mission();

// The plan of the mission of a seed, among a few (the demo; two cards, the
// second behind the first door and the climax behind the second; a longer
// crossing, its key in a dead end half-way). `name`, when given, receives
// the plan's name.
std::vector<MissionStep> mission_steps(std::uint32_t seed, std::string* name = nullptr);

struct MissionOptions {
    std::uint32_t seed = 1;
    int source_map = 1;  // the original map the rooms come from
    int map = 0, next_map = 0;
    std::uint8_t next_egg = 0x1E;
    int first_card = 1;
    std::string plan;  // the plan's name, for the notes
};

// The level of `steps`, with its rooms from the source map. The Result's
// rooms are the placed rooms (their bounding boxes), its links the
// corridors, its locks the card doors. Empty rooms and a note when the
// rooms cannot be laid out.
// When the steps cannot be laid out, they are tried again from their
// `alt_parent`s.
// A layout that the floor plan does not let the player walk through
// (check_walkable) is drawn again, up to 40 times.
Result generate_mission(const GameData& data, const std::vector<MissionStep>& steps, const MissionOptions& options);

// From the start, the player's box (3 x 3 cells of 16 units) walks over the
// level's walkable cells and doors (WalkGrid) to each room's entrance, to
// the exit and to each key. Empty when it does, else what it cannot reach.
// `failed_room`, when given, receives the room that cannot be reached (-1: none).
std::string check_walkable(const Result& result, const GameData& data, int* failed_room = nullptr);

}  // namespace crusader::editor::gen
