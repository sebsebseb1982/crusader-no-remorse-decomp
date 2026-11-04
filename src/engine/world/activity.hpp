#pragma once
// NPC activities outside combat (TYPE.C segment 1108). See docs/combat.md,
// "Activités hors combat".
//
//   1 Stand (type 0x258)       waits; turns to its default activity 1 when it
//                              sees the controlled NPC
//   2 Loiter (0x257)           fidgets or walks to a point within 0xA0 of
//                              where it started
//   3 (0x255)                  walks straight on; blocked twice, gives up:
//                              activity 7, or 5 for shapes 0x2F5..0x2F7,
//                              0x344, 0x597; turns back after the first block
//   7 Surrendering (0x25F)     hands up (action 0x39), activity locked, faces
//                              the controlled NPC
//   8 Guard (0x25E)            watches; attacks (activity 5) when it sees the
//                              controlled NPC; else waits or fidgets
//
// Every activity process starts by showing the NPC standing (action 0) with
// its weapon put away (TYPE_1108_0000). Not reproduced: sounds (barks),
// the loiter's handling of messages from its path finder (1108:0D3E).

#include "core/anim_dat.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

#include <cstdint>

namespace crusader {

// TYPE_1108_00c2: the NPC, mostly on screen, facing within two directions of
// the controlled NPC, has a line of fire to it in the direction of the
// controlled NPC, or (without firing frames) sees it within range
// (Item::I_getRangeIfVisible). With `look_turn` (the special frames,
// WPNCACHE_1160_01f0), an NPC looking aside (actions 0x1E, 0x1F, from frame
// 2) faces four directions away from its direction.
bool sees_controlled(const World& world, const AnimDat& anims, ObjId npc, bool look_turn = false);

// Item::I_getRangeIfVisible (1130:1C01): a sweep with `item`'s shape from
// it to `other` (raised by (footpad height - 1) * 8) meets `other` first:
// the distance / 16 (rounded up when its low four bits are set), when under
// 49; else 0.
int range_if_visible(const World& world, ObjId item, ObjId other);

// CRUSADER_1138_2daf: a shot by the controlled NPC wakes the NPCs on screen
// (2..255, on the map). One in combat, or doing its default activity 2, or
// with its activity locked, that guards (8) attacks instead (5); it then
// targets the controlled NPC (in combat: always). Any other takes its default
// activity 2 if it can see the controlled NPC (only checked for shapes 0x2F5,
// 0x2F6, 0x2F7, 0x595, 0x597: the others hear it), and targets it if that put
// it in combat. The controller does it at most once every 240 ticks
// (CRUSADER_1138_2d72).
void wake_npcs_on_screen(Kernel& kernel, World& world, const AnimDat& anims);

// TYPE_1108_0913: a fidget (action 0x1E or 0x1F, then 0) when not busy.
void fidget(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc);

// Starts the process of a non-combat activity (Actor::I_setActivity).
// Returns its pid (0 for none).
Pid start_activity_process(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc, std::uint16_t activity);

}  // namespace crusader
