#include "engine/world/activity.hpp"

#include "engine/audio/audio.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/pathfind.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace crusader {

namespace {

// random_range(n) (SYSTEM.C 12D8:04FF), UCMachine::I_rndRange (12E0:0293)
// and the C library's rand() * n / 0x8000 (core/random.hpp), each where the
// original uses it.
int rnd(World& w, int n) { return random_range(w.state.random_state, static_cast<std::uint16_t>(n)); }
int crt_rnd(World& w, int n) { return crt_rand_below(w.state.crt_rand_state, n); }

int rnd_between(World& w, int lo, int hi) {
    return random_between(w.state.random_state, static_cast<std::int16_t>(lo), static_cast<std::int16_t>(hi));
}

std::uint16_t word_at(std::span<const std::uint8_t> r, std::size_t at) {
    return static_cast<std::uint16_t>(r[at] | r[at + 1] << 8);
}

// TYPE_1108_026e: on screen, with a default activity 1 it is not doing yet,
// the NPC that sees the controlled NPC takes its default activity 1.
// @orig 1108:026E
bool spot_controlled(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc) {
    const Item* self = world.get(npc);
    const auto r = world.npc_record(npc);
    if (!self || r.empty() || !world.is_partly_on_screen(*self)) return false;
    const std::uint16_t alert = word_at(r, npc::kDefaultActivity + 2);
    if (alert == 0 || word_at(r, npc::kActivity) == alert) return false;
    if (!sees_controlled(world, anims, npc)) return false;
    set_activity(kernel, world, anims, npc, 0x71);
    return true;
}

// TYPE_1108_0000: the common start of activity processes.
// @orig 1108:0000
class ActivityProcess : public Process {
public:
    ActivityProcess(World& world, const AnimDat& anims, ObjId npc, std::uint16_t type) : world_(world), anims_(anims) {
        set_owner(npc);
        set_type(type);
    }

protected:
    void on_added() override {
        const Item* self = world_.get(owner());
        if (!self || !anims_.action(self->shape, 0)) return;
        set_pose(world_, anims_, owner(), 0);
        if (auto r = world_.npc_record(owner()); !r.empty()) r[npc::kStance] &= static_cast<std::uint8_t>(~0x02);
    }

    World& world_;
    const AnimDat& anims_;
};

// Activity 1 (1108:0359).
// @orig 1108:0359
class StandProcess : public ActivityProcess {
public:
    StandProcess(World& w, const AnimDat& a, ObjId npc) : ActivityProcess(w, a, npc, 0x258) {}

protected:
    void run() override {
        if (!spot_controlled(kernel(), world_, anims_, owner())) sleep(60);
    }
};

// Activity 2 (1108:0BFA): `count` walks (0xFFFF: no end).
// @orig 1108:0BFA
class LoiterProcess : public ActivityProcess {
public:
    LoiterProcess(World& w, const AnimDat& a, ObjId npc, std::uint16_t count)
        : ActivityProcess(w, a, npc, 0x257), count_(count) {
        if (const Item* self = w.get(npc)) home_ = w.location(*self);
    }

protected:
    void run() override {
        const ObjId npc = owner();
        if (actor_busy(kernel(), npc)) return;
        if (count_ != 0xFFFF && count_-- == 0) {
            terminate(0);
            return;
        }
        const Item* self = world_.get(npc);
        const bool can_fidget = self && anims_.action(self->shape, 0x1E);
        if (rnd(world_, 2) == 0 && can_fidget) {
            fidget(kernel(), world_, anims_, npc);
            return;
        }
        const int dx = rnd_between(world_, -0xA0, 0xA0);
        const int dy = rnd_between(world_, -0xA0, 0xA0);
        const Pid walk = walk_to(kernel(), world_, anims_, npc, 0, {home_.x + dx, home_.y + dy, home_.z}, 0xC, 0x40, false);
        if (walk != 0) wait_for(walk);
    }

private:
    std::uint16_t count_;
    World::Point home_{};
};

// Activity 3 (1108:0708).
// @orig 1108:0708 1108:0693
class WalkProcess : public ActivityProcess {
public:
    WalkProcess(World& w, const AnimDat& a, ObjId npc) : ActivityProcess(w, a, npc, 0x255) {}

protected:
    void run() override {
        Kernel& k = kernel();
        const ObjId npc = owner();
        const Item* self = world_.get(npc);
        if (!self || spot_controlled(k, world_, anims_, npc) || actor_busy(k, npc)) return;
        const int dir = world_.npc_direction(npc) & 0xF;
        World::Point pos = world_.location(*self);
        if (test_action(world_, anims_, npc, 1, dir, pos).ok) {
            blocked_ = 0;
            const Pid pid = do_anim(k, world_, anims_, npc, 1, kDirCurrent, 0, false);
            if (pid != 0) wait_for(pid);
            return;
        }
        if (++blocked_ > 1) {
            // Blocked twice: shapes 0x2F5, 0x2F6, 0x2F7, 0x344 and 0x597 take
            // activity 7, the others attack (5).
            const std::uint16_t s = self->shape;
            const bool listed = s == 0x2F5 || s == 0x2F7 || s == 0x2F6 || s == 0x344 || s == 0x597;
            set_activity(k, world_, anims_, npc, listed ? 7 : 5);
            return;
        }
        do_anim(k, world_, anims_, npc, 0, kDirCurrent, 0, false);
        const Pid turn = turn_to(k, world_, anims_, npc, (dir + 8) & 0xF, false, false);
        if (Process* p = k.get(turn)) {
            p->sleep(60);
            wait_for(turn);
        }
    }

private:
    int blocked_ = 0;  // +0x44
};

// Activity 7 (1108:0383 / 04AB / 0437).
// @orig 1108:0383 1108:04AB 1108:0437
class SurrenderProcess : public ActivityProcess {
public:
    SurrenderProcess(World& w, const AnimDat& a, ObjId npc) : ActivityProcess(w, a, npc, 0x25F) {}

protected:
    void on_added() override {
        ActivityProcess::on_added();
        do_anim(kernel(), world_, anims_, owner(), 0x39, kDirCurrent, 0, false);
        if (auto r = world_.npc_record(owner()); !r.empty()) r[npc::kStance] |= 0x01;
    }

    void run() override {
        Kernel& k = kernel();
        const ObjId npc = owner();
        const ObjId controlled = world_.state.controlled_npc;
        const Item* self = world_.get(npc);
        const Item* other = world_.get(controlled);
        if (self && other && !actor_busy(k, npc) && world_.is_mostly_on_screen(*self)) {
            const int dir = direction_8(self->x, self->y, other->x, other->y);
            if ((world_.npc_direction(npc) & 0xF) == dir) {
                // Once it sees the controlled NPC, a plea drawn by shape.
                if (!barked_ && range_if_visible(world_, npc, controlled) != 0) {
                    barked_ = true;
                    plead(*self);
                }
            } else {
                turn_to(k, world_, anims_, npc, dir, true, false);  // Actor::I_turnToward
            }
        }
        sleep(60);
    }

    void on_terminate() override {
        if (auto r = world_.npc_record(owner()); !r.empty()) r[npc::kStance] &= static_cast<std::uint8_t>(~0x01);
    }

private:
    // Tables DS:1EA4, 1EB0, 1EB8, 1EC0, 1EC8; other shapes say nothing.
    void plead(const Item& self) {
        static constexpr int k2F7[] = {0xE9, 0xE0, 0xEB, 0xE2, 0xE1, 0xEA};
        static constexpr int k2F6[] = {0xB4, 0xC5, 0xC6, 0xE8};
        static constexpr int k344[] = {0xE3, 0xE4, 0xEC, 0xF6};
        static constexpr int k2F5[] = {0xDE, 0xDF, 0x8A, 0x8B};
        static constexpr int k597[] = {0xD6, 0xFF, 0xD7};
        int sfx = -1;
        switch (self.shape) {
        case 0x2F7: sfx = k2F7[crt_rnd(world_, 6)]; break;
        case 0x2F6: sfx = k2F6[crt_rnd(world_, 4)]; break;
        case 0x344: sfx = k344[crt_rnd(world_, 4)]; break;
        case 0x2F5: sfx = k2F5[crt_rnd(world_, 4)]; break;
        case 0x597: sfx = k597[crt_rnd(world_, 3)]; break;
        default: return;
        }
        if (Audio* audio = world_.audio()) audio->sources.play(world_, sfx, owner());
    }

    bool barked_ = false;  // +0x42
};

// Activity 8 (1108:0A0E).
// @orig 1108:0A0E
class GuardProcess : public ActivityProcess {
public:
    GuardProcess(World& w, const AnimDat& a, ObjId npc) : ActivityProcess(w, a, npc, 0x25E) {}

protected:
    void run() override {
        Kernel& k = kernel();
        const ObjId npc = owner();
        if (actor_busy(k, npc)) {
            sleep(60);
            return;
        }
        if (sees_controlled(world_, anims_, npc)) {
            set_activity(k, world_, anims_, npc, 5);
            return;
        }
        if (rnd(world_, 2) == 0) {
            sleep(static_cast<std::uint32_t>(crt_rnd(world_, 3) * 60 + 60));
            return;
        }
        const Item* self = world_.get(npc);
        if (self && anims_.action(self->shape, 0x1E)) fidget(k, world_, anims_, npc);
    }
};

}  // namespace

// @orig 1108:00C2
bool sees_controlled(const World& world, const AnimDat& anims, ObjId npc, bool look_turn) {
    const ObjId controlled = world.state.controlled_npc;
    const Item* self = world.get(npc);
    const Item* other = world.get(controlled);
    if (controlled == 0 || !self || !other || !world.is_mostly_on_screen(*self)) return false;
    int facing = world.npc_direction(npc) & 0xF;
    if (look_turn) {
        // Looking left or right (actions 0x1E, 0x1F) from its third frame,
        // the NPC sees four directions aside.
        const auto r = world.npc_record(npc);
        const int last = r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8;
        if ((last == 0x1E || last == 0x1F) && r[npc::kAnimFrame] >= 2) {
            const bool south = facing == 8 || facing == 10 || facing == 12;
            const bool back = last == 0x1E ? !south : south;
            facing = (facing + (back ? -4 : 4)) & 0xF;
        }
    }
    const int d = direction_8(self->x, self->y, other->x, other->y) & 0xF;
    bool near = false;
    for (int k = -2; k <= 2; ++k) near = near || facing == ((d + k) & 0xF);
    if (!near) return false;
    // The line of fire is looked for towards the controlled NPC, whatever
    // the facing within the two directions.
    bool muzzle_found = false;
    if (fire_distance(world, anims, npc, controlled, d, 0, 0, 0, &muzzle_found) != 0) return true;
    return !muzzle_found && range_if_visible(world, npc, controlled) != 0;
}

// @orig 1130:1C01
int range_if_visible(const World& world, ObjId item_id, ObjId other_id) {
    const Item* item = world.get(item_id);
    const Item* other = world.get(other_id);
    if (!item || !other) return 0;
    const World::Point from = world.location(*item);
    World::Point to = world.location(*other);
    const ShapeInfo* info = world.shape_info(other->shape);
    const int height = info ? info->footpad_z : 0;
    if (height > 1) to.z = (to.z + (height - 1) * 8) & 0xFF;
    const SweepResult r = sweep(world, item->shape, item_id, from, to, kSweepNoGround);
    if (r.blocker != other_id && r.blocker != 0) return 0;
    int distance = std::max({std::abs(static_cast<std::int16_t>(from.x - to.x)),
                             std::abs(static_cast<std::int16_t>(from.y - to.y)), std::abs(from.z - to.z)}) / 16;
    if (distance & 0xF) ++distance;
    return distance < 0x31 ? distance : 0;
}

// @orig 1138:2DAF
void wake_npcs_on_screen(Kernel& kernel, World& world, const AnimDat& anims) {
    const ObjId controlled = world.state.controlled_npc;
    for (ObjId id = 2; id < 0x100; ++id) {
        const Item* self = world.get(id);
        if (!self || (self->z & 0xFF) == 0xFE || id == controlled || !world.is_partly_on_screen(*self)) continue;
        const auto r = world.npc_record(id);
        if (r.empty()) continue;
        const std::uint16_t activity = word_at(r, npc::kActivity);
        const bool settled = in_combat(world, id) || activity == word_at(r, npc::kDefaultActivity + 4) ||
                             (r[npc::kStance] & 0x01);
        if (settled) {
            const bool guard = activity == 8;
            if (guard) set_activity(kernel, world, anims, id, 5);
            if (guard || in_combat(world, id)) set_target(kernel, world, anims, id, controlled);
            continue;
        }
        const std::uint16_t s = self->shape;
        const bool looks = s == 0x2F5 || s == 0x2F6 || s == 0x2F7 || s == 0x595 || s == 0x597;
        if (looks && range_if_visible(world, id, controlled) == 0) continue;
        set_activity(kernel, world, anims, id, 0x72);
        if (in_combat(world, id)) set_target(kernel, world, anims, id, controlled);
    }
}

void fidget(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc) {
    if (actor_busy(kernel, npc)) return;
    do_anim(kernel, world, anims, npc, rnd(world, 2) == 0 ? 0x1E : 0x1F, kDirCurrent, 0, false);
    do_anim(kernel, world, anims, npc, 0, kDirCurrent, 1, false);
}

// @orig 10F0:2D02
Pid start_activity_process(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc, std::uint16_t activity) {
    std::unique_ptr<Process> p;
    switch (activity) {
    case 2: p = std::make_unique<LoiterProcess>(world, anims, npc, std::uint16_t{0xFFFF}); break;
    case 3: p = std::make_unique<WalkProcess>(world, anims, npc); break;
    case 4: case 6: return 0;
    case 7: p = std::make_unique<SurrenderProcess>(world, anims, npc); break;
    case 8: p = std::make_unique<GuardProcess>(world, anims, npc); break;
    default: p = std::make_unique<StandProcess>(world, anims, npc); break;  // 1 and others
    }
    return kernel.add(std::move(p));
}

}  // namespace crusader
