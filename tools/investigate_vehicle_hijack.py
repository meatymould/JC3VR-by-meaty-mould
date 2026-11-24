"""Read-only pinned-PE interaction inventory. Output belongs outside source Git.
Requires existing pefile/capstone; no debugger, game launch or engine writes.
"""
import argparse, bisect, hashlib, json, re, struct
from pathlib import Path

def inventory(path):
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    data=Path(path).read_bytes()
    expected='a53c2d6b2837f45d97244fc096e68f27ba9036e0582a0d9aada2e2f222dac293'
    if hashlib.sha256(data).hexdigest()!=expected:raise ValueError('unverified executable; no address claims')
    pe=pefile.PE(data=data,fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
    base=pe.OPTIONAL_HEADER.ImageBase
    landmarks={
        0xF24FB0:'4881c2081a0000', # embedded interaction user
        0xF24FC4:'4881c1280b0000', # seat array
        0x7D5DB2:'4881c1081a0000',
        0xA38360:'488b4110c3',      # graph root getter
        0xA37200:'f30f108288010000', # line fraction condition input
        0xA37220:'f30f10828c010000', # move fraction condition input
        0x7D19AA:'48ff6038',        # button condition -> action virtual
        0xA16A46:'4c8d0523350100', # OpenVehicleDoor registered wrapper
        0xA2A0BE:'440fb6c78bd5498bcce854d25900', # wrapper ABI forwarding
        0xFC733C:'410fb6f80f28f38bf2488bd9',
        0xFC737C:'488d8b68160000', # vehicle door controller
        0xF96E40:'c70001000000',   # request state, not a hinge matrix write
        0xA3D027:'ff5008',         # transition predicate list
    }
    for address,expected_bytes in landmarks.items():
        b=bytes.fromhex(expected_bytes)
        if pe.get_data(address,len(b))!=b:raise ValueError(f'landmark mismatch {address:x}')
    radians=struct.unpack('<f',pe.get_data(0x234D4E0,4))[0]*struct.unpack('<f',pe.get_data(0x22FED18,4))[0]
    if abs(radians-0.01745329252)>1e-7:raise ValueError('door angle conversion mismatch')
    code_ranges=[(s.VirtualAddress,s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData)) for s in pe.sections if s.Characteristics&0x20000000]
    entries=sorted((e.struct.BeginAddress,e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
    starts=[a for a,b in entries]
    def owner(address):
        n=bisect.bisect_right(starts,address)-1
        return entries[n][0] if n>=0 and address<entries[n][1] else None
    strings={}
    for m in re.finditer(rb'[ -~]{5,240}',data):
        s=m.group().decode()
        if any(k in s.lower() for k in ('hijack','vehicleenter','entervehicle','buttonpressedcondition','interactiongraph','dooropen','opendoor','vehicledoor','graphtransitioncondition','graphentrycondition','animationtimeintervalcondition')) or (s.startswith(('S_','ACT_')) and any(k in s.lower() for k in ('enter_vehicle','entering_vehicle','stunt_to_driver','idle_hang_stunt'))):
            strings[pe.get_rva_from_offset(m.start())]=s
    md=Cs(CS_ARCH_X86,CS_MODE_64)
    xrefs=[]
    # Verify only complete decoded RIP-relative LEAs. These are address uses,
    # not a claim that every use is a function call or execution dependency.
    for section in pe.sections:
        if not section.Characteristics&0x20000000:continue
        blob=section.get_data()
        for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]',blob):
            off=m.start()
            if off+7>len(blob):continue
            address=section.VirtualAddress+off
            target=address+7+struct.unpack_from('<i',blob,off+3)[0]
            if target not in strings:continue
            ins=next(md.disasm(blob[off:off+7],address),None)
            if ins and ins.size==7:xrefs.append({'at':address,'function':owner(address),'string':strings[target],'target':target})
    tables=[]
    # MSVC x64 complete-object locator: image-relative type/self references,
    # signature1. A validated COL pointer precedes a vftable. This identifies
    # methods but does NOT establish their ABI or whether they are safe hooks.
    for rva,name in strings.items():
        if not name.startswith('.?AV'):continue
        td=rva-16
        for section in pe.sections:
            if section.Characteristics&0x20000000:continue
            blob=section.get_data();needle=struct.pack('<I',td)
            for m in re.finditer(re.escape(needle),blob):
                col=section.VirtualAddress+m.start()-12
                if col<0:continue
                raw=pe.get_data(col,24)
                if len(raw)!=24 or struct.unpack_from('<I',raw)[0]!=1 or struct.unpack_from('<I',raw,20)[0]!=col:continue
                for sec2 in pe.sections:
                    b=sec2.get_data()
                    for ref in re.finditer(re.escape(struct.pack('<Q',base+col)),b):
                        table=sec2.VirtualAddress+ref.start()+8;methods=[]
                        for slot in range(0,0xA0,8):
                            fn=struct.unpack('<Q',pe.get_data(table+slot,8))[0]-base
                            # Leaf methods/thunks may have no unwind entry.
                            if not any(a<=fn<b for a,b in code_ranges):break
                            methods.append({'slot':slot,'rva':fn,'function':owner(fn)})
                        if methods:tables.append({'name':name,'rva':table,'col':col,'methods':methods})
    # Identify derived AFSM condition classes rather than mistaking the base
    # class's constant-false placeholder for an executable transition gate.
    bases={rva-16 for rva,name in strings.items() if name in
           ('.?AVCInteractionGraphEntryAFSMC@@','.?AVCInteractionGraphTransitionAFSMC@@')}
    derived=[]
    for section in pe.sections:
        if section.Characteristics&0x20000000:continue
        blob=section.get_data()
        for m in re.finditer(rb'\x01\x00\x00\x00',blob):
            off=m.start();col=section.VirtualAddress+off
            if off+24>len(blob):continue
            sig,disp,cd,td,chd,self_rva=struct.unpack_from('<6I',blob,off)
            if self_rva!=col:continue
            try:
                raw=pe.get_data(chd,16)
                _,_,n,array=struct.unpack('<4I',raw)
                if not 1<=n<=128:continue
                refs=struct.unpack('<'+'I'*n,pe.get_data(array,n*4))
                inherited={struct.unpack('<I',pe.get_data(bcd,4))[0] for bcd in refs}
                if not inherited&bases:continue
                name=pe.get_data(td+16,240).split(b'\0')[0].decode('ascii')
                derived.append({'name':name,'col':col,'type':td,'bases':sorted(inherited&bases)})
            except (ValueError,struct.error,UnicodeDecodeError):continue
    return {'sha256':expected,'verified_landmarks':list(landmarks),'door_degrees_to_radians':radians,
            'strings':[{'rva':rva,'name':name} for rva,name in strings.items()],
            'address_uses':xrefs,'vtables':tables,'derived_afsm_conditions':derived}

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('exe');p.add_argument('output');args=p.parse_args()
    if Path(args.output).resolve().is_relative_to(Path(__file__).resolve().parents[1]):
        raise ValueError('derived binary inventory must stay outside the source repository')
    result=inventory(args.exe)
    Path(args.output).write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('Read-only inventory:',len(result['strings']),'strings,',len(result['address_uses']),'address uses,',len(result['vtables']),'vtables.')
    for r in result['address_uses']:
        print(f"{r['at']:08X} fn={r['function'] or 0:08X} {r['string']}")
    for t in result['vtables']:
        print(f"VTABLE {t['rva']:08X} {t['name']}",[(hex(m['slot']),hex(m['rva'])) for m in t['methods'][:8]])
    for d in result['derived_afsm_conditions']:print(f"DERIVED col={d['col']:08X} {d['name']}")
