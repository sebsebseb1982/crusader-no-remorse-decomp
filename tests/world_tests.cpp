// Items, maps and item intrinsics on synthetic worlds (no game data).
#include "core/language.hpp"
#include "core/shape.hpp"
#include "core/typeflags.hpp"
#include "core/xform.hpp"
#include "engine/kernel.hpp"
#include "engine/usecode/item_intrinsics.hpp"
#include "engine/usecode/uc_machine.hpp"
#include "engine/usecode/uc_process.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/gravity.hpp"
#include "engine/world/pathfind.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <algorithm>
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

void test_typeflag_decode() {
    // family 6 (container): byte1 high nibble 6, byte2 bit0 0; footpad 3x4x4;
    // weight 7, volume 9; NPC flag.
    const std::uint8_t rec[9] = {0x03, 0x61, 0x60, 0x10, 0x02, 0x00, 0x20, 7, 9};
    const ShapeInfo s = ShapeInfo::decode(rec);
    CHECK(s.is(ShapeInfo::kFixed) && s.is(ShapeInfo::kSolid) && s.is(ShapeInfo::kDraw));
    CHECK(s.family == 6);
    CHECK(s.footpad_x == 3 && s.footpad_y == 4 && s.footpad_z == 4);
    CHECK(s.is(ShapeInfo::kNpc));
    CHECK(s.weight == 7 && s.volume == 9);
}

void test_world_ids() {
    World w;
    auto& a = w.add(Item{});
    auto& b = w.add(Item{});
    CHECK(a.id == kFirstItemId && b.id == kFirstItemId + 1);
    w.destroy(kFirstItemId);
    CHECK(w.add(Item{}).id == kFirstItemId);
}

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

// Item::I_cast passes its value to usecode event 0x11 (10A8:2F3E). The event
// body stores the argument it receives in the item's quality (I_setQ, 0x0F5).
void test_item_cast_argument() {
    World w;
    Item it;
    it.shape = 0x123;
    const ObjId id = w.add(it).id;

    // Caller (event 0): push 0x1234; push this; cast(this, 0x1234); add_sp 6;
    // ret.
    const std::vector<std::uint8_t> caller = {0x0B, 0x34, 0x12, 0x40, 0x06, 0x0F,
                                              0x06, 0xE8, 0x00, 0x6E, 0xFA, 0x50};
    // Event 0x11: push word bp+0x0A (the cast value); push this; setQ(this,
    // value); add_sp 6; ret.
    const std::vector<std::uint8_t> handler = {0x3F, 0x0A, 0x40, 0x06, 0x0F,
                                               0x06, 0xF5, 0x00, 0x6E, 0xFA, 0x50};
    std::vector<std::uint32_t> offsets(0x12, 0);
    offsets[0] = 1;
    offsets[0x11] = 1 + static_cast<std::uint32_t>(caller.size());
    std::vector<std::uint8_t> code(caller);
    code.insert(code.end(), handler.begin(), handler.end());

    // The item's usecode class is its shape (0x123).
    std::vector<std::vector<std::uint8_t>> classes(0x124);
    classes[0x123] = UsecodeFile::make_class(offsets, code);
    const auto file = UsecodeFile::from_classes(std::move(classes));
    Kernel k;
    UCMachine m(k, file);
    register_item_intrinsics(m, w);
    const Pid pid = m.start_event(0x123, 0, id);
    for (int i = 0; i < 10 && k.get(pid); ++i) k.run_slice();
    for (int i = 0; i < 10; ++i) k.run_slice();  // the cast event it spawned
    CHECK(m.faults().empty());
    CHECK(w.get(id) != nullptr && w.get(id)->quality == 0x1234);
}

// Ultima8Engine::I_setAvatarInStasis (10F0:3696): setting stasis on a living
// player who is in combat takes him out of it (NPC_10f0_32a0).
void test_stasis_leaves_combat() {
    constexpr std::size_t table = AnimDat::kShapes * 4, action = table + AnimDat::kActions * 4;
    std::vector<std::uint8_t> bytes(action + 4 + 8 * 4 * 8);
    auto u32 = [&](std::size_t at, std::size_t value) {
        for (int k = 0; k < 4; ++k) bytes[at + k] = static_cast<std::uint8_t>(value >> (k * 8));
    };
    u32(4, table);
    u32(table + 4, action);
    bytes[action] = 4;
    const AnimDat anims = AnimDat::parse(bytes);

    World w;
    Item player;
    player.id = 1;
    player.shape = 1;
    w.add_with_id(player);
    w.npc_record(1)[npc::kCombatFlags] = 0x08;
    CHECK(in_combat(w, 1));

    // setAvatarInStasis(1)
    const std::vector<std::uint8_t> code = {0x0B, 0x01, 0x00, 0x0F, 0x02, 0x0C, 0x00, 0x6E, 0xFE, 0x50};
    const auto file = UsecodeFile::from_classes({UsecodeFile::make_class({1}, code)});
    Kernel k;
    UCMachine m(k, file);
    register_world_intrinsics(m, w, &anims);
    const Pid pid = m.start_event(0, 0, 1);
    for (int i = 0; i < 10 && k.get(pid); ++i) k.run_slice();
    CHECK(m.faults().empty());
    CHECK(w.state.avatar_in_stasis);
    CHECK(!in_combat(w, 1));
}

void test_item_intrinsics() {
    World w;
    Item it;
    it.shape = 0x123;
    it.frame = 4;
    it.quality = 0x1234;
    it.flags = 0x00F0;
    const ObjId id = w.add(it).id;

    // push_dword bp 6 (this); calli 4, 0x001 (getFrame); add_sp 4;
    // push_retval; word_to_dword; pop_res; ret
    CHECK(run_on_item(w, id, {0x40, 0x06, 0x0F, 0x04, 0x01, 0x00, 0x6E, 0xFC, 0x5E, 0x60, 0x08, 0x50}) == 4);

    // setFrame(this, 9): push 9; push this; calli 6, 0x002; add_sp 6; ret
    run_on_item(w, id, {0x0A, 0x09, 0x40, 0x06, 0x0F, 0x06, 0x02, 0x00, 0x6E, 0xFA, 0x50});
    CHECK(w.get(id)->frame == 9);

    // getQHi -> 0x12, getQLo -> 0x34
    CHECK(run_on_item(w, id, {0x40, 0x06, 0x0F, 0x04, 0x26, 0x00, 0x6E, 0xFC, 0x5E, 0x60, 0x08, 0x50}) == 0x12);
    CHECK(run_on_item(w, id, {0x40, 0x06, 0x0F, 0x04, 0x10, 0x00, 0x6E, 0xFC, 0x5E, 0x60, 0x08, 0x50}) == 0x34);

    // andStatus(this, 0x0030) then getStatus
    run_on_item(w, id, {0x0A, 0x30, 0x40, 0x06, 0x0F, 0x06, 0x1A, 0x00, 0x6E, 0xFA, 0x50});
    CHECK(w.get(id)->flags == 0x0030);

    // destroy(this)
    run_on_item(w, id, {0x40, 0x06, 0x0F, 0x04, 0x0A, 0x00, 0x6E, 0xFC, 0x50});
    CHECK(w.get(id) == nullptr);
}

// Typeflags with a container (shape 6, family 6, footpad 2x3x4) and a
// family-7 shape 7; other shapes are plain.
std::vector<ShapeInfo> test_typeflags() {
    std::vector<ShapeInfo> tf(16);
    tf[6].family = 6;
    tf[6].footpad_x = 2;
    tf[6].footpad_y = 3;
    tf[6].footpad_z = 4;
    tf[7].family = 7;
    return tf;
}

// No free NPC slot: create_npc takes the slot of an NPC outside the fast
// area, which must have status bit 0x0002 for the French executable; the US
// one takes any.
void test_create_npc_reuses_a_slot() {
    const auto fill = [](World& w) {
        for (ObjId id = 2; id < kNpcSlots; ++id) {
            Item npc;
            npc.id = id;
            npc.shape = 0x3A0;
            npc.flags = id == 2 ? kItemFastArea : 0;  // NPC 2 is in the fast area
            w.add_with_id(npc);
        }
    };
    {
        World w(test_typeflags());
        fill(w);
        CHECK(w.create_npc(0x3A1, 0) == 0);  // none has the bit
        w.get(9)->flags |= 0x0002;
        CHECK(w.create_npc(0x3A1, 0) == 9 && w.get(9)->shape == 0x3A1);
    }
    set_text_language(Language::English);
    {
        World w(test_typeflags());
        fill(w);
        CHECK(w.create_npc(0x3A1, 0) == 3 && w.get(3)->shape == 0x3A1);
    }
    set_text_language(Language::French);
}

void test_created_npc_teleport() {
    // Mission 2 creates an NPC, aligns it with the teleporter, then destroys
    // it. Its map must agree with the grid from birth; otherwise teleport
    // skips unlinking and leaves a freed id in the cell. Also cover leaving
    // the map, returning, and reusing the freed slot.
    World w(test_typeflags());
    w.load_map(3);
    w.set_camera(5000, 5000, 0);
    for (int reuse = 0; reuse < 2; ++reuse) {
        const ObjId npc = w.create_npc(3, 0);
        w.pop_to_coords(5000, 5000, 0);
        CHECK(w.get(npc)->map_number == 3 && w.cells_consistent());
        w.teleport(npc, 5500, 5000, 0, 3);
        CHECK(w.cells_consistent());
        w.teleport(npc, 5510, 5010, 0, 0);
        CHECK(w.get(npc)->off_map && w.cells_consistent());
        CHECK(w.cell(5500 >> World::kCellShift, 5000 >> World::kCellShift).empty());
        w.teleport(npc, 0, 0, 0, 3);
        CHECK(!w.get(npc)->off_map && w.cells_consistent());
        w.destroy(npc);
        CHECK(!w.get(npc) && w.cells_consistent());
    }
}

void test_pending_stack() {
    World w(test_typeflags());
    // DAMAGE.FLX layout: shape 3 has 50 hit points; absent entries give 1.
    std::vector<std::uint8_t> damage(16 * 6);
    damage[3 * 6] = 1;
    damage[3 * 6 + 5] = 50;
    w.set_damage_table(damage);

    CHECK(w.create(0x800, 0) == 0);
    const ObjId a = w.create(3, 2);
    CHECK(a != 0 && w.pending_top() == a);
    CHECK(w.get(a)->flags & kItemEthereal);
    CHECK(w.get(a)->hit_points == 50);
    CHECK(w.pop_to_coords(100, 200, 300) == a);
    CHECK(w.pending_count() == 0);
    CHECK(w.get(a)->x == 100 && w.get(a)->y == 200 && w.get(a)->z == 250);  // z capped
    CHECK(!(w.get(a)->flags & kItemEthereal));

    // Conveyor passengers are pushed before their successful legal move.
    // Finishing it must put the passenger back, leaving older pending items.
    const ObjId waiting = w.create(5, 0);
    w.push(a);
    SweepResult moved;
    moved.end = {120, 220, 24};
    finish_move(w, a, moved);
    CHECK(w.get(a)->x == 120 && w.get(a)->y == 220 && w.get(a)->z == 24);
    CHECK(!(w.get(a)->flags & kItemEthereal));
    CHECK(w.pending_count() == 1 && w.pending_top() == waiting && w.cells_consistent());
    // An ethereal item below the top must not pop a different item.
    w.push(a);
    w.move_item(waiting, 300, 400, 0);
    CHECK(w.pending_count() == 2 && w.pending_top() == a);
    CHECK(w.pop() == a);
    w.destroy(waiting);

    Item box;
    box.shape = 6;
    box.x = 1000;
    box.y = 2000;
    box.z = 16;
    const ObjId c = w.add(box).id;
    const ObjId b = w.create(4, 0);
    CHECK(w.get(b)->hit_points == 1);
    w.push(a);
    CHECK(w.pending_count() == 2 && w.pending_top() == a);
    CHECK(w.pop_to_container(c) == a);
    CHECK(w.pop_to_container(c) == b);  // goes first
    CHECK((w.get(c)->contents == std::vector<ObjId>{b, a}));
    CHECK(w.get(a)->parent == c && w.get(a)->x == c && (w.get(a)->flags & kItemContained));
    const auto loc = w.location(*w.get(a));
    CHECK(loc.x == 1000 && loc.y == 2000 && loc.z == 16);

    // push/pop puts a contained item back into its container.
    w.push(b);
    CHECK(w.get(c)->contents == std::vector<ObjId>{a});
    CHECK(w.pop() == b);
    CHECK((w.get(c)->contents == std::vector<ObjId>{b, a}));
    w.push(b);
    CHECK(w.pop_to_end(c) == b);
    CHECK((w.get(c)->contents == std::vector<ObjId>{a, b}));

    // Not a container: popped where it is.
    const ObjId d = w.create(5, 0);
    CHECK(w.pop_to_container(a) == d);
    CHECK(w.get(d)->parent == 0 && w.get(d)->x == 1000 && w.get(d)->z == 16);

    // Destroying a pending item removes it from the stack.
    const ObjId e = w.create(5, 0);
    w.destroy(e);
    CHECK(w.pending_count() == 0 && w.pop() == 0);
}

void test_position_intrinsics() {
    World w(test_typeflags());
    Item box;
    box.shape = 6;
    box.x = 1000;
    box.y = 2000;
    box.z = 16;
    const ObjId c = w.add(box).id;
    const ObjId a = w.create(5, 0);
    w.pop_to_container(c);
    // push this; calli 4, n; add_sp 4; push_retval; word_to_dword; pop_res; ret
    auto get = [&](ObjId id, std::uint16_t n) {
        return run_on_item(w, id, {0x40, 0x06, 0x0F, 0x04, static_cast<std::uint8_t>(n),
                                   static_cast<std::uint8_t>(n >> 8), 0x6E, 0xFC, 0x5E, 0x60, 0x08, 0x50});
    };
    CHECK(get(a, 0x013) == 1000 && get(a, 0x014) == 2000 && get(a, 0x009) == 16);  // outermost container
    CHECK(get(c, 0x120) == 1000 - 16);  // getCX: x - footpad_x * 8
    CHECK(get(c, 0x11E) == 2000 - 24);  // getCY
    CHECK(get(c, 0x11F) == 16 + 16);    // getCZ: z + footpad_z * 4
}

void test_fast_area() {
    // Camera in cell (10, 4): s = 14, d = 6.
    const FastArea area = FastArea::around(10 * 512 + 100, 4 * 512 + 7);
    CHECK((area == FastArea{9, 19, 3, 9}));
    CHECK(area.contains_cell(10, 4) && area.contains_cell(13, 4) && area.contains_cell(10, 1));
    CHECK(!area.contains_cell(16, 4) && !area.contains_cell(10, 0) && !area.contains_cell(-1, 0));

    std::vector<ShapeInfo> tf = test_typeflags();
    tf[8].flags = ShapeInfo::kNoisy;  // has fast-area usecode events
    World w(tf);
    std::vector<std::pair<ObjId, std::uint16_t>> events;
    std::vector<ObjId> failed;
    WorldHooks hooks;
    hooks.start_event = [&](ObjId id, std::uint16_t ev) -> std::uint16_t {
        events.emplace_back(id, ev);
        return 0;
    };
    hooks.fail_processes = [&](ObjId id) { failed.push_back(id); };
    w.set_hooks(hooks);
    auto place = [&](std::uint16_t shape, std::int32_t cx, std::int32_t cy, std::uint16_t flags = 0) {
        Item i;
        i.shape = shape;
        i.x = cx * 512 + 10;
        i.y = cy * 512 + 10;
        i.flags = flags;
        return w.add(i).id;
    };
    const ObjId near_item = place(8, 10, 4);
    const ObjId plain = place(5, 11, 4);
    const ObjId far_item = place(8, 30, 4);
    const ObjId disposable = place(5, 10, 4, kItemDisposable);

    w.update_fast_area(10 * 512, 4 * 512);
    CHECK(w.get(near_item)->flags & kItemFastArea);
    CHECK(w.get(plain)->flags & kItemFastArea);
    CHECK(!(w.get(far_item)->flags & kItemFastArea));
    CHECK(!(w.get(disposable)->flags & kItemFastArea));  // glob items never enter
    CHECK((events == std::vector<std::pair<ObjId, std::uint16_t>>{{near_item, std::uint16_t{0x0F}}}));

    // Moving far away: items leave, disposable ones are destroyed.
    events.clear();
    w.update_fast_area(30 * 512, 4 * 512);
    CHECK(!(w.get(near_item)->flags & kItemFastArea) && !(w.get(plain)->flags & kItemFastArea));
    CHECK(w.get(disposable) == nullptr);
    CHECK(w.get(far_item)->flags & kItemFastArea);
    CHECK(std::ranges::count(failed, near_item) == 1 && std::ranges::count(failed, plain) == 1);
    CHECK(std::ranges::count(events, std::pair<ObjId, std::uint16_t>{near_item, std::uint16_t{0x10}}) == 1);
    CHECK(std::ranges::count(events, std::pair<ObjId, std::uint16_t>{far_item, std::uint16_t{0x0F}}) == 1);

    // Popping into the fast area enters it.
    const ObjId created = w.create(5, 0);
    w.pop_to_coords(30 * 512, 4 * 512, 0);
    CHECK(w.get(created)->flags & kItemFastArea);
}

void test_item_search() {
    std::vector<ShapeInfo> tf = test_typeflags();
    tf[9].family = 3;
    World w(tf);
    auto place = [&](std::uint16_t shape, std::int32_t x, std::int32_t y, std::uint8_t frame = 0) {
        const ObjId id = w.create(shape, frame);
        w.pop_to_coords(x, y, 0);
        return id;
    };
    // Cells are 512 units: (700, 700) is in cell (1, 1).
    const ObjId centre = place(5, 700, 700);
    const ObjId a = place(9, 800, 700, 2);  // same cell as the centre
    const ObjId b = place(5, 700, 750, 3);  // same cell, popped later: chain head
    const ObjId c = place(9, 1600, 700);    // cell (3, 1)
    const ObjId d = place(9, 600, 1600);    // cell (1, 3)
    place(9, 2000, 700);                    // out of range (x > 700 + 1000)
    CHECK(w.cells_consistent());
    CHECK((std::vector<ObjId>(w.cell(1, 1).begin(), w.cell(1, 1).end()) == std::vector<ObjId>{b, a, centre}));

    // "$": everything in range, cell by cell (x inner, y outer), chain order.
    const std::uint8_t all[] = {'$'};
    CHECK((w.area_search(centre, 1000, all) == std::vector<ObjId>{b, a, centre, c, d}));
    // "@ %9 = $": shape == 9.
    const std::uint8_t shape9[] = {'@', '%', 9, 0, '=', '$'};
    CHECK((w.area_search(centre, 1000, shape9) == std::vector<ObjId>{a, c, d}));
    // ": %3 = ` %2 = & $": family 3 and frame 2.
    const std::uint8_t fam_frame[] = {':', '%', 3, 0, '=', '`', '%', 2, 0, '=', '&', '$'};
    CHECK((w.area_search(centre, 1000, fam_frame) == std::vector<ObjId>{a}));
    // "B 5 9 $": shape in {5, 9}; "! $" on true gives ~1 (still true).
    const std::uint8_t list[] = {'B', 5, 0, 9, 0, '$'};
    CHECK(w.matches_loop_script(*w.get(a), list) && w.matches_loop_script(*w.get(b), list));
    const std::uint8_t not_true[] = {0x01, '!', '$'};
    CHECK(w.matches_loop_script(*w.get(a), not_true));
    const std::uint8_t less[] = {'`', '%', 3, 0, '<', '$'};  // frame < 3
    CHECK(w.matches_loop_script(*w.get(a), less) && !w.matches_loop_script(*w.get(b), less));

    // Moving items keeps the cells right.
    w.push(a);
    CHECK(w.cells_consistent());
    w.destroy(c);
    CHECK(w.cells_consistent());
    CHECK((w.area_search(centre, 1000, all) == std::vector<ObjId>{b, centre, d}));

    // Container search: contents in order.
    Item box;
    box.shape = 6;
    box.x = 1000;
    box.y = 1000;
    const ObjId container = w.add(box).id;
    w.pop_to_container(container);  // a
    CHECK((w.container_search(container, all) == std::vector<ObjId>{a}));
}

void test_surface_search() {
    std::vector<ShapeInfo> tf = test_typeflags();
    // Shape 10: 4 x 4 footprint, 2 high (16 z units). Shape 11: 2 x 2, flat, not fixed (height 1).
    tf[10].footpad_x = 4;
    tf[10].footpad_y = 4;
    tf[10].footpad_z = 2;
    tf[11].footpad_x = 2;
    tf[11].footpad_y = 2;
    World w(tf);
    auto place = [&](std::uint16_t shape, std::int32_t x, std::int32_t y, std::int32_t z) {
        const ObjId id = w.create(shape, 0);
        w.pop_to_coords(x, y, z);
        return id;
    };
    const ObjId table = place(10, 1000, 1000, 8);       // x 937..1000, z 8..24
    const ObjId on_top = place(11, 990, 990, 24);       // rests on it
    const ObjId beside = place(11, 1100, 1000, 24);     // same height, footprint apart
    const ObjId floor = place(11, 1000, 1000, 7);       // its top (7 + 1) is the table's bottom
    const ObjId far_below = place(11, 1000, 1000, 0);
    const std::uint8_t all[] = {'$'};
    const std::uint8_t any_shape11[] = {'@', '%', 11, 0, '=', '$', 0};  // 7 bytes: not a fast path
    CHECK((w.surface_search(table, 1, all) == std::vector<ObjId>{on_top}));
    CHECK((w.surface_search(table, 2, all) == std::vector<ObjId>{floor}));
    CHECK((w.surface_search(table, 3, any_shape11) == std::vector<ObjId>{floor, on_top}));
    (void)beside;
    (void)far_below;
}

// Intrinsic `n` on item `a` with the object id `b`: push b; push this;
// calli 6, n; add_sp 6; push_retval; word_to_dword; pop_res; ret
std::uint32_t pair_intrinsic(World& w, std::uint16_t n, ObjId a, ObjId b) {
    return run_on_item(w, a, {0x0B, static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(b >> 8), 0x40, 0x06, 0x0F,
                              0x06, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8), 0x6E, 0xFA, 0x5E,
                              0x60, 0x08, 0x50});
}

// Footprint tests (Item::I_isOn and its aliases, isCompletelyOn, isCentreOn,
// overlaps): boxes x - fx*16 .. x, open intervals in x and y.
void test_footprint_intrinsics() {
    std::vector<ShapeInfo> tf = test_typeflags();
    tf[10].footpad_x = 4;  // 64 x 64, 16 high
    tf[10].footpad_y = 4;
    tf[10].footpad_z = 2;
    tf[11].footpad_x = 2;  // 32 x 32, flat
    tf[11].footpad_y = 2;
    tf[12].footpad_x = 4;  // 64 x 16, 8 high
    tf[12].footpad_y = 1;
    tf[12].footpad_z = 1;
    World w(tf);
    auto place = [&](std::uint16_t shape, std::int32_t x, std::int32_t y, std::int32_t z) {
        const ObjId id = w.create(shape, 0);
        w.pop_to_coords(x, y, z);
        return id;
    };
    auto test = [&](std::uint16_t n, ObjId a, ObjId b) { return pair_intrinsic(w, n, a, b); };
    const ObjId table = place(10, 1000, 1000, 8);    // x and y 936..1000, z 8..24
    const ObjId on_top = place(11, 990, 990, 24);    // inside, on top
    const ObjId edge = place(11, 1030, 1000, 24);    // x 998..1030: across the edge
    const ObjId touching = place(11, 1032, 1000, 24);  // x 1000..1032: only touches
    const ObjId inside = place(11, 980, 980, 10);    // within the table's height
    CHECK(test(0x046, on_top, table) == 1 && test(0x126, edge, table) == 1 && test(0x046, touching, table) == 0);
    CHECK(test(0x046, inside, table) == 0 && test(0x046, table, on_top) == 0);
    CHECK(test(0x02B, on_top, table) == 1 && test(0x02B, edge, table) == 0);
    CHECK(test(0x05A, on_top, table) == 1 && test(0x05A, edge, table) == 0);  // centre x 1014
    CHECK(test(0x0A5, table, inside) == 1 && test(0x0A5, inside, table) == 1 && test(0x0A5, table, on_top) == 0);
    // A flipped item swaps its x and y extents.
    const ObjId bar = place(12, 1000, 1300, 0);  // x 936..1000, y 1284..1300
    const ObjId flipped = place(12, 1000, 1100, 0);
    w.get(flipped)->flags |= kItemFlipped;       // x 984..1000, y 1036..1100
    CHECK(test(0x046, place(11, 980, 1290, 8), bar) == 1);
    CHECK(test(0x046, place(11, 980, 1090, 8), flipped) == 0);
    CHECK(test(0x046, place(11, 1010, 1090, 8), flipped) == 1);
}

void test_collisions() {
    std::vector<ShapeInfo> tf(32);
    auto shape = [&](int s, int fx, int fy, int fz, bool fixed) {
        tf[s].footpad_x = static_cast<std::uint8_t>(fx);
        tf[s].footpad_y = static_cast<std::uint8_t>(fy);
        tf[s].footpad_z = static_cast<std::uint8_t>(fz);
        tf[s].flags = ShapeInfo::kSolid | (fixed ? ShapeInfo::kFixed : 0u);
    };
    shape(20, 4, 4, 0, true);   // floor tile, 64 x 64, flat
    shape(21, 1, 1, 4, false);  // mover, 16 x 16 x 32
    shape(22, 1, 4, 8, true);   // wall, 16 x 64 x 64
    shape(23, 2, 2, 1, true);   // step, 32 x 32 x 8
    World w(tf);
    auto place = [&](std::uint16_t s, std::int32_t x, std::int32_t y, std::int32_t z) {
        const ObjId id = w.create(s, 0);
        w.pop_to_coords(x, y, z);
        return id;
    };
    for (int x = 63; x < 512; x += 64) {
        for (int y = 63; y < 512; y += 64) place(20, x, y, 0);
    }
    w.set_camera(300, 300, 0);

    // A wall stops the box right before it.
    const ObjId m = place(21, 100, 100, 0);
    const ObjId wall = place(22, 140, 100, 0);  // x 125..140
    CHECK(try_move(w, m, {120, 100, 0}, kSweepSupportOnly | kSweepStepUp).ok);
    SweepResult r = try_move(w, m, {140, 100, 0}, kSweepSupportOnly | kSweepStepUp);
    CHECK(!r.ok && r.blocker == wall && r.end.x == 124 && r.axes == 1);
    // Without ground check the same move is still blocked.
    CHECK(!sweep(w, 21, m, {100, 100, 0}, {140, 100, 0}, kSweepNoGround).ok);

    // A step of 8 is climbed.
    const ObjId m2 = place(21, 300, 100, 0);
    place(23, 340, 108, 0);  // x 309..340, top at 8
    r = try_move(w, m2, {320, 100, 0}, kSweepSupportOnly | kSweepStepUp);
    CHECK(r.ok && r.end.z == 8);
    // Without kSweepStepUp it blocks.
    CHECK(!try_move(w, m2, {320, 100, 0}, kSweepSupportOnly).ok);

    // No floor beyond x 511: no support, refused.
    const ObjId m3 = place(21, 500, 300, 0);
    CHECK(try_move(w, m3, {505, 300, 0}, kSweepSupportOnly | kSweepStepUp).ok);  // box still on the floor
    CHECK(!try_move(w, m3, {540, 300, 0}, kSweepSupportOnly | kSweepStepUp).ok);
    CHECK(sweep(w, 21, m3, {500, 300, 0}, {540, 300, 0}, kSweepNoGround).ok);

    // Walking off a step: the ground up to 9 below is followed down, straight
    // down from the end point (ITEMFIND_10e8_162f passes param_4 as the
    // start of the second sweep).
    const ObjId m4 = place(21, 330, 200, 8);
    place(23, 330, 200, 0);  // under it, x 299..330
    r = try_move(w, m4, {380, 200, 8}, kSweepSupportOnly | kSweepStepUp);
    CHECK(r.ok && r.end.z == 0);
    // Down a staircase (SAVEGAME TEST9): the box still overlaps the step it
    // leaves; a drop from the start would cross that step and leave the box
    // 8 above the next one, where it would fall.
    place(23, 330, 400, 8);  // upper step, x 299..330, top at 16
    place(23, 362, 400, 0);  // lower step, x 331..362, top at 8
    const ObjId m5 = place(21, 335, 400, 16);
    r = try_move(w, m5, {350, 400, 16}, kSweepSupportOnly | kSweepStepUp);
    CHECK(r.ok && r.end.z == 8);

    // Free positions (ITEMFIND.C 00D9): the wall's box blocks, the flat
    // floor never does, nor does the space above the wall; the item standing
    // there counts too, unless ignored; a box reaching below x 0 is never free.
    ObjId first = 0;
    CHECK(!position_free(w, 21, {130, 90, 0}, &first) && first == wall);
    CHECK(position_free(w, 21, {130, 90, 64}, &first) && first == 0);
    CHECK(position_free(w, 21, {200, 300, 0}));
    CHECK(!position_free(w, 21, {100, 100, 0}, &first) && first == m);
    CHECK(position_free(w, 21, {100, 100, 0}, nullptr, m));
    CHECK(!position_free(w, 21, {10, 300, 0}));
    CHECK(!box_free(w, 16, 16, 1, 0x8001, {140, 100, 63}) && box_free(w, 16, 16, 1, 0x8000, {140, 100, 63}));
}

// ATTACK.C 1118:08E4: the path finder tries each direction with a test walk
// of a whole half stride (two frames here). East, the first frame is clear and
// the second meets a wall: the original turns aside, north or south. The port
// tried one frame and walked east into the wall.
void test_pathfinder_test_walk() {
    constexpr std::size_t table = AnimDat::kShapes * 4, action = table + AnimDat::kActions * 4;
    std::vector<std::uint8_t> bytes(action + 4 + 8 * 4 * 8);
    auto u32 = [&](std::size_t at, std::size_t value) {
        for (int k = 0; k < 4; ++k) bytes[at + k] = static_cast<std::uint8_t>(value >> (k * 8));
    };
    u32(4, table);
    u32(table + 4, action);
    bytes[action] = 4;
    bytes[action + 1] = 1;
    bytes[action + 2] = 1;
    for (std::size_t f = 0; f < 8 * 4; ++f) {
        bytes[action + 4 + f * 8 + 4] = 4;  // distance 4 * 2 = 8
        bytes[action + 4 + f * 8 + 5] = 2;  // ground check
    }
    const AnimDat anims = AnimDat::parse(bytes);
    std::vector<ShapeInfo> tf(4);
    tf[1].flags = ShapeInfo::kSolid | ShapeInfo::kNpc;
    tf[1].footpad_x = tf[1].footpad_y = tf[1].footpad_z = 1;
    tf[2].flags = tf[3].flags = ShapeInfo::kSolid | ShapeInfo::kFixed;
    tf[2].footpad_x = tf[2].footpad_y = 16;
    tf[3].footpad_x = 1;
    tf[3].footpad_y = 16;
    tf[3].footpad_z = 8;
    World w(tf);
    w.create(2, 0);
    w.pop_to_coords(255, 255, 0);
    w.create(3, 0);
    w.pop_to_coords(125, 255, 0);
    Item npc;
    npc.id = 2;
    npc.shape = 1;
    npc.x = npc.y = 100;
    w.add_with_id(npc);
    w.set_camera(100, 100, 0);
    w.state.controlled_npc = 0;
    Kernel k;
    walk_to(k, w, anims, 2, 0, {200, 100, 0}, 10, 0, false);
    int dir = -1;
    for (int f = 0; f < 3 && dir < 0; ++f) {
        k.advance_time(16667);
        k.run_slice();
        k.for_each_process([&](const Process& p) {
            if (const auto* a = dynamic_cast<const ActorAnimProcess*>(&p); a && a->owner() == 2 && a->action() == 1)
                dir = a->direction();
        });
    }
    std::printf("path finder test walk: first walk towards %d\n", dir);
    CHECK(dir == 0 || dir == 8);
}

// TYPE_1108_0708, activity 3 (walk): an NPC between two walls, blocked ahead
// then behind, takes activity 7 for shapes 0x2F5, 0x2F6, 0x2F7, 0x344, 0x597
// and attacks (5) otherwise. The port had it the other way round.
void test_walk_activity_blocked() {
    constexpr std::size_t table = AnimDat::kShapes * 4, action = table + AnimDat::kActions * 4;
    std::vector<std::uint8_t> bytes(action + 4 + 8 * 4 * 8);
    auto u32 = [&](std::size_t at, std::size_t value) {
        for (int k = 0; k < 4; ++k) bytes[at + k] = static_cast<std::uint8_t>(value >> (k * 8));
    };
    u32(4 * 1, table);
    u32(4 * 0x2F5, table);
    u32(table + 4, action);
    bytes[action] = 4;
    bytes[action + 1] = 1;
    bytes[action + 2] = 1;
    for (std::size_t f = 0; f < 8 * 4; ++f) {
        bytes[action + 4 + f * 8 + 4] = 4;  // distance 4 * 2 = 8
        bytes[action + 4 + f * 8 + 5] = 2;  // ground check
    }
    const AnimDat anims = AnimDat::parse(bytes);
    for (const std::uint16_t shape : {std::uint16_t{1}, std::uint16_t{0x2F5}}) {
        std::vector<ShapeInfo> tf(0x300);
        tf[shape].flags = ShapeInfo::kSolid | ShapeInfo::kNpc;
        tf[shape].footpad_x = tf[shape].footpad_y = tf[shape].footpad_z = 1;
        tf[2].flags = tf[3].flags = ShapeInfo::kSolid | ShapeInfo::kFixed;
        tf[2].footpad_x = tf[2].footpad_y = 16;
        tf[3].footpad_x = 1;
        tf[3].footpad_y = 16;
        tf[3].footpad_z = 8;
        World w(tf);
        w.create(2, 0);
        w.pop_to_coords(255, 255, 0);
        w.create(3, 0);
        w.pop_to_coords(112, 255, 0);  // east wall, x 97..112
        w.create(3, 0);
        w.pop_to_coords(80, 255, 0);   // west wall, x 65..80
        Item npc;
        npc.id = 2;
        npc.shape = shape;
        npc.x = npc.y = 96;            // x 81..96, 1 unit from each wall
        w.add_with_id(npc);
        w.set_camera(96, 96, 0);
        w.state.controlled_npc = 0;
        w.npc_record(2)[npc::kDirection] = 4;
        Kernel k;
        set_activity(k, w, anims, 2, 3);
        CHECK(w.npc_word(2, npc::kActivity) == 3);
        int activity = 3;
        for (int f = 0; f < 600 && activity == 3; ++f) {
            k.advance_time(16667);
            k.run_slice();
            activity = w.npc_word(2, npc::kActivity);
        }
        std::printf("walk activity: shape 0x%X between two walls, activity %d\n", shape, activity);
        CHECK(activity == (shape == 0x2F5 ? 7 : 5));
    }
}

void test_walk_alignment() {
    // A two-frame half stride: the first step is clear, the second meets the
    // doorpost. A full-stride probe must choose the free side before walking.
    constexpr std::size_t table = AnimDat::kShapes * 4, action = table + AnimDat::kActions * 4;
    std::vector<std::uint8_t> bytes(action + 4 + 8 * 4 * 8);
    auto u32 = [&](std::size_t at, std::size_t value) {
        for (int b = 0; b < 4; ++b) bytes[at + b] = static_cast<std::uint8_t>(value >> (b * 8));
    };
    u32(4, table);
    u32(table + 4, action);
    bytes[action] = 4;
    bytes[action + 1] = 1;
    bytes[action + 2] = 1;
    for (std::size_t f = 0; f < 8 * 4; ++f) {
        bytes[action + 4 + f * 8 + 4] = 4;  // distance 4 * 2 = 8
        bytes[action + 4 + f * 8 + 5] = 2;  // ground check
    }
    const AnimDat anims = AnimDat::parse(bytes);
    for (bool closed : {false, true}) {
        std::vector<ShapeInfo> tf(4);
        tf[1].flags = ShapeInfo::kSolid | ShapeInfo::kNpc;
        tf[1].footpad_x = tf[1].footpad_y = tf[1].footpad_z = 1;
        tf[2].flags = tf[3].flags = ShapeInfo::kSolid | ShapeInfo::kFixed;
        tf[2].footpad_x = tf[2].footpad_y = 16;
        tf[3].footpad_x = 1;
        tf[3].footpad_y = closed ? 16 : 4;
        tf[3].footpad_z = 8;
        World w(tf);
        w.create(2, 0);
        w.pop_to_coords(255, 255, 0);
        w.create(3, 0);
        w.pop_to_coords(125, closed ? 255 : 100, 0);
        Item player;
        player.id = player.shape = 1;
        player.x = player.y = 100;
        w.add_with_id(player);
        w.set_camera(100, 100, 0);
        w.state.controlled_npc = 1;
        Kernel k;
        do_anim(k, w, anims, 1, 1, 4);
        for (int f = 0; f < 10; ++f) {
            k.advance_time(16667);
            k.run_slice();
        }
        CHECK(w.get(1));
        std::printf("walk alignment: closed %d, at %d %d\n", closed, w.get(1)->x, w.get(1)->y);
        // Closed: the walk fits nowhere, it becomes standing (action 0) and
        // the player stays (ANIMPRIM.C 016F).
        CHECK(w.get(1)->x == (closed ? 100 : 116));
        CHECK(w.get(1)->y == (closed ? 100 : 116));
        if (closed) CHECK(w.npc_word(1, npc::kLastAnim) == 0);
        CHECK(position_free(w, 1, w.location(*w.get(1)), nullptr, 1));
    }
}

void test_scripted_camera() {
    World w;
    w.state.controlled_npc = w.state.camera_target = 1;
    w.set_follow_player(true);
    w.set_camera(1000, 1000, 0);
    w.track_camera_target(1100, 1100, 0);
    for (int f = 0; f < 10; ++f) w.update_follow_camera();
    CHECK(w.state.camera_x != 1000 || w.state.camera_y != 1000);
    w.move_camera_to(3000, 4000, 0, 0);
    for (int f = 0; f < 100; ++f) w.update_follow_camera();
    CHECK(w.state.camera_x == 3000 && w.state.camera_y == 4000);
    // The next player move resumes following, but an unfollowed camera stays put.
    w.track_camera_target(1100, 1100, 0);
    w.state.camera_target = 0;
    w.update_follow_camera();
    CHECK(w.state.camera_x == 3000 && w.state.camera_y == 4000);
    w.state.camera_target = 1;
    w.update_follow_camera();
    CHECK(w.state.camera_x != 3000 || w.state.camera_y != 4000);
}

void test_support_and_fall() {
    std::vector<ShapeInfo> tf(32);
    tf[20].footpad_x = 4;  // platform 64 x 64, flat, fixed, solid
    tf[20].footpad_y = 4;
    tf[20].flags = ShapeInfo::kSolid | ShapeInfo::kFixed;
    tf[21].footpad_x = 1;  // crate 16 x 16 x 16, solid, heavy
    tf[21].footpad_y = 1;
    tf[21].footpad_z = 2;
    tf[21].flags = ShapeInfo::kSolid;
    tf[21].weight = 10;
    World w(tf);
    auto place = [&](std::uint16_t s, std::int32_t x, std::int32_t y, std::int32_t z) {
        const ObjId id = w.create(s, 0);
        w.pop_to_coords(x, y, z);
        return id;
    };
    place(20, 100, 100, 40);  // x, y 37..100, top at 40
    w.set_camera(300, 300, 0);

    // Fully on the platform: all quarters, standing.
    CHECK(support_mask(w, 21, 90, 90, 40, true, 0) == 0x1F);
    // Half over the edge (x 93..108, middle at 100): only the low-x quarters.
    CHECK(support_mask(w, 21, 108, 90, 40, true, 0) == 0x0C);
    // Beside it: nothing; on the ground (z 0): everything.
    CHECK(support_mask(w, 21, 130, 90, 40, true, 0) == 0);
    CHECK(support_mask(w, 21, 130, 90, 0, true, 0) == 0x1F);
    // Within 9 above the platform with slack, but only if some support is exact.
    CHECK(support_mask(w, 21, 90, 90, 45, true, 0) == 0);

    // A crate dropped beside the platform at height 40 falls to the ground.
    Kernel k;
    const ObjId crate = place(21, 130, 90, 40);
    const Pid g = fall(k, w, crate);
    CHECK(g != 0);
    for (int i = 0; i < 200 && k.process_count(); ++i) {
        k.advance_time(1'000'000 / 60);
        k.run_slice();
    }
    CHECK(w.get(crate)->z == 0 && k.process_count() == 0);
    CHECK(w.cells_consistent());
}

// The gravity step (1040:06D2) beyond a plain fall.
void test_gravity_step() {
    std::vector<ShapeInfo> tf(32);
    tf[20].footpad_x = 4;  // platform 64 x 64, flat, fixed, solid
    tf[20].footpad_y = 4;
    tf[20].flags = ShapeInfo::kSolid | ShapeInfo::kFixed;
    tf[21].footpad_x = 1;  // crate 16 x 16 x 16, solid, heavy
    tf[21].footpad_y = 1;
    tf[21].footpad_z = 2;
    tf[21].flags = ShapeInfo::kSolid;
    tf[21].weight = 10;
    tf[22].footpad_x = 1;  // wall 16 x 64 x 64, fixed, solid
    tf[22].footpad_y = 4;
    tf[22].footpad_z = 8;
    tf[22].flags = ShapeInfo::kSolid | ShapeInfo::kFixed;
    auto place = [](World& w, std::uint16_t s, std::int32_t x, std::int32_t y, std::int32_t z) {
        const ObjId id = w.create(s, 0);
        w.pop_to_coords(x, y, z);
        return id;
    };
    auto run = [](Kernel& k, std::uint32_t ticks_per_slice, int slices) {
        for (int i = 0; i < slices && k.process_count(); ++i) {
            k.advance_time((ticks_per_slice * 1'000'000 + Kernel::kTicksPerSecond - 1) / Kernel::kTicksPerSecond);
            k.run_slice();
        }
    };

    // A crate landing half over the platform's edge (low-x quarters only)
    // stays there; the controlled one is pushed off it and falls to the ground.
    for (const bool controlled : {false, true}) {
        World w(tf);
        w.set_camera(300, 300, 0);
        place(w, 20, 100, 100, 40);
        const ObjId crate = place(w, 21, 108, 90, 48);
        if (controlled) w.state.controlled_npc = crate;
        Kernel k;
        fall(k, w, crate);
        run(k, 1, 600);
        const Item& c = *w.get(crate);
        std::printf("gravity: %s crate over the edge ends at (%d, %d, %d), bouncing %d\n",
                    controlled ? "controlled" : "plain", c.x, c.y, c.z, (c.flags & kItemBouncing) != 0);
        CHECK(k.process_count() == 0 && (c.flags & kItemBouncing));
        if (controlled) CHECK(c.z == 0 && c.x >= 116);
        else CHECK(c.z == 40 && c.x == 108 && c.y == 90);
        CHECK(w.cells_consistent());
    }

    // Thrown at a wall: three quarters of the speed lost, then sent back.
    {
        World w(tf);
        w.set_camera(300, 300, 0);
        place(w, 22, 140, 120, 0);  // x 125..140
        const ObjId crate = place(w, 21, 100, 100, 0);
        Kernel k;
        hurl(k, w, crate, 16, 0, 8, 2);
        int max_x = 0;
        for (int i = 0; i < 600 && k.process_count(); ++i) {
            k.advance_time(1'000'000 / Kernel::kTicksPerSecond);
            k.run_slice();
            max_x = std::max(max_x, static_cast<int>(w.get(crate)->x));
        }
        const Item& c = *w.get(crate);
        std::printf("gravity: crate thrown at the wall reaches x %d, ends at (%d, %d, %d)\n", max_x, c.x, c.y, c.z);
        CHECK(max_x == 124 && c.x < 124 && c.z == 0 && (c.flags & kItemBouncing) && k.process_count() == 0);
    }

    // A new fall clears the "bouncing" flag that the last landing left
    // (1040:0395): a crate thrown up from the ground does not drop the crate
    // resting on it at its first step, as one still bouncing would.
    {
        World w(tf);
        w.set_camera(300, 300, 0);
        const ObjId crate = place(w, 21, 200, 200, 0);
        const ObjId top = place(w, 21, 200, 200, 16);
        w.get(crate)->flags |= kItemBouncing;  // as after a landing
        Kernel k;
        hurl(k, w, crate, 0, 0, 8, 2);
        const bool cleared = !(w.get(crate)->flags & kItemBouncing);
        run(k, 1, 21);  // its first step, after the 20 ticks of sleep
        const bool top_falls = k.find_process(top, GravityProcess::kType) != nullptr;
        std::printf("gravity: re-thrown crate bouncing flag cleared %d, crate on top falling %d\n", cleared, top_falls);
        CHECK(cleared && !top_falls);
    }

    // Slices 4 ticks apart (DS:3A88): each step goes 4 times as far. From z
    // 100 at ticks 20, 36 and 52: 0, then -2 * 4 * 4, then -4 * 4 * 4.
    {
        World w(tf);
        w.set_camera(300, 300, 0);
        const ObjId crate = place(w, 21, 200, 200, 100);
        Kernel k;
        fall(k, w, crate);
        run(k, 4, 13);  // up to tick 52
        std::printf("gravity: 4-tick slices, z %d at tick %u\n", w.get(crate)->z, k.ticks());
        CHECK(k.ticks() == 52 && k.slice_ticks() == 4 && w.get(crate)->z == 4);
    }
}

void test_on_screen() {
    World w(test_typeflags());
    // 20 x 10 frames anchored at (10, 9).
    w.set_frame_lookup([](std::uint16_t, std::uint16_t) { return std::optional<FrameBox>{{20, 10, 10, 9}}; });
    w.set_camera(4000, 4000, 0);
    CHECK(w.state.view_left == -320 && w.state.view_top == 2000 - 300);
    Item i;
    i.shape = 5;
    i.x = 4000;
    i.y = 4000;
    const ObjId centre = w.add(i).id;
    CHECK(w.is_partly_on_screen(*w.get(centre)));  // drawn around (320, 300)
    i.x = 6000;
    const ObjId far_item = w.add(i).id;  // 1000 pixels to the right
    CHECK(!w.is_partly_on_screen(*w.get(far_item)));
    // Right edge: x - y = 2 * (639 + 10 - 320) puts the frame's left column on x = 639.
    i.x = 4000 + 2 * (639 + 10 - 320);
    const ObjId edge = w.add(i).id;
    CHECK(w.is_partly_on_screen(*w.get(edge)));
    w.get(edge)->x += 2;
    CHECK(!w.is_partly_on_screen(*w.get(edge)));
}

void test_directions() {
    // 16 directions: 0 towards -y, 4 towards +x, 8 towards +y, 12 towards -x.
    CHECK(direction_16(100, 100, 100, 100) == 16);
    CHECK(direction_16(100, 100, 100, 50) == 0);
    CHECK(direction_16(100, 100, 150, 100) == 4);
    CHECK(direction_16(100, 100, 100, 150) == 8);
    CHECK(direction_16(100, 100, 50, 100) == 12);
    CHECK(direction_16(100, 100, 150, 50) == 2);   // ratio 1
    CHECK(direction_16(100, 100, 150, 140) == 6);  // ratio 0.8 > 3/4
    CHECK(direction_16(100, 100, 150, 130) == 5);  // ratio 0.6
    CHECK(direction_16(100, 100, 150, 112) == 4);  // ratio 0.24 <= 1/4
    CHECK(direction_16(100, 100, 120, 50) == 1);   // ratio 2.5
    CHECK(direction_16(100, 100, 110, 30) == 0);   // ratio 7
    CHECK(direction_16(100, 100, 50, 20) == 15);   // ratio 1.6
    // Range: direction 4 (towards +x) clamped into [6, 10] gives 6; into
    // [12, 14] it is 14 (6 steps away through 0, against 8 for 12); inside, unchanged.
    CHECK(closest_direction_in_range(100, 100, 150, 100, 16, 6, 10) == 6);
    CHECK(closest_direction_in_range(100, 100, 150, 100, 16, 12, 14) == 14);  // 4 is 6 from 14 via 0, 8 from 12
    CHECK(closest_direction_in_range(100, 100, 150, 100, 16, 2, 6) == 4);
    // 8 directions, same scale.
    CHECK(direction_8(100, 100, 100, 100) == 16);
    CHECK(direction_8(100, 100, 100, 50) == 0);
    CHECK(direction_8(100, 100, 150, 100) == 4);
    CHECK(direction_8(100, 100, 150, 150) == 6);
    CHECK(direction_8(100, 100, 50, 50) == 14);
    CHECK(direction_8(100, 100, 150, 120) == 4);  // close to the x axis
}

// The two generators of the executable (core/random.hpp). Expected values
// from CRUSADER.EXE run in the emulator (tools/diffcheck): SYSTEM.C's
// random_next from its start-up state, the library's rand() from 1.
void test_random_range() {
    std::uint32_t state = kRandomStartState;
    for (std::uint16_t expected : {0x2EB4, 0xAF24, 0x7686, 0x9EDE}) CHECK(random_next(state) == expected);
    std::uint32_t crt = kCrtRandStartState;
    for (std::uint16_t expected : {346, 130, 10982}) CHECK(crt_rand(crt) == expected);
    // random_range(0) is 0 and still draws.
    std::uint32_t a = kRandomStartState, b = kRandomStartState;
    CHECK(random_range(a, 0) == 0 && a != kRandomStartState);
    random_next(b);
    CHECK(a == b);
    // rndRange: signed bounds in either order; a span of 0x10000 gives lo.
    bool seen_lo = false, seen_hi = false;
    for (int i = 0; i < 4000; ++i) {
        const int v = random_between(state, 0x200, -0x200);
        CHECK(v >= -0x200 && v <= 0x200);
        seen_lo |= v == -0x200;
        seen_hi |= v == 0x200;
    }
    CHECK(seen_lo && seen_hi);
    CHECK(random_between(state, -0x8000, 0x7FFF) == -0x8000);
    CHECK(random_seed(0x12345678) == 0x12345679);
}


// A world whose slots were never used (new game, map change) written then
// loaded keeps its free lists: array 9 chains the never-used slots too.
// Saves from before that fix, whose chains stop at the first never-used slot,
// get those slots back in id order.
void test_save_fresh_slots() {
    World w;
    for (int i = 0; i < 3; ++i) {
        Item item;
        item.shape = 0x30;
        item.x = 1000 + 32 * i;
        item.y = 1000;
        w.add(std::move(item));
    }
    w.destroy(kFirstItemId + 1);
    const auto path = std::filesystem::temp_directory_path() / "crusader_fresh_slots.flx";
    auto reload = [&](std::vector<std::uint8_t> table) {
        FlexWriter out(2);
        out.add(0, table);
        out.add(1, w.saved_item_lists());
        out.write(path);
        World loaded;
        loaded.load_saved_items(FlexArchive::open(path));
        return loaded;
    };
    auto same_lists = [&](const World& loaded) {
        return loaded.item_count() == w.item_count() && loaded.free_items().ids == w.free_items().ids &&
               loaded.free_npcs().ids == w.free_npcs().ids;
    };
    const auto table = w.saved_item_table();
    const std::size_t array9 = kItemSlots * (2 + 2 + 1 + 2 + 1 + 2 + 2 + 1 + 1);
    auto chain_length = [&](const World::FreeSlots& list) {
        std::size_t n = 0;
        for (ObjId id = list.ids.front(); id != 0 && n <= kItemSlots; ++n)
            id = static_cast<ObjId>(table[array9 + 2 * id] | table[array9 + 2 * id + 1] << 8);
        return n;
    };
    CHECK(chain_length(w.free_items()) == w.free_items().ids.size());
    CHECK(chain_length(w.free_npcs()) == w.free_npcs().ids.size());
    CHECK(same_lists(reload(table)));

    // The old writing: array 9 left at 0 past the first never-used slots.
    auto broken = table;
    for (ObjId id : {ObjId{kFirstItemId + 10}, ObjId{20}}) {
        broken[array9 + 2 * id] = 0;
        broken[array9 + 2 * id + 1] = 0;
    }
    const World repaired = reload(broken);
    CHECK(same_lists(repaired));
    std::printf("fresh slots: %zu items and %zu NPC slots free after reloading, %zu and %zu from a cut chain\n",
                w.free_items().ids.size(), w.free_npcs().ids.size(), repaired.free_items().ids.size(),
                repaired.free_npcs().ids.size());
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    test_typeflag_decode();
    test_world_ids();
    test_create_npc_reuses_a_slot();
    test_created_npc_teleport();
    test_item_intrinsics();
    test_item_cast_argument();
    test_stasis_leaves_combat();
    test_pending_stack();
    test_position_intrinsics();
    test_fast_area();
    test_directions();
    test_on_screen();
    test_item_search();
    test_surface_search();
    test_footprint_intrinsics();
    test_collisions();
    test_walk_alignment();
    test_pathfinder_test_walk();
    test_walk_activity_blocked();
    test_scripted_camera();
    test_support_and_fall();
    test_gravity_step();
    test_gravity_step();
    test_random_range();
    test_save_fresh_slots();
    std::printf(failures ? "%d failure(s)\n" : "all world checks passed\n", failures);
    return failures ? 1 : 0;
}
