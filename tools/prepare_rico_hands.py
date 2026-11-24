"""Generate local-only hands RBMs from the fingerprinted retail patch archive."""
import argparse
import hashlib
from pathlib import Path
import re
import struct

MODEL_PATH = Path("models/jc_characters/main_characters/rico")
ORIGINALS = (
    (354099200, 2011645, "cec0de17da192f944c0519413842440f8c8a42e9e8e437524009ef5df2cb001c"),
    (356112384, 1096023, "f607c0ab7596c0426bce8409c81e95b77d3b424dc0f7dbc1d99ef93dd8d28c65"),
    (357210112, 221490, "104c5c6aa80829138c971acc1097b0cc41ec3172331eecae8c0b66436f33119d"),
)


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def hand_side(bones, position):
    for side, wrist in enumerate(((-0.447, 1.102, -0.134), (0.447, 1.102, -0.134)), 1):
        base = 25 if side == 1 else 65
        cuff = {base - 1, base + 20, base + 21, base + 22}
        if not bones or not all(base <= bone < base + 19 or bone in cuff for bone in bones):
            continue
        if cuff.intersection(bones) and sum((a - b) ** 2 for a, b in zip(position, wrist)) > 0.14 ** 2:
            continue
        return side
    return 0


def splice_model(source):
    # Keep the original vertex buffers, batch ranges, materials, LOD bounds
    # and index-buffer lengths. Excluded triangles become zero-area.
    if source[:21] != b"\x05\0\0\0RBMDL\x01\0\0\0\x10\0\0\0\0\0\0\0":
        raise ValueError("unsupported RBMDL header")
    footer = [m.start() for m in re.finditer(b"\xef\xcd\xab\x89", source)]
    if not footer or len(footer) != u32(source, 45) or footer[-1] + 4 != len(source):
        raise ValueError("unexpected RBMDL blocks")
    result = bytearray(source)
    kept = [0, 0, 0]
    for start, end in zip([53] + [p + 4 for p in footer], footer):
        kind = u32(source, start)
        scale = struct.unpack_from("<f", source, start + 9)[0]
        if (kind, source[start + 4]) == (0x9D6E332A, 9):
            at, expected_materials = start + 113, 11
        elif (kind, source[start + 4]) == (0x626F5E3B, 6):
            at, expected_materials = start + 61, 9
        else:
            raise ValueError("unsupported character block")
        if u32(source, at) != expected_materials:
            raise ValueError("unexpected material count")
        at += 4
        for _ in range(expected_materials):
            length = u32(source, at)
            if length > 256 or at + 4 + length > end:
                raise ValueError("invalid material string")
            at += 4 + length
        vertex_count = u32(source, at + 16)
        vertices = at + 20
        at = vertices + 24 * vertex_count
        if not 0 < vertex_count <= 65535 or at + 16 > end or u32(source, at) != 1:
            raise ValueError("unexpected vertex stream or skin-batch count")
        count, first, bone_count = struct.unpack_from("<III", source, at + 4)
        if first != 0 or not 0 < bone_count <= 177:
            raise ValueError("unexpected skin-batch range")
        bones = struct.unpack_from(f"<{bone_count}H", source, at + 16)
        at += 16 + 2 * bone_count
        if count % 3 or at + 4 + 2 * count != end or u32(source, at) != count:
            raise ValueError("invalid index layout")
        indices = struct.unpack_from(f"<{count}H", source, at + 4)
        if not indices or max(indices) >= vertex_count or max(bones) >= 177:
            raise ValueError("index out of range")
        sides = []
        for index in range(vertex_count):
            pos = vertices + 24 * index
            weights = source[pos + 8:pos + 12]
            joints = source[pos + 12:pos + 16]
            if not 248 <= sum(weights) <= 257 or any(w and j >= bone_count for w, j in zip(weights, joints)):
                raise ValueError("invalid packed skin weights")
            position = [x * scale / 32767 for x in struct.unpack_from("<3h", source, pos)]
            sides.append(hand_side([bones[j] for w, j in zip(weights, joints) if w], position))
        for i in range(0, count, 3):
            a, b, c = indices[i:i + 3]
            side = sides[a]
            if side and side == sides[b] == sides[c] and len({a, b, c}) == 3:
                kept[side] += 1
            else:
                struct.pack_into("<HH", result, at + 6 + i * 2, a, a)
    if not kept[1] or not kept[2]:
        raise ValueError("both hands must have actual triangles")
    return bytes(result), kept[1:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("output", type=Path, help="fresh staging directory outside the game")
    args = parser.parse_args()
    if args.output.resolve().is_relative_to(args.game.resolve()):
        parser.error("output must be outside the game")
    models = []
    with (args.game / "patch_win64/game4.arc").open("rb") as archive:
        for lod, (offset, size, expected) in enumerate(ORIGINALS, 1):
            archive.seek(offset)
            source = archive.read(size)
            if hashlib.sha256(source).hexdigest() != expected:
                raise ValueError(f"Rico LOD{lod} archive fingerprint mismatch")
            variant, triangles = splice_model(source)
            models.append(variant)
            print(f"LOD{lod}: left/right {triangles}, SHA256 {hashlib.sha256(variant).hexdigest()}")
    args.output.mkdir(exist_ok=False, parents=True)
    model_dir = args.output / "dropzone" / MODEL_PATH
    model_dir.mkdir(parents=True)
    for lod, model in enumerate(models, 1):
        (model_dir / f"rico_body_lod{lod}.rbm").write_bytes(model)


if __name__ == "__main__":
    main()
