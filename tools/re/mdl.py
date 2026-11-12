import os
import struct
class R:
    def __init__(s,d,p=0): s.d=d; s.p=p
    def u(s): v=struct.unpack_from('<i',s.d,s.p)[0]; s.p+=4; return v
    def f(s): v=struct.unpack_from('<f',s.d,s.p)[0]; s.p+=4; return v
    def g(s): v=struct.unpack_from('<d',s.d,s.p)[0]; s.p+=8; return v
    def s(s):
        e=s.d.index(b'\0',s.p); v=s.d[s.p:e].decode('latin1'); s.p=e+1; return v
def bsp(r):
    n=r.u(); idx=[r.u() for _ in range(n)]
    a=r.u(); b=r.u()
    node={'groups':idx}
    if a!=-1: node['l']=bsp(r)
    if b!=-1: node['r']=bsp(r)
    return node
def parse(d):
    r=R(d); crc=r.u(); size=r.u()
    base=r.p; matoff=r.u()
    mr=R(d,base+matoff)
    mats=[]
    n=mr.u()
    for i in range(n):
        c=[mr.f() for _ in range(8)]; fl=mr.u(); t1=mr.s(); t2=mr.s()
        mats.append({'col':c,'flags':fl,'tex':t1,'tex2':t2})
    matend=mr.p
    r.p=base+4
    crc2=r.u(); size2=r.u(); nfr=r.u()
    frames=[]
    for fi in range(nfr):
        fr={'name':r.s(),'u0':r.u(),'m':[r.g() for _ in range(12)]}
        fr['child']=r.u(); fr['sib']=r.u()
        mk=r.u(); fr['mesh']=None
        if mk!=-1:
            m={'name':r.s(),'u':r.u(),'d':[r.g() for _ in range(4)],'f':[r.f() for _ in range(3)]}
            n=r.u(); m['uv']=[(r.f(),r.f()) for _ in range(n)]
            n=r.u(); m['v']=[(r.f(),r.f(),r.f()) for _ in range(n)]
            n=r.u(); m['n']=[(r.f(),r.f(),r.f()) for _ in range(n)]
            ng=r.u(); m['groups']=[]
            if ng:
                nt=r.u()
                for gi in range(ng):
                    g={'id':r.u(),'plane':[r.f() for _ in range(4)]}
                    g['tris']=[(r.u(),r.u(),r.u()) for _ in range(nt)]
                    g['mat']=r.u()
                    m['groups'].append(g)
                    if gi<ng-1: nt=r.u()
            hb=r.u()
            if hb==1: m['bsp']=bsp(r)
            fr['mesh']=m
        frames.append(fr)
    return {'mats':mats,'frames':frames,'end':r.p,'fend':base+4+4+size2,'matoff':base+matoff,'matend':matend,'len':len(d)}
if __name__=='__main__':
    import glob,sys,os,collections
    bad=0; c=collections.Counter()
    for f in sorted(glob.glob(os.environ.get('HG2_SIM','sim')+'/*.M')):
        d=open(f,'rb').read()
        try:
            m=parse(d)
            ok = m['end']==m['matoff'] and m['matend']==len(d)
            if not ok: bad+=1; print(os.path.basename(f),m['end'],m['matoff'],m['matend'],len(d)) if bad<6 else None
        except Exception as e:
            bad+=1; print(os.path.basename(f),'EXC',repr(e)[:80]) if bad<6 else None
    print('bad',bad)
