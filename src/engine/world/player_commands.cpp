#include "engine/world/player_commands.hpp"

#include "core/dtable.hpp"
#include "core/language.hpp"
#include "engine/audio/audio.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/inventory.hpp"
#include "engine/world/item_selection.hpp"

namespace crusader {

namespace {

constexpr ObjId kPlayer = 1;

constexpr int kFirstWeaponSlot = 1;
constexpr int kLastWeaponSlot = 5;
constexpr int kFirstItemSlot = 11;
constexpr int kLastItemSlotDown = 25;  // 13E8:0D5F wraps to 0x19
constexpr int kLastItemSlotUp = 24;    // 13E8:0DC0 wraps after 0x18

constexpr std::uint16_t kMedikit = 0x351;
constexpr std::uint16_t kEnergyCube = 0x582;
constexpr std::uint16_t kDetpac = 0x55F;
constexpr std::uint16_t kDatalink = 0x4D4;
constexpr std::uint16_t kNotUsable = 0x4ED;
constexpr std::uint16_t kKeptWeapon = 0x32E;

constexpr std::uint16_t kActionHolster = 0xB;
constexpr std::uint16_t kActionRunningJump = 0x38;
constexpr std::uint16_t kActionJump = 0x3A;

// From the slot after `current` (step +1 or -1), the first slot holding an
// item, wrapping from `last` to `first` going up and from below `first` to
// `last` going down; the current slot is tried last. 0 when none.
// @orig 13E8:0C07 13E8:0C72 13E8:0D5F 13E8:0DC0
int find_slot(const World& world, int current, int step, int first, int last) {
    auto wrap = [&](int slot) { return slot > last ? first : slot < first ? last : slot; };
    const int start = wrap(current + step);
    int slot = start;
    do {
        if (world.npc_equip(kPlayer, slot) != 0) return slot;
        slot = wrap(slot + step);
    } while (slot != start);
    return 0;
}

bool kneeling(const World& world) {
    const auto r = world.npc_record(kPlayer);
    return !r.empty() && (r[npc::kFlags13] & 0x04);
}

// NPC_10f0_3b57: no weapon, or a weapon whose record keeps the actions.
bool weapon_keeps_actions(const World& world) {
    const Item* weapon = world.get(world.active_weapon(kPlayer));
    return !weapon || dtable::weapon(weapon->shape).keeps_actions();
}

// CRUSADER_1138_0014: the controlled NPC's last action is an idle one.
// @orig 1138:0014
bool idle(const World& world) {
    switch (world.npc_word(world.state.controlled_npc, npc::kLastAnim)) {
    case 0: case 1: case 0x0B: case 0x10: case 0x20: case 0x37: case 0x3A: case 0x3B:
        return true;
    default:
        return false;
    }
}

ObjId active_item(const World& world) {
    const int slot = world.npc_word(kPlayer, npc::kActiveItem);
    return slot == 0 ? 0 : world.npc_equip(kPlayer, slot);
}

// Quality - 1; at 0 the item is destroyed (Item_I_destroy: its looping
// sounds stop) and leaves the player's slots. True when it was destroyed.
// @orig 13E8:14B4
bool use_up(World& world, ObjId id) {
    const std::uint16_t quality = world.get(id)->quality;
    set_quality_and_combine(world, id, static_cast<std::uint16_t>(quality - 1));
    if (world.get(id)->quality != 0) return false;
    if (Audio* audio = world.audio()) audio->sources.stop_item_loops(id);
    world.destroy(id);
    world.unequip(kPlayer, id);
    return true;
}

}  // namespace

bool player_may_act(const World& world) {
    return !world.state.avatar_in_stasis && !world.is_dead(kPlayer) && world.state.controlled_npc == kPlayer;
}

// @orig 13E8:0C07
void previous_weapon(World& world) {
    const int current = world.npc_word(kPlayer, npc::kWeapon);
    if (current == 0) return;
    if (const int slot = find_slot(world, current, -1, kFirstWeaponSlot, kLastWeaponSlot))
        world.set_npc_word(kPlayer, npc::kWeapon, static_cast<std::uint16_t>(slot));
}

// @orig 13E8:0C72
void next_weapon(Kernel& kernel, World& world, const AnimDat& anims) {
    const int current = world.npc_word(kPlayer, npc::kWeapon);
    if (world.state.controlled_npc != kPlayer || current == 0) return;
    const int slot = find_slot(world, current, +1, kFirstWeaponSlot, kLastWeaponSlot);
    if (slot == 0) return;
    world.set_npc_word(kPlayer, npc::kWeapon, static_cast<std::uint16_t>(slot));
    // The pose of the new weapon (CRUSADER_1138_32a8), 0x40 for none.
    std::uint16_t pose = 0x40;
    if (kneeling(world)) pose = weapon_keeps_actions(world) ? 0x2E : 0x2F;
    else if (!idle(world)) pose = weapon_keeps_actions(world) ? 4 : 0x25;
    if (pose != 0x40 && !actor_busy(kernel, kPlayer)) set_pose(world, anims, kPlayer, pose);
}

// @orig 13E8:0D5F
void previous_item(World& world) {
    const int current = world.npc_word(kPlayer, npc::kActiveItem);
    if (current == 0) return;
    if (const int slot = find_slot(world, current, -1, kFirstItemSlot, kLastItemSlotDown))
        world.set_npc_word(kPlayer, npc::kActiveItem, static_cast<std::uint16_t>(slot));
}

// @orig 13E8:0DC0
void next_item(World& world) {
    const int current = world.npc_word(kPlayer, npc::kActiveItem);
    if (current == 0) return;
    if (const int slot = find_slot(world, current, +1, kFirstItemSlot, kLastItemSlotUp))
        world.set_npc_word(kPlayer, npc::kActiveItem, static_cast<std::uint16_t>(slot));
}

// @orig 13E8:14B4
void use_active_item(World& world) {
    const ObjId id = active_item(world);
    if (id == 0) return;
    const std::uint16_t shape = world.get(id)->shape;
    if (shape == kNotUsable) return;
    use_item(world, id);
    if (shape == kDatalink || (shape >= 0x52D && shape <= 0x530)) return;
    if (use_up(world, id)) next_item(world);
}

// @orig 13E8:14B4
void use_carried(World& world, std::uint16_t shape) {
    if (const ObjId id = carried(world, shape)) {
        use_item(world, id);
        use_up(world, id);
    }
}

// @orig 13E8:14B4
void set_off_detpacs(World& world) {
    const Item* player = world.get(kPlayer);
    if (!player) return;
    // 16-bit bounds: one past 0x7FFF becomes negative, then 0x7FFF.
    auto bound = [](std::int32_t v) { return static_cast<std::int32_t>(static_cast<std::int16_t>(v)); };
    for (ObjId id : world.rect_search(bound(player->x - 0x800), bound(player->y - 0x800), bound(player->x + 0x800),
                                      bound(player->y + 0x800))) {
        const Item* item = world.get(id);
        if (item && item->shape == kDetpac) use_item(world, id);
    }
}

// @orig 13E8:14B4 10A8:00C1
void drop_weapon(Kernel& kernel, World& world, const AnimDat& anims) {
    const ObjId id = world.active_weapon(kPlayer);
    if (id == 0) return;
    if (world.get(id)->shape == kKeptWeapon && world.state.difficulty < 2) return;
    next_weapon(kernel, world, anims);
    const Item* player = world.get(kPlayer);
    world.push(id);
    world.pop_to_coords(player->x, player->y, player->z);
}

// @orig 1138:220A; key case at 1138:253F
void jump(Kernel& kernel, World& world, const AnimDat& anims) {
    if (world.state.controlled_npc != kPlayer) return;
    const std::uint16_t last = world.npc_word(kPlayer, npc::kLastAnim);  // Actor_I_getLastAnimSet
    const bool running = last == 3 || last == 0x30 || last == 0x31 || last == kActionRunningJump;  // 10F0:21A0
    if (!actor_busy(kernel, kPlayer)) {
        if (idle(world)) {
            do_anim(kernel, world, anims, kPlayer, kActionJump, kDirCurrent, 0);
        } else if (running) {
            do_anim(kernel, world, anims, kPlayer, kActionRunningJump, kDirCurrent, 0);
        } else if (do_anim(kernel, world, anims, kPlayer, kActionHolster, kDirCurrent, 0) != 0) {
            do_anim(kernel, world, anims, kPlayer, kActionJump, kDirCurrent, 1);
        }
        return;
    }
    // Busy: a run alone becomes a running jump (KERNEL_11d8_151d, vfunc +0x0C).
    if (kernel.count_processes(kPlayer, ActorAnimProcess::kType) != 1) return;
    ActorAnimProcess* anim = current_anim(kernel, kPlayer);
    if (!anim || !running || anim->action() == kActionRunningJump) return;
    anim->fail();
    do_anim(kernel, world, anims, kPlayer, kActionRunningJump, kDirCurrent, 0);
}

// @orig 13E8:14B4
bool player_command(Kernel& kernel, World& world, const AnimDat& anims, std::uint16_t code) {
    const bool may_act = player_may_act(world);
    if (world.state.selection.active) {
        // 13E8:1214: with G under way, the keys are ignored while the player may act.
        if (!(world.state.selection.pick_all && may_act)) selection_key(world, code);
        return true;
    }
    switch (code) {
    case 'G': case 'g':
    case 'S': case 's':
        if (may_act && !world.state.cru_stasis) {
            world.state.crusader_teleporting = false;  // Ultima8Engine_I_clrCrusaderTeleporting
            start_selection(world, code == 'G' || code == 'g');
        }
        return true;
    case 'Q': case 'q':
        if (may_act) previous_weapon(world);
        return true;
    case 'W': case 'w':
        if (may_act) next_weapon(kernel, world, anims);
        return true;
    case 'I': case 'i':
        if (may_act) previous_item(world);
        return true;
    case 'O': case 'o':
        if (may_act) next_item(world);
        return true;
    case 'U': case 'u':
        if (may_act) use_active_item(world);
        return true;
    case 'M': case 'm':
        if (may_act && !world.state.cru_stasis) use_carried(world, kMedikit);
        return true;
    case 'E': case 'e':
        if (may_act && !world.state.cru_stasis) use_carried(world, kEnergyCube);
        return true;
    case 'B': case 'b':
        if (may_act) set_off_detpacs(world);
        return true;
    case 'R': case 'r': {
        // No condition: target designation on or off, and the message box.
        bool& on = world.state.target_designation;
        on = !on;
        if (world.hooks().show_message)
            world.hooks().show_message(on ? tr("TARGETING RETICLE ACTIVE.")
                                          : tr("TARGETING RETICLE INACTIVE."));
        return true;
    }
    case 0x420:  // Ctrl+D
        if (world.state.controlled_npc == kPlayer && may_act) drop_weapon(kernel, world, anims);
        return true;
    default:
        return false;
    }
}

}  // namespace crusader
