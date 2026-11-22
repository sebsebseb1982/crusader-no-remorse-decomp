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
#include <optional>
#include <span>
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
// linked within itself and apart from the map's own links. On its own map, a
// link carried by a single pasted item is kept: a door copied beside another
// still answers the same switch. Pasted arrivals
// whose id the target uses get a free id, and pasted departures that aimed at
// them follow. Returns the number of renumbered links and ids.
int relink_pasted(std::vector<LevelItem>& items, const Level& target, const LevelInfo& info, int source_map,
                  int target_map);

// The other object of a MONSTER pair: for a place (frame 0), the nearest kind
// (frame 1) of its quality low within kMonsterRange; for a kind, the nearest
// place that has it in range. Null when there is none (or no MONSTER class).
const LevelItem* monster_partner(const Level& level, const LevelItem& item, const LevelInfo& info);

// The items that share `item`'s link (its quality low) within `range` units
// (x and y), `item` first. Empty when it has no link.
std::vector<std::uint32_t> linked_group(const Level& level, const LevelItem& item, const LevelInfo& info,
                                        std::int32_t range);

// The shapes whose objects act through TRIGGER on the shape they name
// (trigger_target): switches pass TRIGGER their state (0 or 1) and it acts
// on the linked items of that shape, whereas EVENT and others pass values
// from 0x80 that take other paths, and chests or timers link otherwise. Told
// apart by the original maps: a shape whose class calls TRIGGER
// (`calls_trigger`, by class id) and whose linked objects there name a
// linked item's shape at least 3 times, in 60 % of its uses.
std::vector<bool> find_trigger_users(const std::vector<Level>& maps, const LevelInfo& info,
                                     const std::vector<bool>& calls_trigger);

// What a TRIGGER user does to its targets (TRIGGER event 0x20 with the
// user's state, 0 or 1, then function 0x25 when map bit 2 is set):
//   map number bits 0-1  action: 0 sets the targets' quality high to the
//                        argument, 1 their quality low, 2 sends them
//                        equip(argument) (a MONSTER place: its NPC appears),
//                        3 turns them off into markers 0x476, or back on;
//   bit 2                that set of actions (clear: others, e.g. destroy);
//   bit 3                the state it acts at (0: its first use);
//   bit 4                once: its link is then cleared;
//   quality high         the argument.
// Nothing happens with a map number or a quality low of 0.
struct TriggerSetting {
    std::uint8_t quality_high = 0;
    std::uint8_t flags = 0;  // map number bits 0-4
    int uses = 0;            // how many users have it
    int of = 0;              // users acting on that shape
};
// The setting the original maps use most, by user shape and target shape
// (key: user << 16 | target), and by target for any user (user 0xFFFF).
using TriggerSettings = std::map<std::uint32_t, TriggerSetting>;
TriggerSettings trigger_settings(const std::vector<Level>& maps, const LevelInfo& info, const std::vector<bool>& users);
// The user's own setting on that target when the maps have it 3 times, else
// any user's; null when none.
const TriggerSetting* find_trigger_setting(const TriggerSettings& settings, std::uint16_t user, std::uint16_t target);
void apply_trigger_setting(LevelItem& user, const TriggerSetting& setting);

// A prefab: items saved from a map to be placed again (the editor's
// prefabs folder). File: "CPF1", u16 anchor x, y, z, u16 map it came from,
// then the items as FIXED.DAT records (contents included).
struct Prefab {
    std::int32_t anchor_x = 0, anchor_y = 0, anchor_z = 0;
    int source_map = 0;
    std::vector<LevelItem> items;
    std::vector<std::uint8_t> serialize() const;
    // Nullopt when the bytes are no prefab.
    static std::optional<Prefab> parse(std::span<const std::uint8_t> bytes);
};

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
