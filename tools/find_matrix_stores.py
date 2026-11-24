"""Find code that stores 4x4 matrix rows at given struct offsets (offline).

Looks for SSE stores (movups/movaps/movdqu, [VEX] forms included loosely) with
a disp32 memory operand, and reports 0x300-byte windows that store every row
of every requested matrix offset. Usage:
    python tools/find_matrix_stores.py <exe> 0x94 0xd4 [...]
"""
import struct
import sys
from collections import defaultdict


def main():
    data = open(sys.argv[1], "rb").read()
    bases = [int(value, 16) for value in sys.argv[2:]]
    wanted = {base + row * 16: base for base in bases for row in range(4)}
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional_size
    for index in range(count):
        entry = table + index * 40
        if data[entry:entry + 8].rstrip(b"\0") != b".text":
            continue
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, entry + 8)
        text = data[rawptr:rawptr + rawsize]
        hits = defaultdict(set)
        # 0F 11 / 0F 29 / 0F 7F (+66/F3 prefixes) and VEX C5 xx 11/29/7F.
        for position in range(len(text) - 8):
            if text[position] == 0x0F and text[position + 1] in (0x11, 0x29, 0x7F):
                modrm_at = position + 2
            elif text[position] == 0xC5 and text[position + 2] in (0x11, 0x29, 0x7F):
                modrm_at = position + 3
            else:
                continue
            modrm = text[modrm_at]
            if modrm & 0xC0 != 0x80:
                continue
            disp_at = modrm_at + 1 + (1 if modrm & 7 == 4 else 0)
            disp = struct.unpack_from("<i", text, disp_at)[0]
            if disp in wanted:
                hits[(va + position) // 0x300].add(disp)
        needed = set(wanted)
        for window in sorted(hits):
            stored = hits[window] | hits.get(window + 1, set())
            if needed <= stored:
                print(f"window 0x{window * 0x300:08x}-0x{(window + 2) * 0x300:08x}: stores {sorted(hex(d) for d in stored)}")


if __name__ == "__main__":
    main()
