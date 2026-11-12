# Known bugs
## gun shots replaced by lasers
## [solved] blinking sprites changing between variations (walls, bodies, ...)
After some time in the game (level explored?), some sprites start behaving erratically:
 - Walls keep changing appearance
 - Corpses move

I think I understand a bit more of what is going on: I assume sprites are stored in "collections". For example:
 - all the variants of a wall (for instance, in a level with grey walls and rusty ones, one collection of all the variants of the grey walls and another for the rusty ones)
 - all the orientations of a corpse (for each type of enemy, a collection of the corpse's orientations)

And the problem is that at some point in the game, some sprites (not all) take a random value and "blink".

The behaviour is visible in C:\Users\sblon\Videos\Captures\Crusader_ No Remorse — DECOMP 2026-11-07 18-46-46.mp4

This can be seen pretty much anywhere, but I noticed it recently and made a save on the level where I last saw it: DECORSCLIGNOTE.

**Fixed.** Cause: when an object was destroyed (`World::destroy`), its processes (usecode scripts, egg triggers…) kept running. Its number went back to the head of the free-slot list, and the next object created took it right away. The old object's processes then acted on the new one, for example by changing its frame. This happens mostly with the scenery "glob" objects, destroyed when their area leaves the active area. The more you explore, the more orphaned processes there are: on the save, a camera sweeping the map accounts for 189 instead of 41. The original ends an object's processes when it is destroyed (`Item_forget`, US `10B0:1B7F`: `Kernel_fail_processes(id, any type)`), and the port now does the same. Tests: `test_processes_end_with_their_item` (save `decors-clignotent.sav`) and `test_fast_area`. The "moving sprites" below may have the same cause, to be rechecked in game.

## ambiant sounds keep playing even without source visible
## [solved] jump stance when landing
I get the impression that when the Silencer jumps, on landing he stays in his falling animation until he moves.

**Fixed.** The jump (action 0x3A) has 13 frames: take-off, flight, landing (2108), getting up (2109 to 2111), then standing (frame 0). The landing frame checks that the player has support. When the player lands lower than the starting point, this check makes them fall. The port then ended the animation, and the player stayed frozen on frame 2108 until the next move. In the original (ANIMPRIM, US `1158:1906` / `1158:3090`), the animation continues: it waits for the fall to finish, then plays its last frames and the player ends up standing. Only the fall animation 0x3B, when it replaces the current animation (outside jumps and rolls), ends it. On flat ground, the jump already ended normally. Test: `test_jump_down_ends_standing` (save 13, jump to a floor 16 units lower).

## Moving sprites

## World that appears progressively
This game was originally designed for 4/3 screens. On a modern screen such as 21/9, you can see one of the game's limits: sprites appear progressively, even in the part visible on screen. The same goes for enemies, which can appear suddenly as they move.

So I would like us to think together about a solution to this problem:
 - the level should always be complete in the current view
 - enemies should not appear magically in the middle of the view. Showing them right away also seems problematic: I want neither to be able to snipe them from the other end of the level without them having detected me, nor to have them snipe me from the other end of the level... I don't have many ideas on how to solve this gameplay problem

**Cause.** The original mixes three areas:
 - the displayed view (640×480);
 - the active area (`FastArea`, ±5 cells in `s`, ±3 in `d` around the camera), which extends beyond the screen: the "glob" scenery is only unfolded when it enters, monster eggs create their NPCs there, and AI and animations only run there;
 - the combat area, which is the screen: an NPC only spots the player, keeps them as a target and fires if it is on screen (`is_partly_on_screen` / `is_mostly_on_screen`), and the player's shot only picks its target among the objects on screen.

The native resolution widened the view and the combat area, but not the active area. So its edges were visible.

**Step 1, fixed: the world is complete.** The active area grows with the view while keeping the original's margin (see enhancements.md §6). Test: `test_fast_area`.

**Step 2, in progress: enemy behaviour.**
 - *Done*: the combat area stays the original's 640×480 rectangle, centred like it (see enhancements.md §6). Spotting, keeping the target, firing, waking up on being shot, and the player's shot target choice are limited to it; surveillance cameras and turrets too. The full view is left to visual tests. Tests: `test_on_screen`, `test_combat_zone`.
 - *To do*: check what happens to a player projectile fired straight at an enemy in the side bands (the target choice ignores it, but can the projectile hit it?).
 - *To do*: signal threats outside the combat area by drawing them in darkened greyscale (about 55 to 60% brightness, to be tuned in game), with a short transition when they enter it: living NPCs other than the player, turrets, cameras. Not corpses, mines, projectiles or scenery.
 - *To do*: a "COMBAT ZONE: ORIGINAL / FULL SCREEN" option in Enhancements (`World::set_original_combat_zone`).
