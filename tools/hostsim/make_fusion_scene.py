#!/usr/bin/env python3
"""Draws the synthetic color picture for the fusion host tests.

The thermal tests use a made-up scene (a person, a hot mug, a cold can and a
cold window). This draws a matching room as the HT-HC33's color camera might
see it, places the objects where the default alignment puts the thermal
picture, and writes data/fusion_scene.jpg (what the head streams) and
data/fusion_scene.rgb565 (the same picture decoded, little-endian RGB565).
Run it again only to change the picture. Needs Pillow.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

W, H = 320, 240
SS = 4  # supersampling
THERMAL_FOV, COLOR_FOV = 55, 120  # ThermalCam/config.h defaults
SHARE = THERMAL_FOV / COLOR_FOV
OUT = Path(__file__).resolve().parent / "data"


def at(col, row):
    """Color-picture pixel (supersampled) under a thermal pixel's center.
    The screen mirrors the thermal picture left/right (MIRROR_IMAGE)."""
    u = (31 - col + 0.5) / 32
    v = (row + 0.5) / 24
    return ((0.5 + (u - 0.5) * SHARE) * W * SS, (0.5 + (v - 0.5) * SHARE) * H * SS)


def box(c0, r0, c1, r1):
    (xa, ya), (xb, yb) = at(c0, r0), at(c1, r1)
    return [min(xa, xb), min(ya, yb), max(xa, xb), max(ya, yb)]


def main():
    img = Image.new("RGB", (W * SS, H * SS))
    d = ImageDraw.Draw(img)
    # Wall with soft light from the left, floor at the bottom.
    for y in range(H * SS):
        for x in range(0, W * SS, SS):
            t = x / (W * SS)
            shade = int(205 - 35 * t - 10 * y / (H * SS))
            d.line([(x, y), (x + SS, y)], fill=(shade, shade - 12, shade - 30))
    d.rectangle([0, int(H * SS * 0.80), W * SS, H * SS], fill=(120, 86, 60))
    # Things outside the thermal sensor's view: a door, a plant, a shelf.
    d.rectangle([12 * SS, 40 * SS, 62 * SS, 192 * SS], fill=(150, 110, 70), outline=(90, 60, 35), width=3 * SS)
    d.ellipse([52 * SS, 160 * SS, 58 * SS, 166 * SS], fill=(220, 190, 80))
    d.rectangle([262 * SS, 150 * SS, 290 * SS, 192 * SS], fill=(170, 90, 60))
    for i, (dx, dy) in enumerate([(-14, -30), (0, -40), (14, -28), (-6, -20), (8, -18)]):
        d.ellipse([(276 + dx - 10) * SS, (150 + dy - 8) * SS, (276 + dx + 10) * SS, (150 + dy + 8) * SS],
                  fill=(40 + 10 * i, 120 + 6 * i, 50))
    d.rectangle([240 * SS, 60 * SS, 300 * SS, 66 * SS], fill=(110, 80, 55))
    for i, col in enumerate([(180, 40, 40), (40, 90, 160), (220, 180, 60), (60, 140, 80)]):
        d.rectangle([(246 + 12 * i) * SS, 40 * SS, (254 + 12 * i) * SS, 60 * SS], fill=col)

    # Cold window (thermal: columns >= 25, rows <= 6).
    win = box(31.5, -0.5, 24.5, 6.5)
    d.rectangle(win, fill=(150, 200, 240), outline=(240, 240, 235), width=3 * SS)
    mx, my = (win[0] + win[2]) / 2, (win[1] + win[3]) / 2
    d.line([mx, win[1], mx, win[3]], fill=(240, 240, 235), width=2 * SS)
    d.line([win[0], my, win[2], my], fill=(240, 240, 235), width=2 * SS)
    # Person: head around (11, 8), shoulders and body from row 13 down.
    hx, hy = at(11, 8)
    rx, ry = 3.2 / 32 * SHARE * W * SS, 4.0 / 24 * SHARE * H * SS
    d.rectangle(box(18.5, 12.6, 3.5, 24.5), fill=(45, 85, 150))
    d.ellipse([hx - rx * 0.55, hy + ry * 0.6, hx + rx * 0.55, hy + ry * 1.5], fill=(205, 150, 120))
    d.ellipse([hx - rx, hy - ry, hx + rx, hy + ry], fill=(214, 160, 128))
    d.chord([hx - rx * 1.05, hy - ry * 1.1, hx + rx * 1.05, hy + ry * 0.2], 180, 360, fill=(70, 45, 30))
    for ex in (-0.35, 0.35):
        d.ellipse([hx + ex * rx - 2 * SS, hy - 2 * SS, hx + ex * rx + 2 * SS, hy + 2 * SS], fill=(40, 30, 30))
    # Hot mug (columns 22-26, rows 13-19) with a handle and steam.
    mug = box(26.5, 12.5, 21.5, 19.5)
    d.rounded_rectangle(mug, radius=3 * SS, fill=(235, 235, 230), outline=(160, 160, 160), width=SS)
    d.rectangle([mug[0], mug[1] + (mug[3] - mug[1]) * 0.35, mug[2], mug[1] + (mug[3] - mug[1]) * 0.5], fill=(200, 50, 50))
    hw = (mug[2] - mug[0]) * 0.4
    d.arc([mug[0] - hw, mug[1] + hw * 0.6, mug[0] + hw * 0.5, mug[3] - hw * 0.6], 90, 270, fill=(235, 235, 230),
          width=3 * SS)
    # Cold can (columns 1-3, rows 19-21).
    can = box(3.5, 18.5, 0.5, 21.5)
    d.rounded_rectangle(can, radius=2 * SS, fill=(40, 150, 70), outline=(200, 200, 200), width=SS)
    d.rectangle([can[0], can[1], can[2], can[1] + 2 * SS], fill=(200, 200, 205))

    img = img.filter(ImageFilter.GaussianBlur(SS * 0.4)).resize((W, H), Image.LANCZOS)
    OUT.mkdir(exist_ok=True)
    jpg = OUT / "fusion_scene.jpg"
    img.save(jpg, quality=80, optimize=True)
    decoded = Image.open(jpg).convert("RGB")
    raw = bytearray()
    px = decoded.tobytes()
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        raw += bytes((v & 0xFF, v >> 8))
    (OUT / "fusion_scene.rgb565").write_bytes(bytes(raw))
    print(f"{jpg.name}: {jpg.stat().st_size} bytes, {W}x{H}")


if __name__ == "__main__":
    main()
