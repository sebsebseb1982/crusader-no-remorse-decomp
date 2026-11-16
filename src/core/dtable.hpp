#pragma once
// Weapon, inventory and NPC type tables (DTABLE.C, segment 1120).
//
// The weapon and item tables are static data of the executable (DS:20D6 ..
// DS:2350 in the French CRUSADER.EXE). The NPC type table is entry 0 of
// STATIC/DTABLE.FLX, read whole by DTABLE_1120_0075 (64 records of 0x8E bytes).
// A lookup that finds nothing returns record 0, whose fields are all 0, except
// the NPC lookup which returns -1.

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace crusader::dtable {

// Weapon records (DS:215A, 21 x 0x12 bytes; DTABLE_1120_0145 finds one by
// shape). Fields used so far: +0 shape, +2 sound of a shot, +4 ticks between shots (01ef), +6
// ammo type, +8 shots per burst (02ce), +0xA energy per shot (byte, 01ad),
// +0xC actions kept as they are (byte, 02ee: no remapping, see
// remap_action), +0xD (unaligned) icon frame in GUMPS.FLX shape 3, +0xF
// reload sound (byte, 0351), +0x10 fire type (0528).
struct WeaponRecord {
    std::array<std::uint8_t, 0x12> raw;
    std::uint16_t shape() const { return word(0); }
    std::uint16_t sound() const { return word(2); }        // DTABLE_1120_01cf
    std::uint16_t fire_type() const { return word(0x10); }  // DTABLE_1120_0528
    std::uint16_t shot_delay() const { return word(4); }
    std::uint16_t ammo_type() const { return word(6); }
    std::int16_t burst() const { return static_cast<std::int16_t>(word(8)); }
    std::uint8_t energy_per_shot() const { return raw[0xA]; }
    bool keeps_actions() const { return raw[0xC] != 0; }
    std::uint16_t icon() const { return word(0xD); }
    std::uint8_t reload_sfx() const { return raw[0xF]; }
    std::uint16_t word(int at) const { return static_cast<std::uint16_t>(raw[at] | raw[at + 1] << 8); }
};

// Fire types (FIRETYPE.C, table DS:25B4 "FIRE_TYPE_TABLE", 16 x 0x11 bytes;
// DTABLE_1120_062d finds one by number): +0 number, +2 / +4 damage range
// (DTABLE_1120_0674 draws in it), +6 splash radius (byte), +7 projectiles per
// shot (byte), +0xB no aiming spread (byte), +0xC distance per step in units
// of 16 (word), +0xE range in steps (word), +0x10 impact sprite (byte),
// +8 energy a shield spends to stop it (word), +0xA shield types (byte).
struct FireType {
    std::array<std::uint8_t, 0x11> raw;
    std::uint16_t number() const { return word(0); }
    std::uint16_t min_damage() const { return word(2); }
    std::uint16_t max_damage() const { return word(4); }
    std::int8_t splash() const { return static_cast<std::int8_t>(raw[6]); }
    std::int8_t projectiles() const { return static_cast<std::int8_t>(raw[7]); }
    bool no_spread() const { return raw[0xB] != 0; }
    std::uint16_t speed() const { return word(0xC); }
    std::uint16_t range() const { return word(0xE); }
    bool impact_sprite() const { return raw[0x10] != 0; }
    // Player shields (CRUSADER_1130_026b): energy a shield spends to stop
    // this fire type (+8) and the shield types that can (+0xA, bits).
    std::uint16_t shield_cost() const { return word(8); }
    std::uint8_t shield_mask() const { return raw[0xA]; }
    std::uint16_t word(int at) const { return static_cast<std::uint16_t>(raw[at] | raw[at + 1] << 8); }
};
// nullptr for an unknown number (the original stops with a fatal assertion).
const FireType* fire_type(int number);

int weapon_index(std::uint16_t shape);               // DTABLE_1120_0145 (0 if none)
const WeaponRecord& weapon(std::uint16_t shape);     // record of weapon_index()
const WeaponRecord& weapon_at(int index);            // record `index` (record 0 outside)

// The BA-40 (0x32E, record 1) needs no ammunition at difficulty 1 only:
// wherever the original sets the difficulty (DS:7E19) it also writes the
// pistol's ammo type DS:2172 = (difficulty != 1): the difficulty screen
// (13D0:2975 click, 13D0:29B3 Enter) and loading a game (1058:02F1). Above
// level 1 it takes BA-41 clips (0x33D, type 1): 20 rounds, an ammo counter,
// reloading. The table starts with type 0, as in the executable.
void set_difficulty(int difficulty);

// Shape of the ammunition a weapon uses, 0 for none (DTABLE_1120_027e: word 0
// of the 8-byte ammo record DS:20D6 + 8 * ammo type).
std::uint16_t ammo_shape(std::uint16_t weapon_shape);
// Rounds in a full clip (DTABLE_1120_0180: word 1 of the ammo record).
std::uint16_t clip_size(std::uint16_t weapon_shape);
// Shape of the weapon drawn over the actor (WPNCACHE_1160_0771): the weapon's
// overlay class (table DS:2B82, 3-byte records) picks one of 0x1A1, 0x36D,
// 0x36F, 0x36E, 0x33B (DS:2B64). 0 for a shape not in the table (the
// original stops with a fatal assertion).
std::uint16_t overlay_shape(std::uint16_t weapon_shape);
// Icon frame of a weapon in GUMPS.FLX shape 3 (DTABLE_1120_030e).
std::uint16_t weapon_icon(std::uint16_t weapon_shape);

// Icon of a family 12 inventory item in GUMPS.FLX shape 4 (DTABLE_1120_03a4,
// 8-byte records at DS:2308, icon at +6) and of any other inventory item in
// shape 5 (DTABLE_1120_0455, 4-byte records at DS:22D8, icon at +2).
std::uint16_t item_icon_family12(std::uint16_t shape);
std::uint16_t item_icon(std::uint16_t shape);
// DTABLE_1120_0424: the shape's record in DS:22D8, 0 when none (record 0
// is shape 0).
int inventory_item_index(std::uint16_t shape);

// Names shown when an item is picked up (DISPLAY_1190_070f), far pointers to
// the strings DS:2420.. ("INVALID" for record 0): weapons DS:2350 by
// weapon_index (DTABLE_1120_032e), inventory items DS:2388 by
// inventory_item_index (0474), ammunition DS:23B8 by ammo index (02ab) and
// family 12 items DS:23D0 by their index (03c3).
const char* weapon_name(std::uint16_t shape);
const char* item_name(std::uint16_t shape);
const char* ammo_name(std::uint16_t shape);
const char* family12_name(std::uint16_t shape);
// Icon of an ammunition in GUMPS.FLX shape 5 (DTABLE_1120_0240: word 3 of
// its ammo record, DS:20D6 + 8 * index; DTABLE_1120_020f finds the index by
// shape, 0 when none).
std::uint16_t ammo_icon(std::uint16_t shape);
int ammo_index(std::uint16_t shape);      // DTABLE_1120_020f
int family12_index(std::uint16_t shape);  // DTABLE_1120_0373, 0 when none

// NPC types (DTABLE.FLX entry 0): +2 maximum hit points, +0x3E shape.
class NpcTypes {
public:
    static constexpr std::size_t kRecordSize = 0x8E;
    static constexpr int kRecords = 64;

    static NpcTypes load(const std::filesystem::path& dtable_flx);

    // DTABLE_1120_010a: record index for an NPC shape, -1 if none.
    int index(std::uint16_t shape) const;
    std::uint16_t max_hp(int index) const;
    // A word of record `index` (0 outside the table). Known fields: +2 max hit
    // points, +0x18 weapon (index in the weapon table), +0x1A other weapon,
    // +0x3E shape.
    std::uint16_t word(int index, std::size_t at) const;

private:
    std::vector<std::uint8_t> data_;
};

}  // namespace crusader::dtable
