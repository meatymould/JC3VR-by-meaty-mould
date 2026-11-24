"""Offline observer ABI/layout landmarks on pinned executable; no hook/launch."""
import struct
import sys
from verify_dual_aim_native import verify

def main(path):
    data=verify(path);p=struct.unpack_from('<I',data,60)[0]
    t=p+24+struct.unpack_from('<H',data,p+20)[0]
    sections=[struct.unpack_from('<IIII',data,t+i*40+8) for i in range(struct.unpack_from('<H',data,p+6)[0])]
    base=struct.unpack_from('<Q',data,p+48)[0]
    def read(a,n):
        return next(data[r+a-v:r+a-v+n] for _,v,s,r in sections if v<=a and a+n<=v+s)
    sites={
        0x966380:'4c8bdc5553564154488d6c24884881ec78010000',
        0x9663C2:'4c8be2488bf1', # render context RDX -> R12, self RCX -> RSI
        0x9663F8:'488d9798010000', # gun T1
        0x966404:'488d9758010000', # gun T0
        0x966439:'488b8f28010000', # separate animated model pointer
        0x966498:'e813dfb2ff',     # animated skinning update
        0x9664AF:'e8dce1b2ff',     # animation rule render update
        0x966549:'488b8f30010000', # separate base model instance
        0x966555:'e846009bff',     # base model world transform setter
        0x987122:'48639d90020000', # placement mount slot (5th native argument)
        0x4946B0:'488b81a0000000', # rule system list end, NOT model children
        0x4941C1:'488b94f100020000', # actual model slots +200
    }
    for a,h in sites.items():
        wanted=bytes.fromhex(h)
        if read(a,len(wanted))!=wanted:raise ValueError(f'attachment landmark {a:x} mismatch')
    if struct.unpack('<Q',read(0x23E2EC8+0x70,8))[0]!=base+0x966380:
        raise ValueError('weapon CGameObject UpdateRender vtable mismatch')
    print('PASS: weapon UpdateRender ABI landmarks/vtable, root versus animated paths,')
    print('      placement slot, rule-system identities and separate model slots.')

if __name__=='__main__':main(sys.argv[1])
