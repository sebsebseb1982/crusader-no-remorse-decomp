#include "engine/world/auto_firer.hpp"

#include "engine/kernel.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/combat_control.hpp"

namespace crusader {

// @orig 1150:076C
const AnimFrame* firing_frame(const World& world, const AnimDat& anims) {
    const ObjId npc = world.state.controlled_npc;
    const Item* item = world.get(npc);
    const auto r = world.npc_record(npc);
    if (!item || r.empty()) return nullptr;
    const int dir = r[npc::kDirection];
    const bool kneeling = r[npc::kFlags13] & 0x04;
    const AnimAction* action = anims.action(item->shape, remap_action(world, npc, kneeling ? 0x2A : 8));
    if (!action) return nullptr;
    for (std::size_t i = 0; i < action->frame_count(); ++i) {
        const AnimFrame* f = action->frame(dir, i);
        if (f && f->fires_weapon()) return f;
    }
    return nullptr;
}

// @orig 1138:2FFB
AutoFirer::AutoFirer(World& world, const AnimDat& anims, Fire fire)
    : world_(world), anims_(anims), fire_(std::move(fire)) {
    set_type(kType);
    set_owner(world.state.controlled_npc);
    set_rate(6);
}

// @orig 1138:30B0
void AutoFirer::tick() {
    const ObjId npc = world_.state.controlled_npc;
    const Item* weapon = world_.get(world_.active_weapon(npc));
    if (!weapon || weapon->shape != kWeapon) {
        terminate();
        return;
    }
    if (CombatControl::rounds(world_, npc) == 0) return;
    if (const AnimFrame* f = firing_frame(world_, anims_)) {
        world_.state.auto_firing = true;
        fire_(npc, f->muzzle_a, f->muzzle_b, f->muzzle_z);
        world_.state.auto_firing = false;
    }
}

// @orig 1138:3162
void AutoFirer::on_terminate() {
    if (world_.state.auto_firer == pid()) world_.state.auto_firer = 0;
}

// @orig 1138:2F9F
void stop_auto_firer(Kernel& kernel, World& world) {
    if (world.state.auto_firer == 0) return;
    if (Process* p = kernel.get(world.state.auto_firer)) p->terminate();
    world.state.auto_firer = 0;
}

}  // namespace crusader
