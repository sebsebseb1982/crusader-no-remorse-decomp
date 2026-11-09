#include "engine/world/attack.hpp"

#include "core/dtable.hpp"
#include "engine/audio/audio.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/aim_mode.hpp"
#include "engine/world/activity.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/pathfind.hpp"
#include "engine/world/projectile.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace crusader {

namespace {

constexpr std::uint16_t kTypesEndedBySetActivity[] = {0x204, ActorAnimProcess::kType, 0x258, 0x257,
                                                      0x255, AttackProcess::kType,    0x25F, 0x25E};

// 0..n-1: random_range(n) (SYSTEM.C 12D8:04FF), and the C library's
// rand() * n / 0x8000 (core/random.hpp), each where the original uses it.
int rnd(World& w, int n) { return random_range(w.state.random_state, static_cast<std::uint16_t>(n)); }
int crt_rnd(World& w, int n) { return crt_rand_below(w.state.crt_rand_state, n); }

std::uint16_t word_at(std::span<const std::uint8_t> r, std::size_t at) {
    return static_cast<std::uint16_t>(r[at] | r[at + 1] << 8);
}

void set_word(std::span<std::uint8_t> r, std::size_t at, std::uint16_t v) {
    r[at] = static_cast<std::uint8_t>(v);
    r[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

bool kneeling(const World& w, ObjId id) {
    const auto r = w.npc_record(id);
    return !r.empty() && (r[npc::kFlags13] & 0x04);
}

// NPC_10f0_3b57: no weapon, or a weapon that keeps the actions.
bool weapon_keeps_actions(const World& w, ObjId id) {
    const Item* weapon = w.get(w.active_weapon(id));
    return !weapon || dtable::weapon(weapon->shape).keeps_actions();
}

// NPC_10f0_1f9a: hit within the last 120 ticks.
// @orig 10F0:1F9A
bool recently_hit(const World& w, ObjId id, std::uint32_t now) {
    const auto r = w.npc_record(id);
    if (r.empty()) return false;
    const std::uint32_t hit = r[npc::kLastHit] | r[npc::kLastHit + 1] << 8 | r[npc::kLastHit + 2] << 16 |
                              static_cast<std::uint32_t>(r[npc::kLastHit + 3]) << 24;
    return hit != 0 && now <= hit + 120;
}

// Directions of the action's animation: 16, or 8 (header byte 3 bit 6).
int action_directions(const World& w, const AnimDat& anims, ObjId id, std::uint16_t action) {
    const Item* item = w.get(id);
    const AnimAction* a = item ? anims.action(item->shape, action) : nullptr;
    return a && a->sixteen_directions() ? 16 : 8;
}

int chebyshev3(const Item& a, const Item& b) {
    return std::max({std::abs(static_cast<std::int16_t>(a.x - b.x)), std::abs(static_cast<std::int16_t>(a.y - b.y)),
                     std::abs((a.z & 0xFF) - (b.z & 0xFF))});
}

}  // namespace

bool in_combat(const World& world, ObjId id) {
    const auto r = world.npc_record(id);
    return !r.empty() && (r[npc::kCombatFlags] & 0x08);
}

bool weapon_drawn(const World& world, ObjId id) {
    const auto r = world.npc_record(id);
    return !r.empty() && (r[npc::kStance] & 0x02);
}

// @orig 10F0:1F40
bool in_combat_stance(const World& world, ObjId id) {
    const auto r = world.npc_record(id);
    const std::uint16_t a = r.empty() ? 0 : static_cast<std::uint16_t>(r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8);
    return a != 1 && a != 0 && a != 0x3A && a != 0xB && a != 0x10;
}

bool controlled_npc_still(const World& world, std::uint32_t now) {
    return world.state.player_moved_at == 0 || now >= world.state.player_moved_at + world.state.player_move_window;
}

// @orig 1138:2C02
void set_controlled_npc(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc) {
    const ObjId old = world.state.controlled_npc;
    if (old == npc) return;
    world.state.aim.angle = 1000;
    // DS:2864: a shape without action 1 (ANIMCACH_1178_0944) never aims.
    if (const Item* next = world.get(npc); npc != 0 && next) world.state.aim.no_stand = !anims.action(next->shape, 1);
    if (old != 0) {
        if (world.get(old) && !world.is_dead(old) && in_combat(world, old)) clear_in_combat(kernel, world, anims, old);
        leave_aim_mode(kernel, world);
    }
    world.state.controlled_npc = npc;
    world.state.camera_target = npc;
    const Item* item = world.get(npc);
    if (npc == 0 || !item) return;
    if (npc != 1) {
        kernel.fail_processes(npc, Kernel::kAnyType, 1);
        if (in_combat(world, npc)) clear_in_combat(kernel, world, anims, npc);
    }
    world.set_camera(item->x, item->y, item->z & 0xFF);
}

// @orig 10F0:31A9
void clear_in_combat(Kernel& kernel, World& world, const AnimDat& anims, ObjId id) {
    auto r = world.npc_record(id);
    if (r.empty()) return;
    r[npc::kCombatFlags] &= static_cast<std::uint8_t>(~0x08);
    const Item* item = world.get(id);
    const bool here = item && (item->flags & kItemFastArea) && id != world.state.controlled_npc;
    if (!here) return;
    if (r[npc::kStance] & 0x02) do_anim(kernel, world, anims, id, 0xB, kDirCurrent, 10000, false);
    kernel.fail_processes(id, AttackProcess::kType, 1);
}

// @orig 10F0:2D02
void set_activity(Kernel& kernel, World& world, const AnimDat& anims, ObjId id, std::uint16_t activity) {
    auto r = world.npc_record(id);
    if (r.empty() || world.is_dead(id) || (r[npc::kStance] & 0x01)) return;
    if (id == world.state.controlled_npc) return;
    if (activity >= 0x70 && activity <= 0x72) activity = word_at(r, npc::kDefaultActivity + 2 * (activity - 0x70));
    if (world.state.difficulty == 4 && rnd(world, 2) == 0) {
        if (activity == 5) activity = 10;
        else if (activity == 9) activity = 11;
    }
    const std::uint16_t current = word_at(r, npc::kActivity);
    if (activity == 0 || current == activity) return;
    if (in_combat(world, id) && activity != 0xC) return;
    set_word(r, npc::kPreviousActivity, current);
    set_word(r, npc::kActivity, activity);
    if (in_combat(world, id)) clear_in_combat(kernel, world, anims, id);
    const Item* item = world.get(id);
    if (!item || !(item->flags & kItemFastArea)) return;
    for (std::uint16_t type : kTypesEndedBySetActivity) kernel.fail_processes(id, type, 1);

    switch (activity) {
    case 5: case 9: case 10: case 0xB: case 0xC: {
        r[npc::kCombatFlags] |= 0x08;  // NPC_10f0_3183
        auto process = std::make_unique<AttackProcess>(world, anims, id);
        AttackProcess& attack = *process;
        if (current == 8) attack.set_from_activity_8();
        if (activity == 0xC) attack.set_timed(kernel.ticks(), crt_rnd(world, 10));
        const Pid pid = kernel.add(std::move(process));
        // Constructor (ATTACK_1110_0000), the parts needing the kernel.
        if (word_at(r, npc::kTarget) != world.state.controlled_npc && world.state.controlled_npc == 1)
            set_target(kernel, world, anims, id, 1);
        r[npc::kLongBurst] = 0;
        if (anims.action(item->shape, 0)) {
            set_pose(world, anims, id, 0);
            r[npc::kStance] &= static_cast<std::uint8_t>(~0x02);
        }
        Pid anim = 0;
        if (activity == 9 || activity == 0xB) {
            r[npc::kStance] |= 0x02;
            attack.set_stationary();
            anim = do_anim(kernel, world, anims, id, 7, kDirCurrent, 10000, false);
        } else {
            r[npc::kStance] &= static_cast<std::uint8_t>(~0x02);
            anim = do_anim(kernel, world, anims, id, 0, kDirCurrent, 10000, false);
        }
        if (anim != 0 && kernel.get(pid)) kernel.get(pid)->wait_for(anim);
        if (activity == 10 || activity == 0xB) attack.set_cautious();
        break;
    }
    default:
        start_activity_process(kernel, world, anims, id, activity);  // TYPE_1108_*
        break;
    }
}

// @orig 10F0:1E71
void set_target(Kernel& kernel, World& world, const AnimDat& anims, ObjId id, ObjId target) {
    auto r = world.npc_record(id);
    if (r.empty() || world.is_dead(id) || id == 1 || id == world.state.controlled_npc) return;
    if (!in_combat(world, id)) {
        kernel.fail_processes(id, AttackProcess::kType, 1);
        set_activity(kernel, world, anims, id, 5);
    }
    set_word(r, npc::kTarget, target);
}

// The NPC part of Item_I_enterFastArea (ITEM.C 2CA8).
// @orig 10A8:2B30
void restart_activity(Kernel& kernel, World& world, const AnimDat& anims, ObjId id) {
    auto r = world.npc_record(id);
    const Item* item = world.get(id);
    if (r.empty() || !item) return;
    const std::uint16_t activity = word_at(r, npc::kActivity);
    set_word(r, npc::kPreviousActivity, 0);
    set_word(r, npc::kActivity, 0);
    if (anims.action(item->shape, 0)) set_pose(world, anims, id, 0);
    r[npc::kStance] &= static_cast<std::uint8_t>(~0x02);
    r[npc::kCombatFlags] &= static_cast<std::uint8_t>(~0x08);
    set_activity(kernel, world, anims, id, activity);
}

// @orig 1150:009C
Pid turn_to(Kernel& kernel, World& world, const AnimDat& anims, ObjId id, int dir, bool always, bool relaxed) {
    const Item* item = world.get(id);
    if (!item) return 0;
    int current = world.npc_direction(id) & 0xF;
    const int left = (current - 1) & 0xF, right = (current + 1) & 0xF;
    std::uint16_t action = 0;
    if (kneeling(world, id)) {
        action = 0x2E;
    } else if (!in_combat(world, id) || relaxed) {
        const auto r = world.npc_record(id);
        action = !r.empty() && (r[npc::kStance] & 0x01) ? 0x3C : 0;
    } else if (id == 1) {
        const Item* weapon = world.get(world.active_weapon(id));
        action = weapon && dtable::weapon(weapon->shape).keeps_actions() ? 4 : 0x25;
    } else {
        action = 4;
    }
    const bool sixteen = action_directions(world, anims, id, action) == 16;
    dir &= 0xF;
    if (!sixteen) {
        if (current & 1) current = (current + 1) & 0xF;
        if (dir & 1) dir = (dir + 1) & 0xF;
    }
    Pid last = 0;
    if (current == dir || (!always && (dir == left || dir == right))) return last;
    const int down = current > dir ? current - dir : current + 16 - dir;  // turning anticlockwise
    const int up = 16 - down;
    const bool clockwise = up < down;
    const int steps = (clockwise ? up : down) + 1;
    for (int i = 0; i < steps; i += sixteen ? 1 : 2) {
        const int d = clockwise ? (current + i) & 0xF : (current - i) & 0xF;
        last = do_anim(kernel, world, anims, id, action, d, 10000, false);
    }
    return last;
}

// @orig 1130:1755
int fire_distance(const World& world, const AnimDat& anims, ObjId shooter, ObjId target, int dir, int dx, int dy,
                  int dz, bool* muzzle_found) {
    if (muzzle_found) *muzzle_found = false;
    const Item* from = world.get(shooter);
    if (!from) return 0;
    std::array<World::Point, 2> offsets{World::Point{dx, dy, dz}, {}};
    int tries = 1;
    if (is_npc_id(shooter)) {
        std::uint16_t action = 8;
        if (kneeling(world, shooter)) action = weapon_keeps_actions(world, shooter) ? 0x2A : 0x2B;
        else if (shooter == 1) action = weapon_keeps_actions(world, shooter) ? 8 : 0xD;
        const AnimAction* a = anims.action(from->shape, action);
        int found = 0;
        for (std::size_t i = 0; a && i < a->frame_count() && found < 2; ++i) {
            const AnimFrame* f = a->frame(dir & 0xF, i);
            if (!f || !f->fires_weapon()) continue;
            offsets[found++] = {f->muzzle_a * 16, f->muzzle_b * 16, f->muzzle_z * 8};
        }
        if (found == 0) return 0;
        tries = found;
    }
    if (muzzle_found) *muzzle_found = true;
    for (int i = 0; i < tries; ++i) {
        const World::Point start{from->x + offsets[i].x, from->y + offsets[i].y, ((from->z & 0xFF) + offsets[i].z) & 0xFF};
        const World::Point aim = aim_point(world, shooter, target);
        const auto distance = [&] {
            return std::max(std::abs(static_cast<std::int16_t>(start.x - aim.x)),
                            std::abs(static_cast<std::int16_t>(start.y - aim.y))) >> 4;
        };
        ObjId here = 0;
        if (!position_free(world, kProjectileShape, start, &here)) {
            if (here == target) return distance();
            continue;
        }
        const SweepResult path = sweep(world, kProjectileShape, shooter, start, aim, kSweepNoGround);
        if (path.blocker == 0 || path.blocker == target) return distance();
    }
    return 0;
}

// @orig 1110:0000
AttackProcess::AttackProcess(World& world, const AnimDat& anims, ObjId id) : world_(world), anims_(anims) {
    set_owner(id);
    set_type(kType);
    // Delays (ATTACK_1110_0000): by weapon and difficulty.
    if (const Item* weapon = world.get(world.active_weapon(id))) {
        const std::uint16_t s = weapon->shape;
        if (s == 0x386 || s == 0x388 || s == 0x38E) {
            first_delay_ = 0x3C;
            const Item* self = world.get(id);
            shot_delay_ = world.state.difficulty == 1   ? 0x78
                          : world.state.difficulty == 2 ? 0x5A
                          : self && self->shape == 0x3AC ? 0x0F
                                                          : 0x3C;
        } else {
            first_delay_ = 0x1E;
            shot_delay_ = world.state.difficulty == 1   ? first_delay_
                          : world.state.difficulty == 2 ? 0x14
                          : world.state.difficulty == 3 ? 0x0F
                                                        : 0;
        }
    }
}

void AttackProcess::set_timed(std::uint32_t now, int seconds_minus_8) {
    timed_ = true;
    timeout_ = now + static_cast<std::uint32_t>(seconds_minus_8 + 8) * 60;
}

// @orig 1110:03AA
bool AttackProcess::recently_started(std::uint32_t now) {
    const std::uint32_t limit = double_time_ ? 240 : 120;
    if (started_ != 0) {
        reference_ = started_;
        if (now <= started_ + limit) return true;
    }
    started_ = 0;
    double_time_ = false;
    return false;
}

// @orig 1110:0334
bool AttackProcess::bark_timer(std::uint32_t now) {
    if (bark_time_ == 0 || now >= bark_time_ + 0x1E0) {
        bark_time_ = now;
        return true;
    }
    return false;
}

void AttackProcess::note_sighting(std::uint32_t now) {
    seen_ = now;
    sighted_ = true;
}

bool AttackProcess::sighting_old(std::uint32_t now) const {
    return now >= seen_ + (double_time_ ? 120u : 60u);
}

// @orig 1110:13B7
bool AttackProcess::can_hit(int dir, std::uint32_t now) {
    if (recently_started(now)) return true;
    if (sighted_) return true;
    const auto r = world_.npc_record(owner());
    return fire_distance(world_, anims_, owner(), r.empty() ? 0 : word_at(r, npc::kTarget), dir) != 0;
}

// @orig 1110:141B
void AttackProcess::approach() {
    // ATTACK_1110_141b: walk towards the target (at most 12 steps, stopping
    // within 0x40), waited for.
    double_time_ = false;
    sighted_ = false;
    moved_ = true;
    const ObjId target = world_.npc_word(owner(), npc::kTarget);
    const Pid pid = walk_to(kernel(), world_, anims_, owner(), target, {}, 0xC, 0x40, false);
    if (pid != 0) wait_for(pid);
}

// @orig ?: ATTACK.C helper (segment 1110), not split yet
int AttackProcess::target_direction(ObjId target, int dirs) const {
    const Item* self = world_.get(owner());
    const Item* t = world_.get(target);
    if (!self || !t) return 0;
    const int d = closest_direction_in_range(self->x, self->y, t->x, t->y, dirs, 0, dirs - 1);
    return dirs == 8 ? d * 2 : d;
}

// @orig ?: ATTACK.C barks (segment 1110), not split yet
bool AttackProcess::bark(std::uint16_t shape) {
    // Sound tables DS:1FFD..2031.
    static constexpr std::uint16_t kGuard[] = {0x15, 0x78, 0x80, 0x83, 0xDC, 0xDD};    // shapes 0x2FD, 0x319
    static constexpr std::uint16_t kShape384[] = {0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xE7};
    static constexpr std::uint16_t kShape371[] = {0xFC, 0xFD, 0xFE, 0xC8};
    static constexpr std::uint16_t kShape4D1[] = {0xCC, 0xCD, 0xCE, 0xCF};  // and 0x528
    static constexpr std::uint16_t kShape1B4[] = {0xC7, 0xCA, 0xC9};
    std::int16_t sound = -1;
    switch (shape) {
    case 0x2FD: case 0x319: sound = static_cast<std::int16_t>(kGuard[crt_rnd(world_, 6)]); break;
    case 0x384: sound = static_cast<std::int16_t>(kShape384[crt_rnd(world_, 6)]); break;
    case 0x371: sound = static_cast<std::int16_t>(kShape371[crt_rnd(world_, 4)]); break;
    case 0x4D1: case 0x528: sound = static_cast<std::int16_t>(kShape4D1[crt_rnd(world_, 4)]); break;
    case 0x1B4: sound = static_cast<std::int16_t>(kShape1B4[crt_rnd(world_, 3)]); break;
    default: break;
    }
    if (sound == -1) return false;
    play_bark(sound);
    return true;
}

void AttackProcess::play_bark(std::int16_t sfx) {
    sound_ = sfx;
    if (Audio* audio = world_.audio()) audio->sources.play(world_, sfx, owner());
}

// @orig ?: ATTACK.C barks (segment 1110), not split yet
bool AttackProcess::bark_playing() {
    if (sound_ == -1) return false;
    if (Audio* audio = world_.audio(); audio && audio->sources.is_playing(sound_, owner())) return true;
    sound_ = -1;
    return false;
}

// @orig ?: ATTACK.C shot (segment 1110), not split yet
void AttackProcess::fire(std::uint32_t now, ObjId weapon_id) {
    const ObjId id = owner();
    auto r = world_.npc_record(id);
    int burst = 1;
    if (const Item* weapon = world_.get(weapon_id)) {
        burst = dtable::weapon(weapon->shape).burst();
        if (burst >= 3 && ((double_time_ && rnd(world_, 2) == 0) || rnd(world_, 5) == 0)) {
            r[npc::kLongBurst] = 1;
            burst *= 4;
        }
    }
    const Pid pid = do_anim(kernel(), world_, anims_, id, 8, kDirCurrent, 0, false);
    last_shot_ = now;
    if (started_ == 0) started_ = now;
    burst_left_ = burst;
    if (pid == 0) return;
    wait_for(pid);
    if (--burst_left_ != 0) listening_ = pid;
}

// @orig 1110:22CA
void AttackProcess::next_burst_shot() {
    // Message handler 1110:22CA, on the end of the animation listened to.
    const ObjId id = owner();
    const auto r = world_.npc_record(id);
    const ObjId target = r.empty() ? 0 : word_at(r, npc::kTarget);
    // The burst ends with the target's death, or when the shot animation
    // listened to failed (its flag 0x20).
    if (world_.is_dead(target) || wake_failed()) {
        burst_left_ = 0;
        return;
    }
    // The direction is not doubled for 8-direction actions, as in the original.
    const Item* self = world_.get(id);
    const Item* t = world_.get(target);
    const int dirs = action_directions(world_, anims_, id, 8);
    const int dir = self && t ? closest_direction_in_range(self->x, self->y, t->x, t->y, dirs, 0, dirs - 1) : 0;
    const Pid pid = do_anim(kernel(), world_, anims_, id, 8, dir, 1, false);
    last_shot_ = kernel().ticks();
    if (pid == 0) return;
    wait_for(pid);
    if (--burst_left_ != 0) {
        listening_ = pid;
        return;
    }
    world_.npc_record(id)[npc::kLongBurst] = 0;
    if (self && self->shape != 0x338 && self->shape != 0x4E6 && !controlled_npc_still(world_, kernel().ticks()) &&
        rnd(world_, 2) == 0) {
        sleep(60);
    }
}

// @orig 1110:06B9 1110:14F6
void AttackProcess::run() {
    if (listening_ != 0) {
        listening_ = 0;
        next_burst_shot();
        return;
    }
    Kernel& k = kernel();
    const ObjId id = owner();
    Item* self = world_.get(id);
    auto r = world_.npc_record(id);
    if (!self || r.empty()) return;
    const int dir = world_.npc_direction(id) & 0xF;
    const std::uint32_t now = k.ticks();
    const std::uint16_t shape = self->shape;
    if (actor_busy(k, id)) return;
    const ObjId weapon = world_.active_weapon(id);

    const ObjId controlled = world_.state.controlled_npc;
    if (word_at(r, npc::kTarget) != controlled) set_target(k, world_, anims_, id, controlled != 0 ? controlled : 1);
    const ObjId target = word_at(r, npc::kTarget);
    const Item* t = world_.get(target);
    if (!t || (t->z & 0xFF) == 0xFE || world_.is_dead(target) || !world_.is_partly_in_combat_zone(*t)) {
        set_target(k, world_, anims_, id, 0);
        if (stationary_) return;
        // Wander to a random point within 0x200 (12 steps at most, stopping
        // within 0x40, turning to the point at the end).
        World::Point goal = world_.location(*self);
        goal.x += rnd(world_, 0x401) - 0x200;
        goal.y += rnd(world_, 0x401) - 0x200;
        moved_ = true;
        const Pid pid = walk_to(k, world_, anims_, id, 0, goal, 0xC, 0x40, true);
        if (pid != 0) wait_for(pid);
        return;
    }

    const std::uint16_t stance_action = in_combat(world_, id) ? 4 : 0;
    int dirs = action_directions(world_, anims_, id, stance_action);
    int target_dir = -1;

    if (timed_) {
        if (timeout_ < now) {
            timed_ = false;
            // NPC_10f0_2cc6: activity 5 without the activity change.
            set_word(r, npc::kPreviousActivity, word_at(r, npc::kActivity));
            set_word(r, npc::kActivity, 5);
        } else if (stance_action == 4) {
            if (rnd(world_, 3) == 0) {
                if (rnd(world_, 3) == 0) {
                    turn_to(k, world_, anims_, id, crt_rnd(world_, 16), true, !weapon_drawn(world_, id));
                    return;
                }
                if (!weapon_drawn(world_, id)) return;
                if (target_direction(target, dirs) != dir) return;
                if (bark_playing()) return;
                int burst = 1;
                if (const Item* w = world_.get(weapon)) {
                    burst = dtable::weapon(w->shape).burst();
                    if (burst < 3) {
                        burst = 1;
                    } else if ((double_time_ && rnd(world_, 2) == 0) || rnd(world_, 5) == 0) {
                        r[npc::kLongBurst] = 1;
                        burst *= 4;
                    }
                }
                const Pid pid = do_anim(k, world_, anims_, id, 8, kDirCurrent, 0, false);
                last_shot_ = now;
                if (started_ == 0) started_ = now;
                burst_left_ = burst;
                if (pid == 0) return;
                wait_for(pid);
                if (--burst_left_ != 0) listening_ = pid;
                return;
            }
            const std::uint16_t last = word_at(r, npc::kLastAnim);
            do_anim(k, world_, anims_, id, last == 0xB || last == 0x10 ? 7 : 0xB, kDirCurrent, 0, false);
            return;
        }
    }

    if (target_dir == -1) target_dir = target_direction(target, dirs);
    const int distance = chebyshev3(*self, *t);
    const bool steep = std::abs((self->z & 0xFF) - (t->z & 0xFF)) * 2 >= distance;
    const bool visible = world_.is_mostly_in_combat_zone(*self);
    if ((!stationary_ && !visible) || steep) {
        approach();
        return;
    }

    if (target_dir == dir) {
        const int roll = crt_rnd(world_, 10);
        if (!visible) {
            sleep(20);
            return;
        }
        const bool go = moved_ || recently_started(now) || controlled_npc_still(world_, now) || roll < 3 ||
                        (cautious_ && roll < 4);
        if (!go) {
            sleep(20);
            return;
        }
        moved_ = false;
        const bool hit_possible = recently_hit(world_, id, now) || can_hit(target_dir, now);
        if (sighted_ && (rnd(world_, 5) == 0 || sighting_old(now))) sighted_ = false;
        if (!hit_possible) {
            if (!stationary_) {
                approach();
                return;
            }
            sleep(15);
            return;
        }
        if (!barked_) {
            barked_ = true;
            if (rnd(world_, 3) == 0 && bark(shape)) return;
        } else if (bark_timer(now) && (shape == 0x2DF || shape == 0x383)) {
            static constexpr std::uint16_t kShape2DF[] = {0x82, 0x84, 0x85};
            static constexpr std::uint16_t kShape383[] = {0x9B, 0x9C, 0x9D, 0x9E, 0x9F};
            play_bark(static_cast<std::int16_t>(shape == 0x2DF ? kShape2DF[crt_rnd(world_, 3)] : kShape383[crt_rnd(world_, 5)]));
            return;
        }
        if (!weapon_drawn(world_, id)) {
            started_ = now;
            do_anim(k, world_, anims_, id, 7, kDirCurrent, 10000, true);
            return;
        }
        if (bark_playing()) return;
        const std::uint32_t since_start = now - reference_;
        if (since_start <= first_delay_) {
            if (since_start != 0) sleep(first_delay_ - since_start);
            return;
        }
        const std::uint32_t since_shot = now - last_shot_;
        if (since_shot <= shot_delay_) {
            sleep(shot_delay_ - since_shot);
            return;
        }
        fire(now, weapon);
        return;
    }

    bool hit_possible = false;
    if (recently_started(now) || forced_sight_) {
        note_sighting(now);
        forced_sight_ = false;
        hit_possible = true;
    } else {
        if (dirs != 16) {
            dirs = 16;
            target_dir = target_direction(target, 16);
        }
        hit_possible = fire_distance(world_, anims_, id, target, target_dir) != 0;
        if (hit_possible) note_sighting(now);
    }
    const bool drawn = weapon_drawn(world_, id);
    if (!drawn && hit_possible) {
        started_ = now;
        do_anim(k, world_, anims_, id, 7, kDirCurrent, 10000, true);
        return;
    }
    if (hit_possible || stationary_) {
        turn_to(k, world_, anims_, id, target_dir, true, !drawn);
        return;
    }
    approach();
}

}  // namespace crusader
