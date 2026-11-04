#pragma once
// Falling and thrown items (segment 1040, the original's gravity process:
// 0395 constructor, 06D2 step, 04BC end; Item::I_hurl 12A6, Item::I_fall
// 1334) and the support test under an actor (1040:0072).
//
// A gravity process (type 0x203) carries a velocity (vx, vy, vz) and a
// gravity. Every step (every 4 ticks) the item moves by the velocity through
// a swept-box test, then gravity is taken from vz; a slice that comes 2 to 4
// ticks late (DS:3A88) makes the step, and the sleep, that many times longer.
// A blocked step marks the item as bouncing (flag 0x400) and leaves a quarter
// of the horizontal speed (and of vz, unless it landed). Hitting a wall stops
// (NPCs) or reverses (others) the horizontal speed; a step out of the map
// stops it. Landing ends the fall, except for the controlled NPC (DS:2852)
// that does not stand on two opposite quarters of its footprint: pushed away
// from the side that holds it, it slides on ("rolling", +0x3A) and falls
// again. The process gives up after 200 steps. Out of the fast area it waits
// without sleeping. While an NPC falls its record +0x59 bit 1 is set and
// +0x11 keeps the starting height.
//
// Also written, though never reached in this version: low-friction items
// (flag 0x4000, set by nothing) that roll on after landing; a rebound of the
// controlled NPC when it stands (only an item can rebound, and only the
// controlled NPC, always an NPC, stands); homing on a target (+0x3E, from
// GAMEPAL_1048_02c0, which nothing calls), not reproduced.
//
// At the end of a fall that was not cut short (1040:04BC), a living player
// that dropped 3 steps of 8 or more lands hard (WorldHooks::hard_landing:
// actions 0x3B then 4 or 0, sound 0x8F), unless already in action 0x3A,
// 0x3D, 0x3E, 0x17, 0x18 or running.
//
// Not yet: a dead NPC's landing (it ends its process of type 0xF0).

#include "engine/kernel.hpp"
#include "engine/process.hpp"
#include "engine/world/world.hpp"

#include <cstdint>

namespace crusader {

class GravityProcess : public Process {
public:
    static constexpr std::uint16_t kType = 0x203;

    GravityProcess(World& world, ObjId item, int vx, int vy, int vz, int gravity);

    int vx = 0, vy = 0, vz = 0;
    int gravity = 0;

protected:
    void run() override;
    void on_terminate() override;

private:
    World& world_;
    int steps_left_ = 200;
    bool rolling_ = false;  // +0x3A
};

// The process of type 0xF1 (ITEM.C 0020 / 0078): at its first run, it
// starts usecode event 9 ("release") of its item, then ends. An item has at
// most one (Kernel_owner_has_type).
class ReleaseProcess : public Process {
public:
    static constexpr std::uint16_t kType = 0xF1;

    ReleaseProcess(World& world, ObjId item);

protected:
    void run() override;

private:
    World& world_;
};

// ITEM.C 2970, before `mover` moves: the items resting on it fall
// (Item::I_fall), and those it rests on get a ReleaseProcess unless they
// have one. Teleporter pads, the elevator and recharging stations use the
// release to notice that nobody stands on them any more.
void leave_surface(Kernel& kernel, World& world, ObjId mover);
// The same for the items resting on `item` only (1040:06D2, an item that
// starts falling straight down).
void drop_items_on_top(Kernel& kernel, World& world, ObjId item);
// 1040:11FD, for each item of a cell leaving the fast area (10B0:0386):
// a falling item drops straight down (a sweep to z 0, with its contact
// events) and its gravity process ends.
void drop_out_of_fast_area(Kernel& kernel, World& world, ObjId item);

// Item::I_hurl: starts the item's gravity process, or adds the velocity to
// the running one. A new process first waits 20 ticks. Returns its pid.
Pid hurl(Kernel& kernel, World& world, ObjId item, int vx, int vy, int vz, int gravity);
// Item::I_fall: hurl with no speed and gravity 2, for items that are not
// fixed and have a weight.
Pid fall(Kernel& kernel, World& world, ObjId item);
// GAMEPAL_1048_00a6 (the MissileTracker of ScummVM): the throw from `from`
// to `to` at `speed` with `gravity`, in 16-bit arithmetic. With d the
// distance (dx, dy, dz), range = (max(|dx|, |dy|) + |dx| + |dy|) / 2 and
// n = round(range / speed) steps; vz = (dz + g n (n - 1) / 2) / n climbs so
// that gravity brings the item down on the target. When vz is above
// speed / 4, n becomes (speed / 4) / g if larger, else dz / (speed / 4) if
// larger (speed > 3), and vz is computed again. vx, vy = round(dx / n),
// round(dy / n). Under one step: straight up or down at speed / 4. A
// negative gravity counts as 0. In reach: vz at most speed / 4 and range at
// most speed * (2 speed / g), 3000 without gravity. A speed of 0 (a division
// by zero in the original) gives no velocity.
struct ThrowPath {
    int gravity = 0, range = 0, max_range = 0;
    int vx = 0, vy = 0, vz = 0;
    bool in_reach = false;
};
ThrowPath throw_path(const World::Point& from, const World::Point& to, int speed, int gravity);
// Item::I_shoot(item, target, speed, gravity) (10A8:38D1): hurls the item
// from its position (its outermost container's) along throw_path
// (GAMEPAL_1048_0290), whether the target is in reach or not.
void shoot(Kernel& kernel, World& world, ObjId item, const World::Point& target, int speed, int gravity);

// Support under a box of `shape` at (x, y, z) (1040:0072): one bit per
// quarter of its footprint resting on a solid item whose top is at z (or up
// to 9 below with `slack`): 8 low x / low y, 4 low x / high y, 1 high x /
// low y, 2 high x / high y; 0x10 when two opposite quarters are supported
// (the item stands). At z 0 everything is supported (0x1F).
std::uint8_t support_mask(const World& world, std::uint16_t shape, std::int32_t x, std::int32_t y, std::int32_t z,
                          bool slack, ObjId ignore);

}  // namespace crusader
