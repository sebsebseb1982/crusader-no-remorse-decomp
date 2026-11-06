#include "engine/world/inventory.hpp"

#include "core/dtable.hpp"
#include "engine/world/actor_create.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/chargers.hpp"

#include <algorithm>

namespace crusader {

namespace {

constexpr ObjId kPlayer = 1;
constexpr std::size_t kNpcEnergy = 0x57;

constexpr std::uint8_t kFamilyWeapon = 10;
constexpr std::uint8_t kFamilyAmmo = 11;
constexpr std::uint16_t kDatalink = 0x4D4;

}  // namespace

// @orig 10F0:395E
ObjId carried(const World& world, std::uint16_t shape) {
    for (int slot = 0; slot < 30; ++slot) {
        const ObjId id = world.npc_equip(kPlayer, slot);
        const Item* item = world.get(id);
        if (item && item->shape == shape) return id;
    }
    return 0;
}

namespace {

// NPC_10f0_2b03 / 2b2d / 2b62: the first empty slot of first..first+count-1.
// @orig 10F0:2B03 10F0:2B2D 10F0:2B62
int free_slot(const World& world, int first, int count) {
    for (int slot = first; slot < first + count; ++slot) {
        if (world.npc_equip(kPlayer, slot) == 0) return slot;
    }
    return 0;
}

// NPC_10f0_379a: energy added, clamped to 0..max; nothing when full and
// the amount is not negative.
// @orig 10F0:379A
void add_energy(World& world, int amount) {
    const int max = max_energy(world, kPlayer);
    const int energy = world.npc_mana(kPlayer);
    if (energy == max && amount >= 0) return;
    world.set_npc_word(kPlayer, kNpcEnergy, static_cast<std::uint16_t>(std::clamp(energy + amount, 0, max)));
}

// NPC_10f0_353c: a better battery, full.
void set_battery(World& world, std::uint16_t battery) {
    world.state.battery = battery;
    world.set_npc_word(kPlayer, kNpcEnergy, static_cast<std::uint16_t>(max_energy(world, kPlayer)));
}

// Into the player's contents, then the slot.
void take(World& world, ObjId item, int slot) {
    world.push(item);
    world.pop_to_end(kPlayer);
    set_equip(world, kPlayer, slot, item);
}

// One more on the item carried, up to `limit`; the picked-up one is shown
// (when `show`), then destroyed.
// @orig 13E8:0330
bool add_one(World& world, ObjId item, ObjId carried_id, int limit, bool show) {
    const std::uint16_t q = world.get(carried_id)->quality;
    if (q >= limit) return false;
    set_quality_and_combine(world, carried_id, static_cast<std::uint16_t>(q + 1));
    if (show) world.state.pickups.add(world, item);
    world.destroy(item);
    return true;
}

}  // namespace

// @orig 10F0:2B03 10F0:2B2D 10F0:2B62
int free_equip_slot(const World& world, int first, int count) {
    return free_slot(world, first, count);
}

// @orig 13E8:0330 10A8:2AD2
void set_quality_and_combine(World& world, ObjId item, std::uint16_t quality) {
    Item* it = world.get(item);
    if (!it) return;
    it->quality = quality;
    if (world.hooks().start_event) world.hooks().start_event(item, 0x0C);
}

// @orig 13E8:0330
bool add_item_cru(World& world, ObjId item_id, bool show) {
    const Item* item = world.get(item_id);
    if (!item) return false;
    const std::uint16_t shape = item->shape;
    // DISPLAY_1190_049c, before the item is destroyed; never for the datalink
    // when it is added as a new item or to those carried.
    const auto shown = [&world, item_id, show] {
        if (show) world.state.pickups.add(world, item_id);
    };

    if (shape == 0x4ED) {
        if (const ObjId c = carried(world, shape)) {
            set_quality_and_combine(world, c, static_cast<std::uint16_t>(world.get(c)->quality + item->quality));
            shown();
            world.destroy(item_id);
            return true;
        }
        const int slot = free_slot(world, 11, 15);
        if (slot == 0) return false;
        world.push(item_id);
        world.pop_to_end(kPlayer);
        world.set_frame(*world.get(item_id), 0);
        set_equip(world, kPlayer, slot, item_id);
        world.set_npc_word(kPlayer, kNpcActiveItem, static_cast<std::uint16_t>(slot));
        shown();
        return true;
    }

    const std::uint8_t fam = world.family_of(shape);
    if (fam == kFamilyWeapon) {
        if (carried(world, shape)) return false;
        const int slot = free_slot(world, 1, 5);
        if (slot == 0) return false;
        // A full clip of the weapon's ammunition (DTABLE_1120_027e / 020f).
        set_quality_and_combine(world, item_id, dtable::ammo_shape(shape) ? dtable::clip_size(shape) : 0);
        take(world, item_id, slot);
        shown();
        return true;
    }
    if (fam == kFamilyAmmo) {
        if (const ObjId c = carried(world, shape)) return add_one(world, item_id, c, 20, show);
        const int slot = free_slot(world, 6, 5);
        if (slot == 0) return false;
        set_quality_and_combine(world, item_id, 1);
        take(world, item_id, slot);
        shown();
        return true;
    }
    if (fam != 12 && fam != 13) return false;

    if (shape == 0x111) {
        const int card = item->quality & 0xFF;
        if (card < 0x20) world.state.keycards |= std::uint32_t{1} << card;
        shown();
        world.destroy(item_id);
        if (world.hooks().autosave) world.hooks().autosave();
        return true;
    }
    if (shape == 0x3A2 || shape == 0x3A3 || shape == 0x3A4) {
        shown();
        world.destroy(item_id);
        const std::uint16_t level = shape == 0x3A2 ? 1 : shape == 0x3A4 ? 2 : 3;
        const int bonus = shape == 0x3A2 ? 2500 : shape == 0x3A4 ? 5000 : 10000;
        int energy = 0;
        if (world.state.battery < level) set_battery(world, level);
        else energy = bonus;
        add_energy(world, energy);
        return true;
    }
    if (const ObjId c = carried(world, shape)) {
        const std::uint16_t s = world.get(c)->shape;
        if (s == 0x52E || s == 0x52F || s == 0x530) {
            world.destroy(item_id);
            return true;
        }
        return add_one(world, item_id, c, s == 0x560 ? 20 : 10, show && shape != kDatalink);
    }
    if (shape == 0x52E || shape == 0x52F || shape == 0x530) {
        const std::uint16_t level = static_cast<std::uint16_t>(shape - 0x52E + 1);
        if (world.npc_word(kPlayer, kNpcShieldLevel) < level) world.set_npc_word(kPlayer, kNpcShieldLevel, level);
        // The original shows the shield after destroying it (13E8:0613).
        shown();
        world.destroy(item_id);
        return true;
    }
    const int slot = free_slot(world, 11, 15);
    if (slot == 0) return false;
    world.push(item_id);
    world.pop_to_end(kPlayer);
    world.set_frame(*world.get(item_id), 0);
    set_equip(world, kPlayer, slot, item_id);
    set_quality_and_combine(world, item_id, 1);
    world.set_npc_word(kPlayer, kNpcActiveItem, static_cast<std::uint16_t>(slot));
    if (shape != kDatalink) shown();
    return true;
}

// The original reads the quality of the item it is given for the credits;
// Weasel's shop gives a new one, whose quality is 0.
// @orig 13E8:00C8
bool may_add_item(const World& world, std::uint16_t shape) {
    const ObjId c = carried(world, shape);
    const int quality = c ? world.get(c)->quality : 0;
    if (shape == 0x4ED) return c ? quality < 60000 : free_slot(world, 11, 15) != 0;
    const std::uint8_t fam = world.family_of(shape);
    if (fam == kFamilyWeapon) return free_slot(world, 1, 5) != 0;
    if (fam == kFamilyAmmo) return c ? quality < 20 : free_slot(world, 6, 5) != 0;
    if (fam != 12 && fam != 13) return false;
    if (shape == 0x111 || shape == 0x3A2 || shape == 0x3A3 || shape == 0x3A4) return true;
    if (!c) return free_slot(world, 11, 15) != 0;
    if (shape == 0x52E || shape == 0x52F || shape == 0x530) return true;
    return quality < (shape == 0x560 ? 20 : 10);
}

bool may_use(const World& world, ObjId item) {
    if (!world.get(item)) return false;
    if (!is_npc_id(item)) return true;
    return !world.is_dead(item) && (item == kPlayer || !in_combat(world, item));
}

void use_item(World& world, ObjId item) {
    if (may_use(world, item) && world.hooks().start_event) world.hooks().start_event(item, 1);
}

}  // namespace crusader
