#!/usr/bin/env python3
"""Contact sheets of a plan's room screenshots, to review them a few at a time.

Usage: sheets.py PLAN.json SHOTS_DIR OUT_DIR [--per 6]
  SHOTS_DIR  the screenshots of shoot_rooms.py (<room id>.png)
  OUT_DIR    s00.png, s01.png...: the lit rooms' screenshots, 6 to a sheet
             (2 x 3, each shrunk to 720 x 540), in the plan's order
"""
import argparse
import json
import pathlib

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plan")
    ap.add_argument("shots")
    ap.add_argument("out")
    ap.add_argument("--per", type=int, default=6)
    a = ap.parse_args()
    plan = json.loads(pathlib.Path(a.plan).read_text(encoding="utf-8"))
    ids = [r["id"] for r in plan["rooms"] if r.get("lights")]
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    cols, w, h = 2, 720, 540
    rows = (a.per + cols - 1) // cols
    for k in range(0, len(ids), a.per):
        sheet = Image.new("RGB", (cols * w, rows * h))
        for j, room in enumerate(ids[k:k + a.per]):
            path = pathlib.Path(a.shots) / f"{room}.png"
            if not path.exists():
                continue
            im = Image.open(path)
            im.thumbnail((w, h))
            sheet.paste(im, ((j % cols) * w, (j // cols) * h))
        sheet.save(out / f"s{k // a.per:02d}.png")
    print(f"{len(ids)} rooms on {(len(ids) + a.per - 1) // a.per} sheets in {out}")


if __name__ == "__main__":
    main()
