#pragma once
// The aiming mode of the controlled NPC and target designation (CRUSADER.C,
// segment 1138: 0299..087F and 10C8; the DumbTimer of UNITFILE.C). State in
// GameState::Aim. See docs/combat.md, "Mode de visée".
//
// Aiming mode (DS:2863). Actor::I_doAnim, for the controlled NPC (or shape
// 0x580), enters it (enter_aim_mode, 1138:0299) with the "aiming" actions
// (aim_action) and leaves it (leave_aim_mode, 0692) with all others, the
// Controller's fire state being reset too (1138:2F9F). Item::I_fireWeapon
// enters it before a shot of the controlled NPC. Entering is refused while a
// turning animation runs (DS:288A, turn_action: rolls, side steps, the end
// of a run), and for shape 0x576; at the end of such an animation the mode
// is entered again (1158:2E66). An NPC whose shape has no action 1 (a
// surveillance camera: DS:2864, set on taking control) never enters it.
//
// The crosshair: screen sprite 0x4CC, frame 0 (place_crosshair, 0708), at
// 200 units from the centre of the controlled NPC (getCX, getCY) at the
// angle DS:285E: x + 200 cos a, y + 200 sin a (TRIG.DAT, 16.16, keeping the
// whole part), z the NPC's z plus the muzzle height of its shot (action 8,
// 0x2A kneeling, remapped by the weapon: the first firing frame in its
// direction, 1150:065D). An angle above 360 (1000 on leaving the mode or
// changing the controlled NPC) is first taken from the direction d:
// (22 d + 5 + 270) mod 360, the whole part of 0x160005 d + 0x50006 in
// 16.16 (22 rather than 22.5 degrees a direction). Entering the mode
// places it; afterwards an aiming action marks it for placing again
// (DS:2862), which the Controller does at its next run (1138:087F).
//
// Turning (turn_crosshair, 10C8), from the movement keys of AvatarMover in
// a combat stance: the left and right arrows turn the angle by 5 degrees
// (2 without Shift), the crosshair is placed again, the NPC faces it
// (getDirFromTo16 from its centre) and plays the combat stance (4, or 0x2E
// kneeling) in that direction when it changed. Out of the mode, an NPC
// whose shape has no action 1 turns by one direction instead, when not busy.
// A turn sent by the mouse (DS:5FF6) moves the crosshair by half its rate
// (DS:5FF8), at most 20 degrees; the joystick is not reproduced.
//
// A shot of the controlled NPC in the mode with no target found flies
// towards the crosshair (crosshair_aim, SPRITE_1140_0E88).
//
// Target designation (DS:2889, toggled by R). In the mode, the target found
// in the NPC's direction (TRACKER_1128_0158 in 16 directions) is marked
// when its shape has TYPEFLAG byte 0 bit 4 (ITEM_10a8_30a8): designate
// (0459) clears the previous marker, then, only when the NPC's direction
// differs from the one of the last designation (DS:2890; 0x10 after
// designating nothing, an original quirk: a new target in an unchanged
// direction is not marked) and a line of fire exists (Item_I_fireDistance),
// starts a SpriteProcess 0x59A, frames 0..5, 10 ticks each, which stays on
// frame 5. The marker is at the target's x, y, z: z + 8 h' with h' the
// height of the footprint (h >= 4: h / 2 + 1, h >= 2: h / 2; not an NPC: 2
// less, 1 becomes 0), then 16 (n / 2) back along the longer side n of the
// footprint (none when square). It is searched again: on entering the mode
// (redesignate, 03D0), when turning, at most once a second (DumbTimer,
// check_designation, 02F3), when the target moves (designated_target_moved,
// Item_I_move: the marker follows it while it stays the one found) and when
// it is destroyed (Item_forget). The death of the controlled NPC clears it.
// Port addition, precise aim (World::precise_aim): the target searched is
// one the NPC has a line of fire to (find_target), and the quirk of the
// unchanged direction is left out.

#include "core/anim_dat.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <optional>

namespace crusader {

constexpr std::uint16_t kCrosshairShape = 0x4CC;

// Actor::I_doAnim (NPC.C 10F0:0245): actions that keep / enter the aiming
// mode, and the turning ones (flag +0x40 of the animation, 1158:2E55).
bool aim_action(std::uint16_t action);
bool turn_action(std::uint16_t action);
// The end of Actor::I_doAnim for the new animation `pid` of `actor`.
void aim_after_do_anim(Kernel& kernel, World& world, const AnimDat& anims, ObjId actor, std::uint16_t action,
                       Pid pid);

void enter_aim_mode(Kernel& kernel, World& world, const AnimDat& anims);  // 1138:0299
void leave_aim_mode(Kernel& kernel, World& world);                        // 1138:0692
void place_crosshair(World& world, const AnimDat& anims);                 // 1138:0708
void refresh_crosshair(World& world, const AnimDat& anims);               // 1138:087F

// 1138:10C8. `scancode` is the key's; `dir` and `action` are AvatarMover's,
// changed when the NPC is to turn.
void turn_crosshair(Kernel& kernel, World& world, const AnimDat& anims, std::uint8_t scancode, bool walk_only,
                    int& dir, int& action);

// SPRITE_1140_0E88: the point a shot aims at, the crosshair's x and y with
// `z`, the muzzle's height in the world (Item::I_fireWeapon adds the
// shooter's z to the muzzle offset before passing it; low byte kept).
// Port addition (directional aiming of the controller's right stick): puts the
// crosshair at `angle` (degrees in the world's x, y plane, like DS:285E) and
// turns the NPC to face it with its combat stance, when it is not busy. Out of
// the aiming mode, a drawn weapon only has to enter the mode; when
// `draw_when_holstered` is on, a holstered one is drawn first (action 7, as the
// fire key does).
// During a side step (the left stick's strafe sideways), which suspends the
// mode, the crosshair stays shown at `angle`.
void aim_at_angle(Kernel& kernel, World& world, const AnimDat& anims, int angle, bool draw_when_holstered);
// Removes the crosshair aim_at_angle left shown out of the aiming mode (the
// stick released, or the side step over without the mode entered again).
void drop_stick_crosshair(World& world);

std::optional<World::Point> crosshair_aim(const World& world, int z);

void designate(Kernel& kernel, World& world, const AnimDat& anims, ObjId target);  // 1138:0459
void redesignate(Kernel& kernel, World& world, const AnimDat& anims);              // 1138:03D0
void check_designation(Kernel& kernel, World& world, const AnimDat& anims);        // 1138:02F3
void designated_target_moved(Kernel& kernel, World& world, const AnimDat& anims, ObjId target, std::int32_t x,
                             std::int32_t y, std::int32_t z);

// UNITFILE.C 1430:0299 "DumbTimer", started with the game and running at any
// level: at every run, when more than 60 ticks went by since its last check,
// check_designation.
class DumbTimer : public Process {
public:
    DumbTimer(World& world, const AnimDat& anims) : world_(world), anims_(anims) { set_runs_at_any_level(true); }

protected:
    void run() override;

private:
    World& world_;
    const AnimDat& anims_;
};

}  // namespace crusader
