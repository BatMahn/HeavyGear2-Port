import struct,glob,os,sys
class R:
    def __init__(s,d,p=0,e=None): s.d=d; s.p=p; s.e=len(d) if e is None else e
    def u(s): v=struct.unpack_from('<i',s.d,s.p)[0]; s.p+=4; return v
    def f(s): v=struct.unpack_from('<f',s.d,s.p)[0]; s.p+=4; return v
    def g(s): v=struct.unpack_from('<d',s.d,s.p)[0]; s.p+=8; return v
    def s(s):
        e=s.d.index(b'\0',s.p); v=s.d[s.p:e].decode('latin1'); s.p=e+1; return v
def obj(d,r,user):
    a=r.u(); b=r.u()
    if a>=0 and user: r.u()
    crc=r.u(); start=r.p; size=r.u(); end=start+size
    o={'a':a,'b':b}
    r.u()  # '{'
    t=r.u()
    while t!=1 and r.p<end:
        if t==0xa1 or t==0x14f or t==0x3e: o[{0xa1:'script',0x14f:'walk',0x3e:'name'}[t]]=r.s()
        elif t==0x43:
            r.u(); [r.u() for _ in range(3)]; [r.f() for _ in range(7)]; [r.u() for _ in range(3)]; o['light']=1; r.u()
        elif t==0x3d:
            r.u(); o['pos']=[r.g() for _ in range(3)]; o['rot']=[r.g() for _ in range(3)]
            k=r.u()
            if k==0x42: o['quat']=[r.g() for _ in range(4)]; r.u()
        elif t==0x3c:
            r.u(); o['type']=r.u(); o['def']=r.u(); tm=r.u(); o['team']=tm
            if 99<=tm<=0x6b: r.u()
        else:
            o['unk']=t; break
        t=r.u()
    o['ok']= (r.p==end)
    r.p=end
    return o
def parse(d):
    r=R(d,8); [r.u() for _ in range(6)]; r.s()
    n1=r.u(); b1=[]
    for _ in range(n1): sz=r.u(); b1.append((r.p,sz)); r.p+=sz
    n2=r.u(); b2=[]
    for _ in range(n2): sz=r.u(); b2.append((r.p,sz)); r.p+=sz
    m={'defs':b2,'objs':[],'blocks':{}}
    t=r.u()
    while t!=0 and r.p<len(d):
        if t==0x18: m['title']=r.s(); m['text']=r.s(); r.p+=0x41; r.u(); r.u()
        elif t in (0x1c,0x31,0x32,0x36): r.u(); st=r.p; sz=r.u(); m['blocks'][t]=(st,sz); r.p=st+sz
        elif t==0x33: m['terrain']=r.s()
        elif t in (0x3a,0x3b): m['objs'].append(obj(d,r,t==0x3a))
        elif t==0xdf:
            r.u(); k=r.u()
            while k!=1: k=r.u()
        elif t==0x157: m['msgfile']=r.s()
        else: m['stop']=(t,r.p); break
        t=r.u()
    m['end']=r.p
    return m
if __name__=='__main__':
    for f in sorted(glob.glob(os.environ.get('HG2_DATA','.')+'/Mission/*.ANN')):
        d=open(f,'rb').read(); m=parse(d)
        bad=[o for o in m['objs'] if not o['ok']]
        print(os.path.basename(f), m.get('title'), m.get('terrain'), len(m['objs']), 'bad',len(bad), m.get('stop'), m['end'],len(d), bad[:1])
