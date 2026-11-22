#!/usr/bin/env python3
"""desktop/xted.ico, the icon of xtedw.exe, from the shapes of desktop/xted.svg (drawn again here
with PIL, four times bigger and scaled down for smooth edges). Run it after changing the SVG:
    python3 tools/make_icon.py
"""
import os
from PIL import Image, ImageDraw

S = 4                       # drawn at 4 x 256, scaled down
W = 256 * S


def box(x0, y0, x1, y1):
    return [x0 * S, y0 * S, x1 * S, y1 * S]


def draw():
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # the window: a gradient from #2a3142 to #171b25 inside a rounded square
    grad = Image.new("RGBA", (W, W))
    gd = ImageDraw.Draw(grad)
    for y in range(W):
        t = y / (W - 1)
        c = tuple(int(a + (b - a) * t) for a, b in zip((0x2a, 0x31, 0x42), (0x17, 0x1b, 0x25)))
        gd.line([(0, y), (W, y)], fill=c + (255,))
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box(12, 20, 244, 236), radius=28 * S, fill=255)
    im.paste(grad, (0, 0), mask)
    # the title bar, rounded at the top only
    bar = Image.new("L", (W, W), 0)
    ImageDraw.Draw(bar).rounded_rectangle(box(12, 20, 244, 84), radius=28 * S, fill=255)
    ImageDraw.Draw(bar).rectangle(box(12, 56, 244, 84), fill=0)
    im.paste((0x3a, 0x43, 0x57, 255), (0, 0), bar)
    for cx, col in ((40, "#ff6b6b"), (62, "#ffd166"), (84, "#06d6a0")):
        d.ellipse(box(cx - 7, 31, cx + 7, 45), fill=col)
    # the prompt: a chevron
    d.line([(40 * S, 86 * S), (66 * S, 106 * S), (40 * S, 126 * S)], fill="#06d6a0", width=12 * S,
           joint="curve")
    for x, y in ((40, 86), (66, 106), (40, 126)):
        d.ellipse(box(x - 6, y - 6, x + 6, y + 6), fill="#06d6a0")
    # lines of text and the cursor
    for x0, y0, x1, col in ((80, 98, 152, "#e8ecf4"), (40, 140, 96, "#7aa2f7"),
                            (106, 140, 200, "#c3cad9"), (40, 172, 160, "#c3cad9")):
        d.rounded_rectangle(box(x0, y0, x1, y0 + 14), radius=7 * S, fill=col)
    d.rounded_rectangle(box(170, 164, 186, 194), radius=3 * S, fill="#ffd166")
    return im.resize((256, 256), Image.LANCZOS)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    img = draw()
    sizes = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (256, 256)]
    img.save(os.path.join(root, "desktop", "xted.ico"), sizes=sizes)
    img.save(os.path.join(root, "build", "xted-icon-256.png"))


if __name__ == "__main__":
    main()
