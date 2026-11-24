"""Read-only pinned executable/source/asset checks for physical hijack prototype.
Uses existing pefile; no launch, memory attachment, patching or asset rewriting.
"""
import argparse,hashlib,json,re,struct
from pathlib import Path
from extract_weapon_icons import hash_name

def verify(exe,assets):
    import pefile
    data=exe.read_bytes()
    if hashlib.sha256(data).hexdigest()!='a53c2d6b2837f45d97244fc096e68f27ba9036e0582a0d9aada2e2f222dac293':raise ValueError('unverified executable')
    pe=pefile.PE(data=data,fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase
    source=(Path(__file__).resolve().parents[1]/'src/game/physical_hijack.cpp').read_text()
    pairs={'kEventBytes':0x7fadd0,'kFinishBytes':0x78e540,'kVehicleBytes':0x7d5e30,
           'context':0x3bc2e0,'caller':0x7fc8ab,'openCall':0x7fbc0f,'openCase':0x7fb8ce,'doorExists':0xfc7540}
    for key,rva in pairs.items():
        raw=re.search(r'\b'+key+r'\[\]\{([^}]+)\}',source).group(1)
        expected=bytes(int(x.strip(),16) for x in raw.split(','))
        if pe.get_data(rva,len(expected))!=expected:raise ValueError(f'landmark mismatch {key}/{rva:x}')
    if struct.unpack('<Q',pe.get_data(0x23be0b8+0x18,8))[0]!=base+0x78e540:raise ValueError('finish-condition vtable mismatch')
    # Native task decoder46ABD0: first byte shifted8, plus second byte.
    if pe.get_data(0x46abd0,16)!=bytes.fromhex('410fb64c24ffc1e108410fb6042403c8'):raise ValueError('task index decoder mismatch')
    ns=json.loads((assets/'rico_base/rtpc_000.json').read_text())['nodes'];root={p['key']:p['value'] for p in ns[1]['props']}
    groups={k:[n for n in ns if n['parent']==next(x['index'] for x in ns if x['key']==k and x['parent']==1)] for k in (0x1d8fd18a,0x49e4a9fe,0x4a7ffa82,0x60950642)}
    states=groups[0x60950642];trans=groups[0x49e4a9fe];conditions=groups[0x4a7ffa82];tasks=groups[0x1d8fd18a]
    def label(n):return next((p['value'] for p in n['props'] if p['key']==0x3a4ca003),None)
    def u16(k):
        b=bytes(root[k]);return struct.unpack('>'+'H'*(len(b)//2),b)
    lists=u16(0xa3b244fc);targets=u16(0x4c2b5478);condition_stream=u16(0x42a539b5)
    result={'native_landmarks':len(pairs),'vtable_check':True,'task_decoder_check':True,'states':[]}
    for name in ('S_PASSENGER_SIDE_OPEN_DOOR_VEHICLE','S_DRIVER_SIDE_OPEN_DOOR_VEHICLE'):
        state=next(n for n in states if label(n)==name)
        if hash_name(name)!=state['key']:raise ValueError('state hash mismatch')
        props={p['key']:p['value'] for p in state['props']};raw=bytes(props[0x1d8fd18a]);indices=struct.unpack('>'+'H'*(len(raw)//2),raw)
        if any(i>=len(tasks) for i in indices):raise ValueError('task index range')
        off,count=props[0x133a0d9f];out=[]
        for ti in lists[off:off+count]:
            co,cc,flags=trans[ti]['props'][0]['value'];ci=condition_stream[co:co+cc]
            out.append({'target':label(states[targets[ti]]),'conditions':[label(conditions[i]) for i in ci],'flags':flags})
            if not any(label(conditions[i])=='C_WAIT_FOR_ANIM_TO_FINISH' and root[0x8943ee06][i]==0x1720f94c for i in ci):raise ValueError('exit not guarded by verified rule-finish condition')
        result['states'].append({'name':name,'hash':f'{state["key"]:08X}','task_indices':indices,'outgoing':out})
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('exe',type=Path);p.add_argument('assets',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args()
    if a.output.resolve().is_relative_to(Path(__file__).resolve().parents[1]):raise ValueError('derived asset details stay outside source Git')
    result=verify(a.exe,a.assets);a.output.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
