"""Make an inspection preview of a one-shot synthetic OpenXR hand-eye readback."""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--opaque", action="store_true", help="Inspect RGB even when game backbuffer alpha is zero")
    args = parser.parse_args()
    pixels = np.fromfile(args.source, dtype=np.uint8)
    expected = args.width * args.height * 4
    if pixels.size != expected:
        raise ValueError(f"{pixels.size} bytes; expected {expected}")
    image = pixels.reshape(args.height, args.width, 4)
    rgb = image[:, :, :3]
    blue = (rgb[:, :, 0] < 100) & (rgb[:, :, 1] > 100) & (rgb[:, :, 2] > 140)
    orange = (rgb[:, :, 0] > 150) & (rgb[:, :, 1] > 65) & (rgb[:, :, 2] < 100)
    for label, mask in (("blue", blue), ("orange", orange)):
        ys, xs = np.where(mask)
        if xs.size:
            print(f"{label}: pixels={xs.size} bbox=[{xs.min()},{ys.min()}..{xs.max()},{ys.max()}]")
        else:
            print(f"{label}: pixels=0")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    preview = Image.fromarray(rgb, "RGB") if args.opaque else Image.fromarray(image, "RGBA")
    preview.resize((args.width // 4, args.height // 4)).save(args.output)
    print(f"preview={args.output}")


if __name__ == "__main__":
    main()
