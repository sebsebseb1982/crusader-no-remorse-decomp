# Crusader: No Limit

*Crusader: No Limit* is the procedural level generator of the port. It builds new levels for *Crusader: No Remorse* out of the game's own pieces: the floors, walls and objects of the original maps, and the scripts (usecode classes) that drive them. It writes them into a mod folder, like the level editor (`docs/EDITEUR.md`), and the game plays them with `--mod`.

Goals:

1. **Biomes**: a level uses one or more of the looks of the original missions (base corridors, warehouses, labs...), learned from the original data.
2. **Verticality**: several storeys, joined by lifts, elevators, stairs and catwalks.
3. **Finishable**: every level can be finished; the generator proves it before writing it, and the tests check it in the game's own physics.
4. **The game's interactions**: key cards, doors, switches, keypads and computers, cameras and alarms, turrets, mines, energy walls, steam and valves, explosive barrels, robots, teleporters, NPCs...

## How the original levels are built

What the generator relies on, measured on the 63 maps of `FIXED.DAT` and the 3,072 globs of `GLOB.FLX` (French 1.08):

- A map is a grid of 512 × 512-unit **cells**. 99 % of the cells that have scenery hold **one glob egg**: the glob is the cell's floor and walls.
- Pieces use a **16-unit grid** (the `TYPEFLAG` footprint unit) and their position is their far corner (largest x, y; lowest z).
- **Floors** are flat tiles of 16 × 16, 8 × 8 or 4 × 4 footprint units (256, 128 or 64 units square), height 0.
- **Walls** are 1 unit thick and 1, 2, 4, 8 or 16 units long, one shape per length and direction, 12 high (96 units): **a storey is 96 units**. Glob eggs sit at z 0 (36,800) or 96 (4,400). Railings are 2 high.
- Walls stand inside the cell along its edges (glob x 7 or 255: the first or last 16 units).
- Floor and wall shapes come in families that the designers combined: floor 0x6A with walls 0x23 / 0x24, floors 0xCC or 0x14 with walls 0x08 / 0x09... Those families are the **biomes**.
- Doors are 6 units wide (96 units) and 11 high; the player is 3 × 3 (48 units).

Interactions (usecode classes placed on the original maps, with the number of maps that use them):

| Kind | Classes |
|---|---|
| Doors | `MDOOR_*`, `RDOOR_*`, `SDOOR_*`, `GDOOR_*` (19 to 29 maps), `DOOREGG` |
| Key cards | `CARD_NS` / `CARD_EW` readers (45 maps), card 0x111 (quality = card number) |
| Switches, panels | `SWITCH*`, `SSWITCH*`, `PANEL*`, `SPANEL`, `VALVE`, `TRIGPAD`, `PPLATE2`, `LWPLATE*`, `TIMER`, `EVENT` (through `TRIGGER`, see `docs/EDITEUR.md`) |
| Barriers | energy walls `EWALL*`, `ETWALL*`, lasers `LAZER*`, `LASERI`, shock walls `WSHOCK*`, `SHOCK*`, force floors `FFFLOOR*` |
| Codes and computers | `KEYPAD*` (22 maps), `BIGCOM*`, `MONIT*`, `COPY_*` |
| Surveillance | cameras `CAMERA*`, `CAM_*`, `SURCAM*`, `EYECAM*`; alarms `ALARM_*`, `MALRM_*`, `ALARMBOX`, `ALARMHAT`, `ALRMTRIG`, `CAMERA` egg |
| Defences | turrets `GATGUN*`, `WALGUN*`, `BASEGUN`, `GOVGUN*`, `BLASER*`, `SENTRY`; mines `FLORMINE`, `LANDMINE`, `BETTY`; `DEATHFL*`, `DEATHBOX`, `FLAMEBOX`, `STEAMBOX` |
| Explosives | barrels `BARREL*`, `MBARREL`, `D_BARREL`, `BIGCAN`, rolling barrels `ROLL*` |
| Moving | `ELEVATOR` / `ELEVPLAT` (30 / 45 maps), `LIFT`, `LIFTPIT`, conveyors `CONV*`, `CONVEY_*`, `HOVER*`, `SLIDEFLR` |
| Teleporters | pads `TELEPAD`, `RTELEPAD`, `MTELEPAD`, `STELEPAD`, `GTELEPAD`, `SPIDPAD`; teleport eggs |
| Robots | `VETRON`, spider bombs `CRU_SPID`, robot control (the engine's controlled NPC, `docs/combat.md`) |
| Supplies | `MEDIKIT`, rechargers `RECHAG*`, `RECHFL*`, chests `CHEST_*`, crates `MCRATE`, `BOX_*`, `VALUEBOX`, `SKILLBOX`, `GENERATR` |
| NPCs | MONSTER pairs (44 maps, 4,600 NPCs), monster eggs, reinforcements fired by alarms and switches |
| Scenery and scripts | `SIGN*`, `TRANSFOR`, `JOHN`, `PEPSI*`, `SFXTRIG`, `MUSIC`, `SECRTEGG`, `ONCEEGG`, `TRIGEGG` |

## Design

The generator works in four passes, each one checkable on its own:

1. **Mission graph** (abstract): the rooms to cross, from the start to the exit, and what bars the way between them. Generated as a lock-and-key graph: each lock (a key-card door, an energy wall cut by a switch, a keypad whose code is on a computer, an elevator to power...) has its key placed in a part of the level reachable *before* the lock. Optional branches hold supplies and secrets; cycles give shortcuts.
2. **Layout**: the rooms placed on the storeys (rectangles on the 256-unit grid), the corridors, and the vertical links (lifts, stairs) between storeys.
3. **Building**: the pieces of the biome (floors, walls with their openings, doors, railings), then the interactions and their links (quality low, `TRIGGER` settings: `level_tools`), the NPCs (MONSTER pairs), the supplies and the props.
4. **Verification**:
   - **logical**: a solver plays the mission graph (what is reachable with what is held, what each switch opens) and must reach the exit;
   - **physical**: the level is loaded in the engine, without a window, and a search over the 16-unit grid moves the player's box with the engine's own walking move (`try_move`: steps, support, collisions) from the start to every key, switch and exit the graph needs.

Levels that fail are dropped and the next seed is tried; the report says why.

The code is in `src/editor/generator/`, without SDL, so the editor, the tests and later the game itself (a "random mission" from the menu) can call it.

## Using it

```sh
crusader-no-remorse-editor data --generate 42 --mod mods/gen-42
crusader-no-remorse data --mod mods/gen-42 --map <map printed by the generator>
```

`--levels N` chains N levels: each exit teleporter leads to the next level's start. `--biome N` picks the biome (default: drawn from the seed).

### Saving the seed

Every generation records its seeds in `NOLIMIT.JSON`, in the mod folder (next to the mod's `SAVEGAME` folder): for each generated map, the seed and the options it was generated with, and what they gave (biome, number of rooms). In the editor, the seed of a generated map shows under the map's number, and goes to `NOLIMIT.JSON` when the project is saved.

That file is enough to share levels: copied alone into an empty folder,

```sh
crusader-no-remorse-editor data --regenerate mods/shared
```

generates the same levels again, byte for byte. A seed gives the same level only with the same generator version (`"version"` in the file, raised whenever the generator changes what a seed gives: `--regenerate` refuses another version) and the same game data.

## Milestones

Each milestone ends with levels that can be played by hand, and tests that check them.

| # | Milestone | Playable result |
|---|---|---|
| 1 | **Shell**: biomes learned from the globs; rooms and corridors on one storey with doors; start and exit teleporter; chained levels; physical verification in the engine | walk from the start to the exit of a generated level, through doors |
| 2 ✅ | **Locks and keys**: mission graph and solver; key-card doors, switches and energy walls, panel doors, keypads with their code on a computer | a level where the exit is behind one to three locks |
| 3 | **Verticality**: storeys at z 96 and above, lifts and elevators, stairs, catwalks and railings over lower rooms, multi-storey verification | a level that climbs two storeys to the exit |
| 4 | **Opposition**: MONSTER pairs by difficulty and room, cameras and alarms with their reinforcements, turrets, mines, lasers, steam and valves, explosive barrels | a defended level, its hazards switched off by its controls |
| 5 | **Supplies and props**: medikits, ammunition, chests and crates, rechargers, rooms furnished from the original maps (prefabs taken from the rooms where the designers placed them), secrets | a level that can be finished at every difficulty |
| 6 | **Several biomes and robots**: biome changes between areas, robot control, teleporter networks | a level crossing two biomes |
| 7 | **In the menu, and a bot**: "Random mission" in the game's menu, with a seed; a bot that plays generated levels to the end in `game_tests`, on a range of seeds | a new mission from the main menu |

## Status

**Milestone 1 done** (24 November 2026): the shell.

- Biomes: 6 learnt from the French data (floors 0x06A, 0x014, 0x23D, 0x0CC with their wall families). A floor is a 16 x 16 tile without a script (a death floor such as `DEATHFL3` is no floor); walls are the shapes 12 high and 1 thick. The far walls (north, west) are the shapes the globs use at the low edge of their cells; the near walls use the same shapes in their black frame when they have one (found from the pixels), as the original maps do.
- Layout: 6 to 10 rooms of 2 x 2 to 3 x 3 floor tiles, in slots of a 4 x 4 grid, grown from the start room; about one extra passage in five makes loops. The exit is the room farthest from the start.
- Corridors of one tile, with an automatic door (`MDOOR_N` / `MDOOR_E` and their `DOOREGG`) at each end.
- Start: arrival egg 0x1E in the first room. Exit: a `TELEPAD` with a departure egg to the next level's start (the last level leads back to the first).
- Editor: *Map > Generate a level (Crusader: No Limit)...* (seed, biome, number of rooms); command line `--generate SEED [--levels N] [--biome B] [--mod DIR]` (default mod folder: `mods/gen-SEED`).
- Seeds saved in the mod's `NOLIMIT.JSON`, shown in the editor; `--regenerate DIR` generates a shared file's levels again.
- Tests (milestone 1): `editor_tests` (no data: determinism, layout, one start, one exit, doors and their eggs, 60 seeds); `generator_data_tests data [first_seed] [seeds]` (on the data, played in the engine: walking to every room and to the exit, a door that opens and is crossed, the exit teleport, the editor's level check).

**Milestone 2 done** (24 November 2026): locks and keys.

- The way to the exit gets 1 to 3 locks (option `locks`, -1: drawn), on passages that no other way goes round (bridges; when the loops leave none, they are dropped). Each lock bars the doorway of the room behind it; its key is in a room reached before it, preferably off the way.
- Four kinds, each built as on the original maps (bytes measured on `FIXED.DAT`):

| Lock | Barrier | Key | Settings |
|---|---|---|---|
| Card door | `MDOOR_N` / `MDOOR_E`, quality = card number, no `DOOREGG` | card 0x111 on the floor of the key room (quality = its number, 1 to 31), reader `CARD_NS` / `CARD_EW` in the corridor | reader: quality 0x300 + number, target the door, map bits 7 |
| Switch and energy wall | `EWALLNS` / `EWALLEW` frame 0 in the doorway | `SWITCHNS` on the north wall of the key room | switch: quality 0x100 + link, target the wall, map bits 7 |
| Panel door | the door, quality = link | `PANELNS` on the north wall of the key room | panel: quality 0x300 + link, target the door, map bits 7 |
| Keypad door | the door, quality = link | `KEYPADNS` / `KEYPADEW` in the corridor, a `VALUEBOX` beside it, a monitor `MONITNS` on a desk `MDESKNS` in the key room | keypad: quality 0x300 + link; `VALUEBOX`: quality = link, code 0; monitor: quality = message << 8 + link, map number 0 |

- What the game's scripts require, found on the way:
  - `TRIGGER` (and the monitor's and keypad's search for their `VALUEBOX`) looks no farther than global 0x40 × 32 = **3,200 units**: switches, panels and monitors stay within one slot of their lock, else the lock becomes a card door.
  - Readers, keypads, switches and panels stand on the **south or east face** of a wall, the one the camera sees (on the original maps, always): readers and keypads go on the corridor's far wall, beside the doorway, whatever side the player comes from.
  - A `VALUEBOX` whose code is 0 draws one between 100 and 999 when the map loads (`VALUEBOX` cachein); the monitor shows it in its message (`TEXTFILE` 10, 18 or 19: "Le nouveau code est 356", "356 est le code de cette semaine"...).
  - Key cards are a bit of the player for the whole game: chained levels number theirs from different first cards (option `first_card`); the links of doors and switches start at 32, above the card numbers, since a `DOOREGG` opens every door of its link.
- Solver (`check_solvable`): a lock opens once its key room is reached, and, for a card reader or a keypad, the room before the lock too; the exit and then every room must be reached.
- Generator version 2 (`NOLIMIT.JSON` of version 1 refused).
- Editor: number of locks in *Generate a level*.
- Tests: `editor_tests` checks every lock of 60 seeds (key before the lock, ranges, links, numbers, one barrier and one key each) and that the solver finds a key left behind its own lock; `generator_data_tests` plays them in the engine: each lock bars its room, its key and reader are reached on foot, the card is picked up (as the selection mode does), the monitor shows the code that the keypad then takes, the switch or panel is used, and the room is then reached. On seeds 1 to 40: 73 locks (15 card doors, 17 keypad doors, 24 panel doors, 17 energy walls), all open. `--shots DIR` saves a picture of each lock and key.

Next: milestone 3, verticality.
