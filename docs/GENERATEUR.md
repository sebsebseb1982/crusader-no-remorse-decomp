# Procedural level generator

The generator builds new levels for *Crusader: No Remorse* out of the game's own pieces: the floors, walls and objects of the original maps, and the scripts (usecode classes) that drive them. It writes them into a mod folder, like the level editor (`docs/EDITEUR.md`), and the game plays them with `--mod`.

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

## Milestones

Each milestone ends with levels that can be played by hand, and tests that check them.

| # | Milestone | Playable result |
|---|---|---|
| 1 | **Shell**: biomes learned from the globs; rooms and corridors on one storey with doors; start and exit teleporter; chained levels; physical verification in the engine | walk from the start to the exit of a generated level, through doors |
| 2 | **Locks and keys**: mission graph and solver; key-card doors, switches and energy walls, pressure plates, keypads with their code on a computer | a level where the exit is behind two or three locks |
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
- Editor: *Map > Generate a level...* (seed, biome, number of rooms); command line `--generate SEED [--levels N] [--biome B] [--mod DIR]` (default mod folder: `mods/gen-SEED`).
- Tests: `editor_tests` (no data: determinism, layout, one start, one exit, doors and their eggs, 60 seeds); `generator_data_tests data [first_seed] [seeds]` (on the data, played in the engine: walking to every room and to the exit, a door that opens and is crossed, the exit teleport, the editor's level check).

Next: milestone 2, locks and keys.
