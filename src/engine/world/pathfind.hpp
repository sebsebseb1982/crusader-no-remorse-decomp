#pragma once
// NPC walking towards a point or an item ("PathFinder", ATTACK.C segment 1118,
// type 0x204). See docs/combat.md, "Se déplacer".
//
// Not a search over a graph: each turn, the process picks the next step's
// direction from the NPC's position by test walks (action 1, or 0x24 in
// combat: a whole half stride in ANIMPRIM.C test mode, test_action, aimed at
// the goal item) in candidate directions, then plays the real walk in the
// chosen direction and waits for it. The test walk ends where it stopped;
// the next one starts from there.
//
// Choosing a direction (1118:08E4), from position p:
//   d = 8-direction from p to the goal. When p is closer to the goal (x, y
//   Chebyshev) than ever before, a test walk towards d is made; if it passes,
//   wall following stops.
//   Candidates: d + {0, +2, -2, +4, -4, +6, -6, -8} (or the mirror table
//   {0, -2, +2, ...}, chosen at random when the process starts). Following a
//   wall: from the reverse of the last direction, turning by 2 the way of the
//   chosen side, the candidates 2, 4, 5, 6, 7 of that list.
//   The first candidate whose test walk passes is taken. Reaching the goal
//   item (the test walk blocked first by it, sweep +0x49, when it is a solid
//   shape with height) or the stop distance ends the walk after this step. When none passes:
//   no way. Taking a candidate other than d starts wall following, on the
//   side given by the candidate's rank.
//   In combat, when the NPC then has a line of fire to its target, it stops
//   (action 0) and its attack process is told it saw the target.
// Each turn (1118:0428): the goal item's position is refreshed; standing on
// the goal's x / y ends the walk (success when z matches too); no way or
// `max_steps` steps end it as a failure; otherwise the NPC turns to the
// direction (SPRITE_1150_009C) and walks one animation that way.
// At the end (1118:072E): with `final_turn`, it turns to the goal; if it did
// not turn, it takes its stance (action 4 in combat with its weapon drawn,
// else 0).
//
// Not reproduced: the plan-only mode (+0x49, the whole path simulated at
// once), unused by the attack process.

#include "core/anim_dat.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

#include <cstdint>

namespace crusader {

// Starts a path finder for `npc`, ending the one it may have (+0x59 bit 5).
// Returns its pid.
Pid walk_to(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc, ObjId item, World::Point goal,
            int max_steps, int stop_distance, bool final_turn);

class PathfinderProcess : public Process {
public:
    static constexpr std::uint16_t kType = 0x204;

    // ATTACK_1118_0000 (to a point; `item` = 0) and ATTACK_1118_01dc (to an
    // item, whose position is the goal). `stop_distance` ends the walk when
    // the simulated position comes that close (0 = never).
    PathfinderProcess(World& world, const AnimDat& anims, ObjId npc, ObjId item, World::Point goal, int max_steps,
                      int stop_distance, bool final_turn);

protected:
    void run() override;
    void on_terminate() override;

private:
    int choose_direction(World::Point& pos);

    World& world_;
    const AnimDat& anims_;
    ObjId item_;                  // +0x4D
    World::Point goal_;           // +0x36
    int last_dir_ = 0x10;         // +0x3B
    int candidate_ = 0;           // +0x3D
    bool wall_ = false;           // +0x3E
    bool side_ = false;           // +0x3F
    bool solid_goal_ = false;     // +0x40
    int best_ = 0;                // +0x41
    int stop_distance_;           // +0x43
    int max_steps_;               // +0x45
    int steps_ = 0;               // +0x47
    bool done_ = false;           // +0x4A
    bool final_turn_;             // +0x4B
    bool stance_ = true;          // +0x4C
    bool mirror_table_ = false;   // +0x4F
};

}  // namespace crusader
