"""Original bounded read-only ADF4 reader for interaction research, not a writer.
Format references: existing Gibbed.JustCause3 AdfFile and deca ff_adf sources.
Unknown types fail explicitly. Pointers are reported, never followed.
"""
import struct

PRIMITIVES={0x580D0A62:'b',0x0ca2821d:'B',0xD13FCF93:'h',0x86d152bd:'H',
 0x192fe633:'i',0x075e4e4f:'I',0xAF41354F:'q',0xA139E01F:'Q',0x7515a207:'f',0xC609F663:'d'}

def parse_adf(data):
    def unpack(fmt,at,source=data):
        if at<0 or at+struct.calcsize(fmt)>len(source):raise ValueError('ADF range')
        return struct.unpack_from(fmt,source,at)
    if data[:4]!=b' FDA':raise ValueError('not ADF')
    version,ni,oi,nt,ot,nh,oh,nn,on,total=unpack('<10I',4)
    if version!=4 or max(ni,nt,nh,nn)>100000 or total>len(data):raise ValueError('ADF header/version/bound')
    lengths=unpack('<'+'B'*nn,on);at=on+nn;names=[]
    for n in lengths:
        raw=unpack(f'<{n+1}s',at)[0];at+=n+1
        if raw[-1:]!=b'\0':raise ValueError('ADF name termination')
        names.append(raw[:-1].decode('utf8'))
    def name(index):
        if index>=len(names):raise ValueError('ADF name index')
        return names[index]
    hashes={};at=oh
    for _ in range(nh):
        end=data.find(b'\0',at,min(at+16385,len(data)))
        if end<0:raise ValueError('ADF hash string bound')
        value=data[at:end].decode('utf8');key=unpack('<Q',end+1)[0];at=end+9;hashes[key]=value
    types={};at=ot
    for _ in range(nt):
        meta,size,align,key,nam,flags,elem,length=unpack('<IIIIQIII',at);at+=36
        t={'name':name(nam),'meta':meta,'size':size,'alignment':align,'flags':flags,'element':elem,'length':length,'members':[]}
        if meta not in (0,1,2,3,4,7,8,9):raise ValueError(f'unsupported ADF metatype {meta}')
        if meta!=0:
            count=unpack('<I',at)[0];at+=4
            if count>100000:raise ValueError('ADF member bound')
            if meta==1:
                for _ in range(count):
                    mn,mtype,msize,offset,default,default_value=unpack('<QIIIIQ',at);at+=32
                    t['members'].append({'name':name(mn),'type':mtype,'size':msize,'offset':offset&0xffffff,'bit':offset>>24})
            elif meta==8:
                for _ in range(count):
                    mn,v=unpack('<QI',at);at+=12;t['members'].append({'name':name(mn),'value':v})
            elif count:raise ValueError('unsupported ADF type payload')
        types[key]=t
    budget=1000000
    def size_of(k):
        if k in PRIMITIVES:return struct.calcsize(PRIMITIVES[k])
        if k==0x8955583e:return 8
        if k not in types:raise ValueError(f'missing ADF type {k:x}')
        return types[k]['size']
    def decode(k,at,buf,depth=0,bit=0):
        nonlocal budget
        budget-=1
        if budget<0 or depth>64:raise ValueError('ADF decode budget/depth')
        size=size_of(k)
        if at<0 or at+size>len(buf):raise ValueError('ADF instance range')
        if k in PRIMITIVES:return unpack('<'+PRIMITIVES[k],at,buf)[0]
        if k==0x8955583e:
            offset,n=unpack('<II',at,buf)
            if offset>=len(buf):raise ValueError('ADF value string offset')
            end=buf.find(b'\0',offset,min(len(buf),offset+16385))
            if end<0:raise ValueError('ADF value string bound')
            return buf[offset:end].decode('utf8')
        t=types[k];meta=t['meta']
        if meta==1:return {m['name']:decode(m['type'],at+m['offset'],buf,depth+1,m['bit']) for m in t['members']}
        if meta in (3,4):
            offset,n=at,t['length']
            if meta==3:offset,flags,n=unpack('<III',at,buf)
            if n>65536:raise ValueError('ADF array count')
            if n==0:return []
            stride=size_of(t['element'])
            if stride<=0 or offset+n*stride>len(buf):raise ValueError('ADF array range/stride')
            return [decode(t['element'],offset+i*stride,buf,depth+1) for i in range(n)]
        if meta==2:return {'unfollowed_pointer':unpack('<Q',at,buf)[0]}
        if meta in (7,8,9):
            if size not in (1,2,4,8):raise ValueError('ADF scalar width')
            v=unpack('<'+{1:'B',2:'H',4:'I',8:'Q'}[size],at,buf)[0]
            if meta==7:return (v>>bit)&1
            if meta==8:return {'value':v,'label':next((m['name'] for m in t['members'] if m['value']==v),None)}
            return {'hash':v,'label':hashes.get(v)}
        raise ValueError(f'unsupported ADF value {meta}/{k:x}')
    instances=[]
    for i in range(ni):
        nhash,key,offset,size,nam=unpack('<IIIIQ',oi+i*24)
        if offset+size>len(data):raise ValueError('ADF instance bound')
        instances.append({'name':name(nam),'type':types.get(key,{}).get('name'),
                          'value':decode(key,0,data[offset:offset+size])})
    return {'version':version,'types':{str(k):v for k,v in types.items()},'instances':instances}
