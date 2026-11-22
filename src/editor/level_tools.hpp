#pragma once
// Level editor helpers that read a whole map: the ids of its teleport eggs,
// free numbers for new links, the renumbering of pasted items, and a check of
// what would not work in the game (a departure to an arrival that does not
// exist, an egg that never hatches...). No SDL: the editor's guided tools,
// its Problems window and the tests build on this.

#include "editor/item_semantics.hpp"
#include "editor/level.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace crusader::editor {

// What the helpers need to know about the game data.
struct LevelInfo {
    std::function<std::uint8_t(std::uint16_t shape)> family;
    // The usecode class exists (EUSECODE.FLX).
    std::function<bool(std::uint16_t cid)> has_class;
};

// Teleport eggs (family 8): frame 1 is an arrival, frame 0 a departure.
bool is_arrival(const LevelItem& item, std::uint8_t family);
bool is_departure(const LevelItem& item, std::uint8_t family);

// Eggs use their quality for something else (glob number, trigger type,
// teleport id, NPC template); other items link to each other by quality low.
bool links_by_quality(std::uint8_t family);
// The item takes part in such links: it has a script (or is a marker 0x476)
// and a non-zero quality low.
bool is_linked_item(const LevelItem& item, const LevelInfo& info);

// Arrival ids of a map, with the number of arrival eggs of each.
std::map<std::uint8_t, int> arrival_ids(const Level& level, const LevelInfo& info);
// The smallest arrival id (1..255) the map does not use, the mission start
// 0x1E left out; 0 when every id is taken.
std::uint8_t free_arrival_id(const Level& level, const LevelInfo& info);
// The smallest quality low (1..255) that no linked item of the map uses, nor
// any trigger egg as its parameter (quality high); 0 when none is left.
std::uint8_t free_link_quality(const Level& level, const LevelInfo& info);

// Pasting `items` (copied from map `source_map`) into `target` (map
// `target_map`): each quality low that links pasted items and that the target
// already uses gets a free number (the same new number for every pasted item
// that had it, trigger egg parameters included), so the pasted group stays
// linked within itself and apart from the map's own links. Pasted arrivals
// whose id the target uses get a free id, and pasted departures that aimed at
// them follow. Returns the number of renumbered links and ids.
int relink_pasted(std::vector<LevelItem>& items, const Level& target, const LevelInfo& info, int source_map,
                  int target_map);

enum class Severity { Error, Warning, Note };

struct Problem {
    Severity severity = Severity::Note;
    std::uint32_t uid = 0;  // the item concerned, 0 for the map as a whole
    std::string text;
};

struct CheckContext {
    LevelInfo info;
    int map = 0;
    // Pieces of a glob (0 when it is empty or does not exist).
    std::function<std::size_t(std::uint16_t glob)> glob_pieces;
    // A map of FIXED.DAT with items.
    std::function<bool(int map)> map_exists;
    // Arrival ids of another map.
    std::function<std::map<std::uint8_t, int>(int map)> arrivals_on;
    // Records of the game's busiest original map: a map above it may leave
    // no room for the pieces of nearby globs.
    std::size_t busiest_records = 8000;
};

// Records of the busiest map of a FIXED.DAT (7 855 in the original game, map 27).
std::size_t busiest_map_records(const std::vector<std::vector<std::uint8_t>>& entries);

// What would not work in the game, errors first.
std::vector<Problem> check_level(const Level& level, const CheckContext& context);

}  // namespace crusader::editor
