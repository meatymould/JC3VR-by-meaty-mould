"""Verify local icon alpha/content and render an opaque QA contact sheet."""
from pathlib import Path
from PIL import Image,ImageDraw
folder=Path(__file__).resolve().parents[1]/'build/weapon-icons'
paths=sorted(folder.glob('*.png'),key=lambda p:int(p.stem))
sheet=Image.new('RGBA',(4*260,((len(paths)+3)//4)*110),(24,28,34,255))
draw=ImageDraw.Draw(sheet)
for i,path in enumerate(paths):
    icon=Image.open(path).convert('RGBA')
    assert icon.size==(232,80)
    amin,amax=icon.getchannel('A').getextrema()
    # BC3 compressed near-transparent backgrounds can round up to alpha2.
    assert amin<=4 and amax>32,path
    x,y=(i%4)*260,(i//4)*110
    sheet.alpha_composite(icon,(x+14,y+20))
    draw.text((x+14,y+4),path.stem,fill='yellow')
sheet.convert('RGB').save(folder/'preview.jpg')
print('verified',len(paths),'icons, transparent backgrounds and nonempty silhouettes')
