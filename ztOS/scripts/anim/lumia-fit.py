import json,math,random
T=json.load(open('tracks.json'))
CX,W=324.0,508.0
EYE=(740/2)/math.tan(math.radians(17))/360*W
MS=1000/60/4.9
tiles={'office':(334,215,216,False,26),'here':(100,219,220,True,26),'pink':(98,219,82,True,11),'purple':(330,219,106,False,11)}
def model(u,x0,w,P,hr):
    life,deg,ka,pa,depth,kz,A,kA,B,kB=P
    u=max(0,min(1,u))
    a=math.radians(deg*math.exp(-ka*u)*(1-u)**pa)
    z=-depth*EYE*math.exp(-kz*u)*(1-u)
    dx=w*(-A*math.exp(-kA*u)+B*math.exp(-kB*u))*math.sqrt(1-u)
    if hr: xh=x0+w+dx; xo=xh-w*math.cos(a); zo=z+w*math.sin(a)
    else:  xh=x0+dx;   xo=xh+w*math.cos(a); zo=z-w*math.sin(a)
    p=lambda x,zz: CX+(x-CX)*EYE/(EYE-zz)
    sc=lambda zz: EYE/(EYE-zz)
    return (p(xo,zo),p(xh,z),sc(zo),sc(z)) if hr else (p(xh,z),p(xo,zo),sc(z),sc(zo))
def cost(P,offs,show=False):
    tot=0;n=0
    for k,(name,(x0,w,h,hr,i0)) in enumerate(tiles.items()):
        for i,r in T[name]:
            if r is None or i<i0: continue
            t=(i-i0)*MS+offs[k]; L,R,sL,sR=model(t/P[0],x0,w,P,hr)
            wgt=0.3 if (name=='office' and i==26) else 1.0   # one frame, possibly blended
            tot+=wgt*((L-r[0])**2+(R-r[1])**2); n+=2*wgt
            if name in ('office','here'):   # their edge heights are clean: they give the depth
                tot+=wgt*((h*sL-r[2])**2+(h*sR-r[3])**2); n+=2*wgt
            if show: print('  %-7s %3d ms  meas %3d %3d  h %3d %3d | fit %3.0f %3.0f  h %3.0f %3.0f | err %+3.0f %+3.0f'%(name,t,r[0],r[1],r[2],r[3],L,R,h*sL,h*sR,L-r[0],R-r[1]))
    return math.sqrt(tot/n)
P=[190,65,3.3,0.3,0.2,3.8,0.44,16,0.16,2.45]; offs=[6,6,6,45]
lo=[150,50,1.5,0.0,0.05,1.0,0.1,6,0.05,0.8]; hi=[300,89,6,1.0,0.4,8,0.9,40,0.4,5]
best=cost(P,offs); random.seed(1)
for it in range(60000):
    Q=P[:];O=offs[:]
    j=random.randrange(len(P)+len(offs)); sc=0.25 if it<20000 else 0.06
    if j<len(P): Q[j]=min(hi[j],max(lo[j],Q[j]*(1+random.gauss(0,sc))))
    else: O[j-len(P)]=min(80,max(0,O[j-len(P)]+random.gauss(0,8 if it<20000 else 2)))
    c=cost(Q,O)
    if c<best: best,P,offs=c,Q,O
names=['life_ms','deg','k_angle','p_angle','depth','k_depth','A_left','k_A','B_over','k_B']
print('rms edge error %.1f px'%best)
print({n:round(v,3) for n,v in zip(names,P)}); print('time offsets ms',[round(o) for o in offs])
cost(P,offs,True)
json.dump({'P':P,'offs':offs},open('fit.json','w'))
