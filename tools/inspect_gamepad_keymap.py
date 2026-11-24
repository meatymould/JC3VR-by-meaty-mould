"""Read-only dump of the installed RTPC pad bindings using JC3's string list."""
from pathlib import Path
import struct
from extract_weapon_icons import hash_name
root=Path(__file__).resolve().parents[1]
data=(root/'build/weapon-icons/keymap_gamepad.bin').read_bytes()
strings=(root/'third_party/research/deca/resources/deca/ghidra_strings/jc3.txt').read_text(errors='replace').splitlines()
names={hash_name(s):s for s in strings if s.isascii()}
def name(x): return names.get(x,hex(x))
def node(at,path):
    key,offset,np,nc=struct.unpack_from('<IIHH',data,at)
    path=path+'/'+name(key)
    props=[]
    for i in range(np):
        key,v,t=struct.unpack_from('<IIB',data,offset+i*9)
        if t==3: value=data[v:data.index(0,v)].decode(errors='replace')
        elif t==2: value=struct.unpack_from('<f',data,offset+i*9+4)[0]
        else: value=name(v)
        props.append((name(key),value))
    if props: print(path,props)
    child=(offset+np*9+3)&~3
    for i in range(nc): node(child+i*12,path)
node(8,'')
