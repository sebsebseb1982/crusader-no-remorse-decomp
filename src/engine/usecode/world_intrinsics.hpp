#pragma once
// Engine-wide intrinsics (clock, randomness, difficulty, alert, stasis...),
// numbered as in the French CRUSADER.EXE.

#include "core/anim_dat.hpp"
#include "engine/usecode/uc_machine.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <vector>

namespace crusader {

// World::I_setAlertActive / I_clrAlertActive (ITEMFIND.C 10E0:0000 / 0121):
// the alert flag, and the alarm lights on the map follow it. Shapes 0x21,
// 0x49, 0x174, 0x271 (frame 0 off, 1 on); the alarm box 0x477 keeps its
// frame 0/1 and adds 2 when on. The NPCs' reactions are in the scripts
// (FREE event 0x3B, ALRMTRIG, ALARMBOX, doors and keypads).
void set_alert(World& world, bool on);

// `anims` is needed by setAvatarInStasis, which takes a living player out of
// combat (0 = not bound).
std::vector<std::uint16_t> register_world_intrinsics(UCMachine& machine, World& world,
                                                     const AnimDat* anims = nullptr);

}  // namespace crusader
