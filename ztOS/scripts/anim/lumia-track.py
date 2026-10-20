# track.py: per distinct frame, each tile's left/right edge x and the heights of those edges
from PIL import Image,ImageChops,ImageStat;import glob,json
fs=sorted(glob.glob('g/s_*.png'));prev=None;uniq=[]
for i,f in enumerate(fs):
  im=Image.open(f).convert('RGB')
  if prev is None or sum(ImageStat.Stat(ImageChops.difference(im,prev)).mean)>0.25: uniq.append(i)
  prev=im
def near(c,tol):
  return lambda p: abs(p[0]-c[0])<=tol and abs(p[1]-c[1])<=tol and abs(p[2]-c[2])<=tol
tiles={ # name: (colour test, search box x0,y0,x1,y1)
 'phone':(near((255,112,0),14),(60,90,648,380)),
 'office':(near((254,73,0),14),(60,330,648,600)),
 'here':(near((10,63,151),14),(60,560,648,830)),
 'pink':(near((253,70,133),16),(60,800,648,930)),
 'purple':(near((128,54,160),14),(60,800,648,930)),
 'iqiy':(near((159,170,35),14),(60,560,648,720)),
}
out={}
for name,(test,(bx0,by0,bx1,by1)) in tiles.items():
  rows=[]
  for i in uniq:
    im=Image.open(fs[i]).convert('RGB');px=im.load();cols={}
    for x in range(bx0,bx1):
      ys=[y for y in range(by0,by1,2) if test(px[x,y])]
      if len(ys)>=5: cols[x]=(min(ys),max(ys)+2,len(ys))
    if len(cols)<6: rows.append((i,None)); continue
    # the largest run of adjacent columns is the tile
    xs=sorted(cols);runs=[[xs[0]]]
    for x in xs[1:]:
      if x-runs[-1][-1]<=6: runs[-1].append(x)
      else: runs.append([x])
    run=max(runs,key=len);a,b=run[0],run[-1]
    k=max(2,len(run)//12)
    hl=max(cols[x][1]-cols[x][0] for x in run[:k]); hr=max(cols[x][1]-cols[x][0] for x in run[-k:])
    top=min(cols[x][0] for x in run); bot=max(cols[x][1] for x in run)
    rows.append((i,(a,b,hl,hr,top,bot)))
  out[name]=rows
json.dump(out,open('tracks.json','w'))
for name,rows in out.items():
  print(name)
  for i,r in rows:
    print('  f%02d'%i, '-' if r is None else 'L %3d R %3d w %3d  hL %3d hR %3d  top %3d bot %3d'%(r[0],r[1],r[1]-r[0],r[2],r[3],r[4],r[5]))
