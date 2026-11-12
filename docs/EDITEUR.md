# Level editor

`crusader-editor` creates and edits the maps of *Crusader: No Remorse*: the scenery, the objects, the NPCs, and everything that drives the game's scripts (eggs, triggers, teleports, links between objects). It works on the same files as the original game and draws the map with the port's renderer, so what you see in the editor is what you get in the game.

The original data is never modified. The editor saves to a **mod folder**, which the game loads on top of the data with `--mod`.

## Running it

```sh
crusader-editor data                                # game data folder, default mod: mods/mon-niveau
crusader-editor data --mod mods/base-secrete --map 3
```

To play the level, press **F5** in the editor (or *File > Test in game*), which saves and then launches:

```sh
crusader data --mod mods/base-secrete --map 3 --egg 0x1E
```

`--map` starts a new game on that map, at the arrival egg `--egg` (0x1E by default, the egg where every mission starts). If an arrival egg is selected, F5 starts from it. A mod's saved games go in `<mod>/SAVEGAME`.

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
| Teleport departure | 0x194 frame 0 | to the egg (low quality) of the map (map number, 0 = this one), delay if high quality ≠ 0, trigger zone |
| Trigger | 0x011 | class 0x900 + low quality (`TRIGEGG`, `ONCEEGG`, `DOOREGG`, `CAMERA`, `MUSIC`, `MISS1EGG`…), parameter = high quality, id = map number, trigger zone |
| Monster (NPC) | 0x24F | NPC type (`DTABLE.FLX`), direction, minimum difficulty, other weapon, three activities (at start, on seeing the player, when shot at); the last three are stored in the template the egg contains |
| Marker | 0x476 | object given by the chest with the same low quality, or object cut by a `TRIGGER` |

The **trigger zone** (half-widths × 32 units, stored in the NPC number; the player must also be within 0x30 in z) is drawn in pink; teleport departures are joined to their arrival by a green line.

Many objects answer each other through their **low quality**: switch → energy walls or doors (`TRIGGER`), valve → steam, chest → markers, camera → alarm. When an object is selected, the ones that share its low quality and have a script are outlined in cyan, and *Select linked objects* takes them all.

The *Script (usecode)* section gives the object's class and the events it handles (`use`, `gotHit` for slabs and plates, `hatch` for eggs, `equip` for switching on…).

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
| Scroll, zoom | right click or middle button and drag, arrows with no selection; mouse wheel |
| Center on the selection | F |
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
