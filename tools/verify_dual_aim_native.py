"""Read-only, standard-library proof checks for the pinned Steam executable.

No game launch, memory attachment, native hook installation or writes. This
verifies exact local evidence, not whether the VR feature hits enemies.
"""
import hashlib
import struct
import sys
from pathlib import Path

EXPECTED = 'a53c2d6b2837f45d97244fc096e68f27ba9036e0582a0d9aada2e2f222dac293'


def verify(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED:
        raise ValueError('unsupported executable fingerprint')
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    count = struct.unpack_from('<H', data, pe+6)[0]
    table = pe+24+struct.unpack_from('<H', data, pe+20)[0]
    sections = [struct.unpack_from('<IIII', data, table+i*40+8) for i in range(count)]

    def read(rva, size):
        for virtual_size, start, raw_size, raw in sections:
            if start <= rva and rva+size <= start+raw_size:
                return data[raw+rva-start:raw+rva-start+size]
        raise ValueError(f'unmapped RVA {rva:x}')

    checks = {
        0x966940: '40534881ecb000000083b9240400000a',
        0x9A419A: 'e861e9feff',
        0x985AE4: '8b83fc030000',
        0x985AFF: '8b8304040000',
        0x985C22: '8b8b6c01000083f9ff',
        0x985C33: '83f901',
        0x92760B: 'e890e40500',
        0x992A18: 'e88330ffff',
        0x9A3FD4: '8b068983fc030000',
        # Base constructor produces r12d=-1, then stores it in force count.
        0x976F85: '4183ccff',
        0x976FAA: '4489a780010000',
        # Native forced-fire branch tests signed >0; projectile tail only
        # decrements a positive count, so the idle -1 sentinel survives shots.
        0x992D88: '4439b7800100007f05',
        0x9923D8: '8b878001000085c07e08',
        # Native block timer: if positive, subtract dt and store without clamp.
        0x992EE6: 'f30f1087740100000f2fc77615',
        0x992EFB: 'f3410f5cc1f30f118774010000',
        # Non-vehicle component world getter copies the SAME +198 matrix
        # that the already-working controller model placement writes.
        0x960390: '488d919801000041b840000000',
    }
    for rva, encoded in checks.items():
        expected = bytes.fromhex(encoded)
        if read(rva, len(expected)) != expected:
            raise ValueError(f'instruction proof mismatch at {rva:x}')
    base = struct.unpack_from('<Q', data, pe+24+24)[0]
    if struct.unpack('<Q', read(0x23E3038+0xC8, 8))[0] != base+0x966940:
        raise ValueError('bullet muzzle getter vtable mismatch')
    for slot, expected in ((0xD8,0x965820),(0xE0,0x927550)):
        actual=struct.unpack('<Q',read(0x23E3038+slot,8))[0]-base
        if actual != expected:
            raise ValueError(f'bullet dispatch vtable+{slot:x}: got {actual:x}, expected {expected:x}')
    print('PASS: pinned PE; selected native muzzle getter; shared aim field; mode enum;')
    print('      exact per-gun caller; native aim-cone and projectile shot-builder calls.')
    print('      native forced-shot -1 sentinel, positive-only activation/decrement.')
    print('      unclamped block-timer expiry; component-world matrix source.')
    return data


if __name__ == '__main__':
    verify(sys.argv[1])
