#!/usr/bin/env python3
"""Screenshots of a light plan's rooms, lit, with markers on the lights.

Usage: shoot_rooms.py PLAN.json OUT_DIR [--labels LABELS.npy] [--lightview EXE] [--data DIR] [--only ID,ID...]
       shoot_rooms.py PLAN.json --prune
  --prune  drops the lights whose floor spot is not seen (nothing drawn
           there: what black slabs hide), and rewrites PLAN.json

For each room with lights: OUT_DIR/<id>.png, the room lit by the 3D lighting
at the game's default ambient light (85 %) with its ceiling lights, each
light marked (numbered disc at the bulb, a dashed line down to the floor and
the spot under it), and in a corner the same view without the ceiling
lights. With the room grid of plan_rooms.py (--labels), what is not the
room is darkened and the room's floor outlined. OUT_DIR/README.md lists the
rooms, lit or not, and why.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
KIND_FR = {"warm": "blanc chaud", "neon": "néon froid", "sodium": "sodium orangé", "dim": "faible", "red": "rouge"}
KIND_RGB = {"warm": (255, 214, 150), "neon": (170, 225, 255), "sodium": (255, 170, 80), "dim": (230, 200, 140),
            "red": (255, 80, 60)}
SKIP_FR = {"small": "trop petite (placard, recoin)", "liquid": "éclairée par une piscine radioactive",
           "outside": "extérieur, sans plafond", "roof": "un toit (rien ne s'élève au-dessus)", "lit": "déjà éclairée par ses lampes",
           "dark": "volontairement sombre", "hidden": "son sol est caché à l'écran (dalles noires)", "none": "pas une pièce (vide, toit, coulisses)"}


def font(size):
    for name in ("C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def screen(x, y, z):
    return (x - y) / 4, (x + y) / 8 - z


def view_of(room):
    """Screen size and camera (x, y at z 0) framing the room."""
    x0, y0, x1, y1 = room["bbox"]
    fz, ceil = room["floor_z"], room.get("ceiling", 96)
    xs = [screen(x, y, z)[0] for x in (x0, x1) for y in (y0, y1) for z in (fz,)]
    ys = [screen(x, y, z)[1] for x in (x0, x1) for y in (y0, y1) for z in (fz, fz + ceil)]
    w = max(640, int(max(xs) - min(xs)) + 160)
    h = max(480, int(max(ys) - min(ys)) + 140)
    w, h = min(w, 2600), min(h, 1900)
    w, h = w // 4 * 4, h // 4 * 4
    sx, sy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2 - 20
    if "view" in room:  # hand-framed: [x, y, w, h]
        cx, cy, w, h = room["view"]
        sx, sy = screen(cx, cy, fz)
    # left = ((cx - cy) >> 2) - w / 2, top = ((cx + cy) >> 3) - h / 2 (camera z 0).
    cx = int(2 * sx + 4 * sy)
    cy = int(4 * sy - 2 * sx)
    left = ((cx - cy) >> 2) - w // 2
    top = ((cx + cy) >> 3) - h // 2
    return w, h, cx, cy, left, top


def highlight(img, room, labels, left, top, bare):
    """Darkens what is not the room (its floor and the walls up to its
    ceiling, as seen) and outlines its floor where it is seen (`bare`, the
    view without lighting, is not black there)."""
    n = int(room["id"][1:])
    x0, y0, x1, y1 = (v // 32 for v in room["bbox"])
    cy, cx = np.nonzero(labels[y0:y1, x0:x1] == n)
    fz = room["floor_z"]
    floor = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(floor)
    for yy, xx in zip(cy + y0, cx + x0):
        X0, Y0, X1, Y1 = xx * 32, yy * 32, xx * 32 + 32, yy * 32 + 32
        pts = [screen(X, Y, fz) for X, Y in ((X0, Y0), (X1, Y0), (X1, Y1), (X0, Y1))]
        d.polygon([(px - left, py - top) for px, py in pts], fill=255)
    seen = bare.convert("L").point(lambda v: 255 if v > 6 else 0).filter(ImageFilter.MaxFilter(5))
    floor = ImageChops.multiply(floor, seen)
    volume = floor.copy()
    for k in range(4, room.get("ceiling", 96) + 1, 4):
        volume = ImageChops.lighter(volume, ImageChops.offset(floor, 0, -k))
    volume = volume.filter(ImageFilter.MaxFilter(5))
    dark = Image.eval(img, lambda v: int(v * 0.4))
    img = Image.composite(img, dark, volume)
    edge = ImageChops.subtract(floor.filter(ImageFilter.MaxFilter(3)), floor.filter(ImageFilter.MinFilter(3)))
    img.paste((255, 225, 120), mask=edge.point(lambda v: 200 if v > 0 else 0))
    return img


def prune(exe, data, plan):
    """Drops the lights over a floor that is not seen."""
    dropped = 0
    for room in plan["rooms"]:
        if not room.get("lights"):
            continue
        w, h, cx, cy, left, top = view_of(room)
        with tempfile.TemporaryDirectory() as tmp:
            flat = pathlib.Path(tmp) / "flat.png"
            subprocess.run([str(exe), str(data), str(plan["map"]), str(cx), str(cy), "--size", f"{w}x{h}", "--flat",
                            "--png", str(flat)], check=True, capture_output=True)
            img = np.array(Image.open(flat).convert("L"))
        keep = []
        for l in room["lights"]:
            fx, fy = screen(l[0], l[1], room["floor_z"])
            x, y = int(fx - left), int(fy - top)
            win = img[max(y - 3, 0):y + 4, max(x - 3, 0):x + 4]
            if win.size and win.max() > 10:
                keep.append(l)
            else:
                dropped += 1
                print(f"{room['id']}: light at {l[0]}, {l[1]} over nothing seen: dropped")
        room["lights"] = keep
    print(f"{dropped} lights dropped")


def shoot(exe, data, plan, room, ceiling_file, out_png, mission, labels=None):
    w, h, cx, cy, left, top = view_of(room)
    with tempfile.TemporaryDirectory() as tmp:
        lit, bare = pathlib.Path(tmp) / "lit.png", pathlib.Path(tmp) / "bare.png"
        base = [str(exe), str(data), str(plan["map"]), str(cx), str(cy), "--size", f"{w}x{h}", "--ambient", "3",
                "--no-lamp", "--ceiling-file", str(ceiling_file)]
        subprocess.run(base + ["--png", str(lit)], check=True, capture_output=True)
        subprocess.run(base + ["--no-ceiling", "--png", str(bare)], check=True, capture_output=True)
        img = Image.open(lit).convert("RGB")
        before = Image.open(bare).convert("RGB")
    if labels is not None:
        img = highlight(img, room, labels, left, top, before)
    d = ImageDraw.Draw(img, "RGBA")
    f_num, f_title, f_small = font(15), font(20), font(14)
    fz = room["floor_z"]
    kind = room.get("kind", "warm")
    # Markers: dashed line from the bulb to the floor, the spot under it, the bulb.
    for i, l in enumerate(room["lights"], 1):
        k = l[4] if len(l) > 4 else kind
        col = KIND_RGB[k]
        bx, by = screen(l[0], l[1], l[2])
        fx, fy = screen(l[0], l[1], fz)
        bx, by, fx, fy = bx - left, by - top, fx - left, fy - top
        y = by
        while y < fy - 4:
            d.line([(bx, y), (bx, min(y + 5, fy - 4))], fill=(255, 255, 255, 200), width=2)
            y += 10
        d.ellipse([fx - 16, fy - 8, fx + 16, fy + 8], outline=(255, 255, 255, 230), width=2)
        d.line([(fx - 5, fy), (fx + 5, fy)], fill=(255, 255, 255, 230), width=2)
        nx, ny = bx, by
        d.ellipse([nx - 12, ny - 12, nx + 12, ny + 12], fill=col + (235,), outline=(20, 20, 20, 255), width=2)
        t = str(i)
        tw = d.textlength(t, font=f_num)
        d.text((nx - tw / 2, ny - 9), t, font=f_num, fill=(10, 10, 10))
    # Caption.
    n = len(room["lights"])
    title = f"Mission {mission} · {room['id']} — {room['name']}"
    line2 = (f"{n} plafonnier{'s' if n > 1 else ''} {KIND_FR[kind]}, à {room['lights'][0][2] - fz} unités "
             f"du sol (plafond ≈ {room.get('ceiling', 96)})")
    lines = [line2] + ([room["note"]] if room.get("note") else [])
    tw = max([d.textlength(title, font=f_title)] + [d.textlength(s, font=f_small) for s in lines])
    d.rectangle([8, 8, 24 + tw, 40 + 20 * len(lines)], fill=(0, 0, 0, 170))
    d.text((16, 12), title, font=f_title, fill=(255, 235, 190))
    for k, s in enumerate(lines):
        d.text((16, 38 + 20 * k), s, font=f_small, fill=(230, 230, 230))
    # The same view without the ceiling lights.
    tw_, th_ = img.width // 4, img.height // 4
    thumb = before.resize((tw_, th_), Image.LANCZOS)
    x, y = img.width - tw_ - 10, img.height - th_ - 10
    d.rectangle([x - 3, y - 22, x + tw_ + 2, y + th_ + 2], fill=(0, 0, 0, 200))
    d.text((x + 2, y - 20), "sans plafonniers", font=f_small, fill=(230, 230, 230))
    img.paste(thumb, (x, y))
    img.save(out_png, optimize=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plan")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--prune", action="store_true")
    ap.add_argument("--lightview", default=str(ROOT / "build/Release/lightview.exe"))
    ap.add_argument("--data", default=str(ROOT / "data"))
    ap.add_argument("--mission", type=int, default=1)
    ap.add_argument("--only", default="")
    ap.add_argument("--labels")
    a = ap.parse_args()
    plan = json.loads(pathlib.Path(a.plan).read_text(encoding="utf-8"))
    if a.prune:
        prune(a.lightview, a.data, plan)
        pathlib.Path(a.plan).write_text(json.dumps(plan, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        return 0
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    ceiling = pathlib.Path(a.plan).resolve()  # lightview --ceiling-file reads the plan itself
    only = set(filter(None, a.only.split(",")))
    labels = np.load(a.labels) if a.labels else None
    index = [f"# Mission {a.mission} — plafonniers (map {plan['map']})", "",
             "Plan : `ceiling_lights/" + pathlib.Path(a.plan).name + "` (les types : `ceiling_lights/kinds.json`). Chaque capture montre la pièce avec "
             "l'éclairage 3D à la lumière ambiante par défaut (85 %) ; les disques numérotés sont les lampes, "
             "le trait pointillé descend jusqu'au point du sol sous chacune ; en bas à droite, la même vue sans "
             "les plafonniers. Le contour jaune délimite la pièce (sol visible), le reste est assombri.", "",
             "Vue de dessus : [plan.png](plan.png) (pièces éclairées en couleur, laissées sombres en gris, "
             "bassins radioactifs en vert, lampes en jaune).", "", "| Pièce | Lampes | Capture |", "|---|---|---|"]
    skipped = []
    for room in plan["rooms"]:
        if not room.get("lights"):
            if room.get("skip") != "small" and room["area"] >= 160:
                skipped.append(room)
            continue
        if only and room["id"] not in only:
            continue
        png = out / f"{room['id']}.png"
        shoot(a.lightview, a.data, plan, room, ceiling, png, a.mission, labels)
        print(png, flush=True)
    for room in plan["rooms"]:
        if room.get("lights"):
            index.append(f"| {room['id']} {room['name']} | {len(room['lights'])} {KIND_FR[room.get('kind', 'warm')]} "
                         f"| [{room['id']}.png]({room['id']}.png) |")
    small = sum(1 for room in plan["rooms"] if room.get("skip") == "small")
    index += ["", "## Laissées sans plafonnier", "",
              f"- {small} placards, recoins et seuils de porte (moins de 160 cases de 32 × 32) : trop petits.",
              "- Les parties de sol cachées à l'écran (sous les dalles noires qui masquent ce que le jeu ne montre "
              "pas) : aucune lampe n'y est posée.", ""]
    if skipped:
        index += ["| Pièce | Pourquoi |", "|---|---|"]
        for room in skipped:
            index.append(f"| {room['id']} {room['name']} | {room.get('note') or SKIP_FR.get(room.get('skip'), '')} |")
    (out / "README.md").write_text("\n".join(index) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
