"""Read-only offline inspection of JC3's weapon UI references."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build/disasm-deps'))
import pefile
import capstone

pe = pefile.PE(r'D:\SteamLibrary\steamapps\common\Just Cause 3\JustCause3.exe')
data = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
if len(sys.argv)>1 and sys.argv[1]=='hudrefs':
    import struct
    names=[b'SetMagazineAmmo',b'SetInventoryAmmo',b'UpdateHealth',b'OnSelectWeapon',
           b'OnSetSelectedMagazineAmmo',b'OnSetSelectedInventoryAmmo']
    targets={}
    for name in names:
        at=0
        while True:
            at=data.find(name+b'\0',at)
            if at<0:break
            targets[at]=name.decode();at+=1
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:continue
        lo,hi=section.VirtualAddress,section.VirtualAddress+section.Misc_VirtualSize
        for at in range(lo,hi-7):
            if data[at] in (0x48,0x4c) and data[at+1]==0x8d and data[at+2]&0xc7==0x05:
                target=at+7+struct.unpack_from('<i',data,at+3)[0]
                if target in targets:
                    print(targets[target],'reference',hex(at))
                    for ins in md.disasm(data[max(lo,at-28):at+80],base+max(lo,at-28)):
                        print(hex(ins.address-base),ins.mnemonic,ins.op_str)
elif len(sys.argv)>1 and sys.argv[1]=='vtable':
    import struct
    start=int(sys.argv[2],0)
    for slot in (range(int(sys.argv[3],0)) if len(sys.argv)>3 else [26,27,28,43,51]):
        target=struct.unpack_from('<Q',data,start+slot*8)[0]-base
        print('slot',slot,'rva',hex(target),'guard',data[target:target+15].hex(' '))
elif len(sys.argv) > 1 and sys.argv[1]=='calls':
    import struct
    target=int(sys.argv[2],0)
    for section in pe.sections:
        if section.Characteristics & 0x20000000:
            lo,hi=section.VirtualAddress,section.VirtualAddress+section.Misc_VirtualSize
            for at in range(lo,hi-5):
                if data[at]==0xe8 and at+5+struct.unpack_from('<i',data,at+1)[0]==target:
                    print('call',hex(at))
elif len(sys.argv) > 1:
    start = int(sys.argv[1], 0)
    if len(sys.argv)>3 and sys.argv[3]=='bytes':
        print('guard bytes:',data[start:start+int(sys.argv[2],0)].hex(' '))
    for ins in md.disasm(data[start:start+int(sys.argv[2],0)], base+start):
        if len(sys.argv)>3 and sys.argv[3]=='c4' and ins.mnemonic!='call' and not any(s in ins.op_str for s in ['0x738','0x748','0x77c','0x79c','0x808','0x809','0x76c']):
            continue
        if len(sys.argv)>3 and sys.argv[3]=='fields' and not any(s in ins.op_str for s in ['0x198','0x18c','0x19c','0x174','0x3b0','0x390']):
            continue
        print(hex(ins.address-base), ins.mnemonic, ins.op_str)
else:
    for name in [b'UpdateWeaponSelection', b'textures/ui/weapon_icons/']:
        target = data.find(name)
        print(name, hex(target))
        # RIP-relative LEAs referencing this exact string, not broad process scanning.
        import struct
        for section in pe.sections:
            if section.Characteristics & 0x20000000:
                lo, hi = section.VirtualAddress, section.VirtualAddress+section.Misc_VirtualSize
                for at in range(lo, hi-7):
                    if data[at:at+2] in (b'\x48\x8d', b'\x4c\x8d') and data[at+2]&0xc7==5:
                        ref=at+7+struct.unpack_from('<i',data,at+3)[0]
                        if ref==target or (name.startswith(b'textures') and target-32 <= ref <= target):
                            print('reference',hex(at))
