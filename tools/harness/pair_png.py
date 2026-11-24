"""Stdlib-only PNG views of a retained eye pair.

usage: pair_png.py <stem> <tag> [step] [x0 y0 w h]
Writes <tag>.png: left | right side by side, sampled every `step` pixels
over the region (default: whole 2048x2208 frame, step 4).
"""
import os
import struct
import sys
import zlib

stem, tag = sys.argv[1], sys.argv[2]
W, H = 2048, 2208
try:  # the capture's .txt records its size
    meta = dict(line.strip().split("=", 1) for line in open(stem + ".txt") if "=" in line)
    W, H = int(meta["width"]), int(meta["height"])
except (OSError, KeyError, ValueError):
    pass
step = int(sys.argv[3]) if len(sys.argv) > 3 else 4
x0, y0, w, h = (int(v) for v in sys.argv[4:8]) if len(sys.argv) >= 8 else (0, 0, W, H)
out = os.environ.get("JC3VR_HARNESS_OUT") or os.path.join(os.environ.get("TEMP", "."), "jc3vr-harness")
os.makedirs(out, exist_ok=True)

def sample(eye):
    data = open(stem + f"_{eye}.rgba", "rb").read()
    rows = []
    for y in range(y0, y0 + h, step):
        row = bytearray()
        base = y * W * 4
        for x in range(x0, x0 + w, step):
            i = base + x * 4
            row += data[i:i + 3]
        rows.append(bytes(row))
    return rows

left, right = sample("left"), sample("right")
gap = b"\xff\x00\xff" * 4
raw = b"".join(b"\x00" + l + gap + r for l, r in zip(left, right))
width = len(left[0]) // 3 * 2 + 4

def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

path = os.path.join(out, tag + ".png")
open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, len(left), 8, 2, 0, 0, 0)) +
                       chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
print(path)
