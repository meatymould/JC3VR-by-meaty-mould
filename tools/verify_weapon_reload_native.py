"""Read-only static proofs for future reload work; NOT approval to call/hook.

Reads the user's pinned legitimate executable only. No launch/process access,
DLL changes, native execution or ammo edits. No game code is redistributed.
"""
import struct
import sys
from verify_dual_aim_native import verify


def main(path):
    data=verify(path)
    pe=struct.unpack_from('<I',data,0x3c)[0]
    optional=pe+24
    table=optional+struct.unpack_from('<H',data,pe+20)[0]
    sections=[struct.unpack_from('<IIII',data,table+i*40+8)
              for i in range(struct.unpack_from('<H',data,pe+6)[0])]
    base=struct.unpack_from('<Q',data,optional+24)[0]

    def read(rva,n):
        for _,start,size,raw in sections:
            if start<=rva and rva+n<=start+size:
                return data[raw+rva-start:raw+rva-start+n]
        raise ValueError(f'unmapped RVA {rva:x}')

    sites={
        0x966244:'8b82b0020000',       # component burst/pending shot counter
        0x9662B2:'8b8128020000',       # shared magazine count
        0x9662CA:'41ff9038010000',     # consume calls magazine setter
        0x96E536:'899128020000',       # native setter writes magazine
        0x976525:'448ba328020000',     # refill reads current magazine
        0x976581:'83bb3002000001',     # separate one-round branch
        0x9765A2:'ff9038010000',       # refill calls setter
        0x976692:'e8d9fff4ff',         # refill updates inventory reserve
        0x7D8F8E:'ff9048010000',       # character requests eligibility
        0x965D94:'ff9050010000',       # timer mode invokes same refill
        0x991C67:'488b01ff9050010000', # event dispatch invokes refill
        0x9836CE:'f30f11b7a0010000',   # scheduled fire duration, not reload proof
        0x983727:'c6878801000001',
    }
    for rva,encoded in sites.items():
        wanted=bytes.fromhex(encoded)
        if read(rva,len(wanted))!=wanted:
            raise ValueError(f'reload proof mismatch at {rva:x}')
    for slot,rva in ((0x138,0x96E520),(0x148,0x9650C0),(0x150,0x976480),(0x168,0x966220)):
        if struct.unpack('<Q',read(0x23E3038+slot,8))[0]!=base+rva:
            raise ValueError(f'bullet weapon vtable slot {slot:x} changed')
    print('PASS: magazine/consume/refill/reserve path, character eligibility,')
    print('      one-round/timer branch, event refill dispatch and scheduled-fire fields.')
    # Static subclass coverage, not a claim that every reload behaves alike.
    for name,table_rva in (('bullet',0x23E3038),('sniper',0x23F0A48),
                           ('RPG',0x23F9D18),('missile',0x23F8948),
                           ('grenade',0x23F80B8),('planted explosive',0x23F8F88)):
        methods=[struct.unpack('<Q',read(table_rva+s,8))[0]-base for s in (0x138,0x148,0x150,0x168)]
        print(name+': setter/can-refill/refill/consume RVAs '+ '/'.join(f'{x:08X}' for x in methods))


if __name__=='__main__':main(sys.argv[1])
