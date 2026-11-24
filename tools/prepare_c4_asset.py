"""Prepare ONLY the installed w401 inactive charge for local JC3VR use.

No assets are embedded or redistributed. Exact installed bundle hash is required.
The JC3General stream layout follows the local Gibbed JC3 research reader;
this narrow decoder is independently implemented and rejects other formats.
Normals are recomputed (packed native normals are not decoded). UVs are the
two signed-normalized shorts; authored material effects are not reproduced.
"""
from pathlib import Path
import hashlib
import math
import struct
import sys
from inspect_hud_explosive_assets import unpack_aaf
from extract_weapon_icons import decode

HASH = '97283fd58ccd76f898107619a479b078547cd3667c78dc47cb6a0c36d882c00e'

def charge_mesh(b):
    if b[:9] != b'\x05\0\0\0RBMDL' or struct.unpack_from('<3I',b,9)!=(1,16,0):
        raise ValueError('unsupported model')
    if struct.unpack_from('<I',b,53)[0]!=0x2cec5ad5 or b[57]!=5:
        raise ValueError('unsupported JC3General block')
    bounds=struct.unpack_from('<6f',b,21)
    scale=struct.unpack_from('<f',b,57+41)[0]
    if not 0.01<scale<0.2: raise ValueError('position scale')
    p=57+0x181
    count=struct.unpack_from('<I',b,p)[0];p+=4
    if count!=20: raise ValueError('material count')
    for _ in range(count):
        size=struct.unpack_from('<I',b,p)[0];p+=4
        if size>256 or p+size>len(b): raise ValueError('material string')
        p+=size
    p+=16
    n=struct.unpack_from('<I',b,p)[0];p+=4
    if not 0<n<8192: raise ValueError('vertex count')
    positions=[tuple(x*scale/32767 for x in struct.unpack_from('<4h',b,p+8*i)[:3]) for i in range(n)];p+=8*n
    measured=tuple(min(v[i] for v in positions) for i in range(3))+tuple(max(v[i] for v in positions) for i in range(3))
    if any(abs(a-c)>scale/32767*2 for a,c in zip(bounds,measured)):
        raise ValueError('decoded geometry does not match authored bounds')
    m=struct.unpack_from('<I',b,p)[0];p+=4
    if m!=n: raise ValueError('attribute count')
    uv=[tuple(x/32767 for x in struct.unpack_from('<2h',b,p+20*i)) for i in range(n)];p+=20*n
    if any(not 0<=x<=1 for v in uv for x in v): raise ValueError('UV range')
    k=struct.unpack_from('<I',b,p)[0];p+=4
    if not 0<k<=24576 or k%3 or p+k*2+4!=len(b): raise ValueError('triangle stream length')
    faces=struct.unpack_from('<'+'H'*k,b,p);p+=2*k
    if max(faces)>=n or struct.unpack_from('<I',b,p)[0]!=0x89abcdef: raise ValueError('faces/footer')
    vertices=[]
    for first in range(0,k,3):
        indices=faces[first:first+3];a,c,d=[positions[i] for i in indices]
        u=[c[i]-a[i] for i in range(3)];v=[d[i]-a[i] for i in range(3)]
        normal=(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0])
        length=math.sqrt(sum(x*x for x in normal))
        if length<1e-12: continue
        normal=tuple(x/length for x in normal)
        vertices.extend((*positions[i],*normal,*uv[i]) for i in indices)
    return vertices,measured

def main():
    root,out=map(Path,sys.argv[1:3])
    with (root/'archives_win64/game2.arc').open('rb') as f:
        f.seek(525676544);packed=f.read(2835232)
    if hashlib.sha256(packed).hexdigest()!=HASH: raise ValueError('installed C4 bundle fingerprint mismatch')
    bundle=unpack_aaf(packed)
    vertices,bounds=charge_mesh(bundle[6275740:6275740+65136])
    texture=decode(bundle[5740972:5740972+43832])
    out.mkdir(parents=True,exist_ok=True)
    # Never silently overwrite an existing user's asset cache.
    mesh=out/'c4_inactive.mesh';png=out/'c4_diffuse.png'
    if mesh.exists() or png.exists(): raise FileExistsError('use a fresh output folder')
    mesh.write_bytes(b'JC4M'+struct.pack('<II',1,len(vertices))+b''.join(struct.pack('<8f',*v) for v in vertices))
    texture.save(png)
    print('original charge vertices',len(vertices),'bounds',bounds,'texture',texture.size)
    print('mesh SHA256',hashlib.sha256(mesh.read_bytes()).hexdigest())
    preview(vertices,texture).save(out/'c4_preview.png')

def preview(vertices,texture):
    """Offline textured raster check; not a headset/runtime confirmation."""
    import numpy as np
    from PIL import Image
    albedo=np.asarray(texture)
    panels=[]
    for tilted in [False,True]:
        image=np.full((512,512,4),(28,31,36,255),dtype=np.uint8)
        depth=np.full((512,512),-np.inf)
        for start in range(0,len(vertices),3):
            tri=np.array(vertices[start:start+3]);x,y,z=tri[:,0],tri[:,1],tri[:,2]
            projected=np.column_stack((256+4500*x,256-4500*(z if not tilted else (z*0.65+y*0.65))))
            d=y if not tilted else (y*0.65-z*0.65)
            lo=np.maximum(np.floor(projected.min(axis=0)).astype(int),0)
            hi=np.minimum(np.ceil(projected.max(axis=0)).astype(int),511)
            if np.any(hi<lo): continue
            a,b,c=projected
            den=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
            if abs(den)<1e-9: continue
            xx,yy=np.meshgrid(np.arange(lo[0],hi[0]+1)+0.5,np.arange(lo[1],hi[1]+1)+0.5)
            wa=((b[1]-c[1])*(xx-c[0])+(c[0]-b[0])*(yy-c[1]))/den
            wb=((c[1]-a[1])*(xx-c[0])+(a[0]-c[0])*(yy-c[1]))/den
            wc=1-wa-wb;weights=np.stack((wa,wb,wc),axis=-1)
            dd=weights@d;region=np.s_[lo[1]:hi[1]+1,lo[0]:hi[0]+1]
            mask=(weights.min(axis=-1)>=0)&(dd>depth[region])
            uv=weights@tri[:,6:8]
            tx=np.clip((uv[:,:,0]*(albedo.shape[1]-1)).astype(int),0,albedo.shape[1]-1)
            ty=np.clip((uv[:,:,1]*(albedo.shape[0]-1)).astype(int),0,albedo.shape[0]-1)
            image[region][mask]=albedo[ty,tx][mask];depth[region][mask]=dd[mask]
        panels.append(image)
    return Image.fromarray(np.concatenate(panels,axis=1))

if __name__=='__main__': main()
