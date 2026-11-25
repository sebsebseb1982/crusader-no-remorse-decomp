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

From the game: **CRUSADER / NO LIMIT** in the main menu, between *Options* and *Credits*. After the difficulty screen, a mission (below, "Missions from set pieces": the rooms of map 1) is generated from a random seed (1 to 999,999) into `mods/mission-SEED` (next to the data folder) and played at once. While it is played, the game reads that folder as with `--mod`: the load and save screens use its `SAVEGAME` folder; *New game* goes back to the game's own missions and saves. A seed drawn again plays the folder already there. The seed is printed on the console ("Crusader: No Limit, seed N: map M in ...mission-N"), and the folder can be played again later with `--mod mods/mission-SEED --map <map>` (the map is in its `NOLIMIT.JSON`, with `"source_map"`: `--regenerate` builds a mission again from it). Until 25 November 2026 the button generated the rule-built levels of milestones 1 to 4 into `mods/gen-SEED`.

From the editor:

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

## Missions from set pieces (demo)

The levels of milestones 1 to 4 are rooms drawn as empty boxes on a grid, with mechanisms put in by rules: they can be finished, but they have no identity, no pace and no intent. The way forward (25 November 2026, a demo level first, judged in play before anything else is built on it) is to build levels **out of the designers' own rooms**, and to **tell the level before building it**.

```sh
crusader-no-remorse-editor data --pieces 1                       # the rooms of map 1, as set pieces
crusader-no-remorse-editor data --mission 1 --source 1           # a mission from them, in mods/mission-1
crusader-no-remorse data --mod mods/mission-1 --map <map printed>
```

**Set pieces** (`pieces.hpp`). A map is flattened (its glob eggs replaced by their pieces) and read as a grid of 16-unit cells at a storey: a cell is walkable when a solid flat piece bears it (not a black void tile, not a death floor) and nothing solid stands over it lower than 80 units (the player's height); door cells are not walkable. The walkable cells, eroded by 64 units so that doorways and passages narrower than 128 units fall away, give one component per room; each walkable cell then goes back to the nearest room. A room's **inside** is its cells and the cells of its box that its walls enclose with them (under its furniture: a stack of crates deeper than a cell from where the player walks), not another room's. It takes what stands within its inside; the walls that border it for half their length at least (not one that runs on past its corner, over the void); what hangs on a wall (thin) when the room lies on the face of the wall that the camera sees (east or south: a fake lift door painted for the room behind stays there); a flat piece that is no floor tile (a hazard stripe, a hatch) only when it lies within a cell of the inside; every floor tile that bears one of its cells or what it takes (tiles are 256 units: the one under a room's border may be held by a neighbour, and lie lower than the storey). Things that touch (crates stacked, barrels side by side, a pyramid of crates) go whole or stay whole: with the room when all of them lie within a cell of its inside, else none of them; never cut. Its **openings** are the runs of its border, 80 units wide or more, that meet another room or a door; and the doors that can be **cut**: a straight wall piece of 128 or 256 units right along its border, with 96 units of the room behind it all along (the way in clear of furniture). Its **outline** is its inside closed by 48 units (dilated, then eroded), so that a dent narrower than a doorway (where a door of the original map led to a passage left behind) is taken in, then **squared** (the user's wish, 25 November 2026: rooms are rectangles, an L at most, never rounded): its bounding rectangle, less its largest empty corner (64 x 64 units at least) for an L. Where the cutting parted an open space on a slant (each walkable cell went to the nearest room, so that the parting ran in 16-unit steps), the steps are taken in as bare floor and the room's walls run straight. A room that would grow by more than a quarter, or whose square has cells with no floor, is not used. A floor tile that runs past the room (concrete 0x14, green tiles 0xCC) is laid again as its quarters of the same look (0x16, 0xCE) that touch the outline. A flat piece that lies on a floor tile all over is laid on the floor (a hazard stripe, a hatch) and goes only within the room; elsewhere it is a piece of the floor and goes as floors do. Its **tags** count what it holds by class (crates and barrels: storage; chairs, monitors, desks: office; transformers, generators: machines; cameras, alarms, turrets: security), and its **local links** are the quality lows whose scripted objects all lie in it. Map 1 gives 59 rooms over its two storeys, in 0.2 s.

**Missions** (`mission.hpp`). A mission is a run of steps, each with a purpose and an intensity. The demo:

| Step | Purpose | Room wanted | Guards (of the room's own, at most; added up to at least) |
|---|---|---|---|
| 0 | arrival, where the player comes in | storage, quiet | none |
| 1 | first contact | a room to cross | 2; 1 |
| 2 | the key room, off the way (from step 0, else 1) | a dead end, an office: the card at its back, as far as can be from its door | 1; 1 |
| 3 | behind the card door | office or security, large | 3; 2 |
| 4 | transit | machines | 2; 1 |
| 5 | climax | the largest, most guarded | 6; 4 |
| 6 | extraction | a small dead end: the exit teleporter | none |

The seed draws the plan (`mission_steps`, from its high bits so that the layout's attempts keep it; the plan's name in the level's notes): the demo above one time in two, else one of:

- **two cards**: arrival, contact, the first card in a room off it, transit, the first card door, behind it the second card's room (guarded) and the second card door, the climax behind it, extraction (8 rooms, 2 locks);
- **crossing**: arrival, a quiet transit, contact, the card in a dead end half-way, a second and harder contact, the card door, climax, extraction (8 rooms).

A locked step names the step whose room holds its card (`MissionStep::key`; else the first key room).

- The rooms of a level come from the source map (`--source`) and from the mission maps (1 to 63) that look like it: those whose floors and walls are shared with it for three quarters at least, four at most, the closest first (`look_alike_maps`; for map 1: maps 60, 6, 5, 2). Those of their upper storeys are brought down to z 0. A room found again on a variant of its map (map 60 repeats map 1) is taken once.
- Only **rooms**, not fragments of a larger space: walking out from each border cell up to 96 units must meet a wall (64 high or more), a door or another room past something (another room right against the border is one space that the cutting parted: open) for four fifths of the border at least (furniture within the room does not count: the room is met again past it), and railings (lower than 64) may close a tenth of it at most (a balcony, a catwalk over a pit). Of the 59 rooms of map 1, 34 pass.
- Each step takes, among the rooms that have the openings it needs (96 to 224 units wide, or a door that can be cut: a door fits in) and are big enough for it (bounding box of 512 x 512 at least; key room 384 x 512, locked room 20 and climax 30 squares of 128, extraction 384 x 384), one that suits its purpose, furnished (4 items with a height that are neither walls, doors nor what hangs on the walls per 512 x 512 at least: below, a bare hall; 8 for the key room, an office and not a bare corridor): a score from the furniture's density, the tags (each counted up to 5), the size and the guard posts, plus chance (up to half the best score), so that seeds give other levels; the best 8 are tried in turn.
- Layout: a depth-first search places each room so that one of its openings faces an opening of its parent's room (opposite sides: the art cannot be turned) **across one line of wall** (16 units), as rooms meet on the original maps, without its cells coming within a cell of another room's; a step that finds no room goes back to the previous one. A card door's room hangs north or west of its parent, so that the reader stands on the face of the wall that the camera sees, in the parent's room.
- The designers' openings are tried first, then the doors cut in walls (their wall piece goes, with what stands flat against it: a sign, a fake lift door). The joint: the wall line between the two openings is made whole where neither room has a wall of its own, and an automatic door (`MDOOR_N` / `MDOOR_E` with its `DOOREGG`), or the card door and its reader, stands in it.
- The rooms keep their floors, walls, furniture and scenery: an item goes with a room as above (its inside). Their scripted objects go when they are linked to something outside the room; teleporters, eggs, map events, lifts and cameras go; doors in openings go (the joint has its own). A room's other doors stay with their `DOOREGG`. Links are numbered anew.
- Guards: the room's own MONSTER pairs, as many as the step keeps, drawn among them. Fighters (guards, soldiers, storm troopers, elites, enforcers...) take a guard's post facing the way the player comes in, and keep the designers' way of fighting when it works alone (activity 5, or 10 for elites and enforcers); workers keep their own ways (they loiter, run, surrender); other kinds are left out for now (turrets, rare robots; the thermatron and the squad members, guard squads and grenadiers, which never fire alone). Where a room's own posts fall short of its step, guards are added: the source map's most common fighter, on clear floor 256 units or more from the doorway, facing it. The demo's tension runs 0, 1, 1, 2, 1, 4, 0: calm, contact, the key, the locked room, a breath, the climax, calm.
- Each room is closed: along its outline (not round its furniture, and straight across the mouth of a dent), every cell outside it that nothing solid stands on gets a wall, but where a joint comes in. These walls show their face, never the black frame of a facade (between two rooms, a black frame reads as a black block).
- **Cameras** (lot 4b, first part): a room's wall cameras (`CAMERANS`, `CAMERAEW`, `CAMERACO`, the ones whose script calls `TRIGGER` when they see the player; on the original maps they mostly call an `ALARMBOX`, whose alert the map's other scripts answer) go with it, on a link of their own: their `TRIGGER` acts on `MONSTER` places (action 2, `equip`, which brings the NPC in), two guards waiting at the room's farthest free spots from its doorway (the second from difficulty 2), not there before the camera sees the player. When the room keeps its alarm box (`ALARMBOX`), the chain is the designers': the camera equips the box (quality high 1, as on the original maps), the box rings the map's alert (alarm lights and sound) and equips the guards' places; the player can switch the alarm off at the box. While the alarm rings, card doors stay shut (the game's scripts): the player switches it off at the box first, as the engine test does on its way. The arrival room keeps no camera nor box (the alarm would ring at once); the way out keeps them with no guards to call. Quality highs as the designers set them: 1 for a camera that rings a box, else 0 (with a camera's own, 0x65 on one of map 1, `TRIGGER` never acted). The class `CAMERA` itself is an egg that moves the view, not a camera: it goes. The engine test stands round each camera and checks that its guards come, the alarm is on, and using the box switches it off.
- **Turrets** (lot 4c): a room's wall guns (`GATGUNEW`, `GATGUNNS`, `BASEGUN`, run by the class `WALLGUN`) go with it, armed, with a wall panel in the room that switches them off. How the guns work (usecode): entering the fast area, a gun starts its process with its NPC number as its **mode**; mode 6 does nothing (fn 40 returns at once), as do quality highs 99 and 199. On the original maps most guns stand in mode 6 until a trigger egg on the way in calls `TRIGGER`, which equips them with its quality high as their new mode (1 to 4, 10; above 99, less 100). `equip` with 90, 91 or 92 turns the gun one step one way, the other way, or fires once: the triple panels of maps 7, 13 and 24 aim a gun (they never switch it off). Here a gun has from the start the mode its designers' triggers gave it (the most common one on its link, else its own, else 2) and fires once in the fast area. The panel, `PANELNS` on the room's north wall or `PANELEW` on its west wall (the faces the camera sees), 16 units out from the wall and 40 up, as the designers hang theirs, calls `TRIGGER` on the guns' shape and link with quality high 6 every time it is used (its frame stays 0): the guns go to mode 6, off. Its place: the middle of a straight wall piece of the room (not where a door is cut nor in an opening), the room walkable 3 cells deep in front of it, nothing hanging or standing in its box, the room going on 8 cells (128 units) past it towards the near wall (a near wall as high as the room hides what stands less than about 112 units behind it, at the panel's height: a first version hung one in a room's north-east corner, unseen), 192 units at least from the guns when the room allows it (a first version hung one under its gun). Guns of one shape a room (a panel acts on one); none in the arrival room, nor in a room with no place for a panel. `check_walkable` checks that the panel can be walked to. About two missions in three have one (seeds 1 to 30: 20), all `GATGUNEW` in modes 10 or 2, from three rooms of maps 1 and 60. The engine test (without the guards, so that only the gun can hurt the player) waits at the room's doorway, then in front of the gun, until the player is hurt, uses the panel, and checks that the player, back where he was shot at, is hurt no more for 600 frames. Lasers (`LAZEREW`, `LAZERNS`) are traps, not machines: their script fires once (event 0x20) when a plate, a trip beam or a trigger egg calls it; they go, as before.
- A room's near walls (south, east) with no room past them take the black frame of their shape (its frame drawn black: a black silhouette under a thin top frame, which hides the strip of the room behind it, as in the game), as the designers drew a building's near side; a shape with no black frame (a lattice panel, the pillars of the face that the next room saw) gives way to the biome's black wall of the same length, or goes (a post). Walls on a 16-unit step count too. Between two rooms, walls keep their face.
- Floor past the rooms (a tile of 256 or 128 units under a room's border runs on where its neighbour was, over the void past a near wall's black frame): black tiles (`BLACK8`, 128 units, one unit above the floor, placed at any 16-unit position) cover it, never touching a room's outline nor the line of its walls. About 1,300 cells a level, some 60 tiles; a strip narrower than a tile between two rooms stays.
- Tidying: what the cutting left standing on no floor, or hanging (a sign, a poster: thin, above the floor) on no wall, is taken out, then what stood on what was taken out (a crate on a crate), in turn (about one item a level since rooms are cut by their inside).
- Before it is kept, the level is walked on its own grid (`check_walkable`): the player's box with a margin, 4 x 4 cells, must go from the start over walkable cells and doors to each room's entrance, to the exit and to each key, and must not reach the room behind a card door with that door shut (rooms side by side could open another way round). Else the layout is drawn again (up to 40 times), without the room that failed.

Reviewed in play (25 November 2026, after the user's criticism of the first demo: missing walls, cut objects, signs hanging in the void, corridor junctions that read as broken walls): each version is now looked at in 640 x 480 views from every room's entrance (`--shots`), and its defects listed, before it is shown.

Second review (25 November 2026, "still many inconsistencies"), each generated room set beside its original in the editor (`--center` on both): half pyramids of crates and crates in the air (rooms cut by the cells where the player walks, without what stands deeper), a hazard stripe and a fake lift door of the room next door standing in the void, a key room that was half of an L-shaped room. Rooms are now cut by their inside (above). Still seen, not fixed:
- floor past a room is hidden by black tiles, but for strips narrower than a tile (128 units) between two rooms;
- (A lone post at a room's near corner is gone with the black frames. The ragged walls and a black hole behind the barrels of a store, both where a room was walled post by post round a dent left by a doorway, are gone with the outline.)
- too few rooms (about 14 with two openings a door fits in, seeds 1 to 5 beginning with the same three rooms): fixed the same day by the doors cut in walls (315 rooms, five seeds, five different levels). With them came three traps, each a way the engine stopped the player where the grid let him through: a fake lift door, flat (no thickness) and 64 high, standing in front of a cut door (flat things now hold their cell in the grid, and go with the wall); hanging machinery at 64 units, under the grid's head height of 48 (now 80, the player's height); crates 80 units in front of a cut door (96 units clear now). The engine test also walks towards a guard post more than 400 units from the doorway, out of the fast area from there, before it counts the guards.

Tests: `generator_data_tests data 1 10 --mission 1` (ctest `generator_mission_tests`, seeds 1 to 3) plays missions in the engine as the other levels: the level check, walking to every room through the card door opened with its card, the automatic doors, the guards of each room at difficulties 1 and 3 (they appear, and shoot at the player who comes in: in 4 guarded rooms out of 5 at least, the silent ones listed; open question: now and then a designer's post set like the others, a guard by a wall, never fires, from whatever side the player comes), the exit; `--shots DIR` saves a 1280 x 960 view of each room from its entrance.

## Milestones

Each milestone ends with levels that can be played by hand, and tests that check them.

| # | Milestone | Playable result |
|---|---|---|
| 1 | **Shell**: biomes learned from the globs; rooms and corridors on one storey with doors; start and exit teleporter; chained levels; physical verification in the engine | walk from the start to the exit of a generated level, through doors |
| 2 ✅ | **Locks and keys**: mission graph and solver; key-card doors, switches and energy walls, panel doors, keypads with their code on a computer | a level where the exit is behind one to three locks |
| 3 ✅ | **Verticality**: two storeys (z 0 and 96, as on the original maps), lifts (`ELEVPLAT`) with their call boxes, railings round the shafts, multi-storey verification; stairs and catwalks over lower rooms left for later | a level that climbs to its exit upstairs |
| 4 | **Opposition**, in lots: 4a guards (MONSTER pairs by difficulty and room) ✅; 4b cameras and alarms with their reinforcements (missions: cameras call guards, ring the alarm box ✅); 4c turrets, switched off by their panels ✅ (missions; lasers are traps, left for 4d); 4d mines, lasers, steam and valves, explosive barrels | a defended level, its hazards switched off by its controls |
| 5 | **Supplies and props**: medikits, ammunition, chests and crates, rechargers, rooms furnished from the original maps (prefabs taken from the rooms where the designers placed them), secrets | a level that can be finished at every difficulty |
| 6 | **Several biomes and robots**: biome changes between areas, robot control, teleporter networks | a level crossing two biomes |
| 7 | **In the menu, and a bot**: "Random mission" in the game's menu (the *CRUSADER / NO LIMIT* button, done: one level from a random seed), with a seed to type; a bot that plays generated levels to the end in `game_tests`, on a range of seeds | a new mission from the main menu |

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

**Milestone 3 done** (24 November 2026): two storeys and lifts.

- Storeys: the original maps have two (glob eggs at z 0 or 96), and so do generated levels. In three levels out of four (option `storeys`, -1: drawn), the rooms from a depth drawn on the way to the exit up are on the upper storey, at z 96: the start is downstairs, the exit upstairs. A room or corridor of the upper storey stands on walls without doorways that close its underside.
- Lifts: a passage between storeys becomes a lift corridor, two tiles long at least (the shaft and a landing). Built as on map 5:

| Piece | Shape | Where, settings |
|---|---|---|
| platform | `ELEVPLAT` 0x12F, 8 × 8 × 1 | in the shaft's tile, against the upper part, at z 1 (its top 9 above the floor: a step); quality = link |
| pad | `TRIGPAD` 0x4CD | on the platform (z 9); `TRIGGER` on shape 0x12F, action 2 (`equip`, argument 0), map number 0x26: sends the platform from one end of its run to the other, z 1 and 88 (its top level with the upper floor) |
| call boxes | `BOX_NS` 0x081 / `BOX_EW` 0x080 | on the corridor's far wall, below before the platform (z 48), above past the shaft (z 144); same settings as the pad |
| relays | 0x4B1, invisible | 32 above each box: same settings with bit 3 (map number 0x2E) |
| railings | `YR8X6NS`... `YR1X1` (48 high) | at z 96 round the shaft, on the three sides where the platform does not touch the upper floor, and from the corridor's walls to them |

- Found on the way: a box (switch, panel) acts through `TRIGGER` in **one state** only, frame 0 unless its map number has bit 3; in the other state, `TRIGGER` passes the use on to the 0x4B1 objects of the same link within 160 units whose bit 3 is the other way. The original lifts' boxes have such a relay beside them (map 1), so that each use calls the lift; without it, every other use does nothing.
- The platform's script (`ELEVPLAT` equip) moves it within z 1 to 159 (its argument): a third storey would need another lift (`ELEVATOR`, the cab between maps). Stairs and catwalks over lower rooms are left for later.
- Generator version 3. Editor: number of storeys in *Generate a level*.
- Tests: `editor_tests` checks the storeys and lifts of 60 seeds (start below and exit above, a lift exactly on the passages between storeys, corridor length, platform, pad, boxes and relays with their settings); `generator_data_tests` crosses the lifts in its walking search (standing on the platform at the bottom leads to the upper floor in front of it, and back) and rides each lift in the engine: the player walks onto the platform, its pad sends it up to z 88, the player walks off onto the upper floor at z 96, then each call box sends it down and up again. On seeds 1 to 40: 24 levels on two storeys, 53 lifts, all ridden; the 73 locks still open.

**Milestone 4, lot 4a done** (24 November 2026): guards.

- Every room but the start one gets NPCs as the original maps make most of theirs: **MONSTER pairs** (shape 0x4D0, the place frame 0 and the kind frame 1, linked by their quality low, `docs/EDITEUR.md`). The place's map number has bit 3: its `enterFastArea` event creates the NPC when the place comes near the player (`MONSTER` equip, `Actor::I_createActorCru`), then both links are cleared.
- How many: 0 or 1 per room, one more from depth 2 and one more in the exit room; one room in two adds one only from difficulty 2 or 3 (map number bits 0-1), which the engine leaves out at lower difficulties.
- Types, among the fighters most used on the original maps (3,500 pairs): guard (4), soldier (9), storm trooper (8), elite (15), weighted 5, 4, 4, 2; one in five carries its other weapon (bit 2). Not the chemsuits (2), though common: on the original maps they loiter and surrender (activities 2, 7, 7), and in the engine they never shoot.
- Activities: guard (8), attack on seeing the player (5) and when shot at (5), the editor's setting and one of the original ones. Each guard faces the doorway the player comes in by (`gen::entry`: from the neighbour nearer the start), so that it sees the player there: a guard facing another doorway of its room does not turn round to a player behind it.
- Spots: 64 units from the walls, clear of doorways, keys, desks, the start and the exit pad, and of each other.
- Option `guards` (0: none; recorded in `NOLIMIT.JSON`), *Guards* in the editor's *Generate a level*. The guards come last: a level without them is the same level, item for item. Generator version 4.
- Tests: `editor_tests` checks the guards of 60 seeds (none in the start room, inside their room, off the exit pad, one link each, pair settings) and that a level without guards is the same; `generator_data_tests` checks the mechanisms on a copy of the level without its guards, then, at difficulties 1 and 3, comes into each guarded room through its doorway (`gen::entry`) and checks that exactly the guards of that difficulty appear, where they stand, of their type, and that the player is shot at, from the doorway or after walking up to a guard (guards farther than about 400 units engage only when the player comes nearer). On seeds 1 to 40: 511 guards, each appearing exactly at its difficulties; in all 454 rooms visited (difficulties 1 and 3) the player is shot at, in 16 after walking up; the 53 lifts and 73 locks still work.

Next: lot 4b, cameras and alarms.
