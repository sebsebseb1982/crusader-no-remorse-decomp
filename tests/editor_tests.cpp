// Level editor model: FIXED.DAT records, containers, globs, egg parameters
// (no game data).

#include "editor/generator/generator.hpp"
#include "editor/item_semantics.hpp"
#include "editor/level.hpp"
#include "editor/level_tools.hpp"
#include "editor/project.hpp"

#include <algorithm>
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

LevelItem item_at(std::uint16_t shape, std::uint8_t frame, std::uint16_t quality, std::uint8_t map = 0) {
    LevelItem i;
    i.uid = new_uid();
    i.shape = shape;
    i.frame = frame;
    i.quality = quality;
    i.map_number = map;
    return i;
}

void check_level_tools() {
    // Shapes of this test: 0x194 teleport egg, 0x011 trigger egg, 0x010 glob
    // egg, 0x24F monster egg; 0x300 a switch and 0x301 a door (scripted),
    // 0x302 a crate (no script).
    LevelInfo info;
    info.family = [](std::uint16_t s) -> std::uint8_t {
        switch (s) {
            case kShapeTeleportEgg: return kFamTeleportEgg;
            case kShapeTriggerEgg: return kFamTriggerEgg;
            case kShapeGlobEgg: return kFamGlobEgg;
            case kShapeMonsterEgg: return kFamMonsterEgg;
            case 0x300: return kFamUsable;
            default: return kFamGeneric;
        }
    };
    info.has_class = [](std::uint16_t cid) { return cid == 0x300 || cid == 0x301 || cid == 0x901; };

    Level level;
    level.items.push_back(item_at(kShapeTeleportEgg, 1, kStartEgg));
    level.items.push_back(item_at(kShapeTeleportEgg, 1, 1));
    level.items.push_back(item_at(kShapeTeleportEgg, 1, 2));
    level.items.push_back(item_at(0x300, 0, 0x0001));  // switch, link 1
    level.items.push_back(item_at(0x301, 0, 0x0001));  // door, link 1
    level.items.push_back(item_at(0x302, 0, 0x0002));  // crate: its quality is no link
    level.items.push_back(item_at(kShapeTriggerEgg, 0, 0x0201));  // trigger type 1, parameter 2
    check(free_arrival_id(level, info) == 3, "free arrival id");
    check(free_link_quality(level, info) == 3, "free link: 1 linked, 2 a trigger parameter");
    check(arrival_ids(level, info).size() == 3, "arrival ids");

    // Pasting the switch, the door and a teleport pair into the same map.
    std::vector<LevelItem> paste = {level.items[3], level.items[4], item_at(kShapeTeleportEgg, 1, 1),
                                    item_at(kShapeTeleportEgg, 0, 1, 5), item_at(kShapeTeleportEgg, 0, 2, 5)};
    const int renumbered = relink_pasted(paste, level, info, 5, 7);
    check(renumbered == 2, "one link and one arrival renumbered");
    check(paste[0].quality_low() == 3 && paste[1].quality_low() == 3, "pasted switch and door stay linked");
    check(paste[2].quality_low() == 3, "pasted arrival gets a free id");
    check(paste[3].quality_low() == 3 && paste[3].map_number == 7, "pasted departure follows its arrival");
    check(paste[4].quality_low() == 2 && paste[4].map_number == 5, "other departure unchanged");
    // A door alone keeps its switch on its own map, not on another one.
    std::vector<LevelItem> door = {level.items[4]};
    check(relink_pasted(door, level, info, 5, 5) == 0 && door[0].quality_low() == 1, "lone door keeps its link");
    check(relink_pasted(door, level, info, 5, 7) == 1 && door[0].quality_low() == 3, "lone door renumbered elsewhere");

    // MONSTER pairs: the bytes of the NPC, split between place and kind point.
    auto [place, kind] = make_monster_pair(100, 200, 8, 6, 0x21);
    check(place.frame == 0 && kind.frame == 1 && place.quality_low() == 0x21 && kind.quality_low() == 0x21,
          "monster pair linked");
    MonsterPair mp = read_monster_pair(place, kind);
    check(mp.npc_type == 6 && mp.on_enter && mp.min_difficulty == 1, "monster pair defaults");
    mp.direction = 5;
    mp.min_difficulty = 3;
    mp.other_weapon = true;
    mp.on_enter = false;
    mp.activity[0] = 1;
    mp.activity[1] = 13;
    mp.activity[2] = 9;
    write_monster_pair(place, kind, mp);
    check(place.npc_number == 5 && place.map_number == 6 && place.quality == 0x0D21, "monster place bytes");
    check(kind.npc_number == 6 && kind.quality == 0x0121 && kind.map_number == 9, "monster kind point bytes");
    LevelInfo minfo = info;
    minfo.has_class = [](std::uint16_t cid) { return cid == kShapeMonster; };
    Level npcs;
    npcs.items = {place, kind};
    LevelItem far = kind;
    far.uid = new_uid();
    far.x += 65;
    npcs.items.push_back(far);
    check(monster_partner(npcs, npcs.items[0], minfo) == &npcs.items[1], "nearest kind point");
    check(monster_partner(npcs, npcs.items[2], minfo) == nullptr, "kind point out of range");

    // Switches (TRIGGER): map bits 5-7 and NPC number name the target shape.
    LevelItem sw;
    sw.map_number = 0x1B;  // flags of its own in bits 0-4
    set_trigger_target(sw, kShapeMonster);
    check(sw.npc_number == 0xD0 && sw.map_number == 0x9B && trigger_target(sw) == kShapeMonster, "switch target");
    // Settings: the user's own when the maps have it 3 times, else any user's.
    TriggerSettings settings;
    settings[0x061u << 16 | 0x565] = {1, 7, 3, 3};
    settings[0x1B8u << 16 | 0x565] = {0, 6, 2, 2};
    settings[0xFFFFu << 16 | 0x565] = {2, 6, 9, 12};
    check(find_trigger_setting(settings, 0x061, 0x565)->flags == 7, "user's own setting");
    check(find_trigger_setting(settings, 0x1B8, 0x565)->quality_high == 2, "too few: any user's");
    check(find_trigger_setting(settings, 0x061, 0x564) == nullptr, "no setting");
    LevelItem user;
    user.quality = 0x0033;
    user.map_number = 0x80;
    apply_trigger_setting(user, settings[0x061u << 16 | 0x565]);
    check(user.quality == 0x0133 && user.map_number == 0x87, "setting applied, target kept");

    // call TRIGGER(0xA17) fn 0x21; spawn 4, 2, ITEM(0xA0C) fn 0x2F
    const std::vector<std::uint8_t> code = {0x5A, 0x02, 0x11, 0x17, 0x0A, 0x21, 0x00,
                                            0x57, 0x04, 0x02, 0x0C, 0x0A, 0x2F, 0x00, 0x50};
    check(code_calls_class(code, 0xA17) && code_calls_class(code, 0xA0C) && !code_calls_class(code, 0xA04),
          "calls found in the bytecode");

    // The switch and the door share link 1; the crate's quality is no link.
    const auto group = linked_group(level, level.items[3], info, 2048);
    check(group.size() == 2 && group[0] == level.items[3].uid && group[1] == level.items[4].uid, "linked group");
    check(linked_group(level, level.items[5], info, 2048).empty(), "no group without a link");

    // Prefabs read back.
    Prefab prefab;
    prefab.anchor_x = 1000;
    prefab.anchor_y = 2000;
    prefab.anchor_z = 16;
    prefab.source_map = 5;
    prefab.items = {level.items[3], level.items[4], make_monster_egg(1010, 2010, 16, 7)};
    const auto bytes = prefab.serialize();
    const auto back = Prefab::parse(bytes);
    check(back && back->anchor_x == 1000 && back->anchor_y == 2000 && back->anchor_z == 16 && back->source_map == 5,
          "prefab header");
    check(back && back->items.size() == 3 && back->items[2].contents.size() == 1 && back->items[1].shape == 0x301,
          "prefab items, contents included");
    check(!Prefab::parse(std::vector<std::uint8_t>{'X', 'P', 'F', '1', 0, 0, 0, 0, 0, 0, 0, 0}), "not a prefab");

    // Problems.
    CheckContext context;
    context.info = info;
    context.map = 5;
    context.glob_pieces = [](std::uint16_t g) -> std::size_t { return g == 7 ? 4 : 0; };
    context.map_exists = [](int m) { return m == 5 || m == 6; };
    context.arrivals_on = [](int m) {
        std::map<std::uint8_t, int> ids;
        if (m == 6) ids[4] = 1;
        return ids;
    };
    auto count = [&](const Level& l, Severity s) {
        const auto problems = check_level(l, context);
        return std::count_if(problems.begin(), problems.end(), [&](const Problem& p) { return p.severity == s; });
    };
    check(count(level, Severity::Error) == 0 && count(level, Severity::Warning) == 0, "a clean map");
    Level bad = level;
    LevelItem to_missing = item_at(kShapeTeleportEgg, 0, 9, 5);
    set_hatch_zone(to_missing, 2, 2);
    LevelItem to_other = item_at(kShapeTeleportEgg, 0, 4, 6);
    set_hatch_zone(to_other, 2, 2);
    LevelItem to_none = item_at(kShapeTeleportEgg, 0, 4, 0);
    set_hatch_zone(to_none, 2, 2);
    bad.items.push_back(to_missing);                              // error: no arrival 9 here
    bad.items.push_back(to_other);                                // fine: map 6 has egg 4
    bad.items.push_back(to_none);                                 // error: map 0
    bad.items.push_back(item_at(kShapeTriggerEgg, 0, 0x0005));    // error: no class 0x905
    bad.items.push_back(item_at(kShapeGlobEgg, 0, 3));            // note: empty glob
    bad.items.push_back(item_at(kShapeGlobEgg, 0, 0));            // fine: glob 0 is none
    bad.items.push_back(item_at(kShapeTeleportEgg, 1, 2));        // warning: id 2 twice
    LevelItem monster = item_at(kShapeMonsterEgg, 0, 0);
    bad.items.push_back(monster);                                 // error: no template
    check(count(bad, Severity::Error) == 4, "errors found");
    check(count(bad, Severity::Warning) == 1, "warnings found");
    bad.items.erase(bad.items.begin());
    check(check_level(bad, context).front().severity == Severity::Error, "errors first");
    check(count(bad, Severity::Note) == 2, "empty glob and missing mission start noted");
}

}  // namespace

// The procedural generator on a made-up biome (docs/GENERATEUR.md).
void check_generator() {
    using namespace crusader::editor::gen;
    Biome biome;
    biome.floor = {0x100, 0};
    biome.floor_variants = {{0x100, 0}, {0x100, 1}};
    for (int side = 0; side < 2; ++side) {
        for (int axis = 0; axis < 2; ++axis) {
            for (int len = 0; len < 5; ++len) {
                biome.walls[side][axis][len] = {static_cast<std::uint16_t>(0x200 + axis * 8 + len),
                                                static_cast<std::uint8_t>(side * 2)};
            }
        }
    }
    // Wall runs: longest pieces first.
    const auto run = biome.wall_run(kFront, kAlongY, 30);
    check(run.size() == 4 && run[0].shape == 0x20C && run[3].shape == 0x209 && run[0].frame == 2, "wall run 30");
    check(biome.can_build(kBack, kAlongX, 1) && biome.can_build(kBack, kAlongX, 0), "post, empty run");
    Biome no_post = biome;
    no_post.walls[kBack][kAlongX][0] = {};
    check(!no_post.can_build(kBack, kAlongX, 3) && no_post.can_build(kBack, kAlongX, 6), "runs without posts");

    const std::vector<Biome> biomes{biome};
    for (std::uint32_t seed = 1; seed <= 60; ++seed) {
        Options o;
        o.seed = seed;
        o.map = 100;
        o.next_map = 101;
        const Result r = generate(biomes, o);
        check(check_solvable(r).empty(), "generated level solvable");
        check(r.rooms.size() >= 6 && r.rooms.size() <= 10 && r.links.size() + 1 >= r.rooms.size(), "rooms and links");
        check(r.exit_room != r.start_room && r.rooms[r.exit_room].depth >= 1, "exit away from the start");
        // Same seed, same level.
        check(generate(biomes, o).level.serialize() == r.level.serialize(), "generation is deterministic");
        // Rooms apart from each other, on whole floor tiles.
        for (std::size_t i = 0; i < r.rooms.size(); ++i) {
            const Room& a = r.rooms[i];
            check((a.x1 - a.x0) % kBlock == 0 && (a.y1 - a.y0) % kBlock == 0 && a.x1 - a.x0 >= 512, "room tiles");
            for (std::size_t j = i + 1; j < r.rooms.size(); ++j) {
                const Room& b = r.rooms[j];
                check(a.x1 <= b.x0 || b.x1 <= a.x0 || a.y1 <= b.y0 || b.y1 <= a.y0, "rooms do not overlap");
            }
        }
        int arrivals = 0, departures = 0, doors = 0, door_eggs = 0;
        for (const LevelItem& i : r.level.items) {
            check(i.x > 0 && i.y > 0 && i.x < kMapSize && i.y < kMapSize, "items on the map");
            if (i.shape == kShapeTeleportEgg && i.frame == 1 && i.quality == kStartEgg) ++arrivals;
            if (i.shape == kShapeTeleportEgg && i.frame == 0 && i.map_number == 101 && i.quality_low() == kStartEgg) {
                ++departures;
            }
            if (i.shape == 0x0A9 || i.shape == 0x046) ++doors;
            if (i.shape == kShapeTriggerEgg && i.quality == 0x0A && i.npc_number == 0x44) ++door_eggs;
        }
        check(arrivals == 1 && departures == 1, "one start, one exit");
        check(doors == static_cast<int>(2 * r.links.size()) && door_eggs == doors, "a door and its egg per corridor end");
    }
    Options o;
    o.biome = 3;  // out of range: drawn
    o.rooms = 2;
    const Result two = generate(biomes, o);
    check(two.rooms.size() == 2 && check_solvable(two).empty(), "two rooms");
}

int main() {
    check_records();
    check_globs();
    check_semantics();
    check_level_tools();
    check_project_files();
    check_generator();
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
