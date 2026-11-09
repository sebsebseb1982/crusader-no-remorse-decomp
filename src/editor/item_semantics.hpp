#pragma once
// What the bytes of a map item mean to the game, for the editor: the level
// designers of Crusader drove the scripts by placing eggs and setting the
// quality, NPC number and map number bytes, which the usecode classes and
// the engine read. Sources: docs/missions.md (eggs, EggHatcher, teleports),
// docs/combat.md (monster eggs), docs/usecode.md (classes, markers, TRIGGER).

#include "editor/level.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crusader::editor {

// TYPEFLAG families.
enum Family : std::uint8_t {
    kFamGeneric = 0,
    kFamQuality = 1,
    kFamGlobEgg = 3,
    kFamTriggerEgg = 4,
    kFamContainer = 6,
    kFamMonsterEgg = 7,
    kFamTeleportEgg = 8,
    kFamWeapon = 10,
    kFamAmmo = 11,
    kFamInventory = 12,
    kFamUsable = 13,
};

const char* family_name(std::uint8_t family);

// Shapes the editor uses to create the eggs.
constexpr std::uint16_t kShapeGlobEgg = 0x010;
constexpr std::uint16_t kShapeTriggerEgg = 0x011;
constexpr std::uint16_t kShapeTeleportEgg = 0x194;
constexpr std::uint16_t kShapeMonsterEgg = 0x24F;
constexpr std::uint16_t kShapeNpcTemplate = 0x002;  // the template inside a monster egg
constexpr std::uint16_t kShapeMarker = 0x476;       // chest contents / object switched off
constexpr std::uint16_t kShapeSteamMarker = 0x3A9;
constexpr std::uint8_t kStartEgg = 0x1E;             // where a mission starts (teleportToEgg)

// Usecode class ranges (Usecode_class_of, World::usecode_class).
constexpr std::uint16_t kClassEggs = 0x900;
constexpr std::uint16_t kClassFree = 0xA00;

// The class whose events an item of the map receives.
std::uint16_t usecode_class_of(const LevelItem& item, std::uint8_t family);

// Usecode event names (Crusader numbering) and what they mean in this game.
const char* event_name(std::uint32_t event);
const char* event_help(std::uint32_t event);

// Area in which the player hatches a trigger egg (family 4) or a departure
// teleport egg (family 8, frame 0): half sizes in usecode units around the
// egg, z within +-0x30 (EggHatcher, 1098:0718).
struct HatchZone {
    std::int32_t half_x = 0, half_y = 0;
    static constexpr std::int32_t kHalfZ = 0x30;
};
HatchZone hatch_zone(const LevelItem& egg);
void set_hatch_zone(LevelItem& egg, int units_x, int units_y);  // units of 32
bool hatches(std::uint8_t family, const LevelItem& item);

// Monster egg (family 7): parameters split between the egg and its first
// content, the NPC template (spawn_from_egg).
struct MonsterEgg {
    int npc_type = 0;           // template NPC number (DTABLE.FLX record)
    int direction = 0;          // egg NPC number low nibble: direction / 2
    int record_12 = 0;          // egg NPC number high nibble
    int min_difficulty = 1;     // egg map number bits 0-1, + 1
    bool other_weapon = false;  // egg map number bit 2
    int activity[3] = {0, 0, 0};  // template quality high, quality low, map number
};
MonsterEgg read_monster_egg(const LevelItem& egg);
void write_monster_egg(LevelItem& egg, const MonsterEgg& m);
// A new monster egg with its template.
LevelItem make_monster_egg(std::int32_t x, std::int32_t y, std::int32_t z, int npc_type);

// Markers 0x476: the object a chest gives, or an object a TRIGGER switched
// off (docs/usecode.md): shape = map number + (NPC number & 0xE0) * 8,
// frame = NPC number & 0xF.
struct MarkerTarget {
    std::uint16_t shape = 0;
    std::uint8_t frame = 0;
};
MarkerTarget read_marker(const LevelItem& marker);
void write_marker(LevelItem& marker, MarkerTarget t);

// NPC activities (Actor::I_setActivity numbers) for the monster egg forms.
const char* activity_name(int activity);

// A short human-readable label of an item (map lists, tooltips).
std::string describe(const LevelItem& item, std::uint8_t family, const std::string& class_name);

}  // namespace crusader::editor
