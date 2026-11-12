import struct,glob,os
from mdl import R
def parse(d):
    r=R(d); crc=r.u(); size=r.u(); n=r.u(); sets=[]
    for si in range(n):
        anims=[]; t=r.u()
        while t==0xd:
            name=r.s(); t=r.u(); keys={}
            while t==0xe:
                kt=r.u(); nk=r.u(); w={0:4,1:3,2:3}.get(kt)
                if w is None: raise Exception(f'keytype {kt} at {r.p}')
                keys[kt]=[(r.u(),[r.f() for _ in range(w)]) for _ in range(nk)]
                t=r.u()
            anims.append((name,keys,t)); t=r.u()
        sets.append((anims,t))
    return sets,r.p,len(d)
if __name__=='__main__':
    for f in sorted(glob.glob(os.environ.get('HG2_SIM','sim')+'/*.XB')):
        try:
            sets,end,ln=parse(open(f,'rb').read())
            import collections
            print(os.path.basename(f),len(sets),end,ln, collections.Counter(t for _,t in sets), len(sets[0][0]), sets[0][0][0][0], {k:len(v) for k,v in sets[0][0][0][1].items()})
        except Exception as e: print(os.path.basename(f),'ERR',e)
