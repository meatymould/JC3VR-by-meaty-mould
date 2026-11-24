"""Read-only, named JC3 interaction asset research; derived output outside Git.
TAB/AAF/SARC references: existing local Gibbed/deca, through established helpers.
RTPC layout independently bounded here from deca ff_rtpc; no asset rewriting.
"""
import argparse,hashlib,json,struct
from pathlib import Path
from extract_weapon_icons import hash_name
from inspect_hud_explosive_assets import unpack_aaf
from inspect_sniper_optics import members
from inspect_vehicle_adf import parse_adf

NAMES=[f'animations/jc_statemachine/main_characters/rico/{n}.afsmb' for n in
       ('rico_base','rico_partial_global','rico_partial_sync','rico_additive_sync')]+[
 'editor/entities/jc_vehicles/02_air/v2102_helicopter_eubus_utility/v2102_helicopter_eubus_utility_civilian_01.ee',
 'editor/entities/jc_vehicles/02_air/v2602_helicopter_urga_lightassault/v2602_helicopter_urga_lightassault_military_01.ee',
 'editor/entities/jc_vehicles/02_air/v2601_helicopter_urga_heavyassault/v2601_helicopter_urga_heavyassault_military_01.ee',
 'editor/entities/jc_vehicles/01_land/v0003_car_autostraad_racinghothatch/v0003_car_autostraad_racinghothatch_civilian_01.ee']
LIMIT=64*1024*1024

def parse_rtpc(data,names=None):
    names=names or {};nodes=[];seen=set();props_total=0
    def check(at,n):
        if at<0 or n<0 or at+n>len(data):raise ValueError('RTPC range')
    def unpack(fmt,at):
        check(at,struct.calcsize(fmt));return struct.unpack_from(fmt,data,at)
    def value(raw,kind):
        if kind in (0,1,12,15,16):return raw
        if kind==2:return struct.unpack('<f',struct.pack('<I',raw))[0]
        if kind==3:
            check(raw,1);end=data.find(b'\0',raw,min(len(data),raw+16385))
            if end<0:raise ValueError('RTPC string bound')
            return data[raw:end].decode('utf8',errors='replace')
        if kind in (4,5,6,7,8):return list(unpack('<'+'f'*{4:2,5:3,6:4,7:9,8:16}[kind],raw))
        if kind in (9,10,11,14):
            n=unpack('<I',raw)[0]
            if n>65536:raise ValueError('RTPC array bound')
            return list(unpack('<'+{9:'I',10:'f',11:'B',14:'Q'}[kind]*n,raw+4))
        if kind==13:return unpack('<Q',raw)[0]
        raise ValueError(f'unknown RTPC type {kind}')
    def node(at,parent,depth):
        nonlocal props_total
        if at in seen or depth>256 or len(nodes)>=100000:raise ValueError('RTPC cycle/depth/count')
        seen.add(at);key,start,np,nc=unpack('<IIHH',at);props_total+=np
        if props_total>1000000:raise ValueError('RTPC property bound')
        children=(start+np*9+3)&~3;check(start,np*9);check(children,nc*12)
        props=[]
        for i in range(np):
            k,raw,t=unpack('<IIB',start+i*9)
            props.append({'key':k,'label':names.get(k), 'type':t,'value':value(raw,t)})
        index=len(nodes);nodes.append({'index':index,'offset':at,'parent':parent,'key':key,'label':names.get(key),'props':props,'children':nc})
        for i in range(nc):node(children+i*12,index,depth+1)
    if data[:4]!=b'RTPC':raise ValueError('not RTPC')
    version=unpack('<I',4)[0];node(8,None,0)
    return {'version':version,'nodes':nodes,'property_count':props_total}

def inspect(root,out,strings):
    if out.resolve().is_relative_to(Path(__file__).resolve().parents[1]):raise ValueError('assets must stay outside source Git')
    out.mkdir(parents=True,exist_ok=True)
    labels={}
    for s in strings.read_text(errors='replace').splitlines():
        if s.isascii():labels.setdefault(hash_name(s),s)
    wanted={hash_name(n):n for n in NAMES};found={};manifest=[]
    for folder in ('archives_win64','dlc_win64','patch_win64'):
        for tab in sorted((root/folder).rglob('*.tab')):
            data=tab.read_bytes()
            if data[:8]!=b'TAB\0\x02\0\x01\0':continue
            for at in range(12,len(data)-11,12):
                key,offset,size=struct.unpack_from('<III',data,at)
                if key in wanted:
                    if size>LIMIT:raise ValueError('archive entry bound')
                    found[wanted[key]]=(tab.with_suffix('.arc'),offset,size)
    for n,(arc,offset,size) in found.items():
        with arc.open('rb') as f:
            f.seek(0,2)
            if offset+size>f.tell():raise ValueError('archive range')
            f.seek(offset);packed=f.read(size)
        raw=unpack_aaf(packed)
        ident=Path(n).stem;folder=out/ident;folder.mkdir(exist_ok=True)
        (folder/'container.bin').write_bytes(raw)
        entry={'name':n,'archive':str(arc),'offset':offset,'packed_size':size,
               'sha256':hashlib.sha256(packed).hexdigest(),'members':[]}
        contents=[(n,raw)] if raw[:4]==b'RTPC' else list(members(raw))
        print(n,'bytes',len(raw),'members',len(contents))
        for ordinal,(name,b) in enumerate(contents):
            item={'name':name,'size':len(b),'magic':b[:8].hex()};entry['members'].append(item)
            if b[:4]==b' FDA' and name.endswith('.vmodc') and any(k in name for k in ('vehicle_parts','vehicle_misc','vehicle_common')):
                decoded=parse_adf(b);target=folder/f'adf_{ordinal:03d}.json'
                target.write_text(json.dumps(decoded,indent=2,allow_nan=False),encoding='utf8')
                item['decoded']=str(target);print(' ADF',name,'types',len(decoded['types']))
            if b[:4]!=b'RTPC':continue
            decoded=parse_rtpc(b,labels);target=folder/f'rtpc_{ordinal:03d}.json'
            target.write_text(json.dumps(decoded,indent=2,allow_nan=False),encoding='utf8')
            item['decoded']=str(target);item['nodes']=len(decoded['nodes'])
            print(' RTPC',name,'nodes',len(decoded['nodes']),'properties',decoded['property_count'])
        manifest.append(entry)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
    print('Found',len(found),'of',len(NAMES),'named assets; originals unchanged.')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('install',type=Path);p.add_argument('output',type=Path);p.add_argument('strings',type=Path)
    a=p.parse_args();inspect(a.install,a.output,a.strings)
