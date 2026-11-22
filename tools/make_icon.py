#!/usr/bin/env python3
"""Draws resources/icon.jpg (256x256, the HBMenu icon): a screen with a play
mark and signal arcs on a blue gradient. Needs Pillow."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

S = 1024  # drawn at 4x, then scaled down
OUT = Path(__file__).resolve().parent.parent / "resources" / "icon.jpg"


def gradient(size, top, bottom):
    img = Image.new("RGB", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            t = (y * 0.75 + x * 0.25) / size
            px[x, y] = tuple(int(a + (b - a) * t) for a, b in zip(top, bottom))
    return img


def main():
    img = gradient(S, (24, 38, 92), (8, 120, 196))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((180, 120, 844, 784), fill=(120, 200, 255, 70))
    img.paste(glow.filter(ImageFilter.GaussianBlur(120)), (0, 0), glow.filter(ImageFilter.GaussianBlur(120)))

    d = ImageDraw.Draw(img)
    white = (245, 249, 255)
    # Signal arcs above the screen.
    cx, cy = 512, 330
    for r, w in ((250, 46), (165, 46)):
        d.arc((cx - r, cy - r, cx + r, cy + r), 215, 325, fill=white, width=w)
    d.ellipse((cx - 34, cy - 34, cx + 34, cy + 34), fill=white)
    # The screen and its stand.
    d.rounded_rectangle((170, 420, 854, 810), radius=64, outline=white, width=44)
    d.rounded_rectangle((420, 840, 604, 880), radius=20, fill=white)
    # The play mark.
    d.polygon([(450, 515), (450, 715), (622, 615)], fill=white)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.resize((256, 256), Image.LANCZOS).save(OUT, "JPEG", quality=92)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
