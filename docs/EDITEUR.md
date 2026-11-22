# Level editor

`crusader-no-remorse-editor` creates and edits the maps of *Crusader: No Remorse*: the scenery, the objects, the NPCs, and everything that drives the game's scripts (eggs, triggers, teleports, links between objects). It works on the same files as the original game and draws the map with the port's renderer, so what you see in the editor is what you get in the game.

The original data is never modified. The editor saves to a **mod folder**, which the game loads on top of the data with `--mod`.

## Running it

```sh
crusader-no-remorse-editor data                     # game data folder, default mod: mods/mon-niveau
crusader-no-remorse-editor data --mod mods/base-secrete --map 3
```

To play the level, press **F5** in the editor (or *File > Test in game*), which saves and then launches:

```sh
crusader-no-remorse data --mod mods/base-secrete --map 3 --egg 0x1E
```

`--map` starts a new game on that map, at the arrival egg `--egg` (0x1E by default, the egg where every mission starts). If an arrival egg is selected, F5 starts from it. A mod's saved games go in `<mod>/SAVEGAME`.

## Getting started

1. *Map > New map*: a 3 × 3 cell floor and the mission start.
2. Place scenery from the *Shapes* and *Globs* tabs, NPCs from the *Eggs* tab.
3. *Eggs* tab > **Mission start** moves the start where you click; **Teleporter pair** places a teleporter and its destination, already linked.
4. Select a switch, press **L** and click the doors or force fields it should drive.
5. Watch the status bar: a red or orange counter means the **level check** found something that would not work; click it.
6. **F5** to play.

## Guided tools

The original levels are made of numbers that must match: a teleporter names the id of its arrival egg, a switch shares its quality low with the doors it opens. These tools set the numbers for you:

| Tool | Where | What it does |
|---|---|---|
| **Mission start** | *Eggs* tab | Click in the view: the arrival egg 0x1E goes there (it moves if the map has one). *Go to it* and *Map > Go to the mission start* find it. |
| **Teleporter pair** | *Eggs* tab | First click: the departure (a trigger area); second click: the arrival. The pair gets an id that the map does not use. |
| **Link by clicking** | **L**, *Edit* menu, or *Properties* | Click the items the selected one acts on: they take its quality low (a free one if it has none). Clicking a linked item unlinks it; Esc or a right click when done. The links are drawn as cyan lines. |
| **Copy, cut, paste** | Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+D | Pastes under the pointer, on this map or another one (the clipboard stays when you open another map). A switch pasted with its doors, an NPC's MONSTER pair, or a teleporter with its arrival, gets numbers the map does not use, so the copy works on its own and leaves the map's own links alone; a door copied alone beside another keeps its switch (*Edit > Paste: renumber links and teleports* turns this off). Ctrl+D (duplicate) does the same. |
| **Teleport destination** | *Properties* of a departure | Pick the map and the arrival from lists; *Open the arrival's map* goes there. |
| **Level check** | status bar, *Map > Check the level* | Lists what would not work in the game, updated at each change; *Show* selects the item. |
| **Tooltip** | pointer over the view | What the item is, where a teleporter leads, its link number. |
| **Examples from the original maps** | *Properties*, *Edit* menu | Every place where the original maps use the selected object, the ones with the most linked items first. Click a row: the map opens with the object and its linked items selected, ready to copy (Ctrl+C) onto your map. |
| **Prefabs** | *Edit > Save the selection as a prefab*, *Prefabs* tab | Saves a group of items (a switch and its doors, a guard post...) in the `prefabs` folder (`--prefabs DIR`), to place again on any map, at the work z, with its links renumbered. |

The level check reports:

| Severity | Problem |
|---|---|
| Error | a departure to a map that does not exist, or to an arrival id the target map does not have (the teleport does nothing) |
| Error | a trigger egg whose type has no usecode class, a monster egg without its NPC template |
| Error | more items than the game's item table holds |
| Warning | several arrival eggs with the same id (the game takes whichever its search finds first) |
| Warning | a departure without a trigger area |
| Warning | a map busier than the busiest original map (7,855 records) |
| Warning | a MONSTER place without its kind, or a kind without its place (no NPC appears) |
| Note | no mission start (the map can still be reached by a teleport), a glob egg whose glob is empty |

On the original maps it finds what the designers left behind: 5 departures to maps that do not exist, 4 trigger eggs of a type without a class, 4 arrival ids used twice, 112 incomplete MONSTER pairs (each at least 128 units from any partner).

## What a level contains

The format is the one used by the original levels; the editor reads and rewrites the game's 63 maps and 3,072 globs byte for byte (`editor_data_tests` test).

| File | Contents | In the editor |
|---|---|---|
| `STATIC/FIXED.DAT` | one entry per map (1 to 255): 16-byte records (x, y, z, shape, frame, flags, quality, NPC number, map number) | the open map |
| `STATIC/GLOB.FLX` | the globs: groups of pieces (floors, walls) placed by a glob egg | *Globs* tab, glob editor |
| `STATIC/SHAPES.FLX`, `TYPEFLAG.DAT` | the shapes (sprites) and their properties | *Shapes* tab (read-only) |
| `USECODE/EUSECODE.FLX` | the scripts (usecode classes) | *Scripts* tab (read-only) |
| `STATIC/DTABLE.FLX` | the NPC types | *Eggs* tab (read-only) |

The game reads `FIXED.DAT`, `GLOB.FLX`, `SHAPES.FLX`, `TYPEFLAG.DAT`, `DAMAGE.FLX`, `ANIM.DAT`, `DTABLE.FLX`, `EUSECODE.FLX` and `UNKDS.DAT` from the mod when they are there, and from the data otherwise.

### Globs

Nearly all of a map's floors and walls come from **globs**: a glob egg (shape 0x010, family 3) with quality *n* places the pieces of glob *n* in the 512 × 512-unit cell it sits in. Only the cell matters, not the egg's exact position, so the editor moves them from cell to cell.

- *Globs* tab: click = brush, double click = open in the glob editor.
- *Edit > Create a glob from the selection*: plain scenery taken from a single cell becomes a new glob, replaced by its egg.
- *Ungroup glob*: the reverse; the egg becomes its pieces.
- A glob is shared by all its eggs, on every map: to change only one place, use *Save as new glob*.

### Eggs: how levels are scripted

The original levels have no map-specific script. The designers placed **eggs** and set the *quality*, *NPC number* and *map number* bytes of objects, which the engine and the usecode classes read. The *Properties > Role in the level* panel decodes them:

| Egg | Shape | Parameters |
|---|---|---|
| Teleport arrival | 0x194 frame 1 | id = low quality; the mission starts on id 0x1E |
| Teleport departure | 0x194 frame 0 | to the egg (low quality) of the map (map number: the game takes it as it is, 0 is no map), delay if high quality ≠ 0, trigger zone |
| Trigger | 0x011 | class 0x900 + low quality (`TRIGEGG`, `ONCEEGG`, `DOOREGG`, `CAMERA`, `MUSIC`, `MISS1EGG`…), parameter = high quality, id = map number, trigger zone |
| Monster (NPC) | 0x24F | (see also *NPCs* below) NPC type (`DTABLE.FLX`), direction, minimum difficulty, other weapon, three activities (at start, on seeing the player, when shot at); the last three are stored in the template the egg contains |
| Marker | 0x476 | object given by the chest with the same low quality, or object cut by a `TRIGGER` |

The **trigger zone** (half-widths × 32 units, stored in the NPC number; the player must also be within 0x30 in z) is drawn in pink; teleport departures are joined to their arrival by a green line, or labelled with their map and egg when they lead to another map.

Many objects answer each other through their **low quality**: switch → energy walls or doors (`TRIGGER`), valve → steam, chest → markers, camera → alarm. When an object is selected, the ones that share its low quality and have a script are outlined in cyan, and *Select linked objects* takes them all.

The *Script (usecode)* section gives the object's class and the events it handles (`use`, `gotHit` for slabs and plates, `hatch` for eggs, `equip` for switching on…).

### Switches and what they act on

Switches, panels, valves, pressure plates, cameras, keypads and other objects act through the `TRIGGER` script on the items that share their quality low, but only on **one shape**, written in their own bytes, with an action chosen by their other bits:

| Bytes of the switch | Meaning |
|---|---|
| map number bits 5-7 and NPC number | the target shape: (map number & 0xE0) × 8 + NPC number |
| map number bits 0-1 | the action: 0 sets the targets' quality high to the argument, 1 their quality low, 2 switches them on (`equip(argument)`: a MONSTER place makes its NPC appear), 3 turns them off into markers, or back on |
| map number bit 2 | those four actions (clear: other ones, such as destroying the targets) |
| map number bit 3 | the state it acts at (0: its first use) |
| map number bit 4 | once: its link is then cleared |
| quality high | the argument |

Nothing happens with a map number or a quality low of 0. **Link by clicking** sets all of this: the clicked item's shape becomes the target, and the action and argument become the setting the original maps use most for that kind of switch on that shape (a switch on energy walls turns them off and on; on a MONSTER place, it makes the NPC appear). *Properties > Acts on* shows the target, the action in words, and the original maps' setting. Which objects work this way is read from the original maps: 17 shapes in No Remorse (`editor_data_tests` lists them); chests, timers and `EVENT` objects use their bytes otherwise.

Checked in the game: on a map made with these functions, a switch linked to an energy wall turns it off, and a switch linked to a MONSTER place makes its NPC appear.

### NPCs

Most NPCs of the original maps (about 4,600, against 199 monster eggs) are **MONSTER pairs**. A pair is two MONSTER objects (shape 0x4D0) linked by their quality low, like a monster egg and its template:

- the **place** (frame 0): the NPC appears where it stands, when the player comes near (its map number has bit 3) or when a script fires it (reinforcements, NPCs coming out of doors);
- the **kind** (frame 1), within 64 units of the place: the NPC's type and two of its activities.

Once the NPC has appeared, both links are cleared. A place that does not appear on its own shows the switches that fire it (select the switch, **L**, click the place); a switch's argument is the place's mode: 0 once, 1 each time, 2 and 3 teleported in. *Eggs* tab: click an NPC type, then in the view: the editor places a pair with a free link, an NPC that guards, attacks on seeing the player or when shot at, and appears when the player comes near. *As a monster egg* places a monster egg instead. Select either object of the pair: *Properties > Role in the level* edits the NPC on both objects at once:

| Setting | Stored in |
|---|---|
| NPC type (`DTABLE.FLX`) | kind, NPC number |
| direction (0 to 7) | place, NPC number (low nibble) |
| lowest difficulty, other weapon | place, map number bits 0-1 and bit 2 |
| appears when the player comes near | place, map number bit 3 |
| starting activity | kind, quality high |
| activity on seeing the player | place, quality high |
| activity when shot at | kind, map number |

### New level

- *Map > New map*: a free slot in `FIXED.DAT`, 3 × 3 cells of the most common floor in mission 1, and the start egg 0x1E.
- *Map > Copy this map to a free slot*: starts from an original map.

Note that a copied map keeps its mission eggs (`MISS1EGG`…) and its teleports, which often point to other maps.

## Controls

| Action | Control |
|---|---|
| Select, add to the selection | click, Ctrl+click or Shift+click |
| Next object under the pointer | repeated Alt+click |
| Rectangle selection | drag on empty space |
| Move | drag the selection (snapped to the grid; Alt: no snapping), arrows, PgUp / PgDn for z |
| Place an object | pick a brush (Shapes, Globs, Eggs), then click; right click or Esc to drop it |
| Link to the selected object | L, then click the objects; Esc when done |
| Scroll, zoom | right click or middle button and drag, arrows with no selection; mouse wheel |
| Center on the selection | F |
| Copy, cut, paste under the pointer | Ctrl+C, Ctrl+X, Ctrl+V |
| Duplicate, delete | Ctrl+D, Del |
| Undo, redo | Ctrl+Z, Ctrl+Y |
| Save, test | Ctrl+S, F5 |

*Z cut* hides everything above it, so you can work on one floor without the ceilings. *Working z* is the height at which new objects are placed. The *View* menu shows or hides the editor markers (eggs are not drawn in the game), invisible objects, globs, zones and links.

## Limits

- The game keeps at most 9,984 objects in memory, including the pieces of nearby globs. The most loaded map in the game has 7,855 records; the status bar warns above 8,000.
- The player always starts with the equipment of a new game (datalink).
- Shapes, scripts and NPC types are read-only for now.

## Planned next

1. **Scripts**: a usecode disassembler in the *Scripts* tab, then an assembler to write new egg classes (0x91E and following) and new functions.
2. **Sprites**: import of PNG images into `SHAPES.FLX` (new shapes and frames, game palette) and editing of `TYPEFLAG.DAT` (footprint, family, solidity).
3. **NPCs**: editing of the `DTABLE.FLX` types (hit points, weapons, shape).
4. **Mission content**: computer texts, keypad codes, music, map sequencing.
