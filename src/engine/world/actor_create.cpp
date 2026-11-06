#include "engine/world/actor_create.hpp"

#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/attack.hpp"

namespace crusader {

namespace {

int rnd(World& w, int n) {
    return random_range(w.state.random_state, static_cast<std::uint16_t>(n));
}

void set_word(std::span<std::uint8_t> r, std::size_t at, std::uint16_t v) {
    r[at] = static_cast<std::uint8_t>(v);
    r[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

}  // namespace

// @orig 10A8:3B10
int hard_weapon(World& w, std::uint16_t shape) {
    switch (shape) {
    case 899: return rnd(w, 3) == 0 ? 10 : 7;
    case 0x319: case 0x2FD: return rnd(w, 4) == 0 ? 0xC : 3;
    case 0x1B4: return rnd(w, 4) == 0 ? 0xD : 9;
    case 0x2CB: return rnd(w, 2) == 0 ? 3 : 7;
    case 0x338: return rnd(w, 3) == 0 ? 5 : 7;
    case 0x371: return rnd(w, 3) == 0 ? 9 : 10;
    case 0x4D1: return rnd(w, 2) == 0 ? 4 : 0xB;
    case 900: return rnd(w, 3) == 0 ? 5 : 10;
    case 0x385: return rnd(w, 4) == 0 ? 8 : 9;
    case 0x3AC: return rnd(w, 2) == 0 ? 9 : 0xD;
    case 0x4E6: return rnd(w, 3) == 0 ? 5 : 0xB;
    case 0x528: return rnd(w, 3) == 0 ? 9 : 8;
    default: return 7;
    }
}

// @orig 10F0:27FB
bool set_equip(World& world, ObjId npc, int slot, ObjId item_id) {
    Item* item = world.get(item_id);
    auto r = world.npc_record(npc);
    if (!item || r.empty() || npc == item_id || slot < 1 || slot >= 30 || world.npc_equip(npc, slot) != 0) return false;
    if (world.family_of(item->shape) == 10 && world.active_weapon(npc) == 0) set_word(r, 0x15, static_cast<std::uint16_t>(slot));
    // Families 11 and 13 refresh the status bar (not needed: it is redrawn).
    world.push(item_id);
    world.pop_to_container(npc);
    set_word(r, 0x1B + 2 * slot, item_id);
    // ITEM_10a8_03c9: an equipped item keeps its slot as z.
    item = world.get(item_id);
    item->z = slot;
    item->flags |= kItemEquipped;
    return true;
}

// @orig 10F0:25B9 10F0:2610 10F0:0145
ObjId create_plain_actor(World& world, const dtable::NpcTypes& types, std::uint16_t shape, std::uint16_t frame) {
    const ObjId npc = world.create_npc(shape, frame);
    if (npc == 0) return 0;
    const int type = types.index(shape);
    auto r = world.npc_record(npc);
    set_word(r, 0x00, types.word(type, 0x02));
    if (const std::uint16_t weapon = types.word(type, 0x18); weapon != 0) {
        const Item* self = world.get(npc);
        const World::Point at{self->x, self->y, self->z & 0xFF};
        const ObjId gun = world.create(dtable::weapon_at(weapon).shape(), 0);
        world.pop_to_coords(at.x, at.y, at.z);
        int slot = 0;
        for (int s = 1; s <= 5 && slot == 0; ++s) {
            if (world.npc_equip(npc, s) == 0) slot = s;
        }
        set_equip(world, npc, slot, gun);
        set_word(r, 0x15, static_cast<std::uint16_t>(slot));
    }
    static constexpr std::size_t kDefaults[] = {0x1E, 0x40, 0x42};
    for (int i = 0; i < 3; ++i) set_word(r, npc::kDefaultActivity + 2 * i, types.word(type, kDefaults[i]));
    return npc;
}

namespace {

struct ActorSpec {
    int type;
    int facing;              // direction / 2
    std::uint8_t extra;      // record +0x12
    int lowest_difficulty;
    bool other_weapon;
    std::uint16_t defaults[3];
    std::uint8_t unk63;
    bool start_default;      // Actor::I_setActivity(0x70) once placed
};

// @orig 10A8:3D3F
ObjId make_actor(Kernel& kernel, World& world, const AnimDat& anims, const dtable::NpcTypes& types,
                 const World::Point& at, const ActorSpec& spec) {
    const std::uint16_t shape = types.word(spec.type, 0x3E);
    int weapon = types.word(spec.type, 0x18);
    int weapon2 = types.word(spec.type, 0x1A);
    if (spec.lowest_difficulty > world.state.difficulty) return 0;
    if (world.state.difficulty == 4) weapon = weapon2 = hard_weapon(world, shape);
    const ObjId npc = world.create_npc(shape, static_cast<std::uint16_t>((spec.facing * 2 + 4) & 0xF));
    if (npc == 0) {
        // No slot: with the alert on, the lights are switched again. A monster
        // egg does it in the other order, which leaves the alert off.
        if (world.state.alert_active) {
            set_alert(world, spec.start_default);
            set_alert(world, !spec.start_default);
        }
        return 0;
    }
    auto r = world.npc_record(npc);
    // NPC_10f0_2610: the first record of the shape, not necessarily `type`.
    set_word(r, 0x00, types.max_hp(types.index(shape)));
    if (shape == 0x597 || shape == 0x3AC) r[npc::kStance] |= 0x04;
    set_word(r, npc::kTactic, 0);
    r[0x0C] = static_cast<std::uint8_t>(spec.facing * 2);
    world.pop_to_coords(at.x, at.y, at.z);
    r[0x5E] = static_cast<std::uint8_t>(at.x);
    r[0x5F] = static_cast<std::uint8_t>(at.x >> 8);
    r[0x60] = static_cast<std::uint8_t>(at.y);
    r[0x61] = static_cast<std::uint8_t>(at.y >> 8);
    r[0x62] = static_cast<std::uint8_t>(at.z);
    r[0x12] = spec.extra;
    for (int i = 0; i < 3; ++i) set_word(r, npc::kDefaultActivity + 2 * i, spec.defaults[i]);
    if (spec.start_default) set_activity(kernel, world, anims, npc, 0x70);
    world.get(npc)->flags |= kItemFromEgg;
    r[npc::kEggQuality] = spec.unk63;

    const int chosen = spec.other_weapon && weapon2 != 0 ? weapon2 : weapon;
    if (chosen != 0) {
        int slot = 0;
        for (int s = 1; s <= 5 && slot == 0; ++s) {
            if (world.npc_equip(npc, s) == 0) slot = s;
        }
        if (slot != 0) {
            const ObjId gun = world.create(dtable::weapon_at(chosen).shape(), 0);
            world.pop_to_coords(0, 0, 0);
            set_equip(world, npc, slot, gun);
            set_word(r, 0x15, static_cast<std::uint16_t>(slot));
        }
    }
    return npc;
}

}  // namespace

// @orig 10A8:3D3F
ObjId create_actor(Kernel& kernel, World& world, const AnimDat& anims, const dtable::NpcTypes& types, ObjId egg_id,
                   ObjId kind_id) {
    const Item* egg = world.get(egg_id);
    const Item* kind = world.get(kind_id);
    if (!egg || !kind) return 0;
    ActorSpec spec{};
    spec.type = kind->npc_number;
    spec.facing = egg->npc_number & 0xF;
    spec.extra = static_cast<std::uint8_t>(egg->npc_number >> 4);
    spec.lowest_difficulty = (egg->map_number & 3) + 1;
    spec.other_weapon = egg->map_number & 4;
    spec.defaults[0] = static_cast<std::uint16_t>(kind->quality >> 8);
    spec.defaults[1] = static_cast<std::uint16_t>(egg->quality >> 8);
    spec.defaults[2] = kind->map_number;
    spec.unk63 = static_cast<std::uint8_t>(egg->quality);
    spec.start_default = false;
    return make_actor(kernel, world, anims, types, {egg->x, egg->y, egg->z & 0xFF}, spec);
}

// @orig 10E0:0247
ObjId spawn_from_egg(Kernel& kernel, World& world, const AnimDat& anims, const dtable::NpcTypes& types, ObjId egg_id) {
    Item* egg = world.get(egg_id);
    if (!egg || egg->frame == 1) return 0;
    world.set_frame(*egg, 1);
    const Item* kind = egg->contents.empty() ? nullptr : world.get(egg->contents.front());
    ActorSpec spec{};
    spec.type = kind ? kind->npc_number : 0;
    spec.facing = egg->npc_number & 0xF;
    spec.extra = static_cast<std::uint8_t>(egg->npc_number >> 4);
    spec.lowest_difficulty = (egg->map_number & 3) + 1;
    spec.other_weapon = egg->map_number & 4;
    if (kind) {
        spec.defaults[0] = static_cast<std::uint16_t>(kind->quality >> 8);
        spec.defaults[1] = static_cast<std::uint16_t>(kind->quality & 0xFF);
        spec.defaults[2] = kind->map_number;
    }
    spec.unk63 = 0;
    spec.start_default = true;
    return make_actor(kernel, world, anims, types, {egg->x, egg->y, egg->z & 0xFF}, spec);
}

}  // namespace crusader
