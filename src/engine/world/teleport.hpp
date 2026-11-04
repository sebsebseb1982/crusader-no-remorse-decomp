#pragma once
// Moving the player to a teleport egg, maybe on another map (SHAPHAND.C
// 1098:011F / 022D, GAMEPAL.C 1028:0436 / 0477).
//
// MainActor::I_teleportToEgg(map, egg, wait) starts a process (vtable
// DS:0FE2). At creation it ends the player's processes of type 0xF0
// (Kernel::I_resetRef) and, in this form only, stops every sound (1098:011F);
// with `wait` it raises the player's "busy" flag (DS:7E10) until one slice
// after the process ends (a second process, vtable DS:1006, waits for it and
// puts the flag back).
// When it runs:
//   1. on another map, the player is teleported to (0, 0, 0) there, which
//      loads the map (World::teleport);
//   2. the destination is the first item of the map, cell by cell, that is
//      an egg (family 8) at frame 1 whose quality low byte is the egg number
//      (loop script ": %8 = ` %1 = & * %FF & %egg = & $");
//   3. found: the player goes there, and falls unless he stands
//      (support_mask bit 0x10); not found: back where he was, on his old map
//      ("No destination for this egg!").
// A teleport is ignored while another one moves the player back (DS:0F89).
//
// MainActor::I_teleportToEgg with one argument (0x097) uses the current map
// and leaves the sounds playing.
//
// EggHatcher (SHAPHAND_1098_068b / 0718, type 0x20F, owned by the egg; ended
// with the egg's other processes when it leaves the fast area): at each
// slice, the player is "in" the egg when his footprint overlaps the box of
// half-sizes 32 * (NPC number >> 4) in x and 32 * (NPC number & 0xF) in y
// around the egg (16-bit unsigned bounds, footprint x / y * 16 below the
// player's x / y) and his z is within 0x30 of the egg's. On the way in, a
// teleport egg (family 8) teleports to egg (quality & 0xFF) of map (the
// egg's map number), waiting when the quality high byte is set
// (SHAPHAND_1098_0072, no sound reset); other eggs get usecode event 7
// ("hatch"). On the way out, event 0x15.
// MainActor::I_switchMap(map) starts a process that, when it runs, ends every
// process and sound (GAMEPAL_1028_03f6) and teleports to egg 0x1E of `map`.

#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

namespace crusader {

constexpr std::uint16_t kTeleportStartEgg = 0x1E;  // where a mission starts

// `stop_sounds` is the explicit-map form's sound reset (1098:011F): true for
// 0x07A and the mission start; false for 0x097 (current map) and for the map
// switch, which has already stopped them (1028:0477).
Pid start_teleport_to_egg(Kernel& kernel, World& world, int map, std::uint16_t egg, bool wait, bool stop_sounds);
Pid start_switch_map(Kernel& kernel, World& world, int map);
Pid start_egg_hatcher(Kernel& kernel, World& world, ObjId egg);

// The egg the teleport goes to (0 if none), in the search order.
ObjId find_teleport_egg(const World& world, std::uint16_t egg);

}  // namespace crusader
