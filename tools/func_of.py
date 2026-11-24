"""Enclosing function (from the exe's .pdata unwind table) for each RVA.

usage: func_of.py <JustCause3.exe> <rva> [rva ...]
Prints begin/end RVAs and the first bytes of each enclosing function. Reads
the PE file only.
"""
import struct
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = pe + 24
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for i in range(count):
        o = optional + optional_size + i * 40
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        sections.append((va, vsize, raw))

    def file_offset(rva):
        for va, vsize, raw in sections:
            if va <= rva < va + vsize:
                return raw + rva - va
        return None

    exception_rva, exception_size = struct.unpack_from("<II", data, optional + 112 + 3 * 8)
    base = file_offset(exception_rva)
    entries = [struct.unpack_from("<III", data, base + i * 12) for i in range(exception_size // 12)]
    for arg in sys.argv[2:]:
        rva = int(arg, 16)
        hits = [e for e in entries if e[0] <= rva < e[1]]
        for begin, end, unwind in hits:
            # Chained unwind info points at the primary function entry.
            info = file_offset(unwind & ~1)
            flags = data[info] >> 3
            primary = begin
            if flags & 4:  # UNW_FLAG_CHAININFO
                codes = data[info + 2]
                chain = info + 4 + ((codes + 1) & ~1) * 2
                primary = struct.unpack_from("<I", data, chain)[0]
            print(f"{rva:#x}: function {begin:#x}-{end:#x} (primary {primary:#x}) bytes {data[file_offset(primary):file_offset(primary) + 16].hex()}")
        if not hits:
            print(f"{rva:#x}: no .pdata entry (leaf)")


main()
