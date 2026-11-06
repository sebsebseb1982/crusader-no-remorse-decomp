#include "engine/world/cheats.hpp"

#include "engine/audio/audio.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/actor_create.hpp"
#include "engine/world/chargers.hpp"
#include "engine/world/inventory.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace crusader {

namespace {

constexpr ObjId kPlayer = 1;

// DS:28BD: J A S S I C A 1 6, then the end.
constexpr std::array<std::uint8_t, 10> kCode = {0x24, 0x1E, 0x1F, 0x1F, 0x17, 0x2E, 0x1E, 0x02, 0x07, 0x00};
constexpr int kCodeSound = 0x103;

constexpr std::uint8_t kScancodeF10 = 0x44;
constexpr std::uint16_t kDeathSilence = 0x261;
constexpr std::uint8_t kImmortal = 0x02;
constexpr std::uint8_t kDead = 0x01;

constexpr std::uint16_t kCredits = 0x4ED;
constexpr std::uint16_t kCheatCredits = 30000;
constexpr std::uint16_t kEndOfTable = 0xFFFF;
// DS:289D, DS:28A9 (Ctrl), DS:28B5 (Alt).
constexpr std::array<std::uint16_t, 6> kWeapons = {0x386, 0x38A, 0x38E, 0x388, 0x334, kEndOfTable};
constexpr std::array<std::uint16_t, 6> kCtrlWeapons = {0x32E, 0x32F, 0x333, 0x330, 0x38D, kEndOfTable};
constexpr std::array<std::uint16_t, 4> kAltWeapons = {0x332, 0x38B, 0x38C, kEndOfTable};
constexpr std::array<std::uint16_t, 4> kStacks = {0x560, 0x39A, 0x55F, 0x351};
constexpr std::array<std::uint16_t, 5> kAmmo = {0x33D, 0x33F, 0x340, 0x341, 0x33E};
constexpr int kMaxStack = 0x14;
constexpr std::uint16_t kFullAmmo = 100;

void say(World& world, const std::string& text) {
    if (world.hooks().show_message) world.hooks().show_message(text);
}

// Item::I_setQuality (10A8:0CEE): only for family 1.
void set_quality(World& world, ObjId id, std::uint16_t quality) {
    Item* item = world.get(id);
    if (item && world.family_of(item->shape) == 1) item->quality = quality;
}

World::Point player_feet(const World& world) {
    const Item* p = world.get(kPlayer);
    return p ? World::Point{static_cast<std::uint16_t>(p->x), static_cast<std::uint16_t>(p->y), p->z & 0xFF}
             : World::Point{0, 0, 0};
}

enum class Given { kNotMade, kRefused, kTaken };

// Item_I_create (frame 0), popToCoords at 0, 0, 0, MainActor::I_addItemCru
// (shown); destroyed when refused.
// @orig 1138:0896
Given give(World& world, std::uint16_t shape) {
    const ObjId id = world.create(shape, 0);
    if (id == 0) return Given::kNotMade;
    world.pop_to_coords(0, 0, 0);
    if (add_item_cru(world, id, true)) return Given::kTaken;
    world.destroy(id);
    return Given::kRefused;
}

// @orig 1138:01BD
void give_ammo(World& world, std::uint16_t shape) {
    if (const ObjId carried_id = carried(world, shape)) {
        set_quality_and_combine(world, carried_id, kFullAmmo);
        return;
    }
    const int slot = free_equip_slot(world, 6, 5);  // NPC_10f0_2b2d
    const ObjId id = world.create(shape, 0);
    if (id == 0) return;
    const World::Point at = player_feet(world);
    world.pop_to_coords(at.x, at.y, at.z);
    set_quality(world, id, kFullAmmo);
    if (slot != 0) {
        world.push(id);
        world.pop_to_end(kPlayer);
        set_equip(world, kPlayer, slot, id);
    }
}

// @orig 1138:0896
void give_everything(World& world, std::uint16_t flags) {
    // NPC_10f0_3dbf: the credits carried.
    if (const ObjId c = carried(world, kCredits)) set_quality(world, c, kCheatCredits);
    give(world, 0x3A3);
    give(world, 0x530);
    // NPC_10f0_34ca(npc, 0): the energy full.
    world.set_npc_word(kPlayer, 0x57, static_cast<std::uint16_t>(max_energy(world, kPlayer)));
    const std::uint16_t* table = (flags & kCtrl) ? kCtrlWeapons.data() : (flags & kAlt) ? kAltWeapons.data() : kWeapons.data();
    for (int i = 0; table[i] != kEndOfTable; ++i) {
        const int slot = free_equip_slot(world, 1, 5);  // NPC_10f0_2b03
        if (slot == 0) break;
        if (carried(world, table[i]) != 0) continue;
        const ObjId id = world.create(table[i], 0);
        if (id == 0) continue;
        const World::Point at = player_feet(world);
        world.pop_to_coords(at.x, at.y, at.z);
        set_equip(world, kPlayer, slot, id);
        if (world.active_weapon(kPlayer) == 0) world.set_npc_word(kPlayer, 0x15, static_cast<std::uint16_t>(slot));
    }
    for (const std::uint16_t shape : kStacks) {
        if (carried(world, shape) != 0) continue;
        for (int i = 0; i < kMaxStack; ++i) {
            if (give(world, shape) == Given::kRefused) break;
        }
    }
    for (const std::uint16_t shape : kAmmo) give_ammo(world, shape);
}

}  // namespace

// @orig 1138:2B03
void cheat_code_key(World& world, const GameKeyEvent& e) {
    if (!e.pressed) return;
    GameState::Cheats& c = world.state.cheats;
    if (kCode[static_cast<std::size_t>(c.typed)] == e.scancode) ++c.typed;
    else c.typed = kCode[0] == e.scancode ? 1 : 0;
    if (kCode[static_cast<std::size_t>(c.typed)] != 0) return;
    c.typed = 0;
    c.hack_mover_allowed = true;
    c.enabled = !c.enabled;
    c.on = c.enabled;
    if (Audio* audio = world.audio()) audio->play_sfx(kCodeSound);
    say(world, c.enabled ? "Cheats are now active." : "Cheats are now inactive.");
}

// @orig 1138:0896
bool cheat_f10(Kernel& kernel, World& world, const dtable::NpcTypes& types, const GameKeyEvent& e, bool us_123) {
    if (!e.pressed || e.scancode != kScancodeF10 || !world.state.cheats.on) return false;
    const ObjId npc = world.state.controlled_npc;  // DS:285A, DS:2852
    // Kernel_fail_processes(DS:2852, any type): owner 0 would be every owner.
    if (npc != 0 && !(us_123 && world.state.avatar_in_stasis)) kernel.fail_processes(npc, Kernel::kAnyType, 1);
    kernel.fail_processes(0, kDeathSilence, 1);
    auto record = world.npc_record(npc);
    if (world.is_dead(npc)) {
        Item* item = world.get(npc);
        if (npc == kPlayer && item) world.set_shape(*item, 1);
        // NPC_10f0_2037.
        if (item) world.clear_broken(*item);
        if (!record.empty()) record[npc::kFlags13] &= static_cast<std::uint8_t>(~kDead);
        // NPC_10f0_355e: the maximum of the controlled NPC's type (DS:2852).
        const Item* controlled = world.get(world.state.controlled_npc);
        world.set_npc_word(npc, 0x00, types.max_hp(types.index(controlled ? controlled->shape : 0)));
        return true;
    }
    if ((e.flags & kCtrl) && !record.empty()) {
        if (record[npc::kFlags13] & kImmortal) {
            record[npc::kFlags13] &= static_cast<std::uint8_t>(~kImmortal);
            say(world, "Immortality disabled.");
        } else {
            record[npc::kFlags13] |= kImmortal;
            say(world, "Immortality enabled.");
        }
    }
    if (npc == kPlayer) give_everything(world, e.flags);
    return true;
}

// @orig 13E8:14B4
bool cheat_command(World& world, std::uint16_t code, bool us_123) {
    GameState::Cheats& c = world.state.cheats;
    switch (code) {
    case '~':
        if (!c.enabled) return true;
        c.on = !c.on;
        say(world, c.on ? "CHEATS ON" : "CHEATS OFF");
        return true;
    case 'T': case 't':
        if (c.enabled) use_item(world, kPlayer);
        return true;
    case 'f':
        if (!c.enabled) return true;
        world.state.avatar_in_stasis = false;  // Ultima8Engine_I_setAvatarInStasis(0)
        c.bounding_boxes = !c.bounding_boxes;
        return true;
    case 0x410:  // Ctrl+Q
        if (!c.enabled) return true;
        c.cd_transfer_display = !c.cd_transfer_display;
        say(world, c.cd_transfer_display ? "CD TRANSFER DISPLAY ACTIVE." : "CD TRANSFER DISPLAY INACTIVE.");
        return true;
    case 0x426: {  // Ctrl+L
        const World::Point at = player_feet(world);
        char text[64];
        std::snprintf(text, sizeof text, "X: %u\nY: %u\nZ: %u", static_cast<unsigned>(at.x),
                      static_cast<unsigned>(at.y), static_cast<unsigned>(at.z));
        say(world, text);
        return true;
    }
    case 0x432: {  // Ctrl+M
        Audio* audio = world.audio();
        if (!audio) return true;
        int track = audio->music.current() + 1;
        if (us_123) {
            if (track == 0x13) track = 1;
        } else {
            if (track == 0x11) track = 0x12;
            if (track == 0x13) track = 0;
        }
        audio->music.play(track);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace crusader
