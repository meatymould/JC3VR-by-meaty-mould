"""Find RIP-relative instructions in JustCause3.exe .text that reference an RVA.

Offline, file-only. Recognizes the common MSVC forms with a disp32 whose
target (next-instruction RVA + disp) equals the requested RVA:
  [REX] 8B/89/8D/3B/39/C7/83/FF/0F-B6 ... modrm with mod=00, rm=101.
Usage: python tools/find_rip_refs.py <exe> <target_rva_hex> [more ...]
Prints: instruction RVA, raw bytes, target.
"""
import struct
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    targets = {int(value, 16) for value in sys.argv[2:]}
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional_size
    for index in range(count):
        entry = table + index * 40
        name = data[entry:entry + 8].rstrip(b"\0")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, entry + 8)
        if name != b".text":
            continue
        text = data[rawptr:rawptr + rawsize]
        # Scan for modrm (mod=00, rm=101) followed by disp32 at every position;
        # the opcode length before modrm is 1 or 2 (plus optional REX), and an
        # immediate may follow. Accept a hit when the target matches for any
        # plausible trailing-immediate size (0, 1, 4).
        for position in range(1, len(text) - 5):
            modrm = text[position]
            if modrm & 0xC7 != 0x05:
                continue
            disp = struct.unpack_from("<i", text, position + 1)[0]
            for immediate in (0, 1, 4):
                next_rva = va + position + 5 + immediate
                if next_rva + disp in targets:
                    start = max(0, position - 3)
                    raw = text[start:position + 5 + immediate].hex()
                    print(f"0x{va + start:08x} {raw} -> 0x{next_rva + disp:08x} (imm={immediate})")


if __name__ == "__main__":
    main()
