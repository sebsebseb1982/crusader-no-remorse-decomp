#include "engine/world/pathfind.hpp"

#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/attack.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace crusader {

namespace {

// DS:208B and DS:2093: candidate offsets from the goal's direction.
constexpr std::array<int, 8> kOffsets = {0, 2, -2, 4, -4, 6, -6, -8};
constexpr std::array<int, 8> kMirroredOffsets = {0, -2, 2, -4, 4, -6, 6, -8};

int distance_xy(const World::Point& a, const World::Point& b) {
    return std::max(std::abs(static_cast<std::int16_t>(a.x - b.x)), std::abs(static_cast<std::int16_t>(a.y - b.y)));
}

std::uint16_t walk_action(const World& world, ObjId npc) { return in_combat(world, npc) ? 0x24 : 1; }

}  // namespace

// @orig 1118:0000 1118:01DC
PathfinderProcess::PathfinderProcess(World& world, const AnimDat& anims, ObjId npc, ObjId item, World::Point goal,
                                     int max_steps, int stop_distance, bool final_turn)
    : world_(world),
      anims_(anims),
      item_(item),
      goal_(goal),
      stop_distance_(stop_distance),
      max_steps_(max_steps),
      final_turn_(final_turn) {
    set_owner(npc);
    set_type(kType);
    mirror_table_ = crt_rand_below(world.state.crt_rand_state, 2) != 0;  // rand() * 2 / 0x8000
    if (const Item* target = world.get(item)) {
        goal_ = world.location(*target);
        const ShapeInfo* info = world.shape_info(target->shape);
        solid_goal_ = info && info->footpad_z != 0 && info->is(ShapeInfo::kSolid);
    }
    if (const Item* self = world.get(npc)) best_ = distance_xy({self->x, self->y, 0}, goal_);
}

// @orig 1118:0000 1118:01DC
Pid walk_to(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc, ObjId item, World::Point goal,
            int max_steps, int stop_distance, bool final_turn) {
    auto r = world.npc_record(npc);
    if (r.empty()) return 0;
    // NPC_10f0_22bf / 22df: one path finder per NPC (+0x59 bit 5).
    if (r[npc::kCombatFlags] & 0x20) kernel.fail_processes(npc, PathfinderProcess::kType, 1);
    r[npc::kCombatFlags] |= 0x20;
    const Pid pid = kernel.add(
        std::make_unique<PathfinderProcess>(world, anims, npc, item, goal, max_steps, stop_distance, final_turn));
    // ATTACK_1118_03cc: a drawn weapon is holstered for the walk.
    if (in_combat(world, npc) && weapon_drawn(world, npc)) do_anim(kernel, world, anims, npc, 0xB, kDirCurrent, 10000, false);
    return pid;
}

// @orig 1118:08E4
int PathfinderProcess::choose_direction(World::Point& pos) {
    const ObjId npc = owner();
    const std::uint16_t action = walk_action(world_, npc);
    const int d = direction_8(pos.x, pos.y, goal_.x, goal_.y) & 0xF;
    const int distance = distance_xy(pos, goal_);
    bool failed = false;
    // Each direction is tried by a test walk (a whole half stride, ANIMPRIM.C
    // test mode) aimed at the item, if any.
    if (distance < best_) {
        best_ = distance;
        if (test_action(world_, anims_, npc, action, d, pos, item_).ok) wall_ = false;
    }

    std::array<int, 9> candidates{};
    int first = 0;
    if (!wall_) {
        const auto& offsets = mirror_table_ ? kMirroredOffsets : kOffsets;
        for (int i = 0; i < 8; ++i) candidates[i] = (d + offsets[i]) & 0xF;
    } else {
        const int turn = (!mirror_table_ == !side_) ? 2 : -2;
        candidates[1] = (last_dir_ + turn + 8) & 0xF;
        for (int k = 1; k < 8; ++k) candidates[k + 1] = (candidates[k] + turn) & 0xF;
        first = 2;
    }

    World::Point next = pos;
    int rank = 0;
    for (int i = first; i <= 0xF; i += 2) {
        const int k = i >> 1;
        if (!wall_) {
            candidate_ = candidates[k];
        } else {
            if (k == 2) continue;
            if (i == 0xE) break;
            candidate_ = candidates[k + 1];
        }
        rank = k;
        const ActionTest step = test_action(world_, anims_, npc, action, candidate_, pos, item_);
        next = step.end;
        failed = !step.ok;
        if (solid_goal_ && step.reached) {
            final_turn_ = true;
            return 0xFF;
        }
        if (stop_distance_ != 0 && distance_xy(goal_, next) <= stop_distance_) {
            final_turn_ = true;
            return 0xFF;
        }
        if (!failed) break;
    }
    if (failed) return 0x10;
    if (candidate_ != d && !wall_) {
        wall_ = true;
        side_ = rank & 1;
    }
    pos = next;

    // In combat: stop as soon as the target can be shot.
    if (in_combat(world_, npc) && npc != world_.state.controlled_npc) {
        const ObjId target = world_.npc_word(npc, npc::kTarget);
        const Item* self = world_.get(npc);
        if (target != 0 && self && world_.is_partly_on_screen(*self) &&
            fire_distance(world_, anims_, npc, target, d) != 0) {
            do_anim(kernel(), world_, anims_, npc, 0, kDirCurrent, 10000, false);
            if (auto* attack = dynamic_cast<AttackProcess*>(kernel().find_process(npc, AttackProcess::kType)))
                attack->force_sight();
            stance_ = false;
            final_turn_ = true;
            return 0xFF;
        }
    }
    return candidate_;
}

// @orig 1118:0428
void PathfinderProcess::run() {
    Kernel& k = kernel();
    const ObjId npc = owner();
    const Item* self = world_.get(npc);
    if (!self || !(self->flags & kItemFastArea)) return;
    if (done_) {
        terminate(1);
        return;
    }
    if (item_ != 0 && solid_goal_) {
        if (const Item* target = world_.get(item_)) goal_ = world_.location(*target);
    }
    World::Point pos{self->x, self->y, self->z & 0xFF};
    if (pos.x == goal_.x && pos.y == goal_.y) {
        terminate(pos.z == (goal_.z & 0xFF) ? 1 : 0);
        return;
    }
    last_dir_ = choose_direction(pos);
    if (last_dir_ == 0x10) {
        terminate(0);
        return;
    }
    if (last_dir_ == 0xFF) {
        done_ = true;
    } else if (steps_ == max_steps_) {
        terminate(0);
        return;
    }
    const int dir = done_ ? candidate_ : last_dir_;
    turn_to(k, world_, anims_, npc, dir, done_, true);
    const Pid walk = do_anim(k, world_, anims_, npc, walk_action(world_, npc), dir, 10000, false);
    if (walk != 0) wait_for(walk);
    ++steps_;
}

// @orig 1118:072E
void PathfinderProcess::on_terminate() {
    // ATTACK_1118_072e.
    const ObjId npc = owner();
    auto r = world_.npc_record(npc);
    if (r.empty()) return;
    r[npc::kCombatFlags] &= static_cast<std::uint8_t>(~0x20);
    const Item* self = world_.get(npc);
    if (!self || (self->z & 0xFF) == 0xFE || world_.is_dead(npc)) return;
    Kernel& k = kernel();
    Pid turned = 0;
    if (final_turn_) {
        const Item* target = world_.get(item_);
        const int dir = target ? direction_8(self->x, self->y, target->x, target->y)
                               : direction_8(self->x, self->y, goal_.x, goal_.y);
        turned = turn_to(k, world_, anims_, npc, dir, true, true);
    }
    if (turned == 0 && stance_) {
        const bool armed = in_combat(world_, npc) && weapon_drawn(world_, npc);
        do_anim(k, world_, anims_, npc, armed ? 4 : 0, kDirCurrent, 0, false);
    }
}

}  // namespace crusader
