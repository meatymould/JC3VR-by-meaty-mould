import struct
import unittest
from inspect_hud_api import Reader, abc_strings, gfx_abc_strings

class HudApiTests(unittest.TestCase):
    def test_strings_and_zero_pools(self):
        abc = struct.pack('<HH', 16, 46) + b'\0\0\0\x03\x04ammo\x06health'
        self.assertEqual(abc_strings(abc), ['ammo', 'health'])

    def test_integer_bounds(self):
        self.assertEqual(Reader(b'\xff\xff\xff\xff\x0f').var32(), 0xffffffff)
        for raw in (b'\x80', b'\xff\xff\xff\xff\x7f'):
            with self.assertRaises(ValueError):
                Reader(raw).var32()
        with self.assertRaises(ValueError):
            Reader(b'\xff\xff\xff\xff\x0f').u30()

    def test_gfx_tag_roundtrip_and_truncation(self):
        abc = struct.pack('<HH', 16, 46) + b'\0\0\0\x02\x04ammo'
        payload = b'\0\0\0\0\0' + abc
        body = b'\x08\0\0\0\0\0' + struct.pack('<H', 82 * 64 + len(payload)) + payload + b'\0\0'
        raw = b'GFX\x0a' + struct.pack('<I', len(body) + 8) + body
        self.assertEqual(list(gfx_abc_strings(raw)), ['ammo'])
        with self.assertRaises(ValueError):
            list(gfx_abc_strings(raw[:-5]))

if __name__ == '__main__':
    unittest.main()
