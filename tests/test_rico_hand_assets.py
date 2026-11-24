import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from prepare_rico_hands import splice_model


def model():
    prefix = b"\x05\0\0\0RBMDL" + struct.pack("<3I6f2I", 1, 16, 0, *([0] * 6), 1, 7)
    block = struct.pack("<IB", 0x9D6E332A, 9) + bytes(108)
    materials = struct.pack("<I", 11) + bytes(11 * 4)
    joints = ((1, 0), (1, 0), (1, 0), (2, 0), (2, 0), (2, 0), (1, 0), (2, 0), (1, 1))
    vertices = b""
    for joint, cross_weight in joints:
        vertices += bytes(8) + bytes([255 - cross_weight, cross_weight, 0, 0, joint, 0, 0, 0]) + bytes(8)
    indices = tuple(range(9))
    batch = struct.pack("<IIII3H", 1, len(indices), 0, 3, 24, 25, 65)
    faces = struct.pack("<I9HI", len(indices), *indices, 0x89ABCDEF)
    return prefix + block + materials + bytes(16) + struct.pack("<I", len(joints)) + vertices + batch + faces


class RicoHandAssetsTest(unittest.TestCase):
    def test_same_length_and_only_excluded_triangle_becomes_degenerate(self):
        source = model()
        variant, counts = splice_model(source)
        self.assertEqual(counts, [1, 1])
        self.assertEqual(variant[:-8], source[:-8])
        self.assertEqual(variant[-8:], struct.pack("<HHI", 6, 6, 0x89ABCDEF))

    def test_truncated_asset_is_rejected(self):
        with self.assertRaises(ValueError):
            splice_model(model()[:-1])

    def test_skin_palette_index_must_be_in_range(self):
        source = bytearray(model())
        first_vertex = 53 + 113 + 4 + 11 * 4 + 20
        source[first_vertex + 12] = 3
        with self.assertRaisesRegex(ValueError, "skin weights"):
            splice_model(source)


if __name__ == "__main__":
    unittest.main()
