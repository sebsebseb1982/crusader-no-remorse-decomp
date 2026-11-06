#pragma once
// The cheats of the executable (GameState::Cheats, kept for the session).
//
// Enabling (CRUSADER_1138_2B03, at the head of the Controller's keys): the
// scancodes of "JASSICA16" (DS:28BD: 24 1E 1F 1F 17 2E 1E 02 07) typed in a
// row. Each press compares its scancode with the next letter (DS:28C7): a
// match moves on, a miss starts again (at 1 when it is the first letter).
// The whole code flips "cheats enabled" (DS:08F0), sets "cheats on" (DS:5FF5)
// to the same, allows the hack mover (DS:8C3A), plays sound 0x103 and says
// "Cheats are now active." / "Cheats are now inactive." in the message box.
// The "-laurie" argument (CRUSYS_1050_093D) enables the cheats too, without
// turning them on.
//
// F10 (CRUSADER_1138_0896, scancode 0x44), while the cheats are on: every
// process of the controlled NPC fails (the US 1.23 keeps them while the
// player is in stasis, DS:7E78), and every DeathSilence (0x261). Then:
//   - a dead controlled NPC is brought back: the player gets his shape 1
//     back, the NPC is no longer broken nor dead (NPC_10f0_2037), its hit
//     points are the maximum of its NPC type; nothing else;
//   - else Ctrl flips its immortality (record +0x13 bit 1), with
//     "Immortality enabled." / "Immortality disabled.";
//   - and for the player: 30000 credits (NPC_10f0_3DBF: Item::I_setQuality
//     of the credits carried); the best battery 0x3A3 and the shield 0x530
//     (picked up, shown); the energy full; the weapons of a table (Ctrl
//     DS:28A9: 0x32E 0x32F 0x333 0x330 0x38D; Alt DS:28B5: 0x332 0x38B 0x38C;
//     else DS:289D: 0x386 0x38A 0x38E 0x388 0x334) not carried, made at the
//     player's feet and equipped in the free weapon slots, unloaded (active
//     when none was), while there is a free slot; 0x560, 0x39A, the detpacs
//     0x55F and the medikits 0x351, when none is carried, picked up (shown)
//     until refused, 20 at most; the ammunition 0x33D, 0x33F, 0x340, 0x341,
//     0x33E (CRUSADER_1138_01BD): quality 100 for the one carried (combine),
//     else a new one equipped in a free ammunition slot (Item::I_setQuality
//     only works for family 1, so it stays empty: as in the original).
//
// Keys of the game view (World_on_key, 13E8:14B4), out of the item selection:
//   ~       (cheats enabled) cheats on / off: "CHEATS ON" / "CHEATS OFF";
//   T / t   (cheats enabled) the player is used (Item::I_use);
//   f       (cheats enabled) the player leaves stasis, and the bounding boxes
//           of the items (DS:2D7C) flip;
//   Ctrl+Q  (cheats enabled) "CD TRANSFER DISPLAY ACTIVE." / "INACTIVE.";
//   Ctrl+L  (always) the player's coordinates: "X: %u\nY: %u\nZ: %u";
//   Ctrl+M  (always) the next music track: 0x11 skipped, back to 0 after
//           0x12; the US 1.23 goes back to 1 after 0x12 and plays 0x11.
//
// Not reproduced (debugging displays of the original):
//   - the bounding boxes of f (DLIST_11A0_02E4), the grid of F7 (DS:2C54),
//     the outlines of Alt+F7 (DS:2C53, CRUSYS_1060_0814) and the egg ranges
//     of Ctrl+F7 (DS:0F88): the port draws none of them;
//   - H, the "hack mover" (DS:5FFA): items moved with the mouse;
//   - Ctrl+V, the version and memory status box (its figures are those of
//     the DOS memory manager);
//   - with no NPC controlled (0), F10 fails the processes of every owner in
//     the original; the port fails none for that call.

#include "core/dtable.hpp"
#include "engine/input/keyboard.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

#include <cstdint>

namespace crusader {

// CRUSADER_1138_2B03: one key event towards the typed code.
void cheat_code_key(World& world, const GameKeyEvent& e);

// CRUSADER_1138_0896, F10: true when it acted (the key is used).
bool cheat_f10(Kernel& kernel, World& world, const dtable::NpcTypes& types, const GameKeyEvent& e, bool us_123);

// World_on_key's cheat and debugging keys: true when `code` is one of them
// (acted on or not).
bool cheat_command(World& world, std::uint16_t code, bool us_123);

}  // namespace crusader
