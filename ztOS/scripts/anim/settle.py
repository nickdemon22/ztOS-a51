# settle.py DIR: when each Start tile first appears and when it has settled, in the last burst
import sys,glob,os
from PIL import Image,ImageChops,ImageStat
fs=sorted(glob.glob(sys.argv[1]+'/*.ppm'));ts=[int(os.path.basename(f)[:-4]) for f in fs]
b=[[0]]
for i in range(len(ts)-1):
  if ts[i+1]-ts[i]>600: b.append([])
  b[-1].append(i+1)
idx=b[-1]; final=Image.open(fs[idx[-1]]).convert('RGB')
tiles={'Terminal (medium, left)':(14,42,177,205),'Settings (medium, right)':(184,42,346,205),
 'DevTest (medium, left)':(14,212,177,374),'Firefox (medium, right)':(184,212,346,374),
 'Console (medium, left)':(14,381,177,544),
 'small top-left':(184,381,262,459),'small top-right':(269,381,346,459),
 'small bottom-left':(184,466,262,544),'small bottom-right':(269,466,346,544),
 'Phone (medium, left)':(14,551,177,700),'People (medium, right)':(184,551,346,700)}
# only the frames after the black pause before Start returns
black=[i for i in idx if max(ImageStat.Stat(Image.open(fs[i]).convert('RGB').crop((0,40,360,700))).mean)<0.5]
idx=[i for i in idx if i>black[-1]] if black else idx
t0=ts[idx[0]]
print('%-26s %8s %8s %8s'%('tile','appears','settles','takes'))
res=[]
for name,box in tiles.items():
  ref=final.crop(box); app=None; done=None
  for i in idx:
    im=Image.open(fs[i]).convert('RGB').crop(box)
    lum=sum(ImageStat.Stat(im).mean)
    d=sum(ImageStat.Stat(ImageChops.difference(im,ref)).mean)
    if app is None and lum>6: app=ts[i]
    if d<6:
      if done is None: done=ts[i]
    else: done=None
  res.append((name,app,done))
for name,app,done in res:
  print('%-26s %8d %8d %8d'%(name,(app-t0)/5,(done-t0)/5,(done-app)/5))
