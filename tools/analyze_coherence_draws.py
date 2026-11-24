"""Summarize JC3VR 'coherence draw sample' trace lines (offline).

For each sampled gameplay draw, reports the view-forward direction of the
Globals block's main VP (c0-c3), second VP (c33-c36, built from the paired
owner+0x5C0 camera) and the frustum-plane axis (c40-c43), plus the angles
between them. With a diagnostic head yaw applied, a slot that still follows
the engine camera shows that yaw as its angle to c0-c3.
Usage: python tools/analyze_coherence_draws.py <bootstrap_worker.log> [since HH:MM:SS]
"""
import math
import re
import sys


def normalize(v):
    length = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / length for c in v]


def angle(a, b):
    a, b = normalize(a), normalize(b)
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))))


def main():
    since = sys.argv[2] if len(sys.argv) > 2 else "00:00:00"
    for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
        if "coherence draw sample" not in line or line[1:9] < since:
            continue
        head, _, block = line.partition("gpu_block=")
        if "readback-failed" in block:
            continue
        values = {int(k): float(v) for k, v in (item.split("=") for item in block.strip().split(","))}
        # Row-vector VP: the clip-w column (elements 3, 7, 11) is view-forward.
        main_forward = [values[3], values[7], values[11]]
        second_forward = [values[132 + 3], values[132 + 7], values[132 + 11]]
        planes = [[values[160 + 4 * p + i] for i in range(3)] for p in range(4)]
        plane_axis = [sum(plane[i] for plane in planes) for i in range(3)]
        eye = re.search(r"eye_label=(\d)", head).group(1)
        number = re.search(r"sample (\d+)", head).group(1)
        print(f"{line[1:13]} #{number} eye={eye} main_fwd={[round(c, 3) for c in normalize(main_forward)]} "
              f"main-vs-second={angle(main_forward, second_forward):5.1f}deg "
              f"main-vs-planes={min(angle(main_forward, plane_axis), angle(main_forward, [-c for c in plane_axis])):5.1f}deg")


if __name__ == "__main__":
    main()
