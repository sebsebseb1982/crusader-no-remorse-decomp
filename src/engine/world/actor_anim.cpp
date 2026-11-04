#include "engine/world/actor_anim.hpp"

#include "core/dtable.hpp"
#include "core/random.hpp"
#include "engine/audio/audio.hpp"
#include "engine/world/activity.hpp"
#include "engine/world/aim_mode.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/gravity.hpp"
#include "engine/world/item_damage.hpp"
#include "engine/world/item_selection.hpp"
#include "engine/usecode/item_intrinsics.hpp"

#include <algorithm>

namespace crusader {

namespace {

// The test run of ANIMPRIM.C 016F: a process with a test position (+0x50)
// plays the action from `pos` without showing anything, and tells whether it
// would end (or reach the end of its half stride) without a blocked step and
// without the actor stumbling. `pos` becomes where it stopped: after a
// blocked walking step, the last place reached; after another blocked step,
// the place it tried to reach (1908, the test branch).
// @orig 1158:016F 1158:1908
bool action_fits(const World& world, const AnimAction& a, std::uint16_t action, ObjId actor, int dir,
                 World::Point& pos) {
    const auto record = world.npc_record(actor);
    const Item* item = world.get(actor);
    if (!item || record.empty()) return false;
    const int count = static_cast<int>(a.frame_count());
    const int flags = a.flags();
    const bool second_half = !(record[npc::kAnimFlags] & 0x04);
    int index = 0;
    if (flags & 0x01) index = second_half ? count / 2 : ((flags & 0x04) ? count - 1 : 0);
    else if ((flags & 0x04) && world.npc_word(actor, npc::kLastAnim) == action) index = 1;
    const int move_dir = a.turned() ? (dir + 4) & 0xF : dir;
    for (int n = 0; n < count; ++n, ++index) {
        if (flags & 0x01) {
            if (second_half) {
                if ((flags & 0x04) && index == count - 1) return true;
            } else {
                if ((index == count / 2 && count != 3) || (index == 2 && count == 3)) return true;
                if (index == count) index = (flags & 0x04) ? 1 : 0;
            }
        }
        if (index >= count) return true;
        const AnimFrame* f = a.frame(dir, static_cast<std::size_t>(index));
        if (!f) return false;
        const int d = f->distance * 2;
        const World::Point target{pos.x + kDirectionDx[move_dir] * d, pos.y + kDirectionDy[move_dir] * d,
                                  std::max(0, pos.z + f->delta_z)};
        if (f->ground_check) {
            SweepResult r = try_move(world, actor, pos, target, kSweepSupportOnly | kSweepStepUp);
            if (!r.ok && r.blocker == 0) r = try_move(world, actor, pos, target, kSweepNoGround);
            if (!r.ok) return false;
            pos = r.end;
            if (!(support_mask(world, item->shape, pos.x, pos.y, pos.z, true, actor) & 0x10)) return false;
        } else {
            SweepResult r = sweep(world, item->shape, actor, pos, target, kSweepNoGround);
            World::Point tried = target;
            if (!r.ok && r.blocker != 0) {
                tried = {pos.x, pos.y, target.z};
                r = sweep(world, item->shape, actor, pos, tried, kSweepNoGround);
            }
            if (!r.ok) {
                pos = tried;
                return false;
            }
            pos = r.end;
        }
    }
    return true;
}

bool is_roll(std::uint16_t action) { return action == 0x17 || action == 0x18 || action == 0x3D || action == 0x3E; }

// The actions that the controlled NPC tests before playing them (016F).
// @orig 1158:016F
bool tested_at_start(std::uint16_t action) {
    switch (action) {
    case 1: case 2: case 3: case 0x17: case 0x18: case 0x22: case 0x23: case 0x24: case 0x26: case 0x2C:
    case 0x2D: case 0x30: case 0x31: case 0x3D: case 0x3E:
        return true;
    default:
        return false;
    }
}

}  // namespace

// ANIMPRIM.C 016F, for the controlled NPC: the action is tried from where it
// stands. A roll that does not fit is tried from 16, 32 and 48 units ahead
// and behind (in that order, where a walking move gets there); the actor
// moves to the first place it fits from, else to the first from which it
// would roll more than 48 units, else the action becomes 0x2E. A walk or run
// is tried from 16 to 80 units to either side (left first, DS:2A42); without
// a place, the action becomes 0x40 (nothing, shape 1398), 0x2E (kneeling),
// 4 with the aim mode (after an action other than standing, walking, 0x3A,
// 0xB, 0x10) or 0, remapped for the weapon.
// @orig 1158:016F
std::uint16_t fit_start(Kernel& kernel, World& world, const AnimDat& anims, ObjId actor, std::uint16_t action,
                        int dir) {
    const Item* item = world.get(actor);
    const AnimAction* a = item ? anims.action(item->shape, action) : nullptr;
    if (!a) return action;
    const World::Point start{item->x, item->y, item->z};
    const auto offset = [&](int d, int distance) {
        return World::Point{start.x + kDirectionDx[d] * distance, start.y + kDirectionDy[d] * distance, start.z};
    };
    const auto move = [&](const SweepResult& r) {
        leave_surface(kernel, world, actor);
        finish_move(world, actor, r);
    };
    World::Point at = start;
    if (action_fits(world, *a, action, actor, dir, at)) return action;
    if (is_roll(action)) {
        int reach[7] = {};
        for (int i = 1; i < 7; ++i) {
            const int d = (i & 1) ? dir : (dir + 8) & 0xF;
            const World::Point there = offset(d, ((i + 1) / 2) << 4);
            const SweepResult r = try_move(world, actor, start, there, kSweepSupportOnly);
            if (!r.ok) continue;
            at = there;
            if (action_fits(world, *a, action, actor, dir, at)) {
                move(r);
                return action;
            }
            reach[i] = std::max(std::abs(start.x - at.x), std::abs(start.y - at.y));
        }
        for (int i = 1; i < 7; ++i) {
            if (reach[i] <= 48) continue;
            const int d = (i & 1) ? dir : (dir + 8) & 0xF;
            move(try_move(world, actor, start, offset(d, ((i + 1) / 2) << 4), kSweepSupportOnly));
            return action;
        }
        return 0x2E;
    }
    // DS:2A42: 16, 16, 32, 32, 48, 48, 64, 64, 80, 80.
    for (int j = 0; j < 10; ++j) {
        const int side = (dir + ((j & 1) ? 4 : 12)) & 0xF;
        const World::Point there = offset(side, (j / 2 + 1) * 16);
        const SweepResult r = try_move(world, actor, start, there, kSweepSupportOnly);
        if (!r.ok) continue;
        at = there;
        if (!action_fits(world, *a, action, actor, dir, at)) continue;
        move(r);
        return action;
    }
    if (item->shape == 1398) return 0x40;
    const auto record = world.npc_record(actor);
    std::uint16_t fallback = 0;
    if (!record.empty() && (record[npc::kFlags13] & 0x04)) {
        fallback = 0x2E;
    } else if (in_combat_stance(world, actor)) {
        enter_aim_mode(kernel, world, anims);
        fallback = 4;
    }
    return remap_action(world, actor, fallback);
}

// @orig 1160:01F0
void special_frame(Kernel& kernel, World& world, const AnimDat& anims, ObjId actor) {
    const Item* item = world.get(actor);
    const auto r = world.npc_record(actor);
    if (!item || r.empty()) return;
    const int last = r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8;
    if (last == 0x1E || last == 0x1F) {
        // Looking aside: seeing the controlled NPC, the NPC takes activity
        // 10 one time in four, else attacks (5).
        if (sees_controlled(world, anims, actor, true)) {
            set_activity(kernel, world, anims, actor, random_range(world.state.random_state, 4) == 0 ? 10 : 5);
        }
        return;
    }
    switch (item->shape) {
    case 0x385:  // robots 901 and 1254, dying: a large explosion with splash damage, which they survive
    case 0x4E6:
        explode(kernel, world, anims, actor, 2, false, true);
        if (Item* i = world.get(actor)) i->flags &= static_cast<std::uint16_t>(~0x0140);
        return;
    case 0x576:  // the burning and the electrocuted: a scream, unless one plays
    case 0x596: {
        // DS:2B12 (record +0x5A bit 2 clear) and DS:2B1A.
        static constexpr int kScreams[] = {101, 102, 103};
        static constexpr int kOtherScreams[] = {11, 10};
        const bool other = r[0x5A] & 0x04;
        const std::span<const int> table = other ? std::span<const int>(kOtherScreams) : std::span<const int>(kScreams);
        const int sfx = table[static_cast<std::size_t>(crt_rand_below(world.state.crt_rand_state, static_cast<int>(table.size())))];
        Audio* audio = world.audio();
        if (!audio) return;
        for (const int s : table) {
            if (audio->sources.is_playing(s, actor)) return;
        }
        audio->sources.play(world, sfx, actor);
        return;
    }
    default:
        return;
    }
}

// @orig 1158:0000
ActorAnimProcess::ActorAnimProcess(World& world, const AnimDat& anims, ObjId actor, std::uint16_t action, int dir,
                                   Pid previous)
    : world_(world), anims_(anims), action_(action), dir_(dir), previous_(previous) {
    set_owner(actor);
    set_type(kType);
    // ANIMPRIM.C 0000: the action's flags are read when the process is made.
    const Item* item = world.get(actor);
    const AnimAction* a = item ? anims.action(item->shape, action) : nullptr;
    destroys_actor_ = a && a->destroys_actor();
}

void ActorAnimProcess::on_added() {
    if (previous_ != 0) wait_for(previous_);
}

// @orig 1158:31DA
void ActorAnimProcess::set_direction(int dir) {
    // ANIMPRIM.C 31DA: the codes 0x10..0x12 start from the actor's direction;
    // 8-direction actions use even directions and turn by 2.
    const Item* item = world_.get(owner());
    const AnimAction* a = item ? anims_.action(item->shape, action_) : nullptr;
    const bool sixteen = a && a->sixteen_directions();
    int d = (dir >= kDirCurrent) ? world_.npc_direction(owner()) : dir;
    if (!sixteen) {
        if (d & 1) d = (d - 1) & 0xF;
        if (dir == kDirTurnRight) d = (d + 2) & 0xF;
        else if (dir == kDirTurnLeft) d = (d - 2) & 0xF;
    } else if (dir == kDirTurnRight) {
        d = (d + 1) & 0xF;
    } else if (dir == kDirTurnLeft) {
        d = (d - 1) & 0xF;
    }
    dir_ = d;
    if (auto record = world_.npc_record(owner()); !record.empty()) record[npc::kDirection] = static_cast<std::uint8_t>(d);
}

// @orig 1158:016F
const AnimAction* ActorAnimProcess::start() {
    // ANIMPRIM.C 016F / 17B2.
    set_direction(dir_);
    auto record = world_.npc_record(owner());
    if (record.empty()) return nullptr;
    if (owner() == world_.state.controlled_npc && tested_at_start(action_) && !(record[npc::kCombatFlags] & 0x20)) {
        action_ = fit_start(kernel(), world_, anims_, owner(), action_, dir_);
        if (action_ >= 0x40) return nullptr;
    }
    const Item* item = world_.get(owner());
    const AnimAction* found = item ? anims_.action(item->shape, action_) : nullptr;
    if (!found || found->frame_count() == 0) return nullptr;
    const AnimAction& a = *found;
    destroys_actor_ = a.destroys_actor();
    std::uint8_t& index = record[npc::kAnimFrame];
    const std::uint8_t flags = a.flags();
    const auto count = static_cast<std::uint8_t>(a.frame_count());
    const std::uint16_t last = static_cast<std::uint16_t>(record[npc::kLastAnim] | record[npc::kLastAnim + 1] << 8);
    if (flags & 0x01) {
        // Looping: start mid-cycle, or where the previous half stride ended.
        if (!(record[npc::kAnimFlags] & 0x04)) index = count >> 1;
        else index = (flags & 0x04) ? static_cast<std::uint8_t>(count - 1) : 0;
    } else if ((flags & 0x04) && last == action_) {
        index = 1;  // 2 when off screen (frame skipping, not reproduced)
    } else {
        index = 0;
    }
    record[npc::kLastAnim] = static_cast<std::uint8_t>(action_);
    record[npc::kLastAnim + 1] = static_cast<std::uint8_t>(action_ >> 8);
    // The controlled NPC is in combat while it plays a combat action
    // (NPC_10f0_3183 / Actor::I_clrInCombat, which does nothing more for it):
    // this is what shows its weapon (WPNOVLAY.DAT, DLIST.C).
    if (owner() == world_.state.controlled_npc) {
        if (in_combat_stance(world_, owner())) record[npc::kCombatFlags] |= 0x08;
        else record[npc::kCombatFlags] &= static_cast<std::uint8_t>(~0x08);
    }
    return found;
}

// @orig 11D8:17DB
const ActorAnimProcess* ActorAnimProcess::next_queued() const {
    const ActorAnimProcess* next = nullptr;
    kernel().for_each_process([&](const Process& p) {
        auto* anim = dynamic_cast<const ActorAnimProcess*>(&p);
        if (anim && anim->owner() == owner() && anim->previous() == pid()) next = anim;
    });
    return next;
}

// @orig 1158:1908 1158:2F21
void ActorAnimProcess::run() {
    // ANIMPRIM.C 1908: out of the fast area the animation waits, without
    // sleeping, for the actor to come back (Item_I_inFastArea). Played out
    // there, its steps found no floor (the glob items only exist in the fast
    // area): a guard shot out of it stumbled on the first frame of its death
    // and stayed standing for good (TEST15, docs/bugs.md).
    if (const Item* item = world_.get(owner()); item && !(item->flags & kItemFastArea)) return;
    // ANIMPRIM.C 1908: nothing happens while the actor falls.
    if (auto r = world_.npc_record(owner()); !r.empty() && (r[npc::kAnimFlags] & 0x02)) {
        sleep(1);
        return;
    }
    if (!step()) {
        // A blocked step fails the process (vfunc +0x2C, 1158:2F21).
        if (blocked_) fail();
        else terminate();
        return;
    }
    const Item* item = world_.get(owner());
    const AnimAction* a = item ? anims_.action(item->shape, action_) : nullptr;
    sleep(a ? std::max<std::uint8_t>(a->ticks_per_frame(), 1) : 1);
}

// @orig 1158:1908
bool ActorAnimProcess::step() {
    Item* item = world_.get(owner());
    if (!item) return false;
    auto record = world_.npc_record(owner());
    if (record.empty()) return false;
    const bool first = !started_;
    if (first) {
        started_ = true;
        if (!start()) return false;
        item = world_.get(owner());
        if (!item) return false;
    }
    const AnimAction* a = anims_.action(item->shape, action_);
    if (!a || a->frame_count() == 0) return false;
    const std::uint8_t flags = a->flags();
    const int count = static_cast<int>(a->frame_count());
    int index = record[npc::kAnimFrame] + (first ? 0 : 1);

    // ANIMPRIM.C 1908: half strides of looping actions.
    if (flags & 0x01) {
        std::uint8_t& anim_flags = record[npc::kAnimFlags];
        if (!(anim_flags & 0x04)) {
            if ((flags & 0x04) && index == count - 1) {
                const ActorAnimProcess* next = next_queued();
                if (!next || next->action() != action_) {
                    anim_flags |= 0x04;
                    return false;
                }
            }
        } else {
            if ((index == count / 2 && count != 3) || (index == 2 && count == 3)) {
                anim_flags &= ~0x04;
                return false;
            }
            if (index == count) index = (flags & 0x04) ? 1 : 0;
        }
    }
    if (index >= count) {
        if (count != 1) record[npc::kAnimFlags] |= 0x04;
        return false;
    }
    record[npc::kAnimFrame] = static_cast<std::uint8_t>(index);

    const AnimFrame* f = a->frame(dir_, static_cast<std::size_t>(index));
    if (!f) return false;

    // ANIMPRIM.C 1908: the frame's step. A blocked step normally ends the
    // animation (+0x3C, the process fails) before the frame shows; an
    // unstoppable action (flag 0x08) plays its remaining frames in place (+0x3B).
    const World::Point start{item->x, item->y, item->z};
    World::Point reached = start;
    const int move_dir = a->turned() ? (dir_ + 4) & 0xF : dir_;
    const int d = f->distance * 2;
    if (!in_place_ && (d != 0 || f->delta_z != 0)) {
        World::Point target{item->x + kDirectionDx[move_dir] * d, item->y + kDirectionDy[move_dir] * d,
                            std::max(0, item->z + f->delta_z)};
        // In combat, an action flagged 0x80 (shape 0x33C) goes one unit up or
        // down per step towards its target's height (z wraps as a byte).
        if ((flags & 0x80) && in_combat(world_, owner())) {
            const ObjId t = static_cast<ObjId>(record[npc::kTarget] | record[npc::kTarget + 1] << 8);
            if (const Item* ti = t != 0 ? world_.get(t) : nullptr) {
                const int z = item->z & 0xFF, tz = ti->z & 0xFF;
                if (z > tz) target.z = (target.z + 255) & 0xFF;
                else if (z < tz) target.z = (target.z + 1) & 0xFF;
            }
        }
        SweepResult r;
        if (f->ground_check) {
            // Walking move needing support, which may climb steps (ITEM.C 1841
            // in mode 1: a refused step sends no contact events). Refused for
            // lack of ground alone, the step goes over the edge anyway and the
            // fall check below makes the actor stumble.
            r = try_move(world_, owner(), target, kSweepSupportOnly | kSweepStepUp);
            if (!r.ok && r.blocker == 0) r = try_move(world_, owner(), target, kSweepNoGround);
        } else {
            // A plain sweep; against an obstacle only the vertical part is kept.
            r = sweep(world_, item->shape, owner(), start, target, kSweepNoGround);
            if (!r.ok && r.blocker != 0) {
                r = sweep(world_, item->shape, owner(), start, {start.x, start.y, target.z}, kSweepNoGround);
            }
        }
        if (r.ok) {
            // Moved, leaving what it stood on or carried (ITEM.C 2970), with
            // the contact events (ITEMFIND.C 123A).
            leave_surface(kernel(), world_, owner());
            finish_move(world_, owner(), r);
            item = world_.get(owner());
            if (!item) return false;
            reached = r.end;
        } else if (f->ground_check && r.blocker == 0) {
            reached = target;
        } else if (flags & 0x08) {
            in_place_ = true;
            if (f->ground_check) reached = target;
        } else {
            blocked_ = true;
            // The support under the actor is still checked (flag 0x80 aside).
            if (!(flags & 0x80)) fall_if_unsupported(start, f->ground_check ? target : start);
            return false;
        }
    }

    world_.set_frame(*item, f->frame);
    if (f->mirrored) item->flags |= 0x0020;
    else item->flags &= static_cast<std::uint16_t>(~0x0020);
    // The frame's sound, restarted on the actor's voice (I_playSFXCru 0x045).
    if (f->sfx != 0) {
        if (Audio* audio = world_.audio()) audio->sources.retrigger(world_, f->sfx, owner());
    }
    // Flag 0x4000: usecode event 0x0E (calledFromAnim, 10A8:2B01) of the
    // actor's class; the NPC classes start NPCDEATH from it on the last frame
    // of their deaths. The script runs later, as a new process.
    if (f->event && world_.hooks().start_event) {
        world_.hooks().start_event(owner(), kCalledFromAnimEvent);
        if (!world_.get(owner()) || terminated()) return false;
    }
    // Flag 0x1000: the special frames (WPNCACHE_1160_01f0).
    if (f->special) {
        special_frame(kernel(), world_, anims_, owner());
        if (!world_.get(owner()) || terminated()) return false;
    }
    if (f->fires_weapon() && world_.hooks().fire_weapon) {
        world_.hooks().fire_weapon(owner(), f->muzzle_a, f->muzzle_b, f->muzzle_z, dir_);
        if (!world_.get(owner())) return false;
    }
    if (f->ground_check && fall_if_unsupported(start, reached)) return false;
    return true;
}

// @orig 1158:3091
bool ActorAnimProcess::fall_if_unsupported(const World::Point& from, const World::Point& to) {
    // ANIMPRIM.C 3091: when no two opposite quarters of the footprint rest on
    // something (ground down to 9 below), the actor stumbles (action 0x3B,
    // replacing its animations, unless rolling or kneeling) and falls with
    // the step as speed. After a blocked step the support is looked for
    // where the actor stayed.
    const Item* item = world_.get(owner());
    if (!item) return false;
    const World::Point& at = blocked_ ? from : to;
    if (support_mask(world_, item->shape, at.x, at.y, at.z, true, owner()) & 0x10) return false;
    auto record = world_.npc_record(owner());
    const std::uint16_t last =
        record.empty() ? 0 : static_cast<std::uint16_t>(record[npc::kLastAnim] | record[npc::kLastAnim + 1] << 8);
    if (last != 0x3B && last != 0x3A && last != 0x3D && last != 0x3E && last != 0x17 && last != 0x18) {
        do_anim(kernel(), world_, anims_, owner(), 0x3B, kDirCurrent, 10000, true);
    }
    hurl(kernel(), world_, owner(), to.x - from.x, to.y - from.y, to.z - from.z, 2);
    return true;
}

// @orig 1158:2B4D 1158:1908
SimulatedStep simulate_step(const World& world, const AnimDat& anims, ObjId actor, std::uint16_t action, int dir,
                            World::Point& pos, ObjId goal) {
    const Item* item = world.get(actor);
    const auto record = world.npc_record(actor);
    const AnimAction* a = item ? anims.action(item->shape, action) : nullptr;
    if (!a || a->frame_count() == 0 || record.empty()) return {};
    // First frame, as ActorAnimProcess::start would pick it.
    const std::uint8_t flags = a->flags();
    const int count = static_cast<int>(a->frame_count());
    const std::uint16_t last = static_cast<std::uint16_t>(record[npc::kLastAnim] | record[npc::kLastAnim + 1] << 8);
    int index = 0;
    if (flags & 0x01) index = !(record[npc::kAnimFlags] & 0x04) ? count >> 1 : ((flags & 0x04) ? count - 1 : 0);
    else if ((flags & 0x04) && last == action) index = 1;
    index = std::min(index, count - 1);
    dir &= 0xF;
    if (!a->sixteen_directions() && (dir & 1)) dir = (dir - 1) & 0xF;
    const AnimFrame* f = a->frame(dir, static_cast<std::size_t>(index));
    if (!f) return {};
    const int move_dir = a->turned() ? (dir + 4) & 0xF : dir;
    const int d = f->distance * 2;
    if (d == 0 && f->delta_z == 0) return {true, false};
    const World::Point target{pos.x + kDirectionDx[move_dir] * d, pos.y + kDirectionDy[move_dir] * d,
                              std::max(0, pos.z + f->delta_z)};
    const std::uint8_t sweep_flags = f->ground_check ? (kSweepSupportOnly | kSweepStepUp) : kSweepNoGround;
    const SweepResult r = try_move(world, actor, pos, target, sweep_flags);
    bool touched = goal != 0 && r.blocker == goal;
    for (const SweepHit& h : r.hits) touched = touched || (goal != 0 && h.item == goal);
    if (r.ok) {
        pos = r.end;
        return {true, touched};
    }
    return {touched, touched};
}

// CRUSADER_1138_32a8: shows the first frame of `action` in the NPC's direction.
// @orig 1138:32A8
void set_pose(World& w, const AnimDat& anims, ObjId id, std::uint16_t action) {
    Item* item = w.get(id);
    auto r = w.npc_record(id);
    if (!item || r.empty()) return;
    if (const AnimAction* a = anims.action(item->shape, action)) {
        if (const AnimFrame* f = a->frame(w.npc_direction(id), 0)) {
            w.set_frame(*item, f->frame);
            if (f->mirrored) item->flags |= 0x0020;
            else item->flags &= static_cast<std::uint16_t>(~0x0020);
        }
    }
    r[npc::kLastAnim] = static_cast<std::uint8_t>(action);
    r[npc::kLastAnim + 1] = static_cast<std::uint8_t>(action >> 8);
    r[npc::kAnimFrame] = 0;
}

bool actor_busy(const Kernel& kernel, ObjId actor) {
    return kernel.count_processes(actor, ActorAnimProcess::kType) != 0;
}

// @orig 11D8:151D
ActorAnimProcess* current_anim(const Kernel& kernel, ObjId actor) {
    ActorAnimProcess* current = nullptr;
    kernel.for_each_process([&](const Process& p) {
        auto* anim = dynamic_cast<const ActorAnimProcess*>(&p);
        if (anim && anim->owner() == actor && !anim->waiting() && !current) {
            current = const_cast<ActorAnimProcess*>(anim);
        }
    });
    return current;
}

// @orig 1130:2105
std::uint16_t remap_action(const World& world, ObjId actor, std::uint16_t action) {
    struct Pair {
        std::uint16_t from, to;
    };
    static constexpr Pair kPairs[] = {{4, 0x25},    {0xB, 0x10},  {7, 0xC},     {8, 0xD},     {0x2E, 0x2F},
                                      {0x2A, 0x2B}, {0xA, 0xF},   {0x30, 0x31}, {3, 0x31},    {0x22, 0x23},
                                      {0x27, 0x19}, {0x24, 0x2C}, {2, 0x2D}};
    static constexpr std::size_t kMap40Pairs = 6;  // entries of DS:277F
    static constexpr std::uint16_t kNpcKept[] = {4, 0xB, 7, 8, 0x24, 2};

    if (actor != 1) {
        if (std::find(std::begin(kNpcKept), std::end(kNpcKept), action) != std::end(kNpcKept)) return action;
        if (action == 0x30) return 3;
    }
    const Item* item = world.get(actor);
    if (item && item->map_number == 40) {
        for (std::size_t i = 0; i < kMap40Pairs; ++i) {
            if (kPairs[i].from == action) return kPairs[i].to;
        }
        return action;
    }
    const Item* weapon = world.get(world.active_weapon(actor));
    if (!weapon || dtable::weapon(weapon->shape).keeps_actions()) return action;
    for (const Pair& p : kPairs) {
        if (p.from != action) continue;
        if (action != 8 && action != 0x2A) return p.to;
        const std::uint16_t overlay = dtable::overlay_shape(weapon->shape);
        const bool big = actor == 1 && (overlay == 0x36E || overlay == 0x33B);
        if (action == 8) return big ? 0x36 : 0xD;
        return big ? 0x32 : 0x2B;
    }
    return action;
}

namespace {

// Actor::I_doAnim (NPC.C 10F0:0245), for the controlled NPC: moving actions
// make NPC shots less accurate for a while (see GameState::player_moved_at).
// @orig 10F0:0245
void note_player_movement(World& world, std::uint16_t action, std::uint32_t now) {
    std::uint32_t window = 0;
    std::uint16_t accuracy = 0;
    switch (action) {
    case 0x01: case 0x02: window = 0x78; accuracy = 3; break;
    case 0x1C: case 0x1D: window = 0x3C; accuracy = 3; break;
    case 0x28: case 0x29: window = 0x4B; accuracy = 3; break;
    case 0x03: case 0x17: case 0x18: case 0x19: case 0x22: case 0x23: case 0x26: case 0x27:
    case 0x38: case 0x3A: case 0x3D: case 0x3E:
        window = 0x78;
        accuracy = 2;
        break;
    default:
        return;
    }
    world.state.player_moved_at = now;
    world.state.player_move_window = window;
    world.state.player_move_accuracy = accuracy;
}

}  // namespace

namespace {

// DS:135F: the actions (after the weapon's remapping) that leave the player
// kneeling; any other stands him up.
constexpr std::uint16_t kKneelingActions[] = {0x28, 0x2E, 0x2F, 0x2A, 0x2B, 0x3D, 0x3E, 0x17, 0x18, 0x32};

// NPC_10f0_221b / 226d, for the player only: record +0x13 bit 2
// (Actor::I_isKneeling) and the footpad height of his shape, 6 kneeling and
// 10 standing.
// @orig 10F0:221B 10F0:226D
void set_kneeling(World& world, ObjId actor, bool kneel) {
    if (actor != 1) return;
    const Item* item = world.get(actor);
    auto record = world.npc_record(actor);
    if (!item || record.empty()) return;
    world.set_footpad_z(item->shape, kneel ? 6 : 10);
    if (kneel) record[npc::kFlags13] |= 0x04;
    else record[npc::kFlags13] &= static_cast<std::uint8_t>(~0x04);
}

}  // namespace

// @orig 10F0:0245
Pid do_anim(Kernel& kernel, World& world, const AnimDat& anims, ObjId actor, std::uint16_t action, int dir,
            std::uint32_t max_queued, bool replace) {
    const Item* item = world.get(actor);
    if (!item || (item->z & 0xFF) == 254) return 0;
    action = remap_action(world, actor, action);
    // NPC.C 10F0:0245: the controlled NPC ends the item selection mode.
    if (actor == world.state.controlled_npc && world.state.selection.active) cancel_selection(world);
    if (world.is_dead(actor)) return 0;
    // NPC_10f0_2315: record +0x59 bit 0x40 (set by NPC_10f0_2335, which
    // nothing calls in this version).
    if (auto r = world.npc_record(actor); !r.empty() && (r[npc::kAnimFlags] & 0x40)) return 0;
    if (!anims.action(item->shape, action)) return 0;
    const std::size_t queued = max_queued == 0 ? (actor_busy(kernel, actor) ? 1 : 0)
                                               : kernel.count_processes(actor, ActorAnimProcess::kType);
    if (max_queued < queued) return 0;
    if (replace) kernel.fail_processes(actor, ActorAnimProcess::kType, 1);

    // The new animation waits for the last one queued (the one no other waits for).
    Pid last = 0;
    std::vector<Pid> queue, followed;
    kernel.for_each_process([&](const Process& p) {
        auto* anim = dynamic_cast<const ActorAnimProcess*>(&p);
        if (!anim || anim->owner() != actor) return;
        queue.push_back(anim->pid());
        if (anim->previous()) followed.push_back(anim->previous());
    });
    for (Pid p : queue) {
        if (std::find(followed.begin(), followed.end(), p) == followed.end()) last = p;
    }
    if (actor == world.state.controlled_npc) note_player_movement(world, action, kernel.ticks());
    // Weapon drawn (record +0x5A bit 1): set by actions 7 and 0xC, cleared by
    // 0xB and 0x10.
    if (auto record = world.npc_record(actor); !record.empty()) {
        if (action == 7 || action == 0xC) record[0x5A] |= 0x02;
        else if (action == 0xB || action == 0x10) record[0x5A] &= static_cast<std::uint8_t>(~0x02);
    }
    set_kneeling(world, actor, std::find(std::begin(kKneelingActions), std::end(kKneelingActions), action) !=
                                   std::end(kKneelingActions));
    const Pid pid = kernel.add(std::make_unique<ActorAnimProcess>(world, anims, actor, action, dir, last));
    aim_after_do_anim(kernel, world, anims, actor, action, pid);
    return pid;
}

void ActorAnimProcess::mark_turning() {
    turning_ = true;
    ++world_.state.aim.turns;
}

// @orig 1158:2E66 1158:2D09 1158:2DC6
void ActorAnimProcess::on_terminate() {
    // 2DC6: failed by the kernel's fail_all (new game, map switch), the
    // animation does nothing more.
    if (failed() && kernel().failing_all()) return;
    // 2E66: the cleanup.
    const Item* item = world_.get(owner());
    const bool placed = item && (item->z & 0xFF) != 254;
    if (placed && (action_ == 0x22 || action_ == 0x23)) {
        // The start of a run ends on a full stride: the next action starts
        // from the middle of its cycle (record +0x59 bit 2 cleared).
        if (auto r = world_.npc_record(owner()); !r.empty()) r[npc::kAnimFlags] &= static_cast<std::uint8_t>(~0x04);
    }
    if (turning_) {
        if (world_.state.aim.turns != 0) --world_.state.aim.turns;
        if (placed && owner() == world_.state.controlled_npc) enter_aim_mode(kernel(), world_, anims_);
    }
    // 2D09 / 2DC6: an action flagged so (byte 3 bit 7 of ANIM.DAT) destroys
    // the actor, done or failed.
    item = world_.get(owner());
    if (destroys_actor_ && item && (item->z & 0xFF) != 254) destroy_item(kernel(), world_, owner());
}

}  // namespace crusader
