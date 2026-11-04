#pragma once
// Actor animation process (ANIMPRIM.C: 0000 constructor, 016F / 17B2 start,
// 1908 step, 31DA direction; ScummVM's ActorAnimProcess) and
// Actor::I_doAnim (NPC.C 0245). See docs/animation.md.
//
// An animation plays one ANIM.DAT action of an actor: each step shows a
// frame, moves the actor by the frame's distance along the direction (with
// collisions), then waits the action's ticks per frame. Animations of one
// actor run one after the other: a new one waits for the last queued one
// (Process_suspend_by_type). The frame index lives in the NPC record (+0x10)
// and carries over between animations; looping walks are cut in half strides
// (record +0x59 bit 2), so that a queued animation can start when a foot is
// down.
//
// At its end (2E66 cleanup, 2D09 / 2DC6), the start of a run (0x22, 0x23)
// clears the half-stride bit, and an action flagged in ANIM.DAT (byte 3
// bit 7) destroys the actor; failed by the kernel's fail_all, it does nothing.
//
// Not yet (compared with the byte-exact C, docs/audit-decomp.md): frame
// skipping (1908: off-screen NPCs and the frameskip option play two frames
// per step), combat height tracking (action flag 0x80), the special frames
// (0x1000, WPNCACHE_1160_01f0), the start fallbacks of 016F for the
// controlled NPC beyond the walk's sideways nudge (rolls, runs, a walk that
// fits nowhere turned into standing), the restart of actions flagged 0x40
// (none in ANIM.DAT), the target of a pathfinder walk passed to the sweep.

#include "core/anim_dat.hpp"
#include "engine/kernel.hpp"
#include "engine/process.hpp"
#include "engine/world/world.hpp"

#include <cstdint>

namespace crusader {

// Direction steps (DS:132F and DS:1340), 16 directions, 0 = towards -y.
constexpr std::int8_t kDirectionDx[16] = {0, 1, 1, 2, 1, 2, 1, 1, 0, -1, -1, -2, -1, -2, -1, -1};
constexpr std::int8_t kDirectionDy[16] = {-1, -2, -1, -1, 0, 1, 1, 2, 1, 2, 1, 1, 0, -1, -1, -2};

// Direction arguments besides 0..15 (ANIMPRIM.C 31DA).
enum : int {
    kDirCurrent = 0x10,    // the actor's direction when the animation starts
    kDirTurnRight = 0x11,  // one step clockwise (two for 8-direction actions)
    kDirTurnLeft = 0x12,
};

// Action numbers seen in the player's animations (names from ScummVM's
// Crusader table, matching the data: frame counts, looping, distances).
enum class ActorAction : std::uint16_t {
    kStand = 0,
    kWalk = 1,
    kRetreat = 2,
    kRun = 3,
    kCombatStand = 4,
};

// NPC record fields used by animations.
namespace npc {
constexpr std::size_t kDirection = 0x0C;
constexpr std::size_t kLastAnim = 0x0D;   // u16
constexpr std::size_t kAnimFrame = 0x10;
constexpr std::size_t kFlags13 = 0x13;    // bit 0 dead, bit 2 kneeling
constexpr std::size_t kWeapon = 0x15;     // u16, equipment slot of the active weapon (0 = none)
constexpr std::size_t kActiveItem = 0x19; // u16, equipment slot of the active inventory item
constexpr std::size_t kAnimFlags = 0x59;  // bit 2: half stride done
}  // namespace npc

class ActorAnimProcess : public Process {
public:
    // Process type of actor animations (Process_set_owner keeps per-item
    // counters for types 0xF0..0xF7).
    static constexpr std::uint16_t kType = 0xF0;
    // Usecode event of a frame flagged 0x4000 (calledFromAnim).
    static constexpr std::uint16_t kCalledFromAnimEvent = 0x0E;

    ActorAnimProcess(World& world, const AnimDat& anims, ObjId actor, std::uint16_t action, int dir, Pid previous);

    std::uint16_t action() const { return action_; }
    int direction() const { return dir_; }
    Pid previous() const { return previous_; }
    // Changes the direction (ANIMPRIM.C 31DA), also of a running animation.
    void set_direction(int dir);
    // True when the last step was refused by a collision (+0x3C; the process
    // then fails).
    bool blocked() const { return blocked_; }
    // 1158:2E55: a turning animation of the controlled NPC (+0x40), counted
    // in DS:288A; its end lets the aiming mode back (1158:2E66).
    void mark_turning();

protected:
    void run() override;
    void on_added() override;
    void on_terminate() override;

private:
    void start(const AnimAction& a);
    bool step();  // false when the animation is over
    bool fall_if_unsupported(const World::Point& from, const World::Point& to);
    const ActorAnimProcess* next_queued() const;

    World& world_;
    const AnimDat& anims_;
    std::uint16_t action_;
    int dir_;
    Pid previous_;
    bool started_ = false;
    bool blocked_ = false;
    bool in_place_ = false;  // +0x3B: an unstoppable action blocked, the rest plays in place
    bool turning_ = false;
    bool destroys_actor_ = false;  // +0x4D bit 7: the action destroys the actor at its end
};

// CRUSADER_1130_2105, applied first by Actor::I_doAnim: the action an actor
// really plays for `action`, given its weapon.
// - NPCs other than the player keep actions 4, 0xB, 7, 8, 0x24 and 2 (table
//   DS:279B); their 0x30 (run with a weapon) becomes 3. Then as the player:
// - an actor whose map field is 40 uses the first six pairs of DS:2747 below
//   whatever its weapon (the loop stops on the length of DS:277F but reads
//   DS:2747);
// - otherwise, holding a weapon whose record does not keep actions (+0xC),
//   the pairs of DS:2747: 4->0x25, 0xB->0x10, 7->0xC, 0x2E->0x2F, 0xA->0xF,
//   0x30->0x31, 3->0x31, 0x22->0x23, 0x27->0x19, 0x24->0x2C, 2->0x2D; shots
//   8 and 0x2A (kneeling) become 0xD and 0x2B, or 0x36 and 0x32 for the player
//   when the weapon overlay is 0x36E or 0x33B.
std::uint16_t remap_action(const World& world, ObjId actor, std::uint16_t action);

// Actor::I_doAnim: queues `action` facing `dir` (0..15 or kDir*) for `actor`.
// Refused (0) when the actor is parked (z 254), dead, has no such action, or
// already has more than `max_queued` animations (0: refused when busy).
// `replace` ends the actor's current animations first. Returns the pid.
Pid do_anim(Kernel& kernel, World& world, const AnimDat& anims, ObjId actor, std::uint16_t action, int dir,
            std::uint32_t max_queued = 10000, bool replace = false);

// ANIMPRIM.C test mode (a position pointer given to the constructor, used by
// the path finder): the first frame `action` would show facing `dir` is
// tried from `pos` instead of the actor's position, without changing
// anything. On success `pos` takes the new position. A step blocked by
// `goal` counts as a success that touched it (result bit 1); other blocked
// steps fail (no support counts as blocked: no falling in this mode).
struct SimulatedStep {
    bool ok = false;
    bool touched_goal = false;
};
SimulatedStep simulate_step(const World& world, const AnimDat& anims, ObjId actor, std::uint16_t action, int dir,
                            World::Point& pos, ObjId goal);

// CRUSADER_1138_32a8: shows the first frame of `action` in the actor's
// direction at once (last action = `action`, frame index 0).
void set_pose(World& world, const AnimDat& anims, ObjId actor, std::uint16_t action);

// Actor::I_isBusy: the actor has an animation.
bool actor_busy(const Kernel& kernel, ObjId actor);
// The running animation of an actor (first of its queue), or nullptr.
ActorAnimProcess* current_anim(const Kernel& kernel, ObjId actor);

}  // namespace crusader
