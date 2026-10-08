#!/usr/bin/env python3
"""Generate assets/icon0.png - the Ghost Tooth dashboard tile icon.

Pure stdlib (no Pillow): the icon has to be reproducible from the repository
alone, and CI must not need an image library.  The drawing is analytic - every
pixel is evaluated against signed distances, which gives smooth edges without
supersampling.

  python3 tools/gt-icon.py [out.png]

The same file is used twice: the Media-tab tile reads it from
/user/app/<TITLE>/sce_sys/icon0.png, and the web UI serves it as its favicon.
"""

import math
import struct
import sys
import zlib

SIZE = 512


def lerp(a, b, t):
    return a + (b - a) * t


def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def smooth(d, edge=1.4):
    """Coverage of a signed distance field: 1 inside, 0 outside."""
    return clamp01(0.5 - d / (2.0 * edge))


def round_rect_sd(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - (hw - r)
    qy = abs(py - cy) - (hh - r)
    ax = max(qx, 0.0)
    ay = max(qy, 0.0)
    outside = math.hypot(ax, ay)
    inside = min(max(qx, qy), 0.0)
    return outside + inside - r


def seg_sd(px, py, x1, y1, x2, y2):
    vx, vy = x2 - x1, y2 - y1
    wx, wy = px - x1, py - y1
    length2 = vx * vx + vy * vy
    t = clamp01((wx * vx + wy * vy) / length2) if length2 else 0.0
    return math.hypot(wx - vx * t, wy - vy * t)


def blend(dst, src, alpha):
    return (
        lerp(dst[0], src[0], alpha),
        lerp(dst[1], src[1], alpha),
        lerp(dst[2], src[2], alpha),
    )


def draw():
    cx, cy = SIZE / 2.0, SIZE * 0.505
    band_r, band_t = 168.0, 34.0
    cup_hw, cup_hh, cup_r = 40.0, 66.0, 20.0
    cup_dx = 150.0
    cup_dy = 66.0

    # The Bluetooth mark: a spine plus two triangles whose far vertices sit on
    # the spine.  Proportions follow the official 32x64 construction.
    top, bot = cy - 104.0, cy + 104.0
    x_r = cx + 60.0
    upper, lower = cy - 39.0, cy + 39.0
    rune = [
        (cx, top, cx, bot),
        (cx, top, x_r, upper),
        (x_r, upper, cx, lower),
        (cx, bot, x_r, lower),
        (x_r, lower, cx, upper),
    ]
    rune_w = 15.0

    rows = []
    for y in range(SIZE):
        row = bytearray()
        fy = y + 0.5
        for x in range(SIZE):
            fx = x + 0.5

            # --- background: deep PS5 blue, slightly brighter at the top
            ty = y / (SIZE - 1.0)
            base = (
                int(lerp(14, 6, ty)),
                int(lerp(34, 16, ty)),
                int(lerp(74, 44, ty)),
            )
            # soft radial glow behind the artwork
            gr = math.hypot(fx - cx, fy - cy) / (SIZE * 0.62)
            glow = smooth(1.0 - gr, 0.55) * 0.55
            col = blend(base, (44, 116, 210), glow)

            # --- headband: an annulus, cut off at the ear line
            dr = math.hypot(fx - cx, fy - cy)
            band = abs(dr - band_r) - band_t / 2.0
            if fy > cy + cup_dy:
                band = 1e3
            band_a = smooth(band)

            # --- ear cups
            cups = min(
                round_rect_sd(fx, fy, cx - cup_dx, cy + cup_dy, cup_hw, cup_hh, cup_r),
                round_rect_sd(fx, fy, cx + cup_dx, cy + cup_dy, cup_hw, cup_hh, cup_r),
            )
            cups_a = smooth(cups)

            shell_a = max(band_a, cups_a)
            if shell_a > 0.0:
                # pale shell with a cool inner shade
                shade = 1.0 - clamp01((shell_a - 0.5) * 2.0)
                shell = (
                    int(lerp(206, 255, shade)),
                    int(lerp(224, 250, shade)),
                    int(lerp(255, 255, shade)),
                )
                col = blend(col, shell, shell_a)

            # --- bluetooth rune punched through the shell in cyan
            rune_sd = SIZE
            for (x1, y1, x2, y2) in rune:
                rune_sd = min(rune_sd, seg_sd(fx, fy, x1, y1, x2, y2) - rune_w / 2.0)
            rune_a = smooth(rune_sd)
            if rune_a > 0.0:
                col = blend(col, (55, 198, 255), rune_a * 0.95)

            row += bytes((max(0, min(255, int(col[0]))),
                          max(0, min(255, int(col[1]))),
                          max(0, min(255, int(col[2]))),
                          255))
        rows.append(bytes(row))
    return rows


def write_png(path, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print("wrote %s (%dx%d, %d bytes)" % (path, SIZE, SIZE, len(png)))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/icon0.png"
    write_png(out, draw())
