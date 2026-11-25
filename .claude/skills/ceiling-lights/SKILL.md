---
name: ceiling-lights
description: Light another map (mission) of Crusader No Remorse or No Regret with ceiling lamps for the 3D lighting - find its rooms, place lamps, review screenshots, install ceiling_lights/mapNNN.json. Use when asked to add ceiling lights / plafonniers / lumières to a mission or map, in crusader-no-remorse-decomp or crusader-no-regret-decomp.
---

# Ceiling lights for a map

The port lights rooms with invisible lamps under their ceilings (CEILING LIGHTS option, enhancements.md section 7d). Each map has an editable JSON file `ceiling_lights/mapNNN.json` (lamps, room by room), the kinds of lamp are in `ceiling_lights/kinds.json`; both are read when the game starts. `tools/lights/README.md` documents the format and the tools. Mission 1 (map 1) is done in both games: use it as the reference for density and look.

This skill works the same in both repos; the tools' defaults already fit the repo's game (`plan_rooms.py --game remorse --floor highest` in No Remorse, `--game regret --floor lowest` in No Regret). Talk to the user in French.

## 0. Before starting

- Check peers and the tree (other sessions edit these repos): `ListAgents`, `git status`, `git stash list`.
- Build the tools in Release: `cmake --build build --config Release --target mapview lightview crusader`. A running exe is locked: rename it (`mv x.exe x.running-HHMMSS.exe`), never kill it.
- Python needs numpy and Pillow. Work files go in the scratchpad (`$S` below), never in the repo.
- In Python edit scripts: the repos are CRLF; for C/C++ escapes (`\n` in strings) write the script with the Write tool, not a bash heredoc, and do not put them in `re.sub` replacements.

## 1. Which map

Ask the user which mission or map if it is not clear. Maps holding a mission start (teleport egg 0x1E) are the candidates; list them:

```sh
for m in $(seq 0 60); do build/Release/mapview.exe data $m --dump > $S/map$m.csv 2>/dev/null || continue
  awk -F, -v m=$m '$13==8 && $3==1 && $11%256==30 {print "map", m, "start", $5, $7}' $S/map$m.csv; done
```

Some missions have two near-identical maps (same start position, e.g. No Remorse 25/26, 27/28): compare their dumps and ask whether both get the plan (copy the file to both numbers if the rooms match).

## 2. Plan

```sh
M=7; NNN=$(printf %03d $M)
build/Release/mapview.exe data $M --dump > $S/map$M.csv           # items: boxes, type flags, class, brightness
python tools/lights/plan_rooms.py $S/map$M.csv $S/plan$M.json --map $M   # + $S/plan$M.json.lab.npy (room grid)
python tools/lights/shoot_rooms.py $S/plan$M.json --prune        # drops lamps over a floor not seen
python tools/lights/plan_map.py $S/plan$M.json $S/plan$M.json.lab.npy $S/top$M.png   # top view (--crop x0 y0 x1 y1 --scale 2 to zoom)
```

The planner prints rooms found, lit, lamps, and why the others get none (`small`, `liquid`, `outside`, `roof`).

## 3. Review (the important part)

```sh
python tools/lights/shoot_rooms.py $S/plan$M.json $S/shots$M --labels $S/plan$M.json.lab.npy --mission N
python tools/lights/sheets.py $S/plan$M.json $S/shots$M $S/sheets$M
```

Look at the top view and every sheet (Read the PNGs); open a full screenshot when in doubt. Each screenshot shows the room outlined, the rest darkened, numbered lamps with a dashed line to the floor, and the unlit view in a corner. Check and fix in the plan JSON:

- **Not rooms**: roofs, the void round the buildings, outdoor ground, a lift shaft. Set `"lights": []` and a `"skip"` (`roof`, `outside`) or a `"note"`.
- **Rooms that need none**: closets, recesses, a room a liquid already lights (green glow), a room dark on purpose. Same: empty `lights`, a `skip` or `note`. The user wants no lamp in closets nor in rooms lit by a radioactive / toxic pool.
- **Lamps**: away from walls (the planner keeps 160 units), not over a hidden floor, spaced about 1150 units (1280 in halls). Few strong lamps give dark, readable shadows; do not add lamps to "fill": the user asked for fewer, stronger ones. Move a lamp by editing `[x, y, z]` (world units; z in z units, 64 over the walls' top: `floor_z + ceiling + 64`, 160 in most rooms, twice the Silencer's 80: a lamp just over a head blows its shadow up into a blot, one under a head makes it endless).
- **Kind** per room (`warm` offices and quarters, `neon` machine rooms, labs, corridors, `sodium` stores and docks, `dim`, `red`), and the room's `name` in French (the automatic one is a guess from its contents). A lamp can have its own radius and kind: `[x, y, z, radius, "kind"]`.
- Merged rooms (a big stretch over a wide opening) are fine when it reads as one space; split by hand only if the screenshot is unreadable.

Iterate: edit `$S/plan$M.json`, re-shoot only the rooms changed (`--only C12,C40`). To try lamps live: `build/Release/lightview.exe data $M X Y --ceiling-file $S/plan$M.json` (X Y: world coordinates, a room's `centre`; O toggles the lamps, F5 re-reads the files). Show the user a few screenshots and the top view before installing, and ask if anything should change.

## 4. Install

```sh
cp $S/plan$M.json ceiling_lights/map$NNN.json        # add "mission": N at the top level
python tools/lights/shoot_rooms.py ceiling_lights/map$NNN.json lights/missionNN --labels $S/plan$M.json.lab.npy --mission N
python tools/lights/plan_map.py ceiling_lights/map$NNN.json $S/plan$M.json.lab.npy lights/missionNN/plan.png
```

`lights/` (the screenshots and their README index) is git-ignored: tell the user where they are. Build `crusader` (it copies `ceiling_lights/` next to the executable) and check in lightview that the lamps show without `--ceiling-file`.

## 5. Tests, docs, commit

- `world_tests`, `ui_tests`, `core_tests` must pass (No Regret: also `regret_data_tests data`).
- Update the "So far" list in enhancements.md section 7d ("So far: mission 1" -> "missions 1 and N") and the "Done:" line of tools/lights/README.md.
- Commit only when the user asks: stage files explicitly, conventional commit, e.g. `feat(lighting): ceiling lights for mission N (map M)`. No Remorse pushes to `origin master`, No Regret to `origin main`.

## Data differences worth knowing

- No Remorse: floors are flat LAND tiles; black slabs (shape 538) cap hidden areas; liquids are pools, canals and their edges (big radioactive rooms).
- No Regret: floors are flat SOLID tiles; a machinery roof (shapes 1593/1594, 942) lies at z 96 and 192 round every room (the planner keeps the lowest floor and calls stretches with nothing above `roof`); its only liquids are the sludge puddles 0x15D..0x15F. The lamps light No Regret's maps only (`Game::render_lit` checks `!world.remorse()`).
