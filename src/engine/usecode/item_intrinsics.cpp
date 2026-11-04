#include "engine/usecode/item_intrinsics.hpp"

#include "engine/audio/audio.hpp"
#include "engine/kernel.hpp"
#include "engine/world/activity.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/gravity.hpp"
#include "engine/world/inventory.hpp"
#include "engine/world/render.hpp"
#include "engine/usecode/uc_process.hpp"

#include <array>
#include <functional>
#include <initializer_list>

namespace crusader {

namespace {

using Args = std::span<const std::uint8_t>;

// Arguments are read from the caller's stack top: the item pointer (pushed
// last) comes first, then the other arguments in reverse push order.
std::uint16_t arg16(Args a, std::size_t pos) {
    return pos + 2 <= a.size() ? static_cast<std::uint16_t>(a[pos] | a[pos + 1] << 8) : 0;
}

std::uint32_t arg32(Args a, std::size_t pos) {
    return pos + 4 <= a.size() ? arg16(a, pos) | std::uint32_t{arg16(a, pos + 2)} << 16 : 0;
}

// Item referenced by the pointer at `pos` (dereferences to a 2-byte object id).
Item* item_arg(UCMachine& m, World& w, Args a, std::size_t pos = 0) {
    std::array<std::uint8_t, 2> id{};
    if (!m.dereference(arg32(a, pos), id)) return nullptr;
    return w.get(static_cast<ObjId>(id[0] | id[1] << 8));
}

// Weight of what an item holds (SHAPHAND_1090_0603): each content's weight
// (TYPEFLAG byte 7) and what it holds in turn. Only families 6 and 7 have
// contents (ITEM.C 0DF6). The original skips the item of DS:0F86, which
// nothing sets in this version.
// @orig 1090:0603
int contents_weight(const World& w, const Item& item) {
    const std::uint8_t family = w.family_of(item.shape);
    if (family != kFamilyContainer && family != kFamilyActorContainer) return 0;
    int total = 0;
    for (ObjId id : item.contents) {
        if (const Item* c = w.get(id)) {
            const ShapeInfo* info = w.shape_info(c->shape);
            total += contents_weight(w, *c) + (info ? info->weight : 0);
        }
    }
    return total;
}

// ITEM.C 04AE: the item's own weight plus its contents'.
// @orig 10A8:04AE
int total_weight(const World& w, const Item& item) {
    const ShapeInfo* info = w.shape_info(item.shape);
    return (info ? info->weight : 0) + contents_weight(w, item);
}

// Shapes that drop what rests on them when destroyed: crates (MCRATE,
// DAMNMCR8), luggage (LUGGAGE, DAMNLUGG), steel boxes (SSBOX_EW, SSBOX_NS) and
// 0x1FD / 0x1FE. Listed in Item::I_destroy (10A8:1365).
bool drops_what_rests_on_it(std::uint16_t shape) {
    return shape == 0x432 || shape == 0x5A0 || shape == 0x1FD || shape == 0x1FE || shape == 0x08F ||
           shape == 0x59F || shape == 0x2B3 || shape == 0x2CA;
}

}  // namespace

// Item::I_destroy (10A8:1365): removes the item and its contents. Its
// looping sounds stop first; an NPC's other sounds play on, detached. An NPC
// loses bit 0 of record +0x59 (NPC_10f0_2130). The items resting on a crate
// fall (Item::I_fall, ITEMFIND_10c8_0f73: a surface search above, every item)
// before it goes; the fall only starts here and runs once the crate is gone.
// Not reproduced: shape 0x4FE with a controlled robot (CRUSYS_1060_02da).
// @orig 10A8:1365
void destroy_item(Kernel& kernel, World& w, ObjId id) {
    const Item* item = w.get(id);
    if (!item) return;
    if (Audio* audio = w.audio()) {
        if (is_npc_id(id)) audio->sources.detach(id);
        audio->sources.stop_item_loops(id);
    }
    if (is_npc_id(id)) {
        if (auto r = w.npc_record(id); !r.empty()) r[npc::kAnimFlags] &= 0xFE;
    }
    if (drops_what_rests_on_it(item->shape)) {
        static constexpr std::uint8_t kAll[] = {'$'};
        for (ObjId above : w.surface_search(id, 1, kAll)) {
            if (above != id && w.get(above)) fall(kernel, w, above);
        }
    }
    w.destroy(id);
}

// @orig none: binds the intrinsics, numbered by USECODE/UNKCOFF.DAT
std::vector<std::uint16_t> register_item_intrinsics(UCMachine& m, World& w) {
    std::vector<std::uint16_t> bound;
    // Usecode events and process cleanup requested by the world.
    w.set_hooks({
        [&m, &w](ObjId id, std::uint16_t event) -> std::uint16_t {
            const Item* item = w.get(id);
            return item ? m.start_event(w.usecode_class(*item), event, id) : 0;
        },
        [&m](ObjId id) { m.kernel().fail_processes(id, Kernel::kAnyType, 1); },
        [&m, &w](ObjId id, std::uint16_t event, std::span<const std::uint8_t> args) {
            if (const Item* item = w.get(id)) m.start_event(w.usecode_class(*item), event, id, args);
        },
    });
    {
        WorldHooks hooks = w.hooks();
        hooks.has_event = [&m, &w](ObjId id, std::uint16_t event) {
            const Item* item = w.get(id);
            return item && m.usecode().has_class(w.usecode_class(*item)) &&
                   m.usecode().event_offset(w.usecode_class(*item), event) != 0;
        };
        w.set_hooks(hooks);
    }
    // `loop` searches (ITEMFIND.C). Arguments in pop order: type 2 (area)
    // a = centre item, b = range; type 4 (container) b = container; type 6
    // (surface) a = item to search above, b = item to search below (0xFFFF =
    // unused; both given: above and below b). Types 3 and 5 (recursive
    // variants) are not used by the scripts and find nothing.
    m.set_item_search([&w](int type, std::uint16_t a, std::uint16_t b, std::span<const std::uint8_t> script) {
        if (type == 2) return w.area_search(a, b, script);
        if (type == 4) return w.container_search(b, script);
        if (type == 6) {
            if (a != 0xFFFF && b != 0xFFFF) return w.surface_search(b, 3, script);
            if (a != 0xFFFF) return w.surface_search(a, 1, script);
            if (b != 0xFFFF) return w.surface_search(b, 2, script);
        }
        return std::vector<ObjId>{};
    });
    // Items destroyed while a loop runs are skipped, as the original's
    // lazy iteration would never reach them.
    m.set_item_exists([&w](ObjId id) { return w.get(id) != nullptr; });
    // Registers `fn` (taking the item) under every alias number.
    auto bind = [&](std::initializer_list<std::uint16_t> numbers,
                    std::function<std::uint32_t(Item&, Args)> fn) {
        for (std::uint16_t n : numbers) {
            m.set_intrinsic(n, [&w, fn](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
                Item* item = item_arg(machine, w, a);
                return item ? fn(*item, a) : 0;
            });
            bound.push_back(n);
        }
    };

    bind({0x001}, [](Item& i, Args) { return i.frame; });
    bind({0x002, 0x05B}, [&w](Item& i, Args a) { w.set_frame(i, arg16(a, 4)); return 0u; });
    bind({0x003}, [](Item& i, Args) { return i.map_number; });
    // Item::I_getSurfaceWeight (10A8:0602): the total weight of the items
    // standing on it (surface search above with the script "$", which on its
    // fast path does not skip the item itself).
    bind({0x059}, [&w](Item& i, Args) {
        static constexpr std::uint8_t kAll[] = {'$'};
        int total = 0;
        for (ObjId id : w.surface_search(i.id, 1, kAll)) {
            if (const Item* on = w.get(id)) total += total_weight(w, *on);
        }
        return static_cast<std::uint32_t>(static_cast<std::uint16_t>(total));
    });
    // Item::I_equip(item, value) (10A8:2A35): event 10 of the item's class
    // when the class has it (Usecode_start_event_if_present, mask 0x400),
    // with the value sign-extended to 4 bytes as its argument; the pid or 0.
    // Item::I_unequip (10A8:2A68) is the same with event 11 (mask 0x800).
    const auto equip_event = [&m, &w](std::uint32_t event) {
        return [&m, &w, event](Item& i, Args a) -> std::uint32_t {
            const std::uint32_t value = static_cast<std::uint32_t>(static_cast<std::int16_t>(arg16(a, 4)));
            const std::array<std::uint8_t, 4> arg = {
                static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
            return m.start_event(w.usecode_class(i), event, i.id, arg);
        };
    };
    bind({0x006, 0x0B6}, equip_event(10));
    bind({0x0B1}, equip_event(11));
    // Item::I_cast (10A8:2F3E): event 0x11 (mask 0x20000) with the value,
    // pushed as a 32-bit argument (unsigned 16-bit, zero-extended); the pid.
    bind({0x0E8}, [&m, &w](Item& i, Args a) -> std::uint32_t {
        const std::uint32_t value = arg16(a, 4);
        const std::array<std::uint8_t, 4> arg = {
            static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), 0, 0};
        return m.start_event(w.usecode_class(i), 0x11, i.id, arg);
    });
    // Item::I_avatarStoleSomething (10A8:2FDA): the same with event 0x13
    // (mask 0x80000), nothing for a dead NPC.
    bind({0x0B2}, [&w, steal = equip_event(0x13)](Item& i, Args a) -> std::uint32_t {
        return is_npc_id(i.id) && w.is_dead(i.id) ? 0 : steal(i, a);
    });
    // Item::I_touch (10A8:2514): marks the item for a redraw (DLIST.C,
    // 11A0:10E5 / 13BE) unless it is ethereal; the view is redrawn whole on
    // every frame here.
    bind({0x025}, [](Item&, Args) { return 0u; });
    // Item::I_use (10A8:2667): event 1 of the item's class; the pid or 0.
    bind({0x081, 0x0D0, 0x0D5}, [&m, &w](Item& i, Args) -> std::uint32_t {
        return may_use(w, i.id) ? m.start_event(w.usecode_class(i), 1, i.id) : 0;
    });
    bind({0x004}, [](Item& i, Args) { return i.flags; });
    bind({0x005}, [](Item& i, Args a) { i.flags |= arg16(a, 4); return 0u; });
    bind({0x01A, 0x034, 0x06B, 0x070, 0x09A, 0x0B3, 0x0C0, 0x0C2, 0x0C4, 0x0E9, 0x0FC, 0x101, 0x104, 0x106,
          0x108, 0x10A, 0x10C, 0x10E, 0x110, 0x114, 0x117, 0x11A, 0x124, 0x12B},
         [](Item& i, Args a) { i.flags &= arg16(a, 4); return 0u; });
    // Position of the outermost container.
    bind({0x009}, [&w](Item& i, Args) { return static_cast<std::uint32_t>(w.location(i).z & 0xFF); });
    bind({0x013}, [&w](Item& i, Args) { return static_cast<std::uint32_t>(w.location(i).x & 0xFFFF); });
    bind({0x014}, [&w](Item& i, Args) { return static_cast<std::uint32_t>(w.location(i).y & 0xFFFF); });
    // getCX/getCY/getCZ: centre of the footprint. x and y are the far corner
    // (footpad units are 16 usecode units, half of it is 8), z the bottom.
    // Unlike getX, these read the item's own fields (container id when contained).
    auto footpad = [&w](const Item& i) {
        const ShapeInfo* info = w.shape_info(i.shape);
        return info ? *info : ShapeInfo{};
    };
    bind({0x120}, [footpad](Item& i, Args) { return static_cast<std::uint32_t>((i.x - footpad(i).footpad_x * 8) & 0xFFFF); });
    bind({0x11E}, [footpad](Item& i, Args) { return static_cast<std::uint32_t>((i.y - footpad(i).footpad_y * 8) & 0xFFFF); });
    bind({0x11F}, [&w, footpad](Item& i, Args) {
        return static_cast<std::uint32_t>((w.location(i).z + footpad(i).footpad_z * 4) & 0xFF);
    });
    // Footprint boxes (ITEM.C). Item::I_getFootpadData (10A8:1AA9) swaps the
    // x and y extents of a flipped item (status 0x20). An item at (x, y, z)
    // covers x - fx*16 .. x, y - fy*16 .. y, z .. z + fz*8; x and y are
    // compared as unsigned 16-bit values, z as signed. The other item is an
    // object id; contained items (status 8) never touch anything, except for
    // isCentreOn which does not check.
    struct Extent {
        std::uint16_t x, y, w, d;
        int z, h;
        bool contained;
    };
    auto extent = [footpad](const Item& i) {
        const ShapeInfo s = footpad(i);
        const bool flipped = i.flags & kItemFlipped;
        const int fx = flipped ? s.footpad_y : s.footpad_x, fy = flipped ? s.footpad_x : s.footpad_y;
        return Extent{static_cast<std::uint16_t>(i.x), static_cast<std::uint16_t>(i.y),
                      static_cast<std::uint16_t>(fx * 16), static_cast<std::uint16_t>(fy * 16),
                      i.z & 0xFF, s.footpad_z * 8, (i.flags & kItemContained) != 0};
    };
    auto below = [](std::uint16_t v, std::uint16_t size) { return static_cast<std::uint16_t>(v - size); };
    // 10A8:1E00: the footprints overlap in x and y (open intervals).
    auto overlap_xy = [below](const Extent& a, const Extent& b) {
        return !a.contained && !b.contained && below(b.x, b.w) < a.x && below(a.x, a.w) < b.x &&
               below(b.y, b.d) < a.y && below(a.y, a.d) < b.y;
    };
    // Binds a test of the item against the object id at offset 4.
    auto bind_pair = [&](std::initializer_list<std::uint16_t> numbers, std::function<bool(const Item&, const Item&)> fn) {
        bind(numbers, [&w, fn](Item& i, Args a) -> std::uint32_t {
            const Item* other = w.get(arg16(a, 4));
            return other && fn(i, *other);
        });
    };
    bind_pair({0x0A5}, [=](const Item& i, const Item& o) {  // Item::I_overlaps (10A8:1CC5)
        const Extent a = extent(i), b = extent(o);
        return overlap_xy(a, b) && a.z < b.z + b.h && b.z < a.z + a.h;
    });
    // Item::I_isOn (10A8:1EEA): resting on top of the other item.
    bind_pair({0x046, 0x048, 0x04A, 0x04C, 0x04E, 0x050, 0x0A6, 0x0BD, 0x0C6, 0x0DC, 0x0F1, 0x0FA, 0x126},
              [=](const Item& i, const Item& o) {
                  const Extent a = extent(i), b = extent(o);
                  return overlap_xy(a, b) && a.z == b.z + b.h;
              });
    // Item::I_isCompletelyOn (10A8:1F5F): on top, the footprint inside the other's.
    bind_pair({0x02B}, [=](const Item& i, const Item& o) {
        const Extent a = extent(i), b = extent(o);
        return !a.contained && !b.contained && a.x <= b.x && below(b.x, b.w) <= below(a.x, a.w) && a.y <= b.y &&
               below(b.y, b.d) <= below(a.y, a.d) && a.z == b.z + b.h;
    });
    // Item::I_isCentreOn (10A8:4377): the centre of the item (getCX / getCY,
    // which ignore flipping) strictly inside the other's footprint, on top.
    bind_pair({0x05A}, [=](const Item& i, const Item& o) {
        const ShapeInfo s = footpad(i);
        const Extent b = extent(o);
        const std::uint16_t cx = below(static_cast<std::uint16_t>(i.x), static_cast<std::uint16_t>(s.footpad_x * 8));
        const std::uint16_t cy = below(static_cast<std::uint16_t>(i.y), static_cast<std::uint16_t>(s.footpad_y * 8));
        return cx < b.x && cx > below(b.x, b.w) && cy < b.y && cy > below(b.y, b.d) && (i.z & 0xFF) == b.z + b.h;
    });
    // Item::I_getFootpadData(item, &x, &y, &z): the extents in footpad units.
    for (std::uint16_t n : {std::uint16_t{0x066}, std::uint16_t{0x127}}) {
        m.set_intrinsic(n, [&w, extent](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
            const Item* item = item_arg(machine, w, a);
            if (!item) return 0;
            const Extent e = extent(*item);
            const std::array<int, 3> values = {e.w / 16, e.d / 16, e.h / 8};
            for (std::size_t k = 0; k < values.size(); ++k) {
                const std::array<std::uint8_t, 2> word = {static_cast<std::uint8_t>(values[k]), 0};
                machine.assign(arg32(a, 4 + 4 * k), word);
            }
            return 0;
        });
        bound.push_back(n);
    }
    bind({0x011}, [](Item& i, Args) { return i.map_number; });  // Actor::I_getMap, same array as getMapArray
    bind({0x041}, [](Item& i, Args) { return i.map_number; });  // Egg::I_getEggId (1098:05C5), same array
    bind({0x016}, [](Item& i, Args) { return i.shape; });
    bind({0x024}, [&w](Item& i, Args a) { w.set_shape(i, arg16(a, 4)); return 0u; });
    bind({0x08D, 0x119}, [&w](Item& i, Args) { w.set_broken(i); return 0u; });
    bind({0x010, 0x02E, 0x068, 0x085, 0x0A2, 0x0AF, 0x0D9, 0x0EA, 0x12F},
         [](Item& i, Args) { return i.quality & 0xFFu; });
    bind({0x026, 0x047, 0x049, 0x04B, 0x04D, 0x04F, 0x051, 0x0B0, 0x0BF, 0x0CA, 0x0F0, 0x0F3, 0x0FB, 0x12C},
         [](Item& i, Args) { return static_cast<std::uint32_t>(i.quality >> 8); });
    bind({0x030}, [](Item& i, Args a) {
        i.quality = static_cast<std::uint16_t>((i.quality & 0x00FF) | (arg16(a, 4) & 0xFF) << 8);
        return 0u;
    });
    bind({0x03E}, [](Item& i, Args a) {
        i.quality = static_cast<std::uint16_t>((i.quality & 0xFF00) | (arg16(a, 4) & 0xFF));
        return 0u;
    });
    bind({0x0F4}, [](Item& i, Args) { return i.quality; });  // Item::I_getQ (10A8:0C94)
    // Item::I_getQuality (10A8:0CAA): the quality only for family 1, else 0.
    bind({0x07F}, [&w](Item& i, Args) -> std::uint32_t { return w.family_of(i.shape) == 1 ? i.quality : 0u; });
    bind({0x0F5}, [](Item& i, Args a) { i.quality = arg16(a, 4); return 0u; });
    bind({0x069, 0x06F, 0x08A, 0x08F, 0x0AE, 0x0F8, 0x100, 0x102, 0x105, 0x107, 0x109, 0x10B, 0x10D, 0x10F, 0x111,
          0x115, 0x11C, 0x125, 0x130},
         [](Item& i, Args) { return i.npc_number; });
    bind({0x06A}, [](Item& i, Args a) { i.npc_number = static_cast<std::uint8_t>(arg16(a, 4)); return 0u; });
    bind({0x03F}, [&w](Item& i, Args) {
        const ShapeInfo* info = w.shape_info(i.shape);
        return info ? info->family : 0u;
    });
    // Item::I_getTypeFlag(item, bit) (10A8:0FEF): bit `bit` of the shape's
    // 9 TYPEFLAG bytes, from bit 0 of byte 0.
    bind({0x11B}, [&w](Item& i, Args a) -> std::uint32_t {
        const ShapeInfo* info = w.shape_info(i.shape);
        const std::uint16_t bit = arg16(a, 4);
        return info && bit / 8u < info->bytes.size() ? (info->bytes[bit / 8u] >> (bit & 7)) & 1u : 0u;
    });
    // Item::I_setQuality(item, quality) (10A8:0CEE): only for family 1.
    bind({0x080, 0x0BB, 0x133}, [&w](Item& i, Args a) -> std::uint32_t {
        if (w.family_of(i.shape) == 1) i.quality = arg16(a, 4);
        return 0;
    });
    // Item::I_getUnkEggType (10A8:0D31): for an egg of family 4, the
    // quality's low byte (its usecode class, World::usecode_class), else 0.
    bind({0x0A3}, [&w](Item& i, Args) -> std::uint32_t {
        return w.family_of(i.shape) == 4 ? i.quality & 0xFFu : 0u;
    });
    // Egg::I_getEggXRange (1098:051D): the high nibble of the NPC number
    // byte (the low one is the y range, teleport.cpp).
    bind({0x0A8}, [](Item& i, Args) -> std::uint32_t { return i.npc_number >> 4; });
    // Egg::I_setEggXRange(egg, range) (1098:055B): the high nibble.
    bind({0x0A4}, [](Item& i, Args a) -> std::uint32_t {
        i.npc_number = static_cast<std::uint8_t>((i.npc_number & 0x0F) | arg16(a, 4) << 4);
        return 0;
    });
    // Item::I_isInNpc (10A8:24B5): whether one of the containers holding the
    // item, going outwards, is an NPC.
    bind({0x067}, [&w](Item& i, Args) -> std::uint32_t {
        for (const Item* it = &i; it && (it->flags & kItemContained); it = w.get(it->parent)) {
            if (is_npc_id(it->parent)) return 1;
        }
        return 0;
    });

    m.set_intrinsic(0x00A, [&w](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        if (Item* item = item_arg(machine, w, a)) destroy_item(machine.kernel(), w, item->id);
        return 0;
    });
    bound.push_back(0x00A);
    // Container::I_destroyContents (10A8:14AF): for a container (family 6 or
    // 7), Item::I_destroy on each content (SHAPHAND_1090_05cb).
    bind({0x02D}, [&m, &w](Item& i, Args) -> std::uint32_t {
        const std::uint8_t family = w.family_of(i.shape);
        if (family != kFamilyContainer && family != kFamilyActorContainer) return 0;
        for (ObjId c : std::vector<ObjId>(i.contents)) destroy_item(m.kernel(), w, c);
        return 0;
    });

    auto bind_raw = [&](std::uint16_t n, UCMachine::Intrinsic fn) {
        m.set_intrinsic(n, std::move(fn));
        bound.push_back(n);
    };
    // Item::I_getFamilyOfType(shape) (1100:054D): the same, from a shape.
    bind_raw(0x0AD, [&w](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        const ShapeInfo* info = w.shape_info(arg16(a, 0));
        return info ? info->family : 0u;
    });
    // Item::I_getPoint(item, point*): writes x (u16), y (u16), z (u8) as stored
    // (container id in x when contained).
    bind_raw(0x05E, [&w](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        Item* item = item_arg(machine, w, a);
        if (!item) return 0;
        const std::array<std::uint8_t, 5> point{
            static_cast<std::uint8_t>(item->x), static_cast<std::uint8_t>(item->x >> 8),
            static_cast<std::uint8_t>(item->y), static_cast<std::uint8_t>(item->y >> 8),
            static_cast<std::uint8_t>(item->z)};
        machine.assign(arg32(a, 4), point);
        return 0;
    });
    // A point (x u16, y u16, z u8) through a pointer.
    auto read_point = [](UCMachine& machine, std::uint32_t ptr) {
        std::array<std::uint8_t, 5> p{};
        machine.dereference(ptr, p);
        return World::Point{p[0] | p[1] << 8, p[2] | p[3] << 8, p[4]};
    };
    // Item::I_legalCreateAtPoint(id*, shape, frame, point*) (10A8:104E) and
    // Item::I_legalCreateAtCoords(id*, shape, frame, x, y, z) (10A8:10DB): a
    // new item on the map where its box is free; the id is written through
    // the pointer. Return 1 on success.
    auto legal_create = [&w](UCMachine& machine, Args a, const World::Point& at) -> std::uint32_t {
        const ObjId id = legal_create_at(w, arg16(a, 4), static_cast<std::uint8_t>(arg16(a, 6)), at);
        if (id == 0) return 0;
        const std::array<std::uint8_t, 2> out{static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id >> 8)};
        machine.assign(arg32(a, 0), out);
        return 1;
    };
    bind_raw(0x05D, [legal_create, read_point](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        return legal_create(machine, a, read_point(machine, arg32(a, 8)));
    });
    bind_raw(0x019, [legal_create](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        return legal_create(machine, a, World::Point{arg16(a, 8), arg16(a, 10), arg16(a, 12) & 0xFF});
    });
    // Item::I_legalMoveToPoint(item, point*, mode, flags) (10A8:174D): sweeps
    // the item from its position (the outermost container's) to the point,
    // with the sweep flags (4, no ground needed, when 0). Reaching it moves
    // the item there and returns 1; otherwise, unless bit 0 of `mode` is set,
    // the item is moved as far as it got, and 0 is returned. Either move
    // first leaves what the item carried or stood on (ITEM.C 2970,
    // leave_surface), then sends the contact events (10E8:123A, finish_move).
    bind_raw(0x031, [&w, read_point](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        Item* item = item_arg(machine, w, a);
        if (!item) return 0;
        const World::Point end = read_point(machine, arg32(a, 4));
        const std::uint8_t flags = static_cast<std::uint8_t>(arg16(a, 10)) ? static_cast<std::uint8_t>(arg16(a, 10)) : 4;
        const SweepResult r = sweep(w, item->shape, item->id, w.location(*item), end, flags);
        if (r.ok || !(arg16(a, 8) & 1)) {
            const ObjId id = item->id;
            leave_surface(machine.kernel(), w, id);
            finish_move(w, id, r);
        }
        return r.ok;
    });
    // CurrentMap::I_canExistAt(shape, x, y, z) (1150:0B67): whether a box with
    // the footprint of `shape` is free at (x, y, z). The original lends the
    // footprint to shape 0x31A (solid, not fixed) for the test, so the mask
    // and the minimum height of 1 are those of 0x31A.
    bind_raw(0x0BC, [&w](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        const ShapeInfo* info = w.shape_info(arg16(a, 0));
        const ShapeInfo* lender = w.shape_info(0x31A);
        const int fx = info ? info->footpad_x : 0, fy = info ? info->footpad_y : 0, fz = info ? info->footpad_z : 0;
        const int h = fz == 0 && !(lender && lender->is(ShapeInfo::kFixed)) ? 1 : fz * 8;
        return box_free(w, fx * 16, fy * 16, h, collision_mask(lender), World::Point{arg16(a, 2), arg16(a, 4), arg16(a, 6) & 0xFF});
    });
    // CurrentMap::I_canExistAtPoint(item, shape, point*) (10A8:4071): the item
    // is unused; whether the box of `shape` (below 0x800), with its own
    // mask, is free at the point (ITEMFIND_10e8_00d9, unflipped).
    bind_raw(0x032, [&w, read_point](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        const std::uint16_t shape = arg16(a, 4);
        return shape < 0x800 && position_free(w, shape, read_point(machine, arg32(a, 6)));
    });
    // Item::I_shoot(item, point*, speed, gravity) (10A8:38D1), gravity.hpp.
    bind_raw(0x08B, [&w, read_point](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        if (Item* item = item_arg(machine, w, a)) {
            shoot(machine.kernel(), w, item->id, read_point(machine, arg32(a, 4)), arg16(a, 8), arg16(a, 10));
        }
        return 0;
    });
    // Item::I_create(id*, shape, frame): new item on the pending stack; its id
    // is written through the pointer (0 on failure). Returns 1 on success.
    bind_raw(0x01F, [&w](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        const ObjId id = w.create(arg16(a, 4), static_cast<std::uint8_t>(arg16(a, 6)));
        const std::array<std::uint8_t, 2> out{static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id >> 8)};
        machine.assign(arg32(a, 0), out);
        return id != 0;
    });
    // Item::I_getEtherealTop() (10B0:2622): the top of the pending stack
    // (DS:7DD4), 0 when empty.
    bind_raw(0x023, [&w](UCMachine&, UCProcess&, Args) -> std::uint32_t { return w.pending_top(); });
    bind({0x007}, [&w](Item& i, Args) -> std::uint32_t { return w.is_partly_on_screen(i); });
    bind({0x089}, [](Item& i, Args a) { i.map_number = static_cast<std::uint8_t>(arg16(a, 4)); return 0u; });
    // Item::I_fall(item), Item::I_hurl(item, vx, vy, vz, gravity).
    bind_raw(0x040, [&w](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        if (Item* i = item_arg(machine, w, a)) fall(machine.kernel(), w, i->id);
        return 0;
    });
    for (std::uint16_t n : std::initializer_list<std::uint16_t>{0x028, 0x08E, 0x0BE, 0x0C1, 0x0C3, 0x0C9, 0x0F7, 0x0F9, 0x118, 0x11D}) {
        bind_raw(n, [&w](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
            Item* i = item_arg(machine, w, a);
            if (!i) return 0;
            auto s16 = [&](std::size_t pos) { return static_cast<int>(static_cast<std::int16_t>(arg16(a, pos))); };
            return hurl(machine.kernel(), w, i->id, s16(4), s16(6), s16(8), s16(10));
        });
    }
    // Item::I_enterFastArea (10A8:2B30): World::enter_fast_area, which the
    // scripts call for items they bring in; the pid of event 0x0F, or 0.
    bind({0x08C}, [&w](Item& i, Args) -> std::uint32_t { return w.enter_fast_area(i); });
    // Item::I_inFastArea.
    bind({0x02F}, [](Item& i, Args) -> std::uint32_t { return (i.flags & kItemFastArea) != 0; });
    // Actor::I_isNPC(item): the id alone decides (1..255), the item need not exist.
    bind_raw(0x008, [](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        std::array<std::uint8_t, 2> id{};
        if (!machine.dereference(arg32(a, 0), id)) return 0;
        return is_npc_id(static_cast<ObjId>(id[0] | id[1] << 8));
    });
    // Actor::I_isDead(item), Actor::I_getDir(item): NPC table, by id alone.
    auto bind_npc = [&](std::initializer_list<std::uint16_t> numbers, std::function<std::uint32_t(ObjId)> fn) {
        for (std::uint16_t n : numbers) {
            bind_raw(n, [fn](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
                std::array<std::uint8_t, 2> id{};
                if (!machine.dereference(arg32(a, 0), id)) return 0;
                return fn(static_cast<ObjId>(id[0] | id[1] << 8));
            });
        }
    };
    bind_npc({0x03C, 0x128}, [&w](ObjId id) -> std::uint32_t { return w.is_dead(id); });
    bind_npc({0x01C, 0x112, 0x121, 0x131}, [&w](ObjId id) -> std::uint32_t { return w.npc_direction(id); });
    // Item::I_getDirFromTo16(x1, y1, x2, y2).
    bind_raw(0x037, [](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        return direction_16(arg16(a, 0), arg16(a, 2), arg16(a, 4), arg16(a, 6));
    });
    // Item::I_getDirToCoords(item, x, y), Item::I_getDirToItem(item, other id):
    // from the item's own x/y fields.
    bind({0x123}, [](Item& i, Args a) -> std::uint32_t { return direction_8(i.x, i.y, arg16(a, 4), arg16(a, 6)); });
    bind({0x00D}, [&w](Item& i, Args a) -> std::uint32_t {
        const Item* other = w.get(arg16(a, 4));
        return direction_8(i.x, i.y, other ? other->x : 0, other ? other->y : 0);
    });
    // Item::I_getDirFromItem(item, other id) (10A8:1A3F): the opposite one.
    bind({0x0C8}, [&w](Item& i, Args a) -> std::uint32_t {
        const Item* other = w.get(arg16(a, 4));
        return (direction_8(i.x, i.y, other ? other->x : 0, other ? other->y : 0) + 4) % 8;
    });
    // Item::I_getClosestDirectionInRange(x1, y1, x2, y2, steps, lo, hi).
    bind_raw(0x027, [](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        const auto s16 = [&](std::size_t pos) { return static_cast<int>(static_cast<std::int16_t>(arg16(a, pos))); };
        return static_cast<std::uint16_t>(closest_direction_in_range(arg16(a, 0), arg16(a, 2), arg16(a, 4),
                                                                     arg16(a, 6), s16(8), s16(10), s16(12)));
    });
    // Item::I_isCrusTypeNPC(shape): TYPEFLAG NPC flag; shape 0x7FE answers 0x701.
    bind_raw(0x103, [&w](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        const std::uint16_t shape = arg16(a, 0);
        if (shape == 0x7FE) return 0x701;
        const ShapeInfo* info = w.shape_info(shape);
        return info && info->is(ShapeInfo::kNpc);
    });
    // Pending stack. The first argument of the pops is an unused item pointer.
    bind_raw(0x022, [&w](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        if (Item* item = item_arg(machine, w, a)) w.push(item->id);
        return 0;
    });
    bind_raw(0x020, [&w](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        w.pop_to_coords(arg16(a, 4), arg16(a, 6), arg16(a, 8) & 0xFF);
        return 0;
    });
    // Item::I_move(item, x, y, z) (10A8:1531): push then popToCoords; the
    // marker of the designated target follows it (World::move_item). The
    // original stops with an assertion for z above 250, the port keeps 250.
    bind_raw(0x06C, [&w](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        if (Item* item = item_arg(machine, w, a)) w.move_item(item->id, arg16(a, 4), arg16(a, 6), arg16(a, 8) & 0xFF);
        return 0;
    });
    // Item::I_getRangeIfVisible(item, other) (1130:1C01): `other` is an id.
    bind_raw(0x044, [&w](UCMachine& machine, UCProcess&, Args a) -> std::uint32_t {
        const Item* item = item_arg(machine, w, a);
        return item ? static_cast<std::uint32_t>(range_if_visible(w, item->id, arg16(a, 4))) : 0u;
    });
    bind_raw(0x0ED, [&w](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        w.pop_to_container(arg16(a, 4));
        return 0;
    });
    bind_raw(0x0EC, [&w](UCMachine&, UCProcess&, Args a) -> std::uint32_t {
        w.pop_to_end(arg16(a, 4));
        return 0;
    });
    bind_raw(0x033, [&w](UCMachine&, UCProcess&, Args) -> std::uint32_t {
        w.pop();
        return 0;
    });
    return bound;
}

}  // namespace crusader
