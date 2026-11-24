"""Bounded installed sniper-model inspection, no process/game writes."""
from pathlib import Path
import sys,struct,hashlib,math,json
from extract_weapon_icons import hash_name
from inspect_hud_explosive_assets import unpack_aaf

IDS=['w161_sniper_rifle_u_sv_45','w162_sniper_rifle_cs111_archangel','w163_sniper_rifle_final_argument']

def members(raw):
    if raw[4:8]!=b'SARC': raise ValueError('SARC')
    p,end=16,16+struct.unpack_from('<I',raw,12)[0]
    while end-p>15:
        length=struct.unpack_from('<I',raw,p)[0];p+=4
        if length>256 or p+length+8>end: raise ValueError('member name')
        name=raw[p:p+length].split(b'\0')[0].decode('ascii');p+=length
        offset,size=struct.unpack_from('<II',raw,p);p+=8
        if offset:
            if offset+size>len(raw): raise ValueError('member range')
            yield name,raw[offset:offset+size]

def positions(b):
    if b[:9]!=b'\x05\0\0\0RBMDL' or struct.unpack_from('<3I',b,9)!=(1,16,0):raise ValueError('RBM')
    p=53;blocks=[]
    while p<len(b):
        tag=struct.unpack_from('<I',b,p)[0];p+=4;start=p
        if tag!=0x2cec5ad5 or b[p]!=5: raise ValueError(f'block {tag:x} at {p:x}')
        scale=struct.unpack_from('<f',b,p+41)[0];p+=0x181
        count=struct.unpack_from('<I',b,p)[0];p+=4;materials=[]
        if count>32:raise ValueError('materials')
        for _ in range(count):
            length=struct.unpack_from('<I',b,p)[0];p+=4
            if length>256:raise ValueError('string')
            materials.append(b[p:p+length].decode('ascii'));p+=length
        p+=16;n=struct.unpack_from('<I',b,p)[0];p+=4
        if not 0<n<65536 or not 0<scale<10:raise ValueError('vertices')
        v=[tuple(x*scale/32767 for x in struct.unpack_from('<4h',b,p+8*i)[:3]) for i in range(n)];p+=8*n
        m=struct.unpack_from('<I',b,p)[0];p+=4
        if m!=n:raise ValueError('attributes')
        p+=20*m;k=struct.unpack_from('<I',b,p)[0];p+=4
        if k==1:
            # Archangel has one indexed range descriptor before the index
            # array. Require its exact observed triangle-range shape.
            range_count,first,primitive,flags=struct.unpack_from('<4I',b,p);p+=16
            k=struct.unpack_from('<I',b,p)[0];p+=4
            if first!=0 or primitive!=2 or flags!=65536 or range_count!=k:raise ValueError('index range')
        if k>196608 or k%3:raise ValueError('faces')
        faces=struct.unpack_from('<'+'H'*k,b,p);p+=k*2
        if not faces or max(faces)>=n or struct.unpack_from('<I',b,p)[0]!=0x89abcdef:raise ValueError('footer')
        p+=4;blocks.append((v,faces,materials))
    return blocks

def main():
    from PIL import Image,ImageDraw
    root,out=map(Path,sys.argv[1:3]);out.mkdir(parents=True,exist_ok=True)
    wanted={hash_name(f'editor/entities/jc_weapons/02_two_handed/{n}/{n}.ee'):n for n in IDS};found={}
    for folder in ['archives_win64','dlc_win64','patch_win64']:
        for tab in sorted((root/folder).rglob('*.tab')):
            data=tab.read_bytes()
            for p in range(12,len(data)-11,12):
                key,offset,size=struct.unpack_from('<III',data,p)
                if key in wanted:found[wanted[key]]=(tab.with_suffix('.arc'),offset,size)
    report={}
    for ident,(arc,offset,size) in found.items():
        if size>32*1024*1024:raise ValueError('bundle bound')
        with arc.open('rb') as f:f.seek(offset);packed=f.read(size)
        raw=unpack_aaf(packed)
        print(ident,arc.name,offset,size,hashlib.sha256(packed).hexdigest())
        model=next(b for n,b in members(raw) if n.endswith('_body_lod1.rbm'))
        blocks=positions(model);verts=[v for block in blocks for v in block[0]]
        measured=[min(v[i] for v in verts) for i in range(3)]+[max(v[i] for v in verts) for i in range(3)]
        print('blocks',len(blocks),'bounds',measured)
        for i,block in enumerate(blocks):print('block',i,'verts',len(block[0]),'materials',[s for s in block[2] if s])
        image=Image.new('RGB',(1400,650),(24,28,34));draw=ImageDraw.Draw(image)
        for v in verts:
            draw.ellipse((1100+v[2]*950-1,330-v[1]*950-1,1100+v[2]*950+1,330-v[1]*950+1),fill=(210,215,225))
            draw.ellipse((1100+v[2]*950-1,500+v[0]*950-1,1100+v[2]*950+1,500+v[0]*950+1),fill=(210,215,225))
        for step in range(-11,4):
            x=1100+step*95;draw.line((x,50,x,600),fill=(45,55,60));draw.text((x,605),f'z={step/10:.1f}',fill='white')
        draw.text((20,10),ident+' side (Y up) / top (X)',fill='white')
        image.save(out/(ident+'.png'))
        report[ident]={'archive':arc.name,'offset':offset,'size':size,'sha256':hashlib.sha256(packed).hexdigest(),'bounds':measured,'vertices':verts}
        # Only small relevant entity/tuning records, not unrelated assets.
        for n,b in members(raw):
            if n.endswith('.epe') or n.endswith('_attach_settings.wtunec'):
                print('record',n,'strings',[s.decode('ascii') for s in __import__('re').findall(rb'[A-Za-z_][A-Za-z_0-9./]{5,}',b)][:60])
    (out/'measurements.json').write_text(json.dumps(report))

if __name__=='__main__':main()
