"""Inspect one-shot raw native-eye GPU captures without altering the source pair."""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


def load(path: Path, width: int, height: int) -> np.ndarray:
    data = np.fromfile(path, dtype=np.uint8)
    expected = width * height * 4
    if data.size != expected:
        raise ValueError(f"{path}: {data.size} bytes, expected {expected}")
    return data.reshape(height, width, 4)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("stem", type=Path)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--save-preview", action="store_true")
    parser.add_argument("--preview-directory", type=Path, default=Path("build"))
    args = parser.parse_args()
    left = load(Path(str(args.stem) + "_left.rgba"), args.width, args.height)
    right = load(Path(str(args.stem) + "_right.rgba"), args.width, args.height)
    print(f"pixel_exact_fraction={np.mean(np.all(left == right, axis=2)):.6f}")
    print(f"mean_absolute_channel_delta={np.mean(np.abs(left.astype(np.int16) - right.astype(np.int16))):.3f}")
    for name, image in (("left", left), ("right", right)):
        print(f"{name}: mean_rgb={np.mean(image[:, :, :3], axis=(0, 1)).round(1).tolist()} "
              f"std_rgb={np.std(image[:, :, :3], axis=(0, 1)).round(1).tolist()}")
        if args.save_preview:
            args.preview_directory.mkdir(parents=True, exist_ok=True)
            Image.fromarray(image, "RGBA").save(args.preview_directory / f"{args.stem.name}_{name}.png")

    # Derivative correlation suppresses common exposure/colour changes. Probe
    # upper scene tiles separately so a static menu overlay cannot dominate.
    for y0, y1 in ((100, 350), (350, 650), (650, 900)):
        for x0, x1 in ((100, 700), (700, 1300), (1300, 1820)):
            a = left[y0:y1:3, x0:x1:3, :3].astype(np.float32).mean(axis=2)
            b = right[y0:y1:3, x0:x1:3, :3].astype(np.float32).mean(axis=2)
            a = np.diff(a, axis=1)
            b = np.diff(b, axis=1)
            best = (-2.0, 0)
            for shift in range(-100, 101):
                if shift < 0:
                    aa, bb = a[:, -shift:], b[:, :shift]
                elif shift > 0:
                    aa, bb = a[:, :-shift], b[:, shift:]
                else:
                    aa, bb = a, b
                if aa.size < 100:
                    continue
                aa, bb = aa - aa.mean(), bb - bb.mean()
                denom = np.linalg.norm(aa) * np.linalg.norm(bb)
                score = float(np.sum(aa * bb) / denom) if denom else 0.0
                if score > best[0]:
                    best = score, shift
            print(f"tile x={x0}:{x1} y={y0}:{y1} best_shift_px={best[1] * 3} correlation={best[0]:.3f}")


if __name__ == "__main__":
    main()
