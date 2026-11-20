// Items and maps checked against the original game data (data/, not in the
// repository): runs locally only.

#include "core/shape.hpp"
#include "core/typeflags.hpp"
#include "core/xform.hpp"
#include "engine/kernel.hpp"
#include "engine/usecode/item_intrinsics.hpp"
#include "engine/usecode/uc_machine.hpp"
#include "engine/usecode/uc_process.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/gravity.hpp"
#include "engine/world/lighting.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"
#include "test_saves.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <optional>
#include <set>
#include <tuple>
#include <filesystem>

using namespace crusader;


namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

// Runs `code` as event 0 of class 0 on `item`, returns the process result.
std::uint32_t run_on_item(World& w, ObjId item, const std::vector<std::uint8_t>& code) {
    const auto file = UsecodeFile::from_classes({UsecodeFile::make_class({1}, code)});
    Kernel k;
    UCMachine m(k, file);
    register_item_intrinsics(m, w);
    std::uint32_t result = 0xDEADBEEF;
    struct Watcher : Process {
        std::uint32_t& out;
        explicit Watcher(std::uint32_t& o) : out(o) {}
        void run() override { out = wake_result(); terminate(); }
    };
    const Pid pid = m.start_event(0, 0, item);
    auto watcher = std::make_unique<Watcher>(result);
    Watcher& ref = *watcher;
    k.add(std::move(watcher));
    ref.wait_for(pid);
    for (int i = 0; i < 10 && k.process_count(); ++i) k.run_slice();
    CHECK(m.faults().empty());
    return result;
}


// Intrinsic `n` on item `a` with the object id `b`: push b; push this;
// calli 6, n; add_sp 6; push_retval; word_to_dword; pop_res; ret
std::uint32_t pair_intrinsic(World& w, std::uint16_t n, ObjId a, ObjId b) {
    return run_on_item(w, a, {0x0B, static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(b >> 8), 0x40, 0x06, 0x0F,
                              0x06, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8), 0x6E, 0xFA, 0x5E,
                              0x60, 0x08, 0x50});
}

void test_original_maps(const std::filesystem::path& data) {
    const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    CHECK(typeflags.size() == 2048);
    std::size_t containers = 0;
    for (const auto& s : typeflags) containers += s.family == 6;
    CHECK(containers == 46);

    const auto fixed = FlexArchive::open(data / "STATIC/FIXED.DAT");
    const auto globs = FlexArchive::open(data / "STATIC/GLOB.FLX");
    std::size_t maps = 0, items = 0, contained = 0, expanded = 0;
    for (std::size_t map = 0; map < fixed.count(); ++map) {
        if (fixed.entry(map).empty()) continue;
        World w(typeflags);
        w.load_fixed_map(fixed, map);
        ++maps;
        items += w.item_count();
        expanded += w.expand_glob_eggs(globs);
        w.for_each([&](const Item& i) { contained += i.parent != 0; });
    }
    std::printf("FIXED.DAT: %zu maps, %zu items, %zu inside containers, %zu items from glob eggs\n", maps, items,
                contained, expanded);
    CHECK(expanded > items);  // most of the scenery comes from globs
    CHECK(maps == 63);

    // Saved game: item table with the player in slot 1.
    const auto save = FlexArchive::open(test_saves(data) / "SAVEGAME.1");
    World sw(typeflags);
    const std::size_t saved = sw.load_saved_items(save);
    const std::size_t free_items = sw.free_items().ids.size(), free_npcs = sw.free_npcs().ids.size();
    std::size_t bad = 0;
    const auto shapes_flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    sw.for_each([&](const Item& i) {
        if (shapes_flex.entry(i.shape).empty()) { ++bad; return; }
        const auto shape = Shape::parse(shapes_flex.entry(i.shape));
        if (i.frame >= shape.frames().size()) ++bad;
    });
    const std::size_t from_globs = sw.expand_glob_eggs(globs);
    std::printf("SAVEGAME.1: %zu items (%zu invalid), +%zu glob items not already saved\n", saved, bad, from_globs);
    std::size_t in_containers = 0, linked = 0;
    sw.for_each([&](const Item& i) {
        if (!(i.flags & kItemContained)) return;
        ++in_containers;
        const Item* parent = sw.get(i.parent);
        if (parent && std::ranges::count(parent->contents, i.id) == 1) ++linked;
    });
    // 8288 slots with a shape, 264 of them free (deleted items keep their
    // data in the table).
    CHECK(saved == 8024);
    CHECK(free_items == 1965 && free_npcs == 250);

    CHECK(in_containers == 12 && linked == 12);
    // Map grid from entry 1: every item on the map in its cell, once.
    CHECK(sw.cells_consistent());
    // The player stands on one of the items below him (Item::I_isOn).
    {
        const std::uint8_t all[] = {'$'};
        const std::vector<ObjId> below = sw.surface_search(1, 2, all);
        const auto on = std::ranges::count_if(below, [&](ObjId id) { return pair_intrinsic(sw, 0x046, 1, id) == 1; });
        std::printf("SAVEGAME.1: player on %td of the %zu items below\n", on, below.size());
        CHECK(on > 0);
    }

    // Saved fast area (entry 1), around the camera: 11 x 7 cells in s, d.
    // Glob items on the map are exactly those of the area; moving the camera
    // across the map replaces them with those of the new area.
    {
        World mw(typeflags);
        mw.load_saved_items(save);
        mw.set_globs(&globs);
        CHECK((mw.fast_area() == FastArea{69, 79, 13, 19}));
        auto glob_items = [&](std::size_t& outside) {
            std::size_t n = 0;
            outside = 0;
            mw.for_each([&](const Item& i) {
                if (!(i.flags & kItemDisposable) || i.parent || (i.flags & kItemEthereal)) return;
                ++n;
                outside += !mw.fast_area().contains(i.x, i.y);
            });
            return n;
        };
        std::size_t outside = 0;
        const std::size_t saved_globs = glob_items(outside);
        CHECK(saved_globs == 539 && outside == 0);
        mw.update_fast_area(27667, 24763);  // player position in SAVEGAME.2
        const std::size_t new_globs = glob_items(outside);
        std::printf("SAVEGAME.1 camera moved: %zu glob items around the camera, %zu after the move\n", saved_globs,
                    new_globs);
        CHECK(outside == 0 && new_globs > 300);

        // SAVEGAME.2 was saved with that fast area: the original made the same glob items.
        World ow(typeflags);
        ow.load_saved_items(FlexArchive::open(test_saves(data) / "SAVEGAME.2"));
        CHECK(ow.fast_area() == mw.fast_area());
        using Key = std::tuple<std::int32_t, std::int32_t, std::int32_t, std::uint16_t, std::uint16_t>;
        auto keys = [](const World& world) {
            std::multiset<Key> k;
            world.for_each([&](const Item& i) {
                // 0x82 = from a glob; other disposable items (0x81) come from elsewhere.
                if ((i.flags & 0x82) == 0x82 && !i.parent && !(i.flags & kItemEthereal))
                    k.emplace(i.x, i.y, i.z, i.shape, i.frame);
            });
            return k;
        };
        const auto ours = keys(mw), original = keys(ow);
        std::vector<Key> only_ours, only_original;
        std::ranges::set_difference(ours, original, std::back_inserter(only_ours));
        std::ranges::set_difference(original, ours, std::back_inserter(only_original));
        std::printf("glob items vs SAVEGAME.2: %zu ours, %zu original, %zu only ours, %zu only original\n",
                    ours.size(), original.size(), only_ours.size(), only_original.size());
        CHECK(only_ours.empty() && only_original.empty());
    }

    // NPC table (entry 10): the NPCs marked dead are those whose slot was
    // freed (their item is gone, the record stays).
    std::size_t dead = 0, dead_freed = 0;
    for (ObjId id = 1; id < 256; ++id) {
        if (!sw.is_dead(id)) continue;
        ++dead;
        dead_freed += !sw.get(id) && std::ranges::count(sw.free_npcs().ids, id) == 1;
    }
    CHECK(dead == 2 && dead_freed == 2);

    CHECK(sw.npc_direction(1) < 16);

    const auto damage = FlexArchive::open(data / "STATIC/DAMAGE.FLX");
    CHECK(damage.entry(0).size() == 2048 * 6);
    CHECK(bad == 0);
    CHECK(sw.get(1) && sw.get(1)->shape == 1 && sw.get(1)->npc_number == 1);
    CHECK(items == 233326);
}

// Draw order of a crate stack on map 1 (bug report image sprites-problems):
// sorting edges between items whose sprite rectangles meet but whose box
// hexagons do not chained the raised floor, the vent grates and the crates
// into cycles, and breaking them drew back crates over front ones.
void test_crate_stack_order(const std::filesystem::path& data) {
    const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    World w(typeflags);
    w.load_fixed_map(FlexArchive::open(data / "STATIC/FIXED.DAT"), 1);
    w.expand_glob_eggs(FlexArchive::open(data / "STATIC/GLOB.FLX"));
    const auto shapes_flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    ShapeCache shapes(shapes_flex, ShapeLayout::World);
    RenderStats stats;
    const auto list = build_display_list(w, shapes, Camera{58846, 52670, 0}, 640, 480, &stats);
    auto at = [&](ObjId id) {
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].item->id == id) return static_cast<int>(i);
        return -1;
    };
    std::printf("crate stack: %zu cycle edges\n", stats.cycle_edges);
    CHECK(stats.cycle_edges == 0);
    // Bottom 2x2 (6283 6282 behind 6285 6284), top 6286 6287, wall 6262 behind.
    CHECK(at(6283) >= 0 && at(6262) < at(6283) && at(6283) < at(6285) && at(6282) < at(6284));
    CHECK(at(6283) < at(6282) && at(6285) < at(6284));
    CHECK(at(6285) < at(6286) && at(6284) < at(6287));
}

// Windows (shape 821, TYPEFLAG translucent) of SAVEGAME.7 go through the
// translucent blitter: their colours 8..14 no longer reach the screen as is.
void test_translucent_windows(const std::filesystem::path& data) {
    const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    World w(typeflags);
    w.load_saved_items(FlexArchive::open(test_saves(data) / "SAVEGAME.7"));
    w.expand_glob_eggs(FlexArchive::open(data / "STATIC/GLOB.FLX"));
    const auto shapes_flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    ShapeCache shapes(shapes_flex, ShapeLayout::World);
    const auto xform = XformTable::load(data / "STATIC/XFORMPAL.DAT");
    const Item* player = w.get(1);
    CHECK(player != nullptr);
    if (!player) return;
    const auto list = build_display_list(
        w, shapes, Camera{world_from_usecode(player->x), world_from_usecode(player->y), player->z}, 640, 480);
    auto count_raw = [&](const XformTable* table) {
        Surface8 screen(640, 480);
        screen.clear(0);
        draw_display_list(screen, list, table);
        int raw = 0;
        for (const DrawItem& d : list) {
            if (d.item->id != 2578) continue;
            for (int y = std::max(d.top, 0); y < std::min(d.bottom, 480); ++y)
                for (int x = std::max(d.left, 0); x < std::min(d.right, 640); ++x)
                    raw += xform.translucent(screen.row(y)[x]);
        }
        return raw;
    };
    const int opaque = count_raw(nullptr), blended = count_raw(&xform);
    std::printf("window 2578: %d translucent colours drawn opaque, %d through the blitter\n", opaque, blended);
    CHECK(opaque > 1000 && blended * 100 < opaque);  // what remains: other items, blends into 8..14
}

// Port lighting: the wall lamp of the bar in SAVEGAME.5 (item 8239, shape
// 0x2A0, a dome, box z 72..96) lights from its bulb while whole; its broken
// frames (4..7) give none, so a lamp shot goes out.
void test_fixture_lights(const std::filesystem::path& data) {
    constexpr ObjId kLamp = 8239;
    const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    World w(typeflags);
    w.load_saved_items(FlexArchive::open(test_saves(data) / "SAVEGAME.5"));
    w.expand_glob_eggs(FlexArchive::open(data / "STATIC/GLOB.FLX"));
    const auto shapes_flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    ShapeCache shapes(shapes_flex, ShapeLayout::World);
    const auto xform = XformTable::load(data / "STATIC/XFORMPAL.DAT");
    const auto palette = Palette::load(data / "STATIC/GAMEPAL.PAL");
    Item* lamp = w.get(kLamp);
    const Item* player = w.get(1);
    CHECK(lamp && lamp->shape == 0x2A0 && lamp->frame == 0 && player);
    if (!lamp || !player) return;
    WorldLighting lighting(shapes, xform);
    auto lamp_lights = [&](LightingOptions opt) {
        const Camera camera{world_from_usecode(player->x), world_from_usecode(player->y), player->z};
        const int left = ((camera.x - camera.y) >> 2) - 320, top = (((camera.x + camera.y) >> 3) - camera.z) - 240;
        const auto list = build_display_list(w, shapes, View{left, top}, 640, 480);
        Surface8 screen(640, 480);
        draw_display_list(screen, list, &xform);
        lighting.capture(w, list, screen, left, top, palette, opt, 0);
        std::vector<std::uint32_t> argb(640 * 480);
        lighting.shade(w, screen, palette, opt, {}, argb.data());
        std::vector<PointLight> out;
        for (const PointLight& l : lighting.lights())
            if (l.owner == kLamp) out.push_back(l);
        return out;
    };
    const LightingOptions on;
    const auto lit = lamp_lights(on);
    CHECK(lit.size() == 1);
    if (lit.size() == 1) {
        const float z = lit[0].pos.z / kLightZScale;
        std::printf("bar lamp: light at %.0f %.0f z %.1f\n", lit[0].pos.x, lit[0].pos.y, z);
        CHECK(z > 76 && z < 100);  // the bulb, high on the lamp
    }
    LightingOptions off = on;
    off.fixture_lights = false;
    CHECK(lamp_lights(off).empty());
    lamp->frame = 4;  // broken
    CHECK(lamp_lights(on).empty());
}

// Port lighting: a shot of a firearm lights a short warm flash at the muzzle,
// ahead of it, fading out in 4 ticks; energy weapons (their shot glows) none.
void test_muzzle_flash(const std::filesystem::path& data) {
    const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    World w(typeflags);
    w.load_saved_items(FlexArchive::open(test_saves(data) / "SAVEGAME.5"));
    w.expand_glob_eggs(FlexArchive::open(data / "STATIC/GLOB.FLX"));
    const auto shapes_flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    ShapeCache shapes(shapes_flex, ShapeLayout::World);
    const auto xform = XformTable::load(data / "STATIC/XFORMPAL.DAT");
    const auto palette = Palette::load(data / "STATIC/GAMEPAL.PAL");
    const Item* player = w.get(1);
    CHECK(player != nullptr);
    if (!player) return;
    WorldLighting lighting(shapes, xform);
    LightingOptions opt;
    opt.player_lamp = false;  // its light is the player's too
    auto player_lights = [&](std::uint32_t ticks) {
        const Camera camera{world_from_usecode(player->x), world_from_usecode(player->y), player->z};
        const int left = ((camera.x - camera.y) >> 2) - 320, top = (((camera.x + camera.y) >> 3) - camera.z) - 240;
        const auto list = build_display_list(w, shapes, View{left, top}, 640, 480);
        Surface8 screen(640, 480);
        draw_display_list(screen, list, &xform);
        lighting.capture(w, list, screen, left, top, palette, opt, ticks);
        std::vector<std::uint32_t> argb(640 * 480);
        lighting.shade(w, screen, palette, opt, {}, argb.data());
        std::vector<PointLight> out;
        for (const PointLight& l : lighting.lights())
            if (l.owner == 1) out.push_back(l);
        return out;
    };
    CHECK(player_lights(100).empty());
    const std::int32_t mz = player->z + 32;
    lighting.shot_fired(1, player->x, player->y, mz, 4, 0xB, 100);  // BA-40, towards +x
    const auto fresh = player_lights(100);
    CHECK(fresh.size() == 1);
    if (fresh.size() == 1) {
        const PointLight& l = fresh[0];
        std::printf("muzzle flash: %.0f %.0f z %.1f, colour %.2f %.2f %.2f\n", l.pos.x, l.pos.y,
                    l.pos.z / kLightZScale, l.color.x, l.color.y, l.color.z);
        CHECK(l.pos.x > world_from_usecode(player->x) && std::abs(l.pos.y - world_from_usecode(player->y)) < 1);
        CHECK(std::abs(l.pos.z / kLightZScale - mz) < 0.5f);
        CHECK(l.color.x > l.color.y && l.color.y > l.color.z && l.color.x > 1.5f);  // warm, bright
        const auto later = player_lights(102);
        CHECK(later.size() == 1 && later[0].color.x < l.color.x * 0.5f);
    }
    CHECK(player_lights(104).empty());
    CHECK(lighting.muzzle_flash_count() == 0);
    lighting.shot_fired(1, player->x, player->y, mz, 4, 5, 200);  // PA-21: the plasma bolt glows itself
    CHECK(player_lights(200).empty());
    lighting.shot_fired(1, player->x, player->y, mz, 4, 2, 300);  // SG-A1
    opt.flash_lights = false;
    CHECK(player_lights(300).empty());
}

// A saved game loaded then written again gives back the original's bytes:
// the item table (minus its unwritten last 10240 bytes, and status bit 0,
// cleared by the loading) and its lists.
void test_save_round_trip(const std::filesystem::path& data) {
    const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    const std::size_t widths[] = {2, 2, 1, 2, 1, 2, 2, 1, 1, 2, 2, 1};
    for (int n : {1, 2, 3, 4, 5, 6, 7, 13}) {
        const auto save = FlexArchive::open(test_saves(data) / ("SAVEGAME." + std::to_string(n)));
        World w(typeflags);
        w.load_saved_items(save);
        const auto table = w.saved_item_table();
        const auto lists = w.saved_item_lists();
        const auto e0 = save.entry(0), e1 = save.entry(1);
        std::size_t field_diffs[12] = {};
        std::size_t at = 0;
        for (std::size_t f = 0; f < 12; ++f) {
            for (std::size_t i = 0; i < kItemSlots * widths[f]; ++i, ++at) {
                const bool status_low = f == 5 && i % 2 == 0;
                field_diffs[f] += ((table[at] ^ e0[at]) & (status_low ? 0xFE : 0xFF)) != 0;
            }

        }
        std::size_t list_diffs = 0;
        for (std::size_t i = 0; i < e1.size(); ++i) list_diffs += i >= lists.size() || lists[i] != e1[i];
        std::printf("SAVEGAME.%d written again: bytes differing per array", n);
        for (std::size_t d : field_diffs) std::printf(" %zu", d);
        std::printf(", lists %zu\n", list_diffs);
        CHECK(table.size() == e0.size() && lists.size() == e1.size());
        CHECK(std::ranges::all_of(field_diffs, [](std::size_t d) { return d == 0; }));
        CHECK(list_diffs == 0);
    }
}
}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path data = argc > 1 ? argv[1] : "data";
    if (!std::filesystem::exists(data / "STATIC/FIXED.DAT")) {
        std::printf("game data not found in %s, skipping\n", data.string().c_str());
        return 0;
    }
    test_original_maps(data);
    test_crate_stack_order(data);
    if (have_test_saves(data)) {
        test_translucent_windows(data);
        test_save_round_trip(data);
        test_fixture_lights(data);
        test_muzzle_flash(data);
    } else {
        ++failures;
    }
    std::printf(failures ? "%d failure(s)\n" : "all world data checks passed\n", failures);
    return failures ? 1 : 0;
}
