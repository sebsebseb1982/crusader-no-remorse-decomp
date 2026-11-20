"""Builds the PortMaster / muOS artwork of a Crusader port from docs/images.

screenshot.png: 640x480 crop at native scale (what the console shows).
cover.png:      400x300 composite (gameplay + cover art), transparent background,
                the format of PortMaster covers (muOS box art).
"""
import sys
from PIL import Image, ImageDraw, ImageFilter

repo, cx, cy, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
shot = Image.open(f"{repo}/docs/images/screenshot.png").convert("RGB")
art = Image.open(f"{repo}/docs/images/cover.png").convert("RGB")

# 1. screenshot 640x480, 1:1
shot.crop((cx - 320, cy - 240, cx + 320, cy + 240)).save(f"{out}/screenshot.png", optimize=True)

# 2. cover 400x300
W, H = 400, 300
cover = Image.new("RGBA", (W, H), (0, 0, 0, 0))
bg = shot.crop((cx - 400, cy - 300, cx + 400, cy + 300)).resize((372, 279), Image.LANCZOS)
cover.paste(bg, (W - 372, 0))
# cover art, framed, with a drop shadow, bottom left
bh = 214
bw = round(art.width * bh / art.height)
box = art.resize((bw, bh), Image.LANCZOS)
framed = Image.new("RGB", (bw + 4, bh + 4), (20, 20, 20))
framed.paste(box, (2, 2))
x, y = 6, H - framed.height - 4
shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rectangle((x + 4, y + 4, x + framed.width + 3, y + framed.height + 3), fill=(0, 0, 0, 170))
cover = Image.alpha_composite(cover, shadow.filter(ImageFilter.GaussianBlur(3)))
cover.paste(framed, (x, y))
cover.save(f"{out}/cover.png", optimize=True)
