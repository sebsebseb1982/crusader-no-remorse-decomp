#!/usr/bin/env python3
"""Finds the rooms of a map and proposes ceiling lights for them.

Usage: plan_rooms.py DUMP.csv OUT.json [--overview OUT.png]
  DUMP.csv  `mapview data MAP --dump`
  OUT.json  a draft plan (tools/lights/README.md): every room found, with the
            lights proposed, or why it gets none ("skip").

A room is a connected stretch of floor (TYPEFLAG land, flat) at one height,
bounded by what stands at least 40 z units high (walls, machines) and by
doors, on a 32-unit grid. It gets no light when it is small (a closet), when
a radioactive pool lights it, or when it has no walls round it (outside).
The lights go on a grid about LIGHT_SPACING apart, under the ceiling (the
walls' top), each moved off what stands too high under it.
"""
import argparse
import collections
import csv
import json
import math

import numpy as np
from PIL import Image, ImageFilter

R = 32  # grid cell, world units
LIGHT_SPACING = 1152  # world units; 1280 in the large halls
MIN_AREA = 160  # cells (about 400 x 400 units): smaller rooms are closets, recesses
# The liquids' shapes, by game (lighting.cpp is_liquid): No Remorse's pools,
# strips and edges; No Regret's sludge puddles only (No Remorse's pool shapes
# are walls there).
LIQUIDS_BY_GAME = {
    "remorse": {0x150, 0x59E, 0x227, 0x228, 0x5A2, 0x55A, 0x56D, 0x56E, 0x1D6, 0x539, 0x54C, 0x580, 0x15D},
    "regret": {0x15D, 0x15E, 0x15F},
}
LIQUIDS = LIQUIDS_BY_GAME["remorse"]
LAND, SOLID = 8, 2


def load(path):
    rows = list(csv.DictReader(open(path, encoding="utf-8")))
    for r in rows:
        for k in ("id", "shape", "frame", "x_left", "x", "y_far", "y", "z", "z_top"):
            r[k] = int(r[k])
        r["tf"] = int(r["typeflags"], 16)
    return rows


def cells(r):
    return (slice(r["y_far"] // R, max(r["y"] // R, r["y_far"] // R + 1)),
            slice(r["x_left"] // R, max(r["x"] // R, r["x_left"] // R + 1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("out")
    ap.add_argument("--map", type=int, default=1)
    ap.add_argument("--game", choices=sorted(LIQUIDS_BY_GAME), default="remorse")
    # Where flat items lie over one another: the highest is the floor (No
    # Remorse's upper rooms), or the lowest (No Regret: a machinery roof lies
    # over everything round the rooms, 96 and 192 z units up).
    ap.add_argument("--floor", choices=("highest", "lowest"), default="highest")
    # A stretch whose edge is less walled than this is outside. Open plan
    # offices and corridors that open wide on rooms can fall under 0.5: lower it.
    ap.add_argument("--min-walled", type=float, default=0.5)
    ap.add_argument("--overview")
    a = ap.parse_args()
    global LIQUIDS
    LIQUIDS = LIQUIDS_BY_GAME[a.game]
    rows = load(a.dump)
    size = (max(r["x"] for r in rows) // R + 4)
    size = max(size, max(r["y"] for r in rows) // R + 4)
    H = W = size

    floorz = np.full((H, W), -1, np.int32)
    # Flat tiles, land or solid (No Regret's floors are solid, not land), not
    # black (the void), not editor items.
    flat = [r for r in rows if r["z_top"] - r["z"] <= 8 and r["x"] - r["x_left"] >= 64 and r["tf"] & (LAND | SOLID)
            and not r["tf"] & 0x10000 and r["class"] != "BLACK16"]
    floors = [r for r in flat if int(r["lum"]) >= 12]
    for r in sorted(floors, key=lambda r: r["z"], reverse=a.floor == "lowest"):
        floorz[cells(r)] = r["z"]
    liquid = np.zeros((H, W), bool)
    for r in rows:
        if r["shape"] in LIQUIDS:
            liquid[cells(r)] = True
            if r["class"] != "BLACK16" and floorz[cells(r)].max() < 0:
                floorz[cells(r)] = r["z"]
    # Black slabs over a floor (shape 538 at the ceiling: the game hides what
    # is under them) and black floors: the void, no room.
    black = [r for r in rows if 0 <= int(r["lum"]) < 12 and r["x"] - r["x_left"] >= 128]
    for r in black:
        sl = cells(r)
        fz = floorz[sl]
        floorz[sl] = np.where(fz <= r["z"], -1, fz)
    # A slab higher up (black, or a floor or roof above) also hides, as seen,
    # the floors behind it: a floor point (x, y, z) shows where (x + d, y + d)
    # at the slab's height zs is, d = 4 (zs - z).
    hidden = np.zeros((H, W), bool)
    for fz in sorted(set(np.unique(floorz[floorz >= 0]).tolist())):
        at = np.abs(floorz - fz) <= 8
        for r in black + floors:
            if r["z"] < fz + 40:
                continue
            d = 4 * (r["z"] - fz) // R
            ys, xs = cells(r)
            ys = slice(max(ys.start - d, 0), max(ys.stop - d, 0))
            xs = slice(max(xs.start - d, 0), max(xs.stop - d, 0))
            hidden[ys, xs] |= at[ys, xs]
    floorz[hidden] = -1

    # Walls and what stands high, by the floor under them; the highest top
    # standing on each cell (what a light must clear).
    wall = np.zeros((H, W), bool)
    top = np.full((H, W), -1, np.int32)
    wall_top = collections.defaultdict(list)
    for r in rows:
        if r["class"] in ("BLACK16",) or r["tf"] & 0x10000:  # editor items
            continue
        sl = cells(r)
        fz = floorz[sl]
        if r["tf"] & SOLID and r["z_top"] - r["z"] >= 40:
            m = (r["z"] <= fz + 24) & (r["z_top"] >= fz + 40)
            wall[sl] |= m | (fz < 0)
        if r["z_top"] - r["z"] > 8 or not r["tf"] & LAND:
            # Standing in the room (not the roof tiles at the ceiling).
            on = (r["z"] >= fz - 8) & (r["z"] <= fz + 64)
            t = top[sl]
            top[sl] = np.where(on, np.maximum(t, r["z_top"]), t)
    door = np.zeros((H, W), bool)
    for r in rows:
        c = r["class"]
        if "DOOR" in c or c.startswith(("ELEV", "GATE")):
            door[cells(r)] = True
    block = wall | door

    lab = np.zeros((H, W), np.int32)
    ok = (floorz >= 0) & ~block
    comps = []
    n = 0
    for y in range(H):
        for x in np.nonzero(ok[y] & (lab[y] == 0))[0]:
            if lab[y, x]:
                continue
            n += 1
            q = collections.deque([(y, x)])
            lab[y, x] = n
            pts = []
            while q:
                cy, cx = q.popleft()
                pts.append((cy, cx))
                for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                    if ok[ny, nx] and not lab[ny, nx] and abs(int(floorz[ny, nx]) - int(floorz[cy, cx])) <= 24:
                        lab[ny, nx] = n
                        q.append((ny, nx))
            comps.append((n, pts))
    comps, lab = split_rooms(comps, lab)
    # Near a radioactive pool (640 units): its green glow lights the place.
    near_liquid = grow(liquid, 20)

    rooms = []
    for n, pts in comps:
        if len(pts) < 16:
            continue
        ys = np.array([p[0] for p in pts])
        xs = np.array([p[1] for p in pts])
        x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
        fz = int(np.median(floorz[ys, xs]))
        mask = lab[y0:y1, x0:x1] == n
        # The ring of cells round the room: walls, or nothing (outside).
        ring = np.zeros((y1 - y0 + 2, x1 - x0 + 2), bool)
        mm = np.pad(mask, 1)
        ring[:, :] = (np.roll(mm, 1, 0) | np.roll(mm, -1, 0) | np.roll(mm, 1, 1) | np.roll(mm, -1, 1)) & ~mm
        ry, rx = np.nonzero(ring)
        ry = ry + y0 - 1
        rx = rx + x0 - 1
        walled = float(np.mean(block[ry, rx])) if len(ry) else 0
        tops = top[ry, rx][block[ry, rx]]
        ceil = int(np.percentile(tops, 60)) - fz if len(tops) else 96
        liq = float(np.mean(near_liquid[ys, xs]))
        room = {
            "id": f"C{n}",
            "name": "",
            "floor_z": fz,
            "ceiling": ceil,
            "area": len(pts),
            "bbox": [int(x0 * R), int(y0 * R), int(x1 * R), int(y1 * R)],
            "centre": [int(xs.mean() * R + R / 2), int(ys.mean() * R + R / 2)],
            "walled": round(walled, 2),
            "liquid": round(liq, 2),
            "kind": "warm",
            "lights": [],
            "contents": contents(rows, lab, n, x0, y0, x1, y1),
        }
        if len(pts) < MIN_AREA:
            room["skip"] = "small"
        elif liq > 0.4:
            room["skip"] = "liquid"
        elif walled < a.min_walled:
            room["skip"] = "outside"
        elif ceil < 40:
            # Nothing stands above it: a roof (No Regret's machinery round the
            # rooms), not a room.
            room["skip"] = "roof"
        else:
            room["lights"] = place(mask & ~near_liquid[y0:y1, x0:x1], x0, y0, fz, ceil, top)
        room["name"], room["kind"] = describe(room, x1 - x0, y1 - y0)
        if room.get("skip") == "small":
            room["name"] = "Placard, recoin" if len(pts) >= 40 else "Recoin"
        elif room.get("skip") == "liquid":
            pool = "Bassin radioactif" if a.game == "remorse" else "Flaque toxique"
            room["name"] = pool if liq > 0.9 else f"{room['name']} ({pool.lower()})"
        elif room.get("skip") == "roof":
            room["name"] = "Toit"
        rooms.append(room)
    rooms.sort(key=lambda r: (-r["area"]))
    for room in rooms:
        del room["contents"]
    json.dump({"map": a.map, "rooms": rooms}, open(a.out, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    lit = [r for r in rooms if r["lights"]]
    print(f"{len(rooms)} rooms, {len(lit)} lit, {sum(len(r['lights']) for r in lit)} lights;",
          collections.Counter(r.get("skip", "lit") for r in rooms))
    np.save(a.out + ".lab.npy", lab)


MACHINES = ("s60", "s87", "s61", "s374", "GENERATR", "TRANSFOR", "VALVE", "SHOCKEW", "SHOCKNS", "RFANNS", "RFANEW")
STORES = ("LUGGAGE", "MCRATE", "BARRELUP", "BBOX", "GBOX_EW", "GBOX_NS", "BOX_EW", "BOX_NS", "CHEST_EW", "CHEST_NS",
          "s509", "s510")
OFFICES = ("SMALCHAI", "MDESKNS", "MDESKEW", "MONITNS", "MONITEW", "BIGCOMEW", "s968")


def describe(room, w, h):
    """A first name and lamp for a room, from what stands in it."""
    c = collections.Counter()
    for item in room["contents"].split():
        k, v = item.rsplit(":", 1)
        c[k] = int(v)
    machines = sum(c[k] for k in MACHINES)
    stores = sum(c[k] for k in STORES)
    offices = sum(c[k] for k in OFFICES)
    if min(w, h) <= 10 and max(w, h) >= 3 * min(w, h):
        return "Couloir", "neon"
    if offices >= 4 and offices * 2 >= machines:
        return "Salle de contrôle", "warm"
    if machines >= 8 and machines >= stores:
        return "Salle des machines" if room["area"] < 6000 else "Hall des machines", "neon"
    if stores >= 6:
        return "Entrepôt", "sodium"
    if offices:
        return "Bureau", "warm"
    if machines:
        return "Local technique", "neon"
    return "Salle", "warm"


def grow(mask, k):
    """mask grown by k cells (a square)."""
    img = Image.fromarray(mask.astype(np.uint8) * 255)
    return np.array(img.filter(ImageFilter.MaxFilter(2 * k + 1))) > 0


def shrink(mask, k):
    img = Image.fromarray(np.pad(mask, k).astype(np.uint8) * 255)
    return (np.array(img.filter(ImageFilter.MinFilter(2 * k + 1))) > 0)[k:-k, k:-k]


def components(mask):
    """4-connected components of mask: a list of (ys, xs) arrays."""
    seen = np.zeros(mask.shape, bool)
    out = []
    h, w = mask.shape
    for y, x in zip(*np.nonzero(mask)):
        if seen[y, x]:
            continue
        seen[y, x] = True
        q = collections.deque([(y, x)])
        pts = []
        while q:
            cy, cx = q.popleft()
            pts.append((cy, cx))
            for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    q.append((ny, nx))
        out.append((np.array([p[0] for p in pts]), np.array([p[1] for p in pts])))
    return out


SPLIT = 3  # cells: passages narrower than 2 * SPLIT + 1 cells (224 units) part rooms


def split_rooms(comps, lab):
    """Parts the stretches of floor into rooms where they narrow: what an
    opening (shrunk then grown by SPLIT cells) keeps are rooms; the narrow
    rest makes a corridor when it is long enough, else it joins the room it
    opens on most."""
    new = np.zeros(lab.shape, np.int32)
    out = []
    n = 0
    for _, pts in comps:
        ys = np.array([p[0] for p in pts])
        xs = np.array([p[1] for p in pts])
        x0, y0 = xs.min() - 1, ys.min() - 1
        mask = np.zeros((ys.max() - y0 + 2, xs.max() - x0 + 2), bool)
        mask[ys - y0, xs - x0] = True
        opened = grow(shrink(mask, SPLIT), SPLIT) & mask
        local = np.zeros(mask.shape, np.int32)
        parts = []
        for cy, cx in components(opened):
            parts.append((cy, cx))
            local[cy, cx] = len(parts)
        for cy, cx in components(mask & ~opened):
            # The rooms it touches, by the length of the border.
            touch = collections.Counter()
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny = np.clip(cy + dy, 0, mask.shape[0] - 1)
                nx = np.clip(cx + dx, 0, mask.shape[1] - 1)
                for v in local[ny, nx]:
                    if v:
                        touch[int(v)] += 1
            if len(cy) >= 120 or not touch:
                parts.append((cy, cx))
                local[cy, cx] = len(parts)
            else:
                local[cy, cx] = touch.most_common(1)[0][0]
        for k in range(1, len(parts) + 1):
            cy, cx = np.nonzero(local == k)
            if not len(cy):
                continue
            n += 1
            new[cy + y0, cx + x0] = n
            out.append((n, list(zip((cy + y0).tolist(), (cx + x0).tolist()))))
    return out, new


def contents(rows, lab, n, x0, y0, x1, y1):
    """What stands in the room: its usecode classes (or shapes), most first."""
    c = collections.Counter()
    for r in rows:
        if not (x0 * R <= r["x"] <= x1 * R and y0 * R <= r["y"] <= y1 * R):
            continue
        cy, cx = (r["y"] - 1) // R, (r["x"] - 1) // R
        if lab[cy, cx] != n and lab[min(cy + 1, lab.shape[0] - 1), cx] != n:
            continue
        if r["z_top"] - r["z"] <= 8 and r["tf"] & LAND:
            continue
        c[r["class"] or f"s{r['shape']}"] += 1
    return " ".join(f"{k}:{v}" for k, v in c.most_common(10))


def place(mask, x0, y0, fz, ceil, top):
    """Lights for a room (mask: its cells from x0, y0): a grid, LIGHT_SPACING
    apart, each moved to the nearest cell of the room with room under the
    ceiling, away from the walls."""
    h, w = mask.shape
    # Distance to the room's edge (cells), to keep the lights off the walls.
    dist = np.zeros(mask.shape, np.int32)
    cur = np.pad(mask, 1)  # padded: np.roll wraps round
    k = 0
    while cur.any():
        k += 1
        dist[cur[1:-1, 1:-1]] = k
        cur = cur & np.roll(cur, 1, 0) & np.roll(cur, -1, 0) & np.roll(cur, 1, 1) & np.roll(cur, -1, 1)
    # 64 z units over the walls' top (160 over the floor in most rooms, twice
    # the Silencer's 80): a lamp is a point, the shadow of a head at h under a
    # lamp at H is H / (H - h) times wider than the head. Under the ceiling
    # (92, 74) a head right under a lamp threw a blot 8 times its width, a
    # head higher than the lamp a shadow without end; at 160 a disc round
    # the feet, a short shadow beside the lamp. The lamp is not drawn.
    z = fz + max(48, min(ceil, 120)) + 64
    sp = (1280 if mask.sum() > 4000 else LIGHT_SPACING) / R
    nx = max(1, round(w / sp))
    ny = max(1, round(h / sp))
    out = []
    yy, xx = np.nonzero(mask)
    free = top[y0 + yy, x0 + xx] <= z - 12
    yy, xx = yy[free], xx[free]
    if not len(yy):
        return []
    # Off the walls: at least 5 cells (160 units) in, or as far in as the
    # room allows (a lamp against a wall would only light that wall).
    d = dist[yy, xx]
    inner = d >= min(6, d.max())
    yy, xx = yy[inner], xx[inner]
    edge = 0.6 * np.minimum(dist[yy, xx], 8)
    if not len(yy):
        return out
    for j in range(ny):
        for i in range(nx):
            cx, cy = (i + 0.5) * w / nx, (j + 0.5) * h / ny
            d = np.hypot(xx + 0.5 - cx, yy + 0.5 - cy) - edge
            b = int(np.argmin(d))
            if d[b] > sp * 0.6:
                continue
            px, py = int((x0 + xx[b] + 0.5) * R), int((y0 + yy[b] + 0.5) * R)
            if any(math.hypot(px - l[0], py - l[1]) < sp * R * 0.55 for l in out):
                continue
            out.append([px, py, int(z)])
    return out


if __name__ == "__main__":
    main()
