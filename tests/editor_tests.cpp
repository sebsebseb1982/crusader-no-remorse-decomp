// Level editor model: FIXED.DAT records, containers, globs, egg parameters
// (no game data).

#include "editor/item_semantics.hpp"
#include "editor/level.hpp"
#include "editor/project.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace crusader;
using namespace crusader::editor;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void record(std::vector<std::uint8_t>& out, std::uint16_t x, std::uint16_t y, std::uint8_t z, std::uint16_t shape,
            std::uint8_t frame, std::uint16_t flags, std::uint16_t quality, std::uint8_t npc, std::uint8_t map,
            std::uint16_t next) {
    const std::uint8_t r[16] = {static_cast<std::uint8_t>(x),       static_cast<std::uint8_t>(x >> 8),
                                static_cast<std::uint8_t>(y),       static_cast<std::uint8_t>(y >> 8),
                                z,
                                static_cast<std::uint8_t>(shape),   static_cast<std::uint8_t>(shape >> 8),
                                frame,
                                static_cast<std::uint8_t>(flags),   static_cast<std::uint8_t>(flags >> 8),
                                static_cast<std::uint8_t>(quality), static_cast<std::uint8_t>(quality >> 8),
                                npc,
                                map,
                                static_cast<std::uint8_t>(next),    static_cast<std::uint8_t>(next >> 8)};
    out.insert(out.end(), r, r + 16);
}

void check_records() {
    std::vector<std::uint8_t> bytes;
    record(bytes, 1991, 1013, 0, 0x24F, 0, 0x2000, 0, 5, 0, 1768);  // monster egg
    record(bytes, 1, 0, 0, 2, 0, 8, 0x0101, 4, 1, 0);                // its template
    record(bytes, 2047, 2559, 0, 16, 0, 0, 2951, 0, 0, 434);         // glob egg
    record(bytes, 100, 200, 8, 0x50, 1, 0, 0, 0, 0, 0);              // container
    record(bytes, 1, 7, 0, 0x60, 0, 8, 0, 0, 0, 0);                  //   item inside
    record(bytes, 2, 9, 0, 0x61, 0, 8, 0, 0, 0, 0);                  //     inside that one
    record(bytes, 1, 3, 0, 0x62, 0, 8, 0, 0, 0, 0);                  //   second item
    record(bytes, 300, 400, 16, 0x70, 2, 0x20, 0x1234, 0x22, 3, 77);  // plain
    const Level level = Level::parse(bytes);
    check(level.items.size() == 4, "top-level items");
    check(level.record_count() == 8, "record count");
    check(level.items[0].contents.size() == 1 && level.items[0].contents[0].npc_number == 4, "monster template");
    check(level.items[2].contents.size() == 2 && level.items[2].contents[0].contents.size() == 1,
          "nested contents");
    check(level.items[2].contents[0].y == 7 && level.items[2].contents[1].shape == 0x62, "content fields");
    check(level.serialize() == bytes, "records written back byte for byte");

    // Removing an item takes its contents along.
    Level copy = level;
    check(copy.remove(copy.items[2].contents[0].uid) && copy.record_count() == 6, "remove with contents");
    check(copy.top_of(copy.items[2].contents[0].uid) == &copy.items[2], "top_of");
    check(copy.top_index(copy.items[3].uid) == 3 && !copy.top_index(copy.items[2].contents[0].uid), "top_index");
    // A partial record at the end is dropped.
    bytes.resize(bytes.size() + 5, 0xAA);
    check(Level::parse(bytes).record_count() == 8, "partial record ignored");
}

void check_globs() {
    Glob g;
    g.pieces = {{0, 0, 0, 0x100, 1}, {255, 128, 24, 0x7FF, 3}};
    const auto bytes = g.serialize();
    check(bytes.size() == 2 + 12 && bytes[0] == 2, "glob layout");
    const Glob back = Glob::parse(bytes);
    check(back.pieces.size() == 2 && back.pieces[1].shape == 0x7FF && back.pieces[1].x == 255 &&
              back.pieces[1].frame == 3,
          "glob read back");
    LevelItem egg;
    egg.x = 0x1234;
    egg.y = 0x0A01;
    egg.z = 250;
    const Point3 p = glob_piece_position(egg, g.pieces[1]);
    check(p.x == 0x1200 + 511 && p.y == 0x0A00 + 257 && p.z == ((250 + 24) & 0xFF), "glob piece position");
}

void check_semantics() {
    // Trigger egg: class 0x900 + quality low, zone from the NPC number.
    LevelItem egg;
    egg.quality = 0x0103;
    egg.npc_number = 0x22;
    check(usecode_class_of(egg, kFamTriggerEgg) == 0x903, "trigger egg class");
    check(usecode_class_of(egg, kFamGeneric) == egg.shape, "shape class");
    const HatchZone z = hatch_zone(egg);
    check(z.half_x == 64 && z.half_y == 64, "hatch zone");
    set_hatch_zone(egg, 20, 3);
    check(egg.npc_number == 0xF3, "hatch zone clamped");
    check(hatches(kFamTeleportEgg, LevelItem{}) && !hatches(kFamGlobEgg, LevelItem{}), "hatching families");

    // Monster egg: egg and template bytes.
    LevelItem m = make_monster_egg(10, 20, 0, 9);
    MonsterEgg spec = read_monster_egg(m);
    check(m.contents.size() == 1 && m.contents[0].flags == kFlagContained && spec.npc_type == 9, "monster egg");
    spec.direction = 6;
    spec.record_12 = 2;
    spec.min_difficulty = 3;
    spec.other_weapon = true;
    spec.activity[0] = 8;
    spec.activity[1] = 1;
    spec.activity[2] = 5;
    write_monster_egg(m, spec);
    check(m.npc_number == 0x26 && m.map_number == 6, "monster egg bytes");
    check(m.contents[0].quality == 0x0801 && m.contents[0].map_number == 5, "monster template bytes");
    const MonsterEgg again = read_monster_egg(m);
    check(again.min_difficulty == 3 && again.other_weapon && again.direction == 6, "monster egg read back");

    // Markers: 0x564 frame 1 is NPC number 0xA1, map number 0x64.
    LevelItem marker;
    write_marker(marker, {0x564, 1});
    check(marker.npc_number == 0xA1 && marker.map_number == 0x64, "marker bytes");
    const MarkerTarget t = read_marker(marker);
    check(t.shape == 0x564 && t.frame == 1, "marker read back");
}

void check_project_files() {
    // A file of the mod folder takes the place of the data's.
    const auto root = std::filesystem::temp_directory_path() / "crusader_editor_tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "data/STATIC");
    std::filesystem::create_directories(root / "mod/STATIC");
    std::ofstream(root / "data/STATIC/A.DAT") << "a";
    std::ofstream(root / "data/STATIC/B.DAT") << "b";
    std::ofstream(root / "mod/STATIC/B.DAT") << "B";
    check(resolve_file(root / "data", root / "mod", "STATIC/A.DAT") == root / "data/STATIC/A.DAT", "data file");
    check(resolve_file(root / "data", root / "mod", "STATIC/B.DAT") == root / "mod/STATIC/B.DAT", "mod file");
    check(resolve_file(root / "data", {}, "STATIC/B.DAT") == root / "data/STATIC/B.DAT", "no mod");

    // Flex entries survive a write.
    FlexEntries flex;
    flex.entries = {{}, {1, 2, 3}, {}, {4}};
    flex.write((root / "x.flx").string());
    const FlexEntries back = FlexEntries::read((root / "x.flx").string());
    check(back.entries == flex.entries, "flex entries written");
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    check_records();
    check_globs();
    check_semantics();
    check_project_files();
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
