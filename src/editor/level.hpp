#pragma once
// Level editor model: the items of one map as FIXED.DAT stores them, and the
// prefab groups of GLOB.FLX. No SDL, no game state: the editor UI and the
// tests build on this.
//
// FIXED.DAT entry (one per map): 16-byte records
//   u16 x, u16 y, u8 z, u16 shape, u8 frame, u16 flags, u16 quality,
//   u8 npc number, u8 map number, u16 next
// A record with flag 0x0008 is inside a container: its x is the nesting depth
// and the container is the last shallower record before it (ITEMCACH.C
// 05D6, World::load_map). Monster eggs (shape 0x24F) hold their NPC template
// that way. `next` is a leftover of the original editor's lists; the game
// ignores it, the editor keeps it so that unchanged maps are written back
// byte for byte.
//
// GLOB.FLX entry: u16 count, then count * { u8 x, u8 y, u8 z, u16 shape,
// u8 frame }. A glob egg (family 3) of quality q puts glob q's pieces at
//   x = (egg.x & ~0x1FF) + 2 * gx + 1   (same for y)   z = (egg.z + gz) & 0xFF
// in usecode units (World::expand_glob_egg).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace crusader::editor {

constexpr std::size_t kRecordSize = 16;
constexpr std::uint16_t kFlagContained = 0x0008;
constexpr std::uint16_t kFlagInvisible = 0x0010;
constexpr std::uint16_t kFlagMirrored = 0x0020;
// Map coordinates are 16-bit (64 x 64 cells of 512 usecode units).
constexpr std::int32_t kMapSize = 0x8000;
constexpr std::int32_t kChunk = 512;  // a map cell, the unit of glob placement

// Records of the game's item table that a map may use: the table has 0x2800
// slots, the first 0x100 being NPCs; glob pieces and created items share it.
constexpr std::size_t kItemTableSlots = 0x2800 - 0x100;

struct LevelItem {
    std::uint32_t uid = 0;  // editor identity, never written
    // Usecode units for an item on the map. For an item in a container, the
    // stored y and z (z is its slot), x being the depth.
    std::int32_t x = 0, y = 0, z = 0;
    std::uint16_t shape = 0;
    std::uint8_t frame = 0;
    std::uint16_t flags = 0;
    std::uint16_t quality = 0;
    std::uint8_t npc_number = 0;
    std::uint8_t map_number = 0;
    std::uint16_t next = 0;
    std::vector<LevelItem> contents;

    std::uint8_t quality_low() const { return static_cast<std::uint8_t>(quality & 0xFF); }
    std::uint8_t quality_high() const { return static_cast<std::uint8_t>(quality >> 8); }
};

// A fresh uid for a new item (and its contents, recursively).
std::uint32_t new_uid();
void assign_uids(LevelItem& item);
// Marks the contents of `container` (at nesting `depth`, 0 on the map) as
// contained, with their depth in x, recursively.
void set_depths(LevelItem& container, std::int32_t depth = 0);

struct Level {
    std::vector<LevelItem> items;  // top-level items, in file order

    // Parses a FIXED.DAT entry. Never throws: a trailing partial record is
    // dropped, a contained record without a container stays at top level.
    static Level parse(std::span<const std::uint8_t> entry);
    std::vector<std::uint8_t> serialize() const;

    // Number of records (items and contents).
    std::size_t record_count() const;

    LevelItem* find(std::uint32_t uid);
    const LevelItem* find(std::uint32_t uid) const;
    // Index in `items` of a top-level item, nullopt for contents or unknown.
    std::optional<std::size_t> top_index(std::uint32_t uid) const;
    // The top-level item that holds `uid` (itself when top level).
    const LevelItem* top_of(std::uint32_t uid) const;
    // Removes an item wherever it is (with its contents). True if found.
    bool remove(std::uint32_t uid);
};

struct GlobPiece {
    std::uint8_t x = 0, y = 0, z = 0;
    std::uint16_t shape = 0;
    std::uint8_t frame = 0;
};

struct Glob {
    std::vector<GlobPiece> pieces;
    static Glob parse(std::span<const std::uint8_t> entry);
    std::vector<std::uint8_t> serialize() const;
};

// Where a glob egg puts a piece (usecode units).
struct Point3 {
    std::int32_t x = 0, y = 0, z = 0;
};
Point3 glob_piece_position(const LevelItem& egg, const GlobPiece& piece);

// An editable copy of a Flex archive.
struct FlexEntries {
    std::vector<std::vector<std::uint8_t>> entries;
    static FlexEntries read(const std::string& path);
    std::vector<std::uint8_t> bytes() const;
    void write(const std::string& path) const;
};

}  // namespace crusader::editor
