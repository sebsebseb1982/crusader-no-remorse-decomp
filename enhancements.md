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
5. [Camera follows the player](#5-camera-follows-the-player)
6. [Native resolution](#6-native-resolution)
7. [Zoom](#7-zoom)
8. [Autosave](#8-autosave)
9. [Toggle run](#9-toggle-run)
10. [Keypad navigation with arrows or gamepad](#10-keypad-navigation-with-arrows-or-gamepad)
11. [Window, fullscreen, mouse and picture](#11-window-fullscreen-mouse-and-picture)
12. [Version lines on the main menu](#12-version-lines-on-the-main-menu)
13. [Two game versions: French 1.08 and US 1.23](#13-two-game-versions-french-108-and-us-123)
14. [Installing from a CD image, no CD needed](#14-installing-from-a-cd-image-no-cd-needed)
15. [Start-up, command line and tools](#15-start-up-command-line-and-tools)
16. [`CRUSADER.CFG` handling](#16-crusadercfg-handling)
17. [Robustness and bug fixes](#17-robustness-and-bug-fixes)
18. [Known deviations from the original](#18-known-deviations-from-the-original)
19. [Platforms, technical base and distribution](#19-platforms-technical-base-and-distribution)

---

## 1. Enhancements screen

**Files:** `src/engine/ui/enhancements_screen.*`, `src/engine/ui/options_screen.*`

The original has nothing like it. This screen gathers every option the port adds.

**Opening it**
- The Options screen has a text button, **ENHANCEMENTS** (French: **AMELIORATIONS**), with a "+" icon. It sits on row 9, left of "Quit", in the Quit button's colours.
- Left from "Quit", or right from the button, moves between the two. Enter opens it.
- If the fonts are not loaded, the button is not shown.

**Look**
- The screen is cleared. A double red frame (colour `0x57`, corners cut over 3 pixels like the original buttons) fills (100,84)–(539,426).
- The title is green (`0x0C`), with the "+" icon.
- 10 rows, 27 px apart. The focused row is bright green (`0x0C`); the others are dark green (`0x75`).
- An on/off option shows its name on the left and its value on the right: ON / OFF (French: MARCHE / ARRET).

| # | Row (EN / FR) | Effect | CFG key |
|---|---|---|---|
| 0 | CONTROLLER / MANETTE | Opens the controller screen | — |
| 1 | CAMERA FOLLOW / SUIVI CAMERA | Toggles camera follow | `followplayer` |
| 2 | DIRECTIONAL MOVE / MODE DIRECTIONNEL | Toggles directional movement | `directionalmove` |
| 3 | DIRECTIONAL AIM / VISEE DIRECTIONNELLE | Toggles directional aiming | `directionalaim` |
| 4 | DRAW ON AIM / DEGAINE EN VISANT | Draws the weapon when the aim stick is pushed | `drawonaim` |
| 5 | NATIVE RESOLUTION / RESOLUTION NATIVE | Toggles native resolution | `nativeresolution` |
| 6 | ZOOM | Cycles AUTO → x1 → x1.5 → x2 → … → xN → AUTO | `viewzoom` |
| 7 | AUTOSAVE / SAUVEGARDE AUTO | Toggles the autosave | `autosave` |
| 8 | TOGGLE RUN / COURSE PERMANENTE | Toggles toggle-run mode | `togglerun` |
| 9 | BACK / RETOUR | Closes the screen | — |

**Controls**
- Up or `8`, down or `2`: moves between rows. The focus stops at the first and last row; it does not wrap.
- Enter activates the row. Escape closes the screen.
- Mouse: clicking a row focuses and activates it.
- While the controller screen is open, it receives every event until it closes.
- Changes apply to the in-memory configuration at once. They are written to `CRUSADER.CFG` when the Options screen closes, like the original options.

---

## 2. Gamepad support

**Files:** `src/core/pad_config.hpp`, `src/platform/sdl_gamepad.*`, `src/engine/ui/gamepad_screen.*`

The original only supports the PC game-port joystick, which the port does not reproduce. The port supports any controller that `SDL_GameController` recognises (Xbox and compatibles), with hot-plugging.
The Enhancements window lets players enable or disable controller rumble and set its intensity in 10% steps with the left/right keys (or by clicking the row). The controller briefly rumbles to preview each selected level. Explosions scale with their size; firing the player's rocket launcher produces a sustained pulse, slightly weaker than a large explosion. The controlled character also rumbles when actually losing hit points, but not when a shield fully absorbs the hit.

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
| LT | None |
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

### 2.9 Controller screen
- Opened from row 0 of the Enhancements screen.
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
- **Release and rebound**: a released stick springs back past the centre. A turn of more than **120°** between two frames releases the key instead of pressing the opposite one; a push that way within **80 ms** of a release is ignored (pressed after that if still held); a weak push (below the press threshold) never changes the sector of a held key.
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
- Angle conversion: world angle = `315° − screen angle`, because screen east is 315° in the world.
- On each update, `aim_at_angle` applies when all of these hold:
  - aiming (combat) mode is active, or the weapon is holstered, **DRAW ON AIM** is on and the avatar is free (it is then drawn);
  - standing up is not forbidden and no turn-around is in progress;
  - no modal window or cutscene is open;
  - the player is not in stasis and controls the Silencer (NPC 1).
- The crosshair is moved when the angle changes.
- If the avatar does not face the crosshair (16-way direction measured from its feet) and is not busy, it turns with its guard animation: `0x2E` kneeling, `4` standing.
- If target designation is active, the target search runs again after the turn.
- **Leaving the aiming mode**: releasing the stick does not leave it, as in the original. Walking with the left stick (not aiming) leaves it at once; a **side step** or a **roll** leaves it for the duration of the animation. To put the weapon away, double-click the **right mouse button**; on the controller, the **jump** action (B by default) holsters a drawn weapon first, then jumps.

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

## 9. Toggle run

**Option:** TOGGLE RUN (`togglerun`, off by default)

**Original**: the Silencer runs while Shift is held.

**With the option**
- Each **press** of Shift (left or right) **flips** running on or off. Key auto-repeats are ignored.
- The Shift press itself is not passed to the game.
- The movement controller sees Shift held **exactly while running is on**, whatever the physical state of the key.
- Turning the option off resets running to "off".

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

---

## 13. Two game versions: French 1.08 and US 1.23

**Files:** `src/core/language.*`, `src/core/data_info.*`, `src/engine/usecode/intrinsic_numbering.cpp`, UI screens

An original executable handles one language. The port accepts the **French 1.08** and **US 1.23** data and adapts to the data it finds. The engine is the French 1.08 one; the US differences that matter have been ported routine by routine.

**Rules**
- The language is detected when the data loads. French is the default.
- The texts stored in `CRUSADER.EXE` (item names, messages, Weasel's shop) are kept in both languages in the port and chosen by language. The US data also names grenades 2 to 4 (CONCUSSION, NERVE GAS, EMP), left "INVALID" in French. The port's own screens follow the same language (`tr(fr, en)`).
- The US scripts call the native routines (intrinsics) under **their own numbering**. The port translates it.
- Some US screens place their buttons for the English images: the port uses the US positions with US data (F4/F5/Alt+X questions, help, main menu, options, difficulty).
- Save names: the US version refuses `<`, `:` and `;`.
- NPC slot recycling when the table is full follows each version: French recycles only an NPC with status bit `0x0002`, US any NPC outside the active area.
- **No `CRUSADER.CFG`** (US data copied without the installer): the installer's defaults apply, i.e. **dubbed videos** (`bigvideo`) and **subtitles** on.
- Music files (`M01.AMF`…) are looked up **case-insensitively**, for Linux file systems.
- Saves from one version do not load with the other (different usecode).

---

## 14. Installing from a CD image, no CD needed

**Files:** `src/core/iso_image.*`

The original reads much of its data from the CD and needs it in the drive. The port reads everything from `data/` and never needs a CD.

**Rules**
- A `.iso` file can be passed on the command line, or dropped into `data/`. The first `*.iso` found is used.
- If `data/` does not exist, it is created with a `PUT_GAME_DATA_HERE.txt` note.
- If `data/STATIC/` already exists, the ISO is ignored: the data is considered installed.
- Otherwise the ISO (ISO 9660, 2048-byte sectors or raw 2352-byte images) is extracted into `data/`, about 550 MB, with progress printed to stderr.
  - The `CATALOG/` folder (a demo browser unrelated to the game) is skipped.
  - The `;1` version suffix of file names is dropped.
- The ISO can be deleted afterwards.
- If no data is found, a message says to copy an installation (French 1.08 or US 1.23) or the ISO.
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
| `nativeresolution` | true | `false` |
| `autosave` | true | `false` |
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
- Cheats: the typed code, `-laurie`, F10, Ctrl+F10, `~`, T, f, Ctrl+Q, Ctrl+L, Ctrl+M and Weasel's free level work as in the original; its debugging displays (bounding boxes of f, F7, Alt+F7, Ctrl+F7), the "hack mover" (H) and the memory status (Ctrl+V) are not reproduced (`src/engine/world/cheats.hpp`).

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
- `Crusader.sh` launcher, which checks the data and logs to `log.txt`.
- `port.json` and `gameinfo.xml` metadata.
- The game reads the controller itself, **without `gptokeyb`**, whose default keys doubled every input.
- Select + Start quits the game.
- An update script over MTP for the RG40XX-H.

**Releases and tests**
- Versions follow SemVer, computed from Conventional Commits. Each release is built and tested on Linux and Windows (see `docs/RELEASING.md`).
- Unit tests run without game data. End-to-end tests (`e2e/`) run on the real data and on bug-reproduction saves (`e2e/saves`). Several enhancements have their own tests: autosave, keypad, scripted camera, configuration, controller settings.
- `diffcheck` runs routines of the original executable in an emulator (Unicorn) and compares them with the port on random inputs (directions, throw paths, random generators: no difference over 20,000 to 50,000 inputs each).
