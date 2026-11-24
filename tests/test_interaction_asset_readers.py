"""Synthetic layout/bounds tests; no game data required or written."""
import sys,struct,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from inspect_vehicle_interaction_assets import parse_rtpc
from inspect_vehicle_adf import parse_adf
from analyse_hijack_state import analyse

def adf_fixture():
    b=bytearray(512);b[:4]=b' FDA';names=['Data','array','door_count','A[f32]']
    struct.pack_into('<10I',b,4,4,1,208,2,64,0,0,4,232,len(b))
    struct.pack_into('<IIIIQIIII',b,64,1,16,4,0xFEED,0,0,0,0,2)
    struct.pack_into('<QIIIIQ',b,104,1,0xBEEF,12,0,0,0)
    struct.pack_into('<QIIIIQ',b,136,2,0x075e4e4f,4,12,0,0)
    struct.pack_into('<IIIIQIIII',b,168,3,12,4,0xBEEF,3,0,0x7515a207,0,0)
    struct.pack_into('<IIIIQ',b,208,0,0xFEED,320,24,0)
    b[232:236]=bytes(len(s) for s in names);at=236
    for s in names:b[at:at+len(s)+1]=s.encode()+b'\0';at+=len(s)+1
    struct.pack_into('<4I2f',b,320,16,0,2,2,0.5,1.25)
    return b

class Readers(unittest.TestCase):
    def test_capture_analysis(self):
        text='hijack state BEGIN count=2 overflow=0 qpc_frequency=1000\n'+\
          'hijack state sample=0 qpc=1000 tid=4 flags=31 hash=12345678 node=A vehicle=B seat=-1\n'+\
          'hijack state sample=1 qpc=1050 tid=4 flags=31 hash=87654321 node=C vehicle=B seat=0\n'+\
          'hijack state END\n'
        result=analyse(text,{'87654321':'S_EXAMPLE'})
        self.assertEqual(result['changes'][1]['native_seat'],0)
        self.assertEqual(result['changes'][1]['seconds'],0.05)
        self.assertEqual(result['changes'][1]['state_label'],'S_EXAMPLE')
        with self.assertRaises(ValueError):analyse(text.replace('count=2','count=3'))
        with self.assertRaises(ValueError):analyse(text.replace('flags=31','flags=15'))
        with self.assertRaises(ValueError):analyse('hijack state BEGIN count=0 overflow=0 qpc_frequency=1000')
    def test_rtpc_values(self):
        b=bytearray(80);b[:4]=b'RTPC';struct.pack_into('<I',b,4,1)
        struct.pack_into('<IIHH',b,8,1,20,2,0)
        struct.pack_into('<IIB',b,20,2,64,3);struct.pack_into('<IIB',b,29,3,0x3f000000,2)
        b[64:69]=b'door\0';j=parse_rtpc(b)
        self.assertEqual(j['nodes'][0]['props'][0]['value'],'door')
        self.assertEqual(j['nodes'][0]['props'][1]['value'],0.5)
        with self.assertRaises(ValueError):parse_rtpc(b[:30])
    def test_rtpc_cycle_and_type(self):
        b=bytearray(40);b[:4]=b'RTPC';struct.pack_into('<I',b,4,1)
        struct.pack_into('<IIHH',b,8,1,8,0,1)
        with self.assertRaises(ValueError):parse_rtpc(b)
        struct.pack_into('<IIHH',b,8,1,20,1,0);struct.pack_into('<IIB',b,20,2,0,99)
        with self.assertRaises(ValueError):parse_rtpc(b)
    def test_adf_nonempty_and_empty_arrays(self):
        b=adf_fixture();j=parse_adf(b)['instances'][0]['value']
        self.assertEqual(j,{'array':[0.5,1.25],'door_count':2})
        struct.pack_into('<I',b,328,0)
        self.assertEqual(parse_adf(b)['instances'][0]['value']['array'],[])
    def test_adf_bounds_and_type(self):
        b=adf_fixture()
        with self.assertRaises(ValueError):parse_adf(b[:340])
        struct.pack_into('<I',b,328,65537)
        with self.assertRaises(ValueError):parse_adf(b)
        b=adf_fixture();struct.pack_into('<I',b,64,6)
        with self.assertRaises(ValueError):parse_adf(b)

if __name__=='__main__':unittest.main()
