# Enhancements and changes from the original game

This document lists everything this C++20/SDL2 port adds to or changes from *Crusader: No Remorse* (Origin, 1995, DOS executable). For each item it gives the rules as the code implements them.

The rest of the engine reproduces the original. Each function carries an `@orig <segment:offset>` tag that points to the routine it rewrites in `CRUSADER.EXE` (French 1.08). Port-only code is tagged `@orig none` or commented "Port addition". This document is built from those markers, the configuration (`src/core/game_config.hpp`), the project documentation and the git history.

The changes fall into four groups:

- **New options**, all in the Enhancements screen and all off by default unless stated otherwise (sections 1 to 10).
- **Quality-of-life and platform changes** that are always on (sections 11 to 16).
- **Robustness**: places where the original would crash or misbehave and the port does not (section 17).
- **Known deviations**: what the port does differently or does not reproduce yet (section 18).

---

## Contents

1. [Enhancements screen](#1-enhancements-screen)
2. [Gamepad support](#2-gamepad-support)
3. [Directional movement (left stick)](#3-directional-movement-left-stick)
4. [Directional aiming (right stick)](#4-directional-aiming-right-stick)
   - [4b. Precise aim](#4b-precise-aim)
5. [Camera follows the player](#5-camera-follows-the-player)
6. [Native resolution](#6-native-resolution)
7. [Zoom](#7-zoom)
   - [7b. Impact marks](#7b-impact-marks)
   - [7c. Casings](#7c-casings)
   - [7d. 3D lighting](#7d-3d-lighting)
   - [7e. Leaking barrels](#7e-leaking-barrels)
   - [7f. Glass debris](#7f-glass-debris)
8. [Autosave](#8-autosave)
   - [8b. Language](#8b-language)
9. [Toggle run](#9-toggle-run)
   - [9b. Original game bug fixes](#9b-original-game-bug-fixes)
10. [Keypad navigation with arrows or gamepad](#10-keypad-navigation-with-arrows-or-gamepad)
   - [10b. Loot from remains (cheat)](#10b-loot-from-remains-cheat)
11. [Window, fullscreen, mouse and picture](#11-window-fullscreen-mouse-and-picture)
12. [Version lines on the main menu](#12-version-lines-on-the-main-menu)
   - [12b. Christmas hat](#12b-christmas-hat)
13. [Game versions: French 1.08, US 1.23 and German 1.07](#13-game-versions-french-108-us-123-and-german-107)
14. [Installing from a CD image, no CD needed](#14-installing-from-a-cd-image-no-cd-needed)
15. [Start-up, command line and tools](#15-start-up-command-line-and-tools)
16. [`CRUSADER.CFG` handling](#16-crusadercfg-handling)
17. [Robustness and bug fixes](#17-robustness-and-bug-fixes)
18. [Known deviations from the original](#18-known-deviations-from-the-original)
19. [Platforms, technical base and distribution](#19-platforms-technical-base-and-distribution)
20. [Level editor](#20-level-editor)

---

## 1. Enhancements screen

**Files:** `src/engine/ui/enhancements_screen.*`, `src/engine/ui/options_screen.*`

The original has nothing like it. This screen gathers every option the port adds.

**Opening it**
- The Options screen has a text button, **ENHANCEMENTS** (French: **AMELIORATIONS**), with a "+" icon. It sits on row 9, left of "Quit", in the Quit button's colours.
- Left from "Quit", or right from the button, moves between the two. Enter opens it.
- If the fonts are not loaded, the button is not shown.

**Look**
- The screen is cleared. A double red frame (colour `0x57`, corners cut over 3 pixels like the original buttons) starts at (100,34), 439 px wide; it ends 22 px below the last row (at y 465 at most).
- The title is green (`0x0C`), with the "+" icon: ENHANCEMENTS (French: AMELIORATIONS) on the first page, the page's name on the others.
- Rows 26 px apart, 9 at most on a page, BACK / RETOUR last. The focused row is bright green (`0x0C`); the others are dark green (`0x75`).
- An on/off option shows its name on the left and its value on the right: ON / OFF (French: MARCHE / ARRET).

**Pages.** The first page has one row per page: CONTROLS / CONTROLES, DISPLAY / AFFICHAGE, EFFECTS / EFFETS, GAME / JEU, CHEATS / TRICHE, then BACK / RETOUR, which closes the window.

*CONTROLS / CONTROLES*

| Row (EN / FR) | Effect | CFG key |
|---|---|---|
| CONTROLLER / MANETTE | Opens the controller screen | — |
| DIRECTIONAL MOVE / MODE DIRECTIONNEL | Toggles directional movement | `directionalmove` |
| DIRECTIONAL AIM / VISEE DIRECTIONNELLE | Toggles directional aiming | `directionalaim` |
| DRAW ON AIM / DEGAINE EN VISANT | Draws the weapon when the aim stick is pushed | `drawonaim` |
| PRECISE AIM / VISEE PRECISE | Shots only lock on a target they can reach | `preciseaim` |
| TOGGLE RUN / COURSE PERMANENTE | Toggles toggle-run mode | `togglerun` |
| VIBRATION / VIBRATIONS | Controller rumble | `vibration` |
| INTENSITY / INTENSITE | Rumble strength, 0 to 100 % by 10 (left / right) | `vibrationintensity` |

*DISPLAY / AFFICHAGE*

| Row (EN / FR) | Effect | CFG key |
|---|---|---|
| NATIVE RESOLUTION / RESOLUTION NATIVE | Toggles native resolution | `nativeresolution` |
| ZOOM | Cycles AUTO → x1 → x1.5 → x2 → … → xN → AUTO | `viewzoom` |
| SMOOTHING / LISSAGE | Bilinear filtering of the picture | `smoothscaling` |
| CAMERA FOLLOW / SUIVI CAMERA | Toggles camera follow | `followplayer` |
| 3D LIGHTING / ECLAIRAGE 3D | Lights the world in 3D, with shadows (section 7d) | `lighting` |
| VOLUMETRIC SMOKE / FUMEE 3D | With the 3D lighting: smoke of explosions and shots, steam of liquids (section 7d); on by default | `volumetricsmoke` |
| FLASHLIGHT / LAMPE TORCHE | With the 3D lighting: the Silencer's lamp lights in front of him (section 7d); on by default, L or the controller's FLASHLIGHT action (LT) switches it in play | `flashlight` |
| AMBIENT LIGHT / LUMIERE AMBIANTE | With the 3D lighting: how bright the scene stays where no lamp reaches, 0 to 100 % by 5 (left / right); 85 % by default | `ambientlight` |

*EFFECTS / EFFETS*

| Row (EN / FR) | Effect | CFG key |
|---|---|---|
| IMPACT MARKS / IMPACTS | Shots leave marks on the scenery | `impactmarks` |
| BLOOD / SANG | Cycles OFF → ON (the wounded bleed on the scenery) → GORE (much more blood) | `bloodmarks`, `goremode` |
| CASINGS / DOUILLES | Firearms eject casings that stay on the floor (section 7c) | `casings` |
| LEAKING BARRELS / FUITES DES BARILS | Explosive barrels take more bullets; each pierces them, their fuel makes a pool that catches fire when they blow up and burns whoever stands in it; a barrel left in the fire blows up (section 7e) | `barrelleaks` |
| GLASS DEBRIS / ECLATS DE VERRE | A glass wall shatters into pieces that fly the way the shot went, bounce and scatter on the floor; blasts blow them about (section 7f) | `glassdebris` |

*GAME / JEU*

| Row (EN / FR) | Effect | CFG key |
|---|---|---|
| AUTOSAVE / SAUVEGARDE AUTO | Toggles the autosave | `autosave` |
| ORIGINAL BUG FIXES / CORRECTIFS ORIGINAL | Corrects bugs of the original game that can block a mission | `fixoriginalbugs` |
| LANGUAGE / LANGUE | Cycles the language: AUTO → each folder of `languages` → AUTO; a language's game files need a restart (section 8b) | `language` |

*CHEATS / TRICHE*

| Row (EN / FR) | Effect | CFG key |
|---|---|---|
| LOOT FROM REMAINS / BUTIN DES CENDRES | An enemy burnt, blown apart or vaporised drops its loot beside its remains (section 10b) | `remainsloot` |

**Controls**
- Up or `8`, down or `2`: moves between rows. The focus stops at the first and last row; it does not wrap.
- Enter activates the row: opens a page, toggles an option. A page opens on its first row.
- Escape, or BACK / RETOUR, goes back from a page to the first page, with that page's row focused; on the first page it closes the window.
- Mouse: clicking a row focuses and activates it.
- While the controller screen is open, it receives every event until it closes.
- Changes apply to the in-memory configuration at once. They are written to `CRUSADER.CFG` when the Options screen closes, like the original options.

---

## 2. Gamepad support

**Files:** `src/core/pad_config.hpp`, `src/platform/sdl_gamepad.*`, `src/engine/ui/gamepad_screen.*`

The original only supports the PC game-port joystick, which the port does not reproduce. The port supports any controller that `SDL_GameController` recognises (Xbox and compatibles), with hot-plugging.
The Enhancements window lets players enable or disable controller rumble and set its intensity in 10% steps with the left/right keys (or by clicking the row). The controller briefly rumbles to preview each selected level. Explosions scale with their size. Each shot of the player's weapon gives its own pulse: light for the energy weapons and the BA-40, medium for the BA-41, AC-88, AR-7, RP-22 and RP-32 (the RP-22 and RP-32 are machine guns, not rocket launchers: they fire bullets), strong for the SG-A1 and the GL-303. The controlled character also rumbles when actually losing hit points, but not when a shield fully absorbs the hit.

### 2.1 How it works
Each controller input is **translated into a key, or a key combination, of the original controls**. The engine receives ordinary keyboard or mouse events and has no controller-specific logic.
- For a combination, the modifier (Ctrl, Alt, Shift) is pressed **first** and released **last**.
- The key released is **always the one sent on press**, even if a menu opened in between. This prevents stuck keys.

### 2.2 Actions

| Action (EN / FR) | Original key sent |
|---|---|
| NONE / AUCUNE | nothing |
| FIRE / TIR | Space |
| RUN / COURIR | Shift + ↑ |
| ROLL LEFT / RIGHT | Ctrl + ← / → |
| STEP LEFT / RIGHT | Alt + ← / → |
| CROUCH OR STAND / SE BAISSER OU LEVER | Ctrl + ↓ |
| SHIFT / MAJ | Shift alone |
| PREVIOUS / NEXT WEAPON | `q` / `w` |
| PREVIOUS / NEXT ITEM | `i` / `o` |
| USE ITEM / UTILISER OBJET | `u` |
| MEDICAL KIT / TROUSSE | `m` |
| JUMP / SAUT | Tab |
| MENU | Escape |
| ENTER / VALIDER | Enter |
| SELECT / SELECTIONNER | `s` (selection mode, then next item) |
| FORWARD / BACKWARD / TURN LEFT / TURN RIGHT | ↑ / ↓ / ← / → |
| FLASHLIGHT / LAMPE TORCHE | `l` (port key: the Silencer's lamp, section 7d) |

### 2.3 Configurable inputs (24)
- 12 buttons: A, B, X, Y, LB, RB, LT, RT, left stick click (L3), right stick click (R3), Start, Select.
- The 4 directions of the left stick, the right stick and the d-pad.

### 2.4 Default bindings

| Input | Action |
|---|---|
| A | Select |
| B | Jump |
| X | Enter |
| Y | Next weapon |
| LB / RB | Roll left / right |
| LT | Flashlight |
| RT | Fire |
| L3 | Run |
| R3 | Crouch / stand |
| Start | Menu |
| Select | None |
| Left stick ↑ / ↓ | Forward / backward |
| Left stick ← / → | Step left / right |
| Right stick ← / → | Turn left / right |
| Right stick ↑ / ↓ | Default role: aim (mouse movement) |
| D-pad ↑ / ↓ / ← / → | Use item / medkit / previous item / next item |

> The table in `tools/portmaster/README.md` describes an older mapping. The code (`PadSettings::default_buttons`) is authoritative.

### 2.5 Binding rules
- **An action can be on one input only.** Binding it to a new input removes it from the old one.
- The previous action of the chosen input becomes unbound.
- An unbound input:
  - a **button** does nothing (NONE);
  - a **direction** returns to its **default role**: arrow key for the left stick and the d-pad, mouse movement for the right stick.
- A right-stick direction bound to an action **no longer moves the mouse** that way.

### 2.6 Thresholds and stick response
- **Digital directions** (sticks and triggers): on above **16000** / 32767, off below **10000** (hysteresis against bouncing).
- **Right stick as mouse**:
  - dead zone, 5 levels: 3000, 5000, 7000, 10000, 13000 (default: level 2 = 7000);
  - speed, 5 levels: 300, 450, 700, 950, 1250 px/s at full tilt (default: level 3 = 700);
  - **quadratic** response: precise near the centre, full speed at the edge;
  - fractions of a pixel carry over from frame to frame, and reset when the stick returns to the centre.

### 2.7 In menus
In menus, message boxes and screens, some inputs are **fixed**, whatever the bindings, so a bad configuration can never lock the player out:
- **A** = Enter, **B** = Escape, **Start** = Escape.
- Directions keep their role (arrows, mouse) and do not send the action bound to them.

### 2.8 Quitting
Holding **Select + Start** together quits the game. It is the only way out on handhelds (PortMaster), where `gptokeyb` is not used.

### 2.8b Save names without a keyboard (PortMaster)
Handhelds have no keyboard to type a save's name, and the original refuses to save under an empty one. With **`--auto-save-names`**, which only the PortMaster launcher (`Crusader No Remorse.sh`) passes:
- choosing a slot on the Save screen (F9 or the menu's "save game") proposes the **local date and time** as its name, in place of the slot's: `12/11/2026 14:32` (French: day first), `11/12/2026 14:32` (US: month first), `12.11.2026 14:32` (German);
- **A** saves under it, **B** leaves the slot as it was; a keyboard plugged in can still edit the name;
- a console whose clock is not set (before 2020) gets `SAUVEGARDE` / `SAVED GAME` instead.

Files: `src/engine/ui/load_screen.*` (`set_auto_name`, `dated_name`), `src/game/game.cpp` (`auto_save_name_function`), `src/game/main.cpp`.

### 2.9 Controller screen
- Opened from the first row (CONTROLLER / MANETTE) of the Enhancements screen's CONTROLS / CONTROLES page.
- Two columns, rows 18 px apart:
  - on the left, the 22 actions, then "aim speed", "dead zone", "defaults" and "back";
  - on the right, the bound input ("A", "LB", … or "NONE"), or `< n / 5 >` for the two stick settings.
- **Binding**: clicking an action, or pressing Enter on it, makes the screen wait (PRESS A BUTTON).
  - The next controller input is captured and bound. It does not send a key to the game.
  - Escape, a click or **10 seconds** without input cancel the wait.
- **Delete** unbinds the focused action.
- Left/right (or `4`/`6`), or a click on the left/right half of the value, adjusts speed and dead zone.
- "Defaults" restores the whole configuration.
- Outside the wait, the controller drives the screen with the arrows, A (Enter) and B (Escape).
- The focused row is kept between two openings.

### 2.10 Diagnostics
The environment variable `CRUSADER_PAD_DEBUG=1` prints the detected controller (name, GUID, SDL mapping) and each raw and translated input. It helps diagnose a wrong mapping on handhelds.

A wrong mapping is fixed with SDL's own `SDL_GAMECONTROLLERCONFIG` variable (or `SDL_GAMECONTROLLERCONFIG_FILE`, a `gamecontrollerdb.txt`), which takes precedence over everything else. The port also corrects the SDL mappings it knows to be wrong (`kGamepadMappingFixes` in `sdl_gamepad.cpp`): the 8BitDo Ultimate 2 Wireless over Bluetooth on Linux, whose right stick SDL took for the triggers (issue #23).

---

## 3. Directional movement (left stick)

**Option:** DIRECTIONAL MOVE (`directionalmove`, off by default)

The original uses tank controls: up walks forward, left/right turn. With this option, the Silencer **walks in the direction the left stick points**.

**Rules**
- **In play only**: not in a menu, not while the controller screen is capturing an input.
- The stick angle is split into **8 sectors of 45°** (counter-clockwise from east). Each sector sends the matching **numeric keypad** key:
  - 6 → east, 9 (PgUp) → north-east, 8 → north, 7 (Home) → north-west;
  - 4 → west, 1 (End) → south-west, 2 → south, 3 (PgDn) → south-east.
- These are the absolute "screen direction" moves the original already offers on the numeric keypad.
- **Angular hysteresis**: the current sector is kept while the stick stays within **30°** of its axis.
- **Sector change**: the new key is pressed **before** the old one is released, so the avatar turns without stopping.
- **Keypad diagonals held**: at a release, the keypad diagonals (7, 9, 1, 3) count as movement keys still down. The original counts only the gray Home / PgUp / End / PgDn, so a turn from a straight sector to a diagonal one stopped the run (action `0x27`, `0x19` armed) before the key's repeat started it again.
- **Both axes together**: the stick is read once per frame, after all its events: SDL sends X and Y separately, and a released stick read in between paired its new X with its old Y (a ghost direction).
- **Release and rebound**: a released stick springs back past the centre. A turn of more than **120°** between two frames is either that rebound or a quick turn back: the held key stays down for **80 ms**, then, if the stick is still pushed firmly the other way, the new key is pressed before the old one is released (the run turns back without stopping, as in the original when the opposite keypad key goes down before the old one is let go); otherwise the key is released. A push that way within **80 ms** of a release is ignored (pressed after that if still held); a weak push (below the press threshold) never changes the sector of a held key.
- The **RUN** action becomes **Shift alone**, held with the movement key, instead of Shift + ↑.
- When the mode changes, everything the previous mode held down is released.
- **Strafing while aiming**: while the right stick aims (directional aiming) and the controlled NPC has its weapon out (draw actions 7 / 0xC, holstered by 0xB / 0x10), the stick strafes instead of giving the absolute keypad directions. The stick's direction is taken relative to the aim, in 4 sectors of 90° (kept up to 60° from their axis), and gives the original's Alt moves, which keep the aiming mode:
  - towards the aim: `Alt + up`, slow forward (action `0x24`, `0x2C` with a weapon that does not keep the actions);
  - away from it: `Alt + down`, slow backward (action `2`, or `0x2D`);
  - to its left / right: **side steps**, `Alt + left / right` (actions `0x1C` / `0x1D`).
- Without aiming, the stick keeps the absolute directions, weapon out or not.
- Changing between the absolute directions and the strafes releases the old key and Alt **before** pressing the new one, so no key is sent with the wrong modifier.

---

## 4. Directional aiming (right stick)

**Option:** DIRECTIONAL AIM (`directionalaim`, off by default)
**Option:** DRAW ON AIM (`drawonaim`, on by default, used with directional aiming)
**Files:** `src/platform/sdl_gamepad.cpp`, `src/engine/world/aim_mode.cpp` (`aim_at_angle`)

The right stick puts the crosshair directly in the direction it points.

**Rules**
- In play only. In this mode the right stick **no longer turns** the avatar and **no longer moves the mouse**.
- On above 16000, off below 10000 (hysteresis).
- With **DRAW ON AIM** on, pushing the stick while the weapon is holstered **draws it at once** (action `7`, remapped by the weapon), exactly as the fire key does, and the aim mode follows. The trigger no longer has to be pressed first. If the avatar is busy (walking, rolling...) the draw waits until it is free while the stick stays pushed. Switched off, the stick does nothing until the trigger has drawn the weapon, as before.
- Angle conversion (`world_angle_of_screen`): the crosshair goes **where the stick points on screen**, at the muzzle's height. The isometric view shows a step `(x, y)` on the ground as `(x − y, (x + y) / 2)` (y down), so the conversion inverts that projection: screen east is 315° in the world, south 45°, but the world's axes show 26.6° off the screen's horizontal, not 45°. (A mere rotation, `315° − screen angle`, was up to 18.4° off along the world's axes, the corridors' directions: a target there fell in the next 16-way direction.)
- On each update, `aim_at_angle` applies when all of these hold:
  - aiming (combat) mode is active, or the weapon is holstered, **DRAW ON AIM** is on and the avatar is free (it is then drawn);
  - standing up is not forbidden and no turn-around is in progress;
  - no modal window or cutscene is open;
  - the player is not in stasis and controls the Silencer (NPC 1).
- The crosshair is placed again on every update (it follows the avatar when it strafes).
- With **PRECISE AIM**, the stick also picks a target on screen (see [4b](#4b-precise-aim)): the crosshair then points at it on the ground.
- If the avatar does not face the crosshair (16-way direction measured from its centre; that of the target picked by the stick) and is not busy, it turns with its guard animation: `0x2E` kneeling, `4` standing.
- If target designation is active, the target search runs again after the turn, or at once when the stick picks another target in the same direction.
- **Turret of a surveillance monitor** (BASEGUN `0x4C8`, or any NPC whose shape has no action 1): the original never puts it in aiming mode (`DS:2864`), so it has no crosshair and shoots straight ahead in one of its 16 directions, or at the target found that way; the arrow keys turn it one direction at a time. The stick turns it at once to the direction nearest where it points on screen (with **PRECISE AIM**, towards the target it picks there, which its shots then take first), with the same guard animation. Without this, a controller only had the 8 directions of the left stick.
- **Leaving the aiming mode**: releasing the stick does not leave it, as in the original. Walking with the left stick (not aiming) leaves it at once; a **side step** or a **roll** leaves it for the duration of the animation. To put the weapon away, double-click the **right mouse button**; on the controller, the **jump** action (B by default) holsters a drawn weapon first, then jumps.

---

## 4b. Precise aim

**Option:** PRECISE AIM / VISEE PRECISE (`preciseaim`, off by default)
**Files:** `src/engine/world/projectile.cpp` (`find_target`), `src/engine/world/aim_mode.cpp` (`designate`, `stick_pick`)

In the original, a shot of the player locks on the **nearest** target in the 16-way direction it is fired in (`TRACKER_1128_0158`), wherever it is in height and whether or not it can be reached. In levels with several floors, the nearest enemy is often one floor up or down, behind the slab: the shot flies to it and ends in the floor or the ceiling, while the enemy in sight on the player's floor is left alone. A nearer enemy behind a wall does the same.

**Rules**
- For the controlled NPC only (the player, or a robot taken over). Enemy shots are unchanged.
- A candidate the shooter has **no line of fire** to is passed over: the same test the original makes before showing the target marker and before an enemy fires (`Item::I_fireDistance`: a sweep from the muzzle points of the shot animation, in that direction, to the target's aim point). The next candidate, by the original rules (NPCs first, then markable shapes, then the nearest), takes the shot.
- No reachable target: the shot flies to the crosshair, as the original does without a target.
- A shooter whose shot animation has no firing frame is not filtered (the test cannot be made).
- The target marker (target designation, **R**) uses the same search, so it **always shows the target the shot goes to**. The original quirk where a new target in an unchanged direction stayed unmarked is left out with the option.
- Unchanged: the 16 (or 8) directions, the 0x400 range, the spread of the shots.

**With the right stick (directional aiming)**

The search above works on the ground, in 16-way directions. A target on another floor does not show where its place on the ground is: one floor below (96 units) it shows 96 pixels lower, and pointing the stick at it gave another direction. With the option, the stick picks its target **as the screen shows it**:
- The candidates are those the search could take (tracked, in the combat zone, within 0x400) that the avatar has a line of fire to, each in its own direction.
- Each is seen from the crosshair's centre (the avatar's centre, at the muzzle's height) to its aim point, projected with its height. The one nearest in angle to the stick, **within 20°**, is taken; the one already taken stays unless another is more than 5° nearer.
- The avatar faces it (in the 16-way direction the search sees it in), the crosshair points at it on the ground, and as long as it stays in that direction the shots take it **first** (`GameState::Aim::stick_target`), as does the marker.
- No such target: the crosshair goes where the stick points, as without the option.
- The stick target is dropped when the aiming mode is left.
- Test: `test_stick_aim_floor_below` (save `visee-etage-inferieur.sav`: the Silencer on a catwalk, a soldier on the floor below; a camera in the next direction stays out of reach behind a pillar).

---

## 5. Camera follows the player

**Option:** CAMERA FOLLOW (`followplayer`, off by default)
**Files:** `src/engine/world/world.cpp` (`track_camera_target`, `shift_camera`, `update_follow_camera`)

**Original**: the view only moves when the controlled character gets near an edge: within 100 px left or right, 140 px at the top or 70 px at the bottom. It then **jumps** to put the character at x=520, x=120 or the centre.

**With the option**
- The controlled character is kept at the **centre of the view** (horizontal centre, vertical centre + 40 px) at every step.
- **Smooth glide**: each frame, the camera covers about **1/10 of the gap**, at least 1 px.
- If the gap exceeds **200 px horizontally or 150 px vertically** (teleport, map change), the camera jumps straight there.
- Follow only applies when the camera target is the controlled character. It is suspended while a modal window is open.
- **Anti-jitter**: the screen-to-world conversion truncates halves and quarters separately, so the wanted position can land one or two pixels off. The port tries the neighbouring camera positions (±3 units on each axis) and keeps the one whose view is closest to the wanted view. If none beats the current view, the camera does not move.
- The option is read every frame: changing it takes effect at once.

---

## 6. Native resolution

**Option:** NATIVE RESOLUTION (`nativeresolution`, **on by default**)
**Files:** `src/game/main.cpp` (`fit_screen`), `src/game/game.cpp` (`render`, `draw_window`), `src/engine/world/world.cpp` (`set_view_size`)

**Original**: 640×480. **With the option**: the game view fills the whole window and shows more of the surroundings.

**Rules**
- Scale factor = the largest whole number for which `window / factor` is still at least 640×480. Pixels stay square and sharp.
- Logical view size = `max(640, width / factor)` × `max(480, height / factor)`.
- The camera keeps its place relative to the centre: origin at `w/2` from the left and `300 + (h − 480)/2` from the top. The extra pixels are spread around the scene.
- The fast area grows with the view. This is the area where the "glob" scenery is unfolded, monster eggs create their NPCs and items run. The original sets it to ±5 cells in `s = cx + cy` and ±3 in `d = cx − cy` around the camera cell, which goes beyond the 640×480 screen. The port adds one cell of `d` per 256 extra pixels on each side, and one of `s` per 128 extra pixels above and below. The world is always complete in the view, and everything still wakes up out of sight. At 640×480 the area is the original one.
- **The combat zone stays the original's**: the 640×480 screen the original would show (`World::combat_rect`). It is centred on the camera like the original screen when the camera is placed. Then it scrolls by the original rules as the Silencer moves: when he gets within 100 pixels of its left or right edge, 140 of its top or 70 of its bottom, it jumps to put him back (`track_camera_target`). This happens whether the larger view scrolls on its own edges or follows him, and the zone always stays whole inside the view. So the Silencer never leaves it, even when he walks far from the centre of a wide view.
  - An NPC only spots the player, keeps him as a target, fires or is woken by a shot inside this zone. The player's shots only pick their target there.
  - An enemy in the side bands is visible, patrols and comes closer, but behaves like an off-screen enemy in the original.
  - Security cameras (CAMERACO, CAMERANS, CAMERAEW) and wall guns (WALLGUN) sweep as soon as they are in the view (`Item::I_isPartlyOnScreen`, 0x007, tests the whole view). They only spot the player from the combat zone: `Item::I_getRangeIfVisible` (0x044) for the cameras, `Item::I_fireDistance` (0x116) for the guns. A camera in a band therefore sweeps over the Silencer without raising the alarm, as the original never let it see that far.
  - Purely visual tests keep the whole view: animation frame skipping, sound volume, and effect scripts (bubbles, flames, steam).
- **These stay 640×480, centred**: menus, screens (options, load, keypad, computers…), cutscenes, status bar, message boxes, cursor and autosave icon. The rest of the screen is black behind menus and cutscenes.
- Mouse coordinates are mapped to this centred 640×480 block.
- With the option off, or with `--classic` on the command line: the original 640×480 view, stretched to the window.
- The window opens maximised, except with `--classic` or `--fullscreen`.
- Windows: the process declares per-monitor DPI awareness (`permonitorv2`), so the system does not re-stretch the window at 125% or 150%.

---

## 7. Zoom

**Option:** ZOOM (`viewzoom`, default `0` = AUTO)

Zoom sets the **maximum size of a game pixel** in native resolution.

**Rules**
- `AUTO` (0): the largest **whole** factor the window allows (see section 6).
- `x1`, `x1.5`, `x2`, `x3`, `x4`: the chosen factor if the window allows it (window size ÷ 640×480, fractions included), otherwise the largest whole factor. `x1` gives the finest picture and the widest field of view.
- `x1.5` is for windows that are not a whole multiple of 640×480, e.g. 1360×768 (limit 1.6, so only x1 and x1.5 are offered). The view is drawn at window ÷ 1.5 and stretched, so some game pixels are one screen pixel wider than others.
- In the Enhancements screen, the value cycles AUTO → x1 → x1.5 → x2 → x3 → x4 → AUTO, stopping at the largest the current window allows (recomputed when the window changes).
- The value is stored as `1`, `1.5` (`1,5` is read too), `2`… and clamped to 1..4 (0 = AUTO).

---

## 7b. Impact marks

**Options:** IMPACT MARKS / IMPACTS (`impactmarks`) and BLOOD / SANG (`bloodmarks`), both off by default and independent: either can be on without the other. Switching one off forgets its own marks only.
**Files:** `src/engine/world/damage_marks.*`, `src/engine/world/projectile.cpp`, `src/engine/world/item_damage.cpp`, `src/game/game.cpp`

The original leaves no trace of a shot. With this option the scenery keeps one where a shot ends.

**Marks**
- **Bullets** (fire types 1, 2, 0xB, 0xD): a hole of about 2 × 2 black pixels with a dark rim and a few chips.
- **Explosions** (any fire type that splashes: rockets, grenades…): a scorch with a ragged edge, black in the middle. Its radius is 3 × the splash radius, 10 to 32 pixels. It covers the item hit and the scenery around it: a floor gets a scorch squashed to the ground, centred under the blast; a wall face gets it centred across from the blast. The further the face, the smaller the scorch (a quarter of a pixel less per unit). At most 24 items per explosion.
- **Energy weapons** (every other fire type: lasers, plasma, electric…): a small burn, 3 pixels in radius.
- A shot that hits nothing marks the floor right under where it ends, if there is one within 16 units below.

**Rules**
- The mark belongs to the item it lands on and is drawn just after it, inside its opaque pixels, by darkening them through the game palette. Whatever stands in front of the item covers the mark too.
- Only scenery takes marks: not NPCs, shots, translucent shapes (windows, force fields) or editor markers. A face the camera does not see (the back of a wall) takes none.
- A mark goes away when its item moves, changes frame or shape, flips or is destroyed. It also goes on a map change, a load, or when the option is switched off. Past 1024 marks the oldest go first.
- Marks are not saved: a loaded game starts clean.
- Marks only read the world and use their own numbers, never the game's random generators, so the game plays the same with or without them.
- Palette colours 1..14 (cycled or translucent) are never changed or used.

**Blood** (option BLOOD / SANG, `bloodmarks`, `goremode`, off by default; ON or GORE)
- Who bleeds: an NPC that loses hit points to a shot or a blast, if bullets hurt it fully (the living, not the robots). Energy weapons do not draw blood: they burn, freeze or melt. Blood is drawn in the game palette's reds, mixed with the colour underneath.
- **A bullet** (or a blast that does not kill): a spray on the first wall behind the victim, along the shot, at chest height, with drips running down: within 48 + 2 × the hit points lost (up to 150 units), always when the hit kills, 75 % of the time otherwise. Drops, stretched the way they flew, land on the floor behind: 2, plus 1 per 10 points lost, plus 2 for a kill (7 at most). A kill leaves a pool under the body.
- **Killed by a blast**: a bigger pool under the body; 18 to 26 pieces and drops thrown up to 150 units away, in a fan of ±80° away from the blast; sprays on the walls met by seven rays across that fan.
- **GORE**: every bullet sprays the wall behind, within 80 + 3 × the points lost (up to 200 units), bigger; 4 drops plus 1 per 6 points lost, plus 4 for a kill (14 at most), thrown further. A bullet kill adds two sprays 25° to the sides, a bigger pool and 6 to 10 pieces thrown behind. A blast kill: 32 to 44 pieces up to 200 units away in a fan of ±100°, sprays along thirteen rays, a pool up to 20 pixels.
- A wall face the camera does not see takes no spray. Blood marks follow the impact marks' rules: on scenery only, gone when it changes, not saved.

## 7c. Casings

**Option:** CASINGS / DOUILLES (`casings`), off by default, independent of the impact marks and the blood.
**Files:** `src/engine/world/casings.*`, `src/engine/world/damage_marks.*`, `src/game/game.cpp`

The original ejects nothing. With this option every shot of a firearm throws out a casing that stays on the floor.

**Which weapons**
- The weapons that fire bullets (fire types 1, 2, 0xB, 0xD, the same as the bullet holes of section 7b). BA-40, BA-41 (type 0xB), RP-22 and RP-32 (type 1) eject a brass casing. SG-A1 (type 2) and AC-88 (type 0xD) fire several pellets and eject a red shell with a brass head.
- Energy weapons and explosives (PA-21, EM-4, PL-1, PA-31, UV-9, AR-7, GL-303) eject nothing.
- Whoever fires ejects: the player, an NPC or a robot. The pump of the SG-A1 is not shown: its shell flies out with the shot.

**Flight**
- One casing per firing frame of the animation (one per shot, one shell per shotgun blast).
- It starts at the muzzle, pulled back towards the shooter, and flies out to the shooter's right (a quarter turn clockwise from its facing), a little back, and up: 1.2 to 2.2 units a tick sideways, -0.2 to 0.7 back, 2.2 to 3.4 up, with a gravity of 0.32 units a tick per tick. It spins a quarter turn every two ticks.
- It falls to the height of the shooter's feet at the time of the shot, bounces once (30 % of its speed down, half its speed along the ground), then lands. A flight never lasts more than 120 ticks.
- It moves one step per game tick (60 a second), so it freezes when the game does (menus, pause).
- In flight it is drawn right after its shooter (after the player's weapon overlay), so whatever stands in front of the shooter covers it. A casing whose shooter is not drawn is not seen. At most 64 fly at once; past that the oldest goes.

**On the floor**
- A landed casing becomes a mark on the floor right under it (at most 16 units below; down a ledge there is none, and it is lost). A casing is two pixels (bright brass and dark brass), a shell three (dark red, red, brass), each with one pixel of shadow, lying one of four ways.
- Unlike the other marks, it replaces the colour underneath with the palette's nearest brass or red, instead of darkening it.
- It follows the impact marks' rules: drawn inside the floor's opaque pixels, covered by what stands in front, gone on a map change, a load, or when the option is switched off; not saved. At most 512 casings lie on a map; past that the oldest goes, and the other marks stay.
- Casings only read the world and use their own numbers, never the game's random generators, so the game plays the same with or without them.

---

## 7d. 3D lighting

**Option:** 3D LIGHTING / ECLAIRAGE 3D (`lighting`), DISPLAY page, off by default. `--lighting` switches it on for one run.
**Option:** VOLUMETRIC SMOKE / FUMEE 3D (`volumetricsmoke`, written only when off), DISPLAY page, on by default: the smoke and the liquids' steam below. Without the 3D lighting it does nothing.
**Files:** `src/engine/world/lighting.*`, `src/platform/gl_lighting.*`, `src/game/game.cpp`, `src/platform/sdl_presenter.*`; the `lightview` tool and docs/LIGHTVIEW.md

The original draws flat sprites under a fixed light. With this option the world is lit in 3D in true colour.

**Geometry**
- Each pixel of the scene gets a 3D point and a normal, from the box of the item that shows there (TYPEFLAG.DAT footpads): the top, +x or +y face the view ray enters. Characters (the NPC table, ids 1..255, and NPC shapes) are billboards, rounded from their silhouette. The sprite's own shading adds small bumps.

**Lights**
- Items with colour-cycled pixels (palette entries 1..14: lamps, screens, force fields, lasers) light the scene around them in their colour.
- Lamps give light (wall and ceiling lamps, floodlights, red wall lamps and lanterns, strip lights, desk lamps; monitors and wall screens a dimmer one), stronger the brighter the ambient light so that they still show at 85 %, and glow in the air round the bulb (the floodlights the strongest, with a tight halo): a soft halo over what is behind the lamp, cut by what stands in front. A lamp shot goes out. Vending machines and drinks fridges, lit from inside, cast a cool light out of their glass onto the floor in front of them (not on the wall behind) and glow softly over it, a vending machine's light flickering with its neon; shot, they go dark. The alarm lights and ALERT signs light only during an alert, pulsing red.
- Flashes, explosions and shots (sprites) light the surface under them.
- Firearms flash at the muzzle: each shot of the BA-40, BA-41, RP-22, RP-32, SG-A1, AC-88 and GL-303, the player's or an enemy's, lights a warm flash at the end of the gun for a fifteenth of a second, which lights the shooter and the walls around. Where a bullet lands, the spark gives only a small light. Energy weapons and rockets need no flash: their shot glows by itself from the muzzle on.
- The Silencer carries a lamp (FLASHLIGHT / LAMPE TORCHE option, on by default): a beam from his chest the way he faces, tipped down, that lights the floor and what stands in front of him (a cone of 40 degrees, full within 23). He never shadows it himself.
- **L** switches the lamp on or off in play, the same setting as the option (saved); Ctrl+L stays the cheat. On the controller, the FLASHLIGHT action sends L; it is on **LT** by default. Without the 3D lighting, L does nothing, as in the original.
- The rest of the scene keeps most of its light (AMBIENT LIGHT / LUMIERE AMBIANTE option, 85 % of its light by default), so the game stays readable. Lower, the lamps stand out more; 100 does not darken the scene at all.

**Shadows**
- Item boxes cast shadows, cut by their sprites: railings, grids and bodies cast their outline. A light is never shadowed by the item that gives it; lasers, electrified floors and flat items (floors, grates, mats) cast none.

**Smoke** (VOLUMETRIC SMOKE option)
- Explosions leave a dark cloud: six puffs of noise that glow orange for the first second (lit from inside by the explosion), then rise, grow and thin out over five or six seconds. It stays see-through, as much for a big blast as for a small one (the bigger, the thinner), so that what happens in it can be followed.
- A shot leaves a small wisp blown out of the muzzle along its way, slowed down at once and gone in under a second. Where it hits a wall, a floor or an item, a little dust; a shot that ends in the air (out of range) leaves none. Rockets in flight carry a flickering orange light (their exhaust) and leave a trail; other bright shots a trail.
- The light of a shot or an impact is where it is; only explosions and flashes, which the game draws over the scene, light the surface under them.
- Broken machines smoulder for good, a thin wisp rising from them: screens and computers, consoles, fans and vents, chargers, generators and transformers, vending machines and drinks fridges, security cameras; a fan, a thicker dark smoke from its motor, in the middle of its face. Lamps do not: they have no electronics.
- Pierced pipes leak small white puffs of steam out of their hole, except where the game blows its own jet of steam (the valves' STEAM1, STEAM2): there the jet's sprite is the steam, and a shut valve lets nothing out, as in the game.
- The smoke is a volume: in front of a wall it hides it, behind it it is hidden, and the scene's lights colour it.
- It follows the game's clock: it stops with the game (menus, pause), goes on a map change. It only reads the world and uses its own numbers, never the game's random generators.

**Liquids**
- The green liquids (the pools, their strips and edges, ripples and bubbles: shapes 0x150, 0x59E, 0x227, 0x228, 0x5A2, 0x55A, 0x56D, 0x56E, 0x1D6, 0x539, 0x54C, 0x580; no TYPEFLAG bit marks them) glow a faint radioactive green, in slow patches that drift and breathe.
- They light what stands around them (walls, rims, floors, machines, pillars, people at the edge) in green, with one light for each 1024 x 1024 cell of liquid, high over it so that it falls on the floors past the rims and reaches 1600 units; it does not light the liquid itself.
- Around them the ambient light is lower (down to 25 % of it over most of that reach, the strongest liquid near counting, not their sum), so that a room with a pool or a canal is lit by its green glow rather than by the general light. The liquid itself and the lit points away from it keep the ambient light chosen.
- The pools give off a thin steam: wisps that rise from the surface, drift and fade out over seven to eleven seconds, faintly green. They need no memory (each comes back from the game's clock and the pool's place), so they are there as soon as a pool is in view; they go with the smoke (VOLUMETRIC SMOKE option).

**What is not lit**
- Everything drawn over the world: the status bar, the aiming sprites, the cursor, menus, screens and cutscenes, shown in their palette colours. Palette fades and colour cycling still apply.
- Lighting only changes the picture: the game plays the same with or without it.

**Cost**
- On the GPU (OpenGL 3.3, or OpenGL ES 3.0) when there is one: the processor finds which item shows at each pixel and gathers the lights, occluders and smoke; a fragment shader does the rest, in a hidden window of its own, and the picture is read back for the usual presentation. The context is made the first time the option is on.
- Otherwise on the processor, on all its cores, by 16 x 16 tiles: the same picture, slower. `--cpu-lighting` forces it.
- Measured with `lightview` (16 lights, shadows, smoke), a frame takes about 3 ms at 640 x 480, 6 ms at 1280 x 720 and 10 ms at 1920 x 1080 with the GPU, against 7, 14 and 26 ms on the processor.
- The liquids add about 5.5 ms at 1920 x 1080 over two large pools with the GPU, 14 ms on the processor (their steam is marched in half the steps of smoke and lit as at its centre).

## 7e. Leaking barrels

**Option:** LEAKING BARRELS / FUITES DES BARILS (`barrelleaks`), EFFECTS page, off by default.
**Files:** `src/engine/world/barrel_leaks.*`, `src/engine/world/projectile.cpp` (the barrels' damage), `src/engine/world/item_damage.cpp` (a barrel blowing up), `src/engine/world/lighting.*` (the fire's smoke and glow), `src/game/game.cpp`

The explosive barrels (BARRELUP 0xAA upright, BARRELS to BARRELSE 0x151..0x155 lying) take several bullets before they blow up: their class adds up the damage of each hit and explodes at 100 (150 in *No Regret*). In the original the bullets before the last leave no trace. With this option each of them pierces the barrel, and its fuel runs out.

**Tougher barrels**
- A bullet does a barrel 3/5 of its damage (at least 1), after the game's own changes (difficulty, a third for bullets on the lifeless): it takes about 1.7 times as many bullets to blow one up, and so leaks more. Blasts do their full damage, so barrels still blow each other up.

**Holes**
- Only bullets pierce: the fire types that leave a bullet hole (section 7b) or eject a casing (section 7c). Eight holes at most per barrel.
- The hole is where the bullet hit: on the round side of an upright barrel, on the face hit of a lying one. A hole in the top lets nothing out.
- A barrel starts 90 % full. A hole below the fuel's level leaks 1/10 s after the hit; the bullet that blows the barrel up leaks nothing.

**Leak**
- The fuel's speed out of a hole grows with the square root of the fuel's height above it (Torricelli): a jet out of the side, which falls to the floor, then, as the level drops, a weaker one, at last drops running down the side. It stops once the level is down to the hole. Every hole lowers the level.
- A jet is a thin brown stream with light flecks running along it, two pixels wide near a strong hole, with a little splash where it lands. Under each hole the side stays wet down to the floor. A hole on a side the camera does not see leaks without its jet being drawn.

**Pool**
- The fuel makes one pool per barrel: a patch where each jet first landed, one at the foot of the barrel under the hole, and three fingers running on from where the jet landed, mostly the way it flowed; all grow with what ran out of that hole and flow together. A barrel emptied by a low hole makes a pool up to about nine times its width.
- Its edge is no circle: pushed in and out by broad noise (up to 26 units, less for a small pool), then ragged on a finer scale.
- It lies on the floor under the barrel (the highest floor at most 16 below it) and on the floors at that level around, as far as it runs from where it started: not down a ledge, not under what stands on the floor (walls, crates), nor past a wall onto the floor on its other side (a drop splashing against a wall does not streak through it).
- Dark brown, a little see-through at its edge, with a slow sheen.

**Trail**
- The rolling barrels (ROLLB_NS 0x586, ROLLB_EW 0x58B: lying barrels that a shot sets rolling, 8 units a tick, until they meet something and blow up, by their own class) leak too: a bullet pierces them as it does the others (their damage stays the game's own).
- A leaking barrel that moves leaves its pool where it was: a rolling barrel, or a barrel thrown by a blast (20 to 50 units a step, a quarter as far at each step after, so 30 to 70 units in all). On its way each hole that leaks drops the fuel every 6 units: an upright barrel slides, and only its holes below the level leak, at the foot of the side under them; a lying barrel rolls every hole under the fuel, and they all leak under it, where the hole lies along its length.
- The trail is uneven: it wanders from side to side (up to 14 units, over some 120: the barrel wobbles), its drops are wider or narrower (some 10 to 20 units wide, the wider the stronger the leak), and a lying barrel brings each hole round under itself about once a turn: a splash there, a broken dribble while the hole is up.
- The trail goes on in the pool it starts from, then, past 64 drops, in new pools that touch one another; it lies on the floors at that level (none while the barrel falls), and a barrel that ends up at another level starts a new pool there. Once the barrel has been still for 16 ticks, its jets make a pool where it lies. While a lying barrel rolls, its holes and jets are not drawn (they turn round with it).
- When the barrel blows up, its pool catches fire where it stood, and the fire runs back along the trail (2.5 units a tick) to where it started, from one pool to the next (a burning pool lights a pool it touches once its fire has come where they touch); pools of its trail at other levels catch at once, from their end nearest the barrel. A trail burns lower than a pool (its flames down to 45 % of the height, the more of the pool is trail).

**Reflections**
- The pool mirrors what stands behind it: walls, doors, crates, people, the barrel itself, the fire. In this view a mirror in the floor shows, above each of its pixels, the scene mirrored about the line where the floor behind it ends; so each pool pixel takes the pixel as far above that line as it is below it.
- Darkened and browned (more for what is far behind, over 45 pixels), and a little wavy. The rim and the sheen do not mirror; a burning or burnt pool does not.
- It is done on the finished picture, column by column, from what was drawn: a pool mirrors what is on screen up to 120 pixels above it.

**Explosion**
- Every barrel that blows up, shot or not (its last bullet, a blast, another barrel going off), throws what was left in it. Most of it (45 %) burns in the blast. A fifth runs out where it stood: a pool of fire, the bigger the fuller the barrel was, with four fingers, that spreads out in 12 ticks, fast then slower, from a quarter of its size (it no longer lies there at its full size at once). The rest (35 %) flies all round in 12 to 32 burning drops, shared out round the barrel (all directions, give or take), 1.2 to 5.6 units a tick, up to 6 units a tick upwards: they land a few metres off (up to some 250 units), each carrying a share of that fuel by its size. Several barrels going off throw as many sprays.
- A drop flies as a burning blob with a tail along the way it came, drawn over the scene. Where it meets:
  - a floor or the top of something: a pool there catches fire; elsewhere what it carries splashes there, burning 2 to 5 seconds: a blot (5 to 34 units, from the fuel it carries as a pool would), drawn out ahead of it along its way the faster it flew (one to three smaller blots), with three droplets round it thrown forwards;
  - a wall the camera sees: the fuel burns on it 2.5 to 5 seconds, flames rising up the wall, and leaves a sooty stain, black in its middle, of a radius of 13 to 22 units, that stays. It is drawn on whatever shows that point of the wall (a door or a panel drawn over it);
  - someone: they catch fire (see below), for two seconds.
- A drop that meets nothing for 2.5 seconds goes out.

**Fire**
- A blast within reach of a pool, an energy shot (laser, plasma, fire...) that ends on it, a burning drop or a burning pool that touches it sets a pool alight too.
- The fire spreads from where it caught at about 2.5 units a tick, unevenly: faster one way than another (from 0.75 to 1.25 times as far). It is drawn pixel by pixel: every point of the floor it has reached burns, more strongly where the pool is deep, and flames rise off it up to 64 pixels, losing heat as they go up. Noise rising faster than the flames, in streaks taller than wide and swaying a little, breaks them into tongues that lick up and part from the fire. The hotter a pixel, the whiter: white, yellow, orange, deep orange, then red, a see-through dark red and a wisp of smoke at the tips. The flames rise from the near parts of the pool over the far ones; where two meet, the hotter shows. The pool under the fire glows a dark orange.
- The pool burns by patches of 32 x 32 units, each drawn in the game's order as a box of its own (the box of its fuel, kept out of what stands on the floor), so what stands in front of the fire covers it and the fire covers what is behind: a fire just behind a wall the camera sees, drawn black, stays hidden by it.
- It burns 4 to 15 seconds (longer the bigger the pool), dies down for its last 40 %, from the edge in, and leaves a burnt stain that stays.
- With the 3D lighting (section 7d), the fire gives a flickering orange light, its pixels glow as they are, and, with the volumetric smoke, a dark smoke rises off it, lit from below at first: a puff every 7 ticks off a barrel's pool, every 21 off a drop's little one, every 39 off a fire on a wall (each puff costs the lighting all the pixels it covers).
- Cost (`game_tests data --barrel-bench`, lighting on the processor, a barrel's pool burning with five fires on walls): about 4.5 ms more to draw at 640 x 480, 8 ms at 1920 x 1080.

**Burning people**
- Whoever stands in a burning part of a pool (their feet, or 6 units around them, where it burns at a quarter of its strength or more, up to 16 above the floor) catches fire, the Silencer as much as the others.
- Someone on fire takes 5 damage of fire type 0xC, the game's own fire, at once then every quarter of a second, changed by the difficulty as any hit. A human it kills burns and runs, as in the game. It goes on for a second after they leave the fire.
- They are drawn in flames: patches of fire over their body, rising off it, weaker once out of the fire; with the 3D lighting they carry an orange light.
- Only pools, people and fuel barrels catch from a fire: the rest of the scenery does not burn.

**Barrels in the fire**
- A fuel barrel (BARRELUP, BARRELS to BARRELSE; not the rolling ones) standing in a burning part of a pool (the floor at its sides or corners, a few units out, where it burns at a quarter of its strength or more, up to 16 above the floor) heats up: whole or pierced, whether the pool is its own or not.
- After 1 to 2 seconds in the fire (60 to 120 ticks, its own number, kept if it cools off and heats up again: short of what the fire of a splash lasts by it), it blows up: the game deals it 250 damage of fire type 0xC, enough for its class to explode, which then throws its fuel as any barrel blowing up (above) and may light, or blow up, the barrels around it. Out of the fire, it cools off at a fifth of the pace it heated up.
- From a third of the way there, flames climb its sides, higher and stronger as it nears blowing up; with the 3D lighting it glows orange.

**Rules**
- Leaks use their own numbers, never the game's random generators. Only the barrels' damage, the burns and the barrels blowing up in a fire change the game.
- They follow the game's clock (they stop with it) and are not saved: a load or a map change forgets them, and so does switching the option off. At most 32 barrels, 96 pools, 160 drops in flight and 96 fires on walls are followed; past that the oldest go.

## 7f. Glass debris

**Option:** GLASS DEBRIS / ECLATS DE VERRE (`glassdebris`), EFFECTS page, off by default.
**Files:** `src/engine/world/glass_debris.*`, `src/engine/world/render.*` (sprites drawn among the items), `src/engine/world/projectile.cpp` (the shot's way), `src/engine/world/item_damage.cpp` (the blasts), `src/game/game.cpp`

The glass walls and dividers (classes GLASS_EW 0x27B, GLASS_NS 0x2F3, GLDIV_EW 0x541, GLDIV_NS 0x542, in every mission) break at their first hit: their class plays the pane crumbling, lays a pile of broken glass (0x27A, 0x349) on each side where one fits and leaves the wall's foot. In the original the piles never move: blasts only throw what has a weight, and theirs is 0. With this option a pile is shown as the pieces of glass it is made of, and they follow the shots and the blasts.

**Pieces**
- Each pile's sprite is cut into some fifty irregular pieces of about 7 x 5 pixels (pixels go to the nearest of seeds strewn one to a cell), each drawn with the pile's own pixels. While its pieces are shown, the pile itself is not drawn.
- The pieces are drawn and ordered among the items, each with a small box of its own, so walls, people and scenery in front of them hide them as they hide items.

**A wall that breaks**
- The pieces of its piles burst out of the pane, from all along it and from the floor up to 56 units high (low rather than high), the way the shot went: the projectile's own way, from where it was fired to where it hit (the game gives an item it hits a random direction). They go 0.5 to 3.9 units a tick along it, the smaller ones faster, spread to either side, and up to 2 units a tick upwards.
- An explosive shot that hits the wall (rockets, grenades) throws them twice as hard. A blast that breaks it from beside throws them away from itself, 1.2 to 2.1 times as hard, the nearer the harder.
- With a bullet most land in a heap at the foot of the wall, on the side the shot went, and a trail of smaller pieces runs on beyond (some 80 units).

**In flight and on the floor**
- A piece flies under gravity (0.34 units a tick per tick), turning over as it goes (drawn mirrored every 3 to 8 ticks). On the floor it bounces once or twice (a third of its fall back up, its speed along the floor 0.6 times), then slides, slowing down (0.88 of its speed kept each tick, less 0.16), and stops.
- It bounces back off what is solid (0.3 of its speed), and falls off the edge of a floor onto the one below.
- A blast (any fire type that splashes: 3, 4, 0xA, 0xE, 0xF) throws the pieces within its reach (at least 96 units) up and away from it, up to 9 units a tick right beside it, less further off, and the lighter further. It also turns into pieces the piles it reaches that were still whole (those already there in a saved game, or broken without the option).
- A shot of another kind that hits a pile kicks the pieces within 28 units of where it hit the way it went.

**Rules**
- The piles stay the game's own items: once a pile's pieces have all come to rest, the pile moves to their middle, and that is what a saved game keeps. The pieces are not saved: after a load or a map change, or with the option switched off, the piles show again where they stand.
- At most 4000 pieces; past that the piles at rest longest show as piles again.
- Only the piles' moves change the world. The pieces use their own numbers, never the game's random generators, and follow the game's clock.

---

## 8. Autosave

**Option:** AUTOSAVE (`autosave`, **on by default**)
**Files:** `src/game/game.cpp` (`autosave`, `request_autosave`, `draw_autosave_icon`), `src/engine/world/inventory.cpp`, `src/engine/usecode/npc_intrinsics.cpp`, `src/engine/world/teleport.cpp`, `src/engine/ui/load_screen.*`, `src/engine/ui/floppy_icon.hpp`

### 8.1 Slot
- A dedicated file, **`SAVEGAME.12`**, separate from the player's 11 slots and from the quick save (`SAVEGAME.13`).
- Save name: AUTOSAVE (French: SAUVEGARDE AUTO).

### 8.2 Triggers

| Event | When the save happens |
|---|---|
| Picking up a **keycard** (shape `0x111`) | At once |
| A door opened with a keycard: a script calls `I_hasKeycard` and the player has the card | At once |
| A **keypad** accepts the right code (door or chest) | At once, when the keypad closes |
| **Mission start**: teleport to the start egg `0x1E` | Deferred |
| **End of a mission cutscene** | Deferred |

The end of a cutscene does **not** trigger a save for Weasel's shop cutscenes, the intro movies under the start-up menu, or the end credits.

### 8.3 Deferred save
- A deferred request waits **1 second** (60 kernel ticks) so the scripts can settle.
- It then waits until the player **can act**: not in stasis, alive, controlling the Silencer, no modal window.

### 8.4 When no save is written
No save (immediate or deferred) is written if any of these is true:
- the option is off;
- a menu, a modal screen or a cutscene is open;
- no world is loaded;
- the player (NPC 1) does not exist or is dead;
- the player controls another NPC, for example a robot taken over from a monitor.

A write error is logged to stderr and does not stop the game.

### 8.5 Indicator
- A **floppy disk** shows for **2.5 s**, at double size, 8 px from the corner.
- The RGBA source image is converted on the fly to the nearest colour of the current palette.

### 8.6 Loading
- **Load screen**: the autosave is a **12th row** (y = 406). The Save screen keeps the original 11 slots, so the autosave cannot be overwritten by hand.
- **F6** in play: offers to load the autosave, with the same question and notice as F4 (quick load). The key is ignored if the file does not exist, in stasis, or when another NPC is controlled.

---

## 8b. Language

**Option:** LANGUAGE (`language`, AUTO by default)
**Files:** `src/core/language.*`, `src/core/translation.*`, `src/core/data_files.*`, `languages/<id>/port.txt`, `scripts/update_languages.py`; the process: [docs/TRANSLATING.md](docs/TRANSLATING.md)

The port prints some texts itself: its own screens (Enhancements, controller), and the texts the original keeps in its executable (item names, Weasel's shop, a few messages, the version lines). The code writes them in English (`tr("BACK")`). The `languages` folder next to the executable (else in the current folder) holds one folder per language: `port.txt` translates those texts, and any other file replaces the data folder's file with the same path (pictures, fonts, scripts, subtitles...). The repository ships `en/port.txt` (generated, the reference) and `fr/port.txt` (the French `CRUSADER.EXE` 1.08's texts, and the port's own).

- **AUTO**: the texts of the data's language, `fr`, `en` or `de` (as the original, which has the texts of its language only), and no replacement data files.
- **The folders**, by id: the row shows each `port.txt`'s `@name` (ENGLISH, FRANCAIS...). A text `port.txt` does not translate stays in English.
- The texts change at once; `port.txt` is read again after each pick, so a translator sees the changes. The data files are read at start-up: when the language picked has other data files than the ones read, the GAME page says "RESTART TO LOAD ITS GAME FILES". `CRUSADER.CFG` keeps the id in lower case (`language=pt-br`); an unknown id gives AUTO. Without the folder, the texts are in English.
- A data file is looked up in the language folder, then in the mod folder (level editor), then in the data folder. The data's version is still detected from the data folder: a language's files must be translations of that version's. A translated script (`EUSECODE.FLX`) may not load games saved with another language's.

**`port.txt` format** (UTF-8; `src/core/translation.hpp`, docs/TRANSLATING.md)
- `#` comments, blank lines, `@name = <name>`, then `<English text> = <translation>` lines. `\n` is a line break, `\\` a backslash; an empty translation keeps the English text.
- The translations are converted to code page 850, the fonts' code page. The fonts must have the letters used (the menus of the port use capitals without accents; a language folder can bring its own `STATIC/FONTS.FLX`).
- A translation must keep the `%` conversions of its English text, in the same order, or it is ignored (with a message on the error output).
- `scripts/update_languages.py` writes `en/port.txt` from the sources and brings the other `port.txt` files in line; a test (`language_files`) fails when one is out of date or `fr/port.txt` misses a text.

---

## 9. Toggle run

**Option:** TOGGLE RUN (`togglerun`, off by default)

**Original**: the Silencer runs while Shift is held.

**With the option**
- Each **press** of Shift (left or right) **flips** running on or off. Key auto-repeats are ignored.
- The Shift press itself is not passed to the game.
- The movement controller sees Shift held **exactly while running is on**, whatever the physical state of the key.
- Turning the option off resets running to "off".

---

## 9b. Original game bug fixes

**Option:** ORIGINAL BUG FIXES / CORRECTIFS ORIGINAL (`fixoriginalbugs`, off by default)

Some bugs are in the original game itself, in its scripts (usecode) or its level data, and the port plays them as they are. This option corrects those that can block a mission. Off, the game behaves exactly as the original. The crashes and misbehaviours of the original executable are a different matter: the port always avoids them (section 17).

**Keycards lost with an NPC that leaves no body**
**Files:** `src/engine/usecode/item_intrinsics.cpp` (`Container::I_destroyContents`)
- **Original**: many keycards are not placed in a level but handed out when an NPC dies. A keycard egg (shape `0x4E7`) belongs to a group of NPCs. The death script (`NPCDEATH`) creates the card in the inventory of the **first NPC of the group to die**, then moves that inventory into the body. Some NPCs leave no body: the courier robot (`0x45D`) and the shapes `0x385` and `0x4E6`. For them, the script destroys the inventory, card included.
- **Example**: mission 10. Behind the teleporter to the chemist's room, the courier, the chemist and two guards share the group of the green keycard 9, which opens the lift further on. If the courier dies first, the card is gone. Checked in the original under DOSBox. The mission can still be finished with a second green card, hidden on a shelf of a nearby lab.
- **With the option**: when the inventory of an NPC is destroyed this way, its keycards (`0x111`) are put down on the floor where it stood instead.
- Origin fixed it the same way in *No Regret*: its `NPCDEATH` puts the card on the floor where the NPC stood when the NPC is a robot that leaves no body (`NPC` function 42: shapes `0x2CB`, `0x45D`, `0x338`, `0x385`, `0x4E6`, `0x383`, `0x30C`, `0x319`, `0x5E2`), before emptying it. The No Regret port therefore needs no option for it.
- Groups with such an NPC were also found on maps 13, 17 and 29.
- Test: `game_tests --courier-keycard`.

---

## 10. Keypad navigation with arrows or gamepad

**Files:** `src/engine/ui/keypad_gump.*`

The original keypad (door and chest codes) only works with the mouse or the number keys. The port adds an arrow-key selection, useful with a controller.

**Rules**
- The arrows move a selection over the **3 columns × 4 rows** grid: 1–9, clear, 0, enter. The selection stops at the edges; it does not wrap.
- The selection is **inactive until an arrow is pressed**. Until then Enter keeps its original role: validate the typed code, if it has at least one digit.
- With the selection active, Enter **presses the selected button**, with sound `0x3A` for clear and `0x3B` for the others.
- Typing a digit, clearing or clicking turns the selection off.
- Everything else is as in the original: 8 digits at most, cheat code `74697689`, comparison with the code modulo 10000, sounds `0x32`/`0x31`.

---

## 10b. Loot from remains (cheat)

**Option:** LOOT FROM REMAINS / BUTIN DES CENDRES, on the CHEATS / TRICHE page (`remainsloot`, off by default). Asked for in [No Regret's issue #2](https://github.com/sebsebseb1982/crusader-no-regret-decomp/issues/2), for both games.
**Files:** `src/engine/world/death_loot.*`, `src/engine/world/projectile.cpp`, `src/engine/usecode/item_intrinsics.cpp`

**Original**: an enemy burnt, melted or electrocuted by an energy weapon, or set on fire by a blast, leaves no body to search. The death usecode (NPCDEATH) destroys what the enemy carried and makes its remains (ashes, a skeleton…) without the loot a body gets. Killing enemies with plasma therefore costs ammunition and gives nothing back.

**With the option**, the loot a body would have held drops on the floor beside the remains: the weapons the enemy carried (NPCDEATH's function `0x21` moves them into a body), and what NPCDEATH's function `0x22` adds, drawn for the shape the enemy had alive.

**Deaths concerned** (the shapes NPCDEATH leaves without a body): burnt `0x58F`, electrocuted `0x59C`, burning `0x576`, melted `0x596`. The player's own death drops nothing.

**Rules**
- The loot drops when the original would destroy it, as NPCDEATH starts: at the death animation's event (the end of the fall, or of the run for a burning enemy), or when the remains enter the fast area. A death whose animations call no event (the electrocuted `0x59C`) drops it at once.
- Each item takes a free spot around the remains, 32 units away in the 8 directions, then 64; when none is free, the remains' own spot. It then falls if nothing holds it.
- The loot is drawn with the game's random numbers, as for a body: switching the option on changes the following random draws.
- A game saved during a death animation loses that enemy's loot.

---

## 11. Window, fullscreen, mouse and picture

- **Alt+Enter**, at any time: switches between window and desktop fullscreen. The key is not passed to the game.
- `--fullscreen` starts in fullscreen.
- The window is resizable. Title: "Crusader: No Remorse — DECOMP". The game has its own icon (`src/game/res/crusader.ico`, a Windows resource).
- The system cursor is hidden, because the game draws its own.
- In play, SDL relative mouse mode replaces the original's re-centring of the pointer at (320, 200): same movement, without moving the system pointer. When a menu opens, the system pointer is put back on the game's cursor.
- `--gamma <x>` brightens (> 1) or darkens (< 1) the picture. 1 is the original palette.
- Fatal errors are shown in a message box (`SDL_ShowSimpleMessageBox`) instead of a DOS text screen.

---

## 12. Version lines on the main menu

Two small lines are centred at the bottom of the main menu, in the dimmed colour of the option buttons:
1. `PORT V<version>` (French: `PORTAGE V<version>`). A leading `v` in the build version is dropped. The line is hidden if the version is empty.
2. `GAME DATA V<x.yy> <FR|US>` (French: `DONNEES DU JEU …`). The version is read from `CRUSADER.EXE` (the string "CRUSADER: No Remorse Vx.yy"). If it is not found, the line shows UNKNOWN or UNKNOWN LANGUAGE.

### 12b. Christmas hat

The original No Remorse has no Christmas mode; No Regret plays Christmas music from 24 to 31 December. The port borrows those dates:
- From **24 to 31 December** (the computer's local date at launch), the Silencer of the main menu wears a red Christmas hat with white fur. It is drawn over the menu's background, in its palette and with its scanlines.
- **Ctrl+M** (next music track) turns the Christmas mode off until the game is quit: the hat is gone the next time the menu opens.
- There is no option: outside these dates the menu is the original's.

---

## 13. Game versions: French 1.08, US 1.23 and German 1.07

**Files:** `src/core/language.*`, `src/core/data_info.*`, `src/engine/usecode/intrinsic_numbering.cpp`, UI screens

An original executable handles one language. The port accepts the **French 1.08** and **US 1.23** data and adapts to the data it finds. The engine is the French 1.08 one; the US differences that matter have been ported routine by routine.

**Rules**
- The language is detected when the data loads. French is the default.
- The texts stored in `CRUSADER.EXE` (item names, messages, Weasel's shop) are written in English in the port, the French ones in `languages/fr/port.txt`, chosen by language. The US data also names grenades 2 to 4 (CONCUSSION, NERVE GAS, EMP), left "INVALID" in French (the port shows the English names). The port's own screens follow the same language (`tr()`, section 8b).
- The US scripts call the native routines (intrinsics) under **their own numbering**. The port translates it.
- Some US screens place their buttons for the English images: the port uses the US positions with US data (F4/F5/Alt+X questions, help, main menu, options, difficulty).
- Save names: the US version refuses `<`, `:` and `;`.
- NPC slot recycling when the table is full follows each version: French recycles only an NPC with status bit `0x0002`, US any NPC outside the active area.
- **No `CRUSADER.CFG`** (US data copied without the installer): the installer's defaults apply, i.e. **dubbed videos** (`bigvideo`) and **subtitles** on.
- Music files (`M01.AMF`…) are looked up **case-insensitively**, for Linux file systems.
- Saves from one version do not load with the other (different usecode).

**German 1.07** (`data_info.hpp`)
- Its `CRUSADER.EXE` is the French 1.08 one translated: same code and tables, same intrinsic table (`UNKCOFF.DAT`, `UNKDS.DAT`, `OVERLOAD.DAT` byte for byte), so the port runs it with the French numbering. It is told by its banner (`V1.07`) and its start-up message.
- Its texts are in `languages/de/port.txt` (item names, Weasel's shop, messages, `<MEHR>`), the port's own screens translated too (AE, OE, UE for the umlauts the menu fonts lack).
- Its executable moves some buttons for the German pictures; the port does the same: main menu ("options", "credits", "quit"), options screen (the "off" buttons, the video line, "quit", the labels of the volumes, the focus frames), help screen, F4/F5 questions (the Alt+X one stays French), difficulty screen (one centred picture, as in the US version).
- Save names: the German executable maps a US keyboard to a German one (Y and Z swapped, the umlaut keys give Ä, Ö, Ü) and takes neither `<`, `:`, `;` nor quotes; the port takes the umlauts from the player's keyboard layout. The dates proposed by `--auto-save-names` are `18.11.2026 14:32`.
- Nothing is cut from the game itself: the usecode is the French one with German texts (and two signs' frames), and the shapes, the damage table and the animations are the French ones. Only pictures, voices, movies and subtitles differ.
- Not reproduced: the joystick's repeat delay of the German executable (20 ticks instead of 30); the port has its own controller support.

---

## 14. Installing from a CD image, no CD needed

**Files:** `src/core/iso_image.*`

The original reads much of its data from the CD and needs it in the drive. The port reads everything from `data/` and never needs a CD.

**Rules**
- A `.iso` file (or `.gog`, or a `.bin` image with its `.cue` sheet) can be passed on the command line, or dropped into `data/`. The first image found is used; a `.cue` sheet opens the file of its `FILE` line.
- If `data/` does not exist, it is created with a `PUT_GAME_DATA_HERE.txt` note.
- If `data/STATIC/` already exists, the ISO is ignored: the data is considered installed.
- Otherwise the ISO (ISO 9660, 2048-byte sectors or raw 2352-byte images) is extracted into `data/`, about 550 MB, with progress printed to stderr.
  - The `CATALOG/` folder (a demo browser unrelated to the game) is skipped.
  - The `;1` version suffix of file names is dropped.
- The ISO can be deleted afterwards.
- If no data is found, a message says to copy an installation (French 1.08, US 1.23, English 1.01 or German 1.07) or the CD image.
- The US-only CD features are not needed and their CFG keys are ignored: CD transfer icon (`cdicon`), game path (`crupath`), music file per track (`tune1`–`tune16`).

---

## 15. Start-up, command line and tools

- **Normal launch**, without `--save` or `--new-game`: movies T01 and T02, then the start-up menu with no game in progress, as in the original (`Game_start`). In this menu, "Back" and "Save" do nothing. Palette colour cycling stays paused under this menu, to avoid flicker.
- `--new-game`: starts the first mission directly.
- `--save N`: resumes `SAVEGAME.N` directly.
- `--version`: prints the version.
- `--classic`: original 640×480 view (see section 6).
- `--fullscreen`, `--gamma <x>`: see section 11.
- Test and tooling options, without a window:
  - `--frames N --png out.png [--wav out.wav]`: runs N frames and writes the last image (and the sound);
  - `--size WxH`, `--scroll DX DY`, `--walk`, `--fire`, `--use ID`;
  - `--menu`, `--menu-load`, `--menu-credits`, `--menu-options`, `--menu-gamepad`, `--menu-new`, `--movie NAME`, `--movie-overlay NAME`;
  - `--debug-title`: diagnostics in the window title (FPS, processes, missing intrinsics…).
- Bundled tools: `shapeview`, `mapview` (shape and map viewers), `shapedump`, `sortcheck`, `ucsmoke` (runs the scripts of every map and reports missing intrinsics), `useprobe`, `diffcheck` (runs the original routines in an emulator and compares them with the port).

---

## 16. `CRUSADER.CFG` handling

**Files:** `src/core/game_config.*`

**Original**: the whole file is rewritten from the global variables, with `\n\r` line endings.

**Port**
- Only the lines of known keys are replaced. Other lines (sound card, CD drive, mouse buttons…) are **kept, in their order**.
- `\r\n` line endings, as the installer writes them.
- Missing or unreadable file: the defaults of the original data segment (or the US installer defaults, see section 13).

**Added keys** (written only when they differ from the default, so the file stays compatible with the original):

| Key | Default | Written when |
|---|---|---|
| `followplayer` | false | `true` |
| `directionalmove` | false | `true` |
| `directionalaim` | false | `true` |
| `drawonaim` | true | `false` |
| `togglerun` | false | `true` |
| `impactmarks` | false | `true` |
| `bloodmarks` | false | `true` |
| `goremode` | false | `true` |
| `casings` | false | `true` |
| `remainsloot` | false | `true` |
| `nativeresolution` | true | `false` |
| `autosave` | true | `false` |
| `lighting` | false | `true` |
| `volumetricsmoke` | true | `false` |
| `flashlight` | true | `false` |
| `ambientlight` | 85, clamped to 0..100 | ≠ 85 |
| `language` | empty (AUTO) | a language folder is picked: its id (`fr`, `en`, `pt-br`...) |
| `viewzoom` | 0 (auto), 1..4 with decimals (`1.5`) | ≠ 0 |
| `padmap` | see section 2.4 | controller config ≠ default. 24 comma-separated action numbers; `-1` = default role; values clamped |
| `padaim` | 3, clamped to 1..5 | same |
| `paddeadzone` | 2, clamped to 0..4 | same |

The old `padbuttons` key, from an earlier format, is removed on write.

---

## 17. Robustness and bug fixes

The original stops on a fatal assertion, divides by zero or reads past a table in some rare cases. The port keeps the original behaviour whenever it is safe, and degrades gracefully otherwise.

**Bug of the original fixed**
- **F4 / F5 questions** (quick load / quick save): in the French 1.08 executable, their "yes" / "no" buttons used the positions of the Alt+X (quit) question. The port places them where the images are (x 49 / 121; the US executable uses 55 / 135 for all three).

**Crashes avoided**
- **Item table full**: the original stops on an assertion; the port keeps creating items beyond the table. Those extra items cannot be saved.
- **NPC creation without a free slot** (`Actor_I_createActorCru`): the US executable stops on an assertion (`ITEM.C`, line 3279). The port keeps the French behaviour (the alert lights switch back) in both versions.
- **Divisions by zero** in the gravity and throw code (zero speed, 16-bit overflow): the port returns no velocity instead.
- **Out-of-range reads**: credits text without its `#` terminator, music period table, fonts with missing frames, unknown item numbers in the item tables (`DTABLE`), status-bar items missing from `DTABLE.FLX`: the port bounds-checks.
- **Pick-up list**: an item of an unknown kind ends on a fatal assertion in the original (`DISPLAY.C`); the port does not list it.
- **Item searches** (usecode `loop` scripts): a stack overflow or underflow in the search expression is a fatal assertion in the original; the port treats the item as not matching.
- **`fadeToGivenColor`** with a colour component outside 0..63: the original stops on an assertion; the port masks the value.
- **Broken map records**: about 0.7% of the original map records reference shapes or frames that do not exist. The port loads them as they are and skips what it cannot draw.
- **Missing `TRIG.DAT`**: the trigonometry tables are computed.
- **Save write failure**: reported, and the game goes on (the original ends).

---

## 18. Known deviations from the original

These differences are deliberate or not yet resolved. They are documented in the code.

**Saves**
- The port writes only the data it manages, in the original order: entries 9, 90, 91, the item table and its lists (0, 1), NPCs (10), player block (20), tracked items (50–52), usecode globals (60), music (70, 71).
- Screen sprites (30–33), the `YAMM` memory (40, 41) and the kernel processes (100+) are not written. **Port saves therefore cannot be loaded by the original executable yet.** The port does load original saves.
- When loading, saved kernel processes are not read back: the active area is rebuilt, which restarts the "enter" events instead of resuming the running scripts.
- The save name is printed as is. The original passes it to `printf` as a format string, so a `%` in the name caused trouble.
- The cursor is not hidden while typing a save name.

**Input**
- **AZERTY**: the French original converted QWERTY to AZERTY, because its BIOS returned QWERTY codes. The port's key codes already follow the player's layout, so no conversion is done.
- **PC joystick**: not reproduced (replaced by the gamepad, section 2).
- **Ctrl+M** (next music track): not ported.
- In menus: the left button has no double-click delay; the right button is ignored.
- Keypad internals not reproduced: the `0x10` event type, and the keyboard flush the game does on close.

**Display**
- The **frame-skip** option (`frameskip`) is kept in the Options screen and the CFG, but has no effect: frames are never skipped.
- **Projectiles** are drawn as world items instead of sprites, with the same look.
- **Screen sprites** (aiming mode) are drawn opaque; the `0x10` item state blitter is not reproduced.
- **Draw order**: the sorting rules come from ScummVM's reconstruction (the original routine is not located yet), checked against the game.
- The status bar and the pick-up list are redrawn entirely each frame (the original only redraws what changed; same result).
- Blocking fades (fade to black on Alt+X, some transitions) are not reproduced.
- **Colour cycling** is recreated with each game; the original keeps it, with its modified palette, from one game to the next.

**Game logic**
- The two random generators of the executable are reproduced (`random_next` from `SYSTEM.C`, Borland's `rand()`), but their state is not carried from one game to the next within a session.
- A reused item slot does not keep its old coordinates.
- During a cutscene, the timed processes are frozen, and the sound plays as one buffer.
- US data: the reset of the player and inventory between missions that the US executable does is not reproduced.
- Cheats: the typed code, `-laurie`, F10, Ctrl+F10, `~`, T, f, Ctrl+Q, Ctrl+L, Ctrl+M, the "hack mover" (H: drag items and characters with the left button) and Weasel's free level work as in the original; its debugging displays (bounding boxes of f, F7, Alt+F7, Ctrl+F7) and the memory status (Ctrl+V) are not reproduced (`src/engine/world/cheats.hpp`). With a native-resolution view, the hack mover's cursor stays in the 640 x 480 window at the centre of the screen, and the mover no longer clears the player's second ammunition slot when it takes an item (a leftover of Ultima 8 in the original, `src/engine/world/world_mouse.hpp`).

---

## 19. Platforms, technical base and distribution

**From DOS hardware to SDL2**

| Area | Original | Port |
|---|---|---|
| Video | VESA `int 10h`, banked VRAM, asm blitters copied to RAM | 8-bit framebuffer in memory, presented through an `SDL_Texture` |
| Palette | VGA DAC ports, 6-bit components | 256-entry RGB table applied at the 8 → 32-bit conversion |
| Keyboard | keyboard ISR, PC scancodes, BIOS shift state | SDL events translated back to PC scancodes |
| Mouse | `int 33h` driver with callback | SDL mouse events, relative mode in play; the game's own cursor is kept |
| Timer | PIT and PIC interrupts, 60 Hz | `SDL_GetTicks64`; the kernel still ticks at 60 Hz |
| Sound | Asylum Sound System (`ASYLUM.DLL`), DMA and IRQ | software mixer and AMF music player rewritten from `ASYLUM.DLL` (12 sound voices, 8 music voices), 16-bit mono at 22,050 Hz to an SDL audio device |
| Files | DOS `int 21h`, CD loading | `std::filesystem`, everything read from `data/` |
| Memory | Phar Lap 286 extender, selectors, `huge` pointers | **removed**: flat 32/64-bit memory |

**Platforms**
- Windows x64 (SDL2 built into the executable), Linux x64, and **Linux aarch64 handhelds through PortMaster** (muOS, Knulli, ROCKNIX, ArkOS…). The cross-build uses Zig (`tools/portmaster/build.py`).

**PortMaster**
- `Crusader No Remorse.sh` launcher, which checks the data and logs to `log.txt`.
- `port.json` and `gameinfo.xml` metadata.
- The game reads the controller itself, **without `gptokeyb`**, whose default keys doubled every input.
- Select + Start quits the game.
- An update script over MTP for the RG40XX-H.

**Releases and tests**
- Versions follow SemVer, computed from Conventional Commits. Each release is built and tested on Linux and Windows (see `docs/RELEASING.md`).
- Unit tests run without game data. End-to-end tests (`e2e/`) run on the real data and on bug-reproduction saves (`e2e/saves`). Several enhancements have their own tests: autosave, keypad, scripted camera, configuration, controller settings.
- `diffcheck` runs routines of the original executable in an emulator (Unicorn) and compares them with the port on random inputs (directions, throw paths, random generators: no difference over 20,000 to 50,000 inputs each).

## 20. Level editor

**Files:** `src/editor/`, `src/game/game.*` (`game_file`, `set_start`), `docs/EDITEUR.md`

The original ships no editor. `crusader-no-remorse-editor` (Dear ImGui on SDL2, rendered by the port's engine) opens the maps of `FIXED.DAT` and the globs of `GLOB.FLX`, decodes the egg parameters (triggers, teleports, monsters, globs, markers) and the low-quality links, and saves to a mod folder without touching the data. The game reads that folder with `--mod` and starts on a given map with `--map`. Full user guide: `docs/EDITEUR.md`.

**Rules**
- Unmodified maps and globs are rewritten byte for byte (`editor_data_tests`); container contents keep their raw depth (map 244 contains records that are not objects).
- A file in the mod replaces the one in the data: `FIXED.DAT`, `GLOB.FLX`, `SHAPES.FLX`, `TYPEFLAG.DAT`, `DAMAGE.FLX`, `ANIM.DAT`, `DTABLE.FLX`, `EUSECODE.FLX`, `UNKDS.DAT`.
- `--map N`: the new game plays out like the original's, but `MainActor_I_teleportToEgg` receives map N and egg `--egg` instead of 1 and 0x1E (`game_tests --editor-mod`).
