"""Extract ONLY weapon UI icons from a user's JC3 install to a local cache.

TAB and AVTX layout references: Gibbed.JustCause3 and deca (local research).
No game assets are embedded in source or redistributed.
"""
from pathlib import Path
import struct
import io
import re
import sys
from PIL import Image

MASK=0xffffffff
def rot(x,n): return ((x<<n)|(x>>(32-n)))&MASK
def hash_name(text):
    data=text.encode('ascii'); a=b=c=(0xdeadbeef+len(data))&MASK
    while len(data)>12:
        x,y,z=struct.unpack_from('<III',data)
        a=(a+x)&MASK; b=(b+y)&MASK; c=(c+z)&MASK
        a=((a-c)^rot(c,4))&MASK; c=(c+b)&MASK
        b=((b-a)^rot(a,6))&MASK; a=(a+c)&MASK
        c=((c-b)^rot(b,8))&MASK; b=(b+a)&MASK
        a=((a-c)^rot(c,16))&MASK; c=(c+b)&MASK
        b=((b-a)^rot(a,19))&MASK; a=(a+c)&MASK
        c=((c-b)^rot(b,4))&MASK; b=(b+a)&MASK
        data=data[12:]
    tail=data+b'\0'*(12-len(data)); x,y,z=struct.unpack('<III',tail)
    a=(a+x)&MASK; b=(b+y)&MASK; c=(c+z)&MASK
    c=((c^b)-rot(b,14))&MASK; a=((a^c)-rot(c,11))&MASK
    b=((b^a)-rot(a,25))&MASK; c=((c^b)-rot(b,16))&MASK
    a=((a^c)-rot(c,4))&MASK; b=((b^a)-rot(a,14))&MASK
    return ((c^b)-rot(b,24))&MASK

def decode(data):
    if data[:4]!=b'AVTX': raise ValueError('not AVTX')
    fmt=struct.unpack_from('<I',data,8)[0]
    w,h=struct.unpack_from('<HH',data,12)
    header,body=struct.unpack_from('<II',data,32)
    if not (0<w<=4096 and 0<h<=4096 and header+body<=len(data)):
        raise ValueError('invalid AVTX dimensions/body')
    # DDSC stores only the resident smaller mip levels when ATX is present.
    total,resident=data[20],data[21]
    w=max(1,w >> (total-resident)); h=max(1,h >> (total-resident))
    # DDS DX10 wrapper, let Pillow decode the installed BCn asset.
    words=[124,0x1007,h,w,0,0,1]+[0]*11+[32,4,0x30315844,0,0,0,0,0]+[0x1000,0,0,0,0]
    dds=b'DDS '+struct.pack('<31I',*words)+struct.pack('<5I',fmt,3,0,1,0)+data[header:header+body]
    return Image.open(io.BytesIO(dds)).convert('RGBA')

def main():
    root=Path(sys.argv[1]); out=Path(sys.argv[2]); out.mkdir(parents=True,exist_ok=True)
    repo=Path(__file__).resolve().parents[1]
    lists=repo/'third_party/research/gibbed-justcause3-tools-fork/bin/projects/Just Cause 3/files'
    names=set()
    for listing in lists.rglob('*.filelist'):
        for line in listing.read_text(errors='replace').splitlines():
            name=line.lower().replace('\\','/')
            if re.fullmatch(r'textures/ui/weapon_icons/\d+\.ddsc',name): names.add(name)
    names.add('settings/keymap_gamepad.bin')
    # Older community lists omit some installed/DLC weapon IDs (e.g.119).
    # Exact named icon lookups only; no unrelated assets are extracted.
    names.update(f'textures/ui/weapon_icons/{i}.ddsc' for i in range(1,1025))
    wanted={hash_name(n):n for n in names}
    found={}
    # Base first, patch/DLC override later, never overwrite original game files.
    for folder in ['archives_win64','dlc_win64','patch_win64']:
        for tab in sorted((root/folder).rglob('*.tab')):
            data=tab.read_bytes()
            if data[:8]!=b'TAB\0\x02\0\x01\0': continue
            with tab.with_suffix('.arc').open('rb') as arc:
                for at in range(12,len(data)-11,12):
                    key,offset,size=struct.unpack_from('<III',data,at)
                    if key not in wanted: continue
                    arc.seek(offset); raw=arc.read(size)
                    if wanted[key].endswith('.bin'):
                        (out/'keymap_gamepad.bin').write_bytes(raw)
                        continue
                    try: image=decode(raw)
                    except Exception as exc:
                        print('decode failed',wanted[key],raw[:40].hex(),len(raw),exc)
                        continue
                    ident=Path(wanted[key]).stem
                    image.save(out/(ident+'.png'))
                    found[ident]=(str(tab),offset,size,image.size)
    for ident,info in sorted(found.items()): print(ident,info)
    print('extracted',len(found),'original weapon icons')
    if not found: raise SystemExit('no icons extracted')

if __name__=='__main__': main()
