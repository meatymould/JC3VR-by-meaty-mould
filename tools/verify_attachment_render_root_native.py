"""Pinned, read-only native attachment-root ABI checks; no game launch/hook."""
import struct
import sys
from verify_dual_aim_native import verify


def main(path):
    data=verify(path)
    pe=struct.unpack_from('<I',data,60)[0]
    table=pe+24+struct.unpack_from('<H',data,pe+20)[0]
    sections=[struct.unpack_from('<IIII',data,table+i*40+8)
              for i in range(struct.unpack_from('<H',data,pe+6)[0])]

    def read(rva,n):
        return next(data[raw+rva-start:raw+rva-start+n]
                    for _,start,size,raw in sections if start<=rva and rva+n<=start+size)

    sites={
        0x7C24B0:'488bc4488958084889781055488d68a14881ecb0000000',
        0x7C24D2:'498bf80f28f1488bd9', # R8 matrix -> RDI, XMM1 alpha -> XMM6
        0x7FA299:'f3440f1002f30f107204', # context dt, context interpolation factor
        0x7FA2F9:'4c8d81cc270000', # next native quaternion/position state
        0x7FA308:'e8f33184ff', # native interpolate helper
        0x7FA316:'e8b57583ff', # completed pose -> matrix
        0x7FA3CE:'4c8d45000f28ce498d4c24f8e8d180fcff', # matrix, alpha, Rico-8, call
        0xF41CC:'448b05d964c402458bc8448905d364c40241ffc0448905c564c402',
    }
    for rva,hex_bytes in sites.items():
        expected=bytes.fromhex(hex_bytes)
        if read(rva,len(expected))!=expected:raise ValueError(f'root landmark {rva:x} mismatch')
    image=struct.unpack_from('<Q',data,pe+48)[0]
    if struct.unpack('<Q',read(0x23D16D8+0x70,8))[0]!=image+0x7FA220:
        raise ValueError('Rico render vtable mismatch')
    print('PASS: native root consumer ABI, exact caller/base adjustment, interpolation producer, frame publication and vtable')


if __name__=='__main__':main(sys.argv[1])
