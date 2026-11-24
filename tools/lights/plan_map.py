#!/usr/bin/env python3
"""A top view of a light plan: the rooms in colour (lit) or grey (left dark),
the walls, the lights as yellow dots, each room's id.

Usage: plan_map.py PLAN.json LABELS.npy OUT.png [--scale N] [--crop x0 y0 x1 y1 (world units)]
  LABELS.npy  the room grid plan_rooms.py saved next to its plan (PLAN.json.lab.npy)
"""
import argparse
import colorsys
import json

import numpy as np
from PIL import Image, ImageDraw, ImageFont

R = 32


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plan")
    ap.add_argument("labels")
    ap.add_argument("out")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--crop", type=int, nargs=4)
    a = ap.parse_args()
    plan = json.load(open(a.plan, encoding="utf-8"))
    lab = np.load(a.labels)
    by_id = {int(r["id"][1:]): r for r in plan["rooms"] if r["id"][1:].isdigit()}
    img = np.zeros(lab.shape + (3,), np.uint8)
    for n, r in by_id.items():
        m = lab == n
        if r.get("lights"):
            c = colorsys.hsv_to_rgb((n * 0.618) % 1, 0.45, 0.75)
            img[m] = [int(v * 255) for v in c]
        else:
            img[m] = (70, 70, 70) if r.get("skip") != "liquid" else (40, 110, 50)
    ys, xs = np.nonzero(lab)
    if a.crop:
        x0, y0, x1, y1 = (v // R for v in a.crop)
    else:
        x0, y0, x1, y1 = xs.min() - 4, ys.min() - 4, xs.max() + 5, ys.max() + 5
    s = a.scale
    im = Image.fromarray(img[y0:y1, x0:x1]).resize(((x1 - x0) * s, (y1 - y0) * s), Image.NEAREST)
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", max(10, 4 * s))
    except OSError:
        f = ImageFont.load_default()
    for r in plan["rooms"]:
        for l in r.get("lights", []):
            px, py = (l[0] / R - x0) * s, (l[1] / R - y0) * s
            d.ellipse([px - 2 * s, py - 2 * s, px + 2 * s, py + 2 * s], fill=(255, 230, 60), outline=(0, 0, 0))
    for r in plan["rooms"]:
        if r.get("lights") or (r.get("name") and r["area"] >= 160):
            cx, cy = (r["centre"][0] / R - x0) * s, (r["centre"][1] / R - y0) * s
            d.text((cx + 3 * s, cy - 2 * s), r["id"], font=f, fill=(255, 255, 255), stroke_width=2,
                   stroke_fill=(0, 0, 0))
    im.save(a.out)


if __name__ == "__main__":
    main()
