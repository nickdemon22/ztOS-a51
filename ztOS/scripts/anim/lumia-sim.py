# sim.py: ztMETRO's return transform (as in render_frame) applied to the Lumia's tiles, against the measured tracks
import json,math,sys
T=json.load(open('tracks.json'))
CX,CY,W=324.0,540.0,508.0          # the Lumia screen in the cropped capture: centre and width, px
EYE=(740/2)/math.tan(math.radians(17))/360*W   # ztMETRO's eye distance, scaled to this screen
MS_PER_IDX=1000/60/4.9             # the clip is ~4.9x slow motion at 60 fps
LIFE=float(sys.argv[1]) if len(sys.argv)>1 else 190.0
DEG,DEPTH=65.0,0.2
def model(u,x0,w,h,cy,hinge_right):
    fade=math.sqrt(1-u)
    a=math.radians(DEG*math.exp(-3.3*u)*(1-u)**0.3)
    z=-DEPTH*EYE*math.exp(-3.8*u)*(1-u)
    dx=w*(-0.44*math.exp(-16*u)+0.16*math.exp(-2.45*u))*fade
    if hinge_right:
        xh=x0+w+dx; xo=xh-w*math.cos(a); zo=z+w*math.sin(a)   # outer (left) edge comes toward the viewer
    else:
        xh=x0+dx;   xo=xh+w*math.cos(a); zo=z-w*math.sin(a)   # outer (right) edge goes away
    def proj(x,zz):
        s=EYE/(EYE-zz); return CX+(x-CX)*s, s
    ph,sh=proj(xh,z); po,so=proj(xo,zo)
    L,R=(po,ph) if hinge_right else (ph,po); sL,sR=(so,sh) if hinge_right else (sh,so)
    return L,R,h*sL,h*sR
tiles={'office':(334,215,216,468,False,26),'here':(100,219,220,700,True,26),'pink':(98,219,82,865,True,11)}
for name,(x0,w,h,cy,hr,i0) in tiles.items():
    print(name,'(hinge on its %s edge)   measured L R hL hR   |   ztMETRO L R hL hR   |  dL dR'%('right' if hr else 'left'))
    errs=[]
    for i,r in T[name]:
        if r is None or i<i0: continue
        t=(i-i0)*MS_PER_IDX+6; u=min(1,t/LIFE)
        L,R,hL,hR=model(u,x0,w,h,cy,hr)
        errs+= [abs(L-r[0]),abs(R-r[1])]
        print('  %3d ms   %3d %3d %3d %3d   |   %3.0f %3.0f %3.0f %3.0f   |  %+3.0f %+3.0f'%(t,r[0],r[1],r[2],r[3],L,R,hL,hR,L-r[0],R-r[1]))
    print('  mean edge error %.1f px, worst %.0f px (tile width %d px)'%(sum(errs)/len(errs),max(errs),w))
