"""List MSVC x64 RTTI classes (name -> vtable RVA) in JustCause3.exe, offline.

Reads the PE file only; no process access. Usage:
    python tools/rtti_classes.py <JustCause3.exe> [name-substring ...]
Each output line: vtable RVA, complete-object-locator offset, class name.
"""
import re
import struct
import sys


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    table = pe + 24 + optional_size
    result = []
    for index in range(count):
        entry = table + index * 40
        name = data[entry:entry + 8].rstrip(b"\0").decode(errors="replace")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, entry + 8)
        result.append((name, va, max(vsize, rawsize), rawptr, rawsize))
    return image_base, result


def main():
    path = sys.argv[1]
    filters = [f.lower() for f in sys.argv[2:]]
    data = open(path, "rb").read()
    image_base, secs = sections(data)

    def rva_to_offset(rva):
        for _, va, size, rawptr, rawsize in secs:
            if va <= rva < va + rawsize:
                return rawptr + rva - va
        return None

    def offset_to_rva(offset):
        for _, va, size, rawptr, rawsize in secs:
            if rawptr <= offset < rawptr + rawsize:
                return va + offset - rawptr
        return None

    # TypeDescriptor: pVFTable(8) spare(8) name(".?AV...@@")
    descriptors = {}
    for match in re.finditer(rb"\.\?A[VU][ -~]{1,200}?@@\0", data):
        name_offset = match.start()
        rva = offset_to_rva(name_offset - 16)
        if rva is not None:
            descriptors[rva] = match.group(0)[4:-3].decode(errors="replace")
    wanted = {rva: name for rva, name in descriptors.items()
              if not filters or any(f in name.lower() for f in filters)}

    # CompleteObjectLocator (x64): sig=1, offset, cdOffset, pTypeDescriptor,
    # pClassDescriptor, pSelf (all RVAs). Find each and its vtable reference.
    rdata = [s for s in secs if s[0] in (".rdata", ".data")]
    locators = {}
    for _, va, size, rawptr, rawsize in rdata:
        blob = data[rawptr:rawptr + rawsize]
        for position in range(0, len(blob) - 24, 4):
            signature, offset, cd_offset, td = struct.unpack_from("<IIII", blob, position)
            if signature != 1 or td not in wanted:
                continue
            self_rva = struct.unpack_from("<I", blob, position + 20)[0]
            col_rva = va + position
            if self_rva == col_rva:
                locators[col_rva] = (wanted[td], offset)
    # A vtable is preceded by a pointer to its COL.
    targets = {image_base + rva: rva for rva in locators}
    results = []
    for _, va, size, rawptr, rawsize in rdata:
        blob = data[rawptr:rawptr + rawsize]
        for position in range(0, len(blob) - 8, 8):
            value = struct.unpack_from("<Q", blob, position)[0]
            if value in targets:
                col = targets[value]
                name, offset = locators[col]
                results.append((va + position + 8, offset, name))
    for vtable, offset, name in sorted(results, key=lambda item: item[2]):
        print(f"0x{vtable:08x} off=0x{offset:x} {name}")
    print(f"# {len(descriptors)} type descriptors, {len(results)} vtables listed", file=sys.stderr)


if __name__ == "__main__":
    main()
