# Ceiling lights: one plan per mission

The original draws no ceilings and has no light there. With the 3D lighting (CEILING LIGHTS / PLAFONNIERS option, enhancements.md section 7d) the game adds lamps under the rooms' ceilings. They are editable JSON files in `ceiling_lights/` (copied next to the executable when it is built, and in the release packages), read when the game starts:

- `ceiling_lights/kinds.json`: each kind of lamp, its `colour` (red, green, blue, 0..1), `strength` (times the colour; the game makes it brighter the brighter the ambient light), `radius` (reach, lighting-space units: a world unit; a z unit is 3.266) and `dim` (0..1, how much it lowers the ambient light round it). Kinds can be changed or added; without the file the game uses the same ones built in.
- `ceiling_lights/mapNNN.json`: a map's lamps, room by room (NNN its number: `map001.json`), the plan below.

Edit them next to the executable (or in the sources, then build) and restart the game; in `lightview`, F5 reads them again. A file that does not parse is told on the error output (line and column) and gives no lamps. A mod (`--mod DIR`) can bring its own: `DIR/ceiling_lights/` is read first.

Done: missions 1 to 15, on maps 1, 3, 5, 7, 10, 11, 13, 15, 17, 19, 21, 23, 25, 27 and 29, and the variant maps 6 (mission 3), 14 (7), 16 (8), 24 (12), 26 (13) and 28 (14). The screenshots of their rooms are in `lights/missionNN/` and `lights/missionNN-mapMMM/` for the variants (not in git).

## A plan

```json
{"map": 1, "mission": 1, "rooms": [
  {"id": "C286", "name": "Entrepôt", "kind": "sodium", "floor_z": 0, "ceiling": 96,
   "area": 3655, "bbox": [x0, y0, x1, y1], "centre": [x, y],
   "lights": [[x, y, z], [x, y, z, radius, "neon"]]},
  {"id": "C3", "name": "Placard, recoin", "skip": "small", "lights": []}
]}
```

- Coordinates are world units (x, y) and z units (z, the bulb: 22 under the ceiling, the walls' top).
- `kind`: a kind of `kinds.json`: `warm` (offices, quarters), `neon` (machine rooms, labs, corridors), `sodium` (stores), `dim`, `red`, or one added there; a light can override it (5th value), and its radius (4th, lighting-space units, 0 the kind's). An unknown kind is `warm`.
- The game reads only `rooms[].kind` and `rooms[].lights`; the rest describes the room for the tools.
- `skip`: why a room has none: `small` (closets, recesses, door sills), `liquid` (a radioactive pool lights it), `outside`, `roof` (nothing stands above it), `hidden` (its floor is not seen: black slabs hide it).
- `note`, `view` ([x, y, w, h], to frame a screenshot by hand) are optional.

## Making one

```sh
mapview data 1 --dump > map1.csv                       # every item: boxes, type flags, class, brightness
tools/lights/plan_rooms.py map1.csv plan.json          # rooms and proposed lights (+ plan.json.lab.npy, the room grid)
                                                       # --game remorse|regret (the liquids' shapes), --floor highest|lowest
                                                       # --min-walled 0.05: open-plan offices (mission 9) are not outside
tools/lights/shoot_rooms.py plan.json --prune          # drops the lights over a floor not seen
tools/lights/shoot_rooms.py plan.json shots --labels plan.json.lab.npy   # a screenshot per lit room
tools/lights/sheets.py plan.json shots sheets          # 6 to a sheet, to review them
# review, edit names, kinds and lights by hand, then:
cp plan.json ceiling_lights/map001.json                 # the game reads it
tools/lights/shoot_rooms.py ceiling_lights/map001.json lights/mission01 --labels plan.json.lab.npy --mission 1
tools/lights/plan_map.py ceiling_lights/map001.json plan.json.lab.npy lights/mission01/plan.png
```

How `plan_rooms.py` finds the rooms: flat tiles, land or solid and not black, make the floor (on a 32-unit grid; where they lie over one another, the highest: the upper rooms; `--floor lowest` for No Regret, whose machinery roof lies over everything round the rooms), what stands 40 z units or more and doors bound it; black floors and black slabs (shape 538 at the ceiling), and the floors that any slab higher up hides as seen, are the void; a stretch of floor with nothing standing above it is a roof. Each stretch of floor is cut where it narrows under 7 cells (224 units): the narrow parts long enough are corridors, the others join the room they open on most. Lights hang 22 z units under the ceiling on a grid 1152 units apart (1280 in the large halls: few strong lamps, so that one lamp lights most of its stretch of floor and the shadows stay dark), at least 160 units from the walls, clear of what stands under them, none within 640 units of a radioactive pool.

The skill `.claude/skills/ceiling-lights/SKILL.md` walks Claude Code through a new map (`/ceiling-lights`).

`lightview` shows them (O switches them, `--no-ceiling`, F5 reads the files again), and `--ceiling-file MAP.json` shows another map file's lights (a plan being made).
