#!/usr/bin/env python3
"""tools/gt-check-icon.py - verify a PS5 tile icon without any image library.

The shell wants a 512x512 PNG for sce_sys/icon0.png and will show a blank tile
for anything else, which is an annoying bug to find on a television.  CI checks
it here instead: header, IHDR, and that the IDAT stream decompresses to the
expected number of scanlines.

  python3 tools/gt-check-icon.py assets/icon0.png

Copyright (C) 2026 Ghost-tooth-ui contributors
SPDX-License-Identifier: GPL-3.0-or-later
"""

import struct
import sys
import zlib

MAGIC = b"\x89PNG\r\n\x1a\n"
WANT = (512, 512)


def main(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:8] != MAGIC:
        return "not a PNG"
    pos = 8
    chunks = []
    ihdr = None
    idat = b""
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if pos + 12 + length > len(data):
            return "truncated chunk %r" % kind
        stored = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])[0]
        if zlib.crc32(kind + body) & 0xFFFFFFFF != stored:
            return "bad CRC in chunk %r" % kind
        chunks.append(kind.decode("ascii", "replace"))
        if kind == b"IHDR":
            ihdr = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length

    if ihdr is None:
        return "no IHDR"
    width, height = struct.unpack(">II", ihdr[:8])
    depth, color, compose, filt, interlace = struct.unpack(">5B", ihdr[8:13])
    if compose != 0 or filt != 0:
        return "unknown compression/filter method (%d/%d)" % (compose, filt)
    if len(ihdr) != 13:
        return "IHDR is %d bytes, not 13" % len(ihdr)
    if (width, height) != WANT:
        return "%dx%d, the PS5 shell wants %dx%d" % (width, height, *WANT)
    if depth != 8:
        return "bit depth %d, expected 8" % depth
    if color not in (2, 6):
        return "colour type %d, expected truecolour (2) or RGBA (6)" % color
    if interlace:
        return "interlaced PNGs are not accepted by the shell"
    channels = 4 if color == 6 else 3
    stride = width * channels + 1
    raw = zlib.decompress(idat)
    if len(raw) != stride * height:
        return "image data is %d bytes, expected %d" % (len(raw), stride * height)
    print("%s: %dx%d, %s, %d chunks, %.1f KiB - ok"
          % (path, width, height, "RGBA" if channels == 4 else "RGB",
             len(chunks), len(data) / 1024.0))
    return None


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: gt-check-icon.py <icon.png>")
        sys.exit(2)
    problem = main(sys.argv[1])
    if problem:
        print("FAIL " + problem)
        sys.exit(1)
