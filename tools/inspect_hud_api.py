"""Read-only HUD ActionScript string inventory from the user's installed game.

Only names/metadata are reported. No extraction, bytecode execution, game writes,
or live process access. ABC strings are discovery leads, NOT callable API proof.
"""
from pathlib import Path
import re
import struct
import sys
import zlib
from extract_weapon_icons import hash_name

LIMIT = 64 * 1024 * 1024

class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, size):
        if size < 0 or self.at + size > len(self.data):
            raise ValueError('truncated ABC')
        value = self.data[self.at:self.at + size]
        self.at += size
        return value

    def var32(self):
        value = 0
        for shift in range(0, 35, 7):
            byte = self.take(1)[0]
            value |= (byte & 127) << shift
            if not byte & 128:
                if value > 0xffffffff:
                    raise ValueError('oversized ABC integer')
                return value
        raise ValueError('unterminated ABC integer')

    def u30(self):
        value = self.var32()
        if value > 0x3fffffff:
            raise ValueError('oversized U30')
        return value

def abc_strings(data):
    reader = Reader(data)
    minor, major = struct.unpack('<HH', reader.take(4))
    if major not in (46, 47):
        raise ValueError(f'unsupported ABC version {major}.{minor}')
    for _ in range(2):  # signed and unsigned integer pools, same wire encoding
        count = reader.u30()
        if count > len(data):
            raise ValueError('oversized integer pool')
        for _ in range(max(0, count - 1)):
            reader.var32()
    reader.take(max(0, reader.u30() - 1) * 8)  # double pool
    count = reader.u30()
    if count > len(data):
        raise ValueError('oversized string pool')
    return [reader.take(reader.u30()).decode('utf-8', errors='replace')
            for _ in range(max(0, count - 1))]

def gfx_abc_strings(raw):
    if len(raw) < 9:
        raise ValueError('truncated GFx')
    if raw[:3] in (b'CFX', b'CWS'):
        decoder = zlib.decompressobj()
        body = decoder.decompress(raw[8:], LIMIT + 1)
        if len(body) > LIMIT or not decoder.eof:
            raise ValueError('GFx decompression limit/end')
    elif raw[:3] in (b'GFX', b'FWS'):
        body = raw[8:]
    else:
        raise ValueError('unsupported GFx signature')
    at = ((body[0] >> 3) * 4 + 5 + 7) // 8 + 4
    while at + 2 <= len(body):
        head = struct.unpack_from('<H', body, at)[0]
        at += 2
        tag, size = head >> 6, head & 63
        if size == 63:
            size = struct.unpack_from('<I', body, at)[0]
            at += 4
        if at + size > len(body):
            raise ValueError('GFx tag range')
        if tag == 82:
            payload = body[at:at + size]
            name_end = payload.index(b'\0', 4)
            yield from abc_strings(payload[name_end + 1:])
        at += size
        if tag == 0:
            break

def main():
    root = Path(sys.argv[1])
    # Exact patched archive previously fingerprinted in HUD_ASSET_MAP.md.
    tab = root / 'patch_win64/game6.tab'
    index = tab.read_bytes()
    if index[:8] != b'TAB\0\x02\0\x01\0':
        raise ValueError('unexpected TAB header')
    wanted = {hash_name(f'ui/{name}.gfx'): name for name in
              ('root', 'hud', 'shared_lib', 'overlay')}
    found = set()
    with tab.with_suffix('.arc').open('rb') as archive:
        for at in range(12, len(index) - 11, 12):
            key, offset, size = struct.unpack_from('<III', index, at)
            if key not in wanted:
                continue
            if size > LIMIT:
                raise ValueError('archive entry too large')
            archive.seek(offset)
            raw = archive.read(size)
            if len(raw) != size:
                raise ValueError('truncated archive entry')
            name = wanted[key]
            found.add(name)
            strings = sorted(set(gfx_abc_strings(raw)))
            print(f'MOVIE {name}: {len(strings)} unique ABC strings')
            for value in strings:
                if re.search(r'ammo|health|life.?bar|weapon|reticle|tether|nitro|heat|subtit|objective|mission|safe_area|poi|pause|loading', value, re.I):
                    print(name, repr(value))
    if len(found) != len(wanted):
        raise ValueError('missing expected patched movies')

if __name__ == '__main__':
    main()
