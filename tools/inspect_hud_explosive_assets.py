"""Read-only installed HUD/C4 archive inventory; never extracts or edits assets.

TAB/SARC/AAF references: existing local Gibbed/deca sources. Static GFx symbols
do not establish runtime clip paths. Nothing here attaches to the running game.
"""
from pathlib import Path
from collections import Counter
import hashlib
import re
import struct
import sys
import zlib
from extract_weapon_icons import hash_name

LIMIT = 64 * 1024 * 1024
NAMES = ['ui/root.gfx', 'ui/hud.gfx', 'ui/shared_lib.gfx', 'ui/overlay.gfx',
         'editor/entities/jc_weapons/03_thrown/w401_explosives_c4/w401_explosives_c4.ee']

def inflate(raw, expected, window=15):
    if not 0 <= expected <= LIMIT:
        raise ValueError('oversized asset')
    decoder = zlib.decompressobj(window)
    out = decoder.decompress(raw, expected + 1)
    if len(out) != expected or not decoder.eof:
        raise ValueError('decompression length/end mismatch')
    return out

def unpack_aaf(raw):
    if raw[:4] != b'AAF\0':
        return raw
    total, block_size, count = struct.unpack_from('<III', raw, 36)
    if total > LIMIT or count > 64:
        raise ValueError('AAF limits')
    at, chunks = 48, []
    for _ in range(count):
        packed, size, stride = struct.unpack_from('<III', raw, at)
        if raw[at+12:at+16] != b'EWAM' or stride < 16+packed or at+stride > len(raw):
            raise ValueError('invalid AAF block')
        chunks.append(inflate(raw[at+16:at+16+packed], size, -15))
        at += stride
    out = b''.join(chunks)
    if len(out) != total:
        raise ValueError('AAF total mismatch')
    return out

def inspect_sarc(raw):
    if raw[4:8] != b'SARC':
        print('unrecognized EE container', raw[:16].hex());return
    magic, version, index_size = struct.unpack_from('<I4xII', raw)
    if magic != 4 or version != 2 or index_size > len(raw)-16:
        raise ValueError('invalid SARC')
    at, end = 16, 16+index_size
    while end-at > 15:
        length = struct.unpack_from('<I', raw, at)[0];at += 4
        if length > 256 or at+length+8 > end:
            raise ValueError('invalid SARC name')
        name = raw[at:at+length].split(b'\0')[0].decode('ascii');at += length
        offset,size = struct.unpack_from('<II', raw, at);at += 8
        if offset == 0:
            print('external member', name, 'declared bytes',size);continue
        if offset+size > len(raw):
            raise ValueError('SARC member range')
        print('member', name, 'offset', offset, 'bytes', size)
        member = raw[offset:offset+size]
        if name.lower().endswith('.rbm') and member[:9] == b'\x05\0\0\0RBMDL' and len(member)>=57:
            print('  RBM version',struct.unpack_from('<III',member,9),'bounds',struct.unpack_from('<6f',member,21),
                  'blocks',struct.unpack_from('<I',member,49)[0],'first_block',hex(struct.unpack_from('<I',member,53)[0]))
        if name.lower().endswith(('.bin','.rbm','.adf')):
            refs = sorted(set(re.findall(rb'[A-Za-z0-9_./\\-]{8,}\.(?:rbm|ddsc|hmddsc|wtun|ee)',raw[offset:offset+size])))
            for ref in refs[:32]: print('  reference',ref.decode('ascii'))

def inspect_gfx(raw):
    signature, declared = raw[:3], struct.unpack_from('<I', raw,4)[0]
    if signature in (b'CFX',b'CWS'):
        decoder = zlib.decompressobj()
        body = decoder.decompress(raw[8:],LIMIT+1)
        if len(body)>LIMIT or not decoder.eof: raise ValueError('GFx decompression bound/end')
        if len(body)+8 != declared: print('GFx length note: declared',declared,'inflated',len(body)+8)
    elif signature in (b'GFX',b'FWS'):
        body = raw[8:]
        if len(body)+8 != declared: raise ValueError('GFx declared length mismatch')
    else:
        print('unrecognized GFx signature',signature);return
    # Frame RECT bitfield, frame rate and frame count precede SWF tags.
    at = ((body[0] >> 3)*4+5+7)//8 + 4
    counts, symbols = Counter(), []
    while at+2 <= len(body):
        head = struct.unpack_from('<H',body,at)[0];at += 2
        tag,size = head>>6,head&63
        if size == 63:
            size = struct.unpack_from('<I',body,at)[0];at += 4
        if at+size > len(body): raise ValueError('GFx tag range')
        counts[tag] += 1
        if tag == 76:
            payload = body[at:at+size];number = struct.unpack_from('<H',payload)[0];pos = 2
            for _ in range(number):
                ident = struct.unpack_from('<H',payload,pos)[0];pos += 2
                stop = payload.index(b'\0',pos)
                symbols.append((ident,payload[pos:stop].decode('utf-8')));pos = stop+1
        at += size
        if tag == 0: break
    print('tags',sum(counts.values()),'sprites',counts[39],'shapes',sum(counts[t] for t in [2,22,32,83]),
          'edit_text',counts[37],'abc',counts[82],'font4',counts[91])
    for ident,name in symbols: print('symbol',ident,name)

def main():
    root = Path(sys.argv[1]);wanted = {hash_name(n):n for n in NAMES};selected = {}
    for folder in ['archives_win64','dlc_win64','patch_win64']:
        for tab in sorted((root/folder).rglob('*.tab')):
            index = tab.read_bytes()
            if index[:8] != b'TAB\0\x02\0\x01\0': continue
            with tab.with_suffix('.arc').open('rb') as arc:
                for at in range(12,len(index)-11,12):
                    key,offset,size = struct.unpack_from('<III',index,at)
                    if key not in wanted: continue
                    if size > LIMIT: raise ValueError('archive entry too large')
                    arc.seek(offset);raw = arc.read(size)
                    if len(raw) != size: raise ValueError('truncated archive entry')
                    name = wanted[key];print('version',name,tab.name,offset,size)
                    selected[name] = (str(tab.relative_to(root)),offset,raw)
    for name in NAMES:
        if name not in selected: print('NOT FOUND',name);continue
        archive,offset,raw = selected[name]
        print('\nASSET',name,'archive',archive,'offset',offset,'bytes',len(raw),'sha256',hashlib.sha256(raw).hexdigest())
        try:
            if name.endswith('.gfx'): inspect_gfx(raw)
            else: inspect_sarc(unpack_aaf(raw))
        except (ValueError,struct.error,zlib.error,IndexError) as exc: print('inspection failed',exc)

if __name__ == '__main__': main()
