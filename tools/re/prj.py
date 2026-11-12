import struct,zlib,os,sys
def load(prjpath,mappath):
    m=open(mappath,'rb').read(); d=open(prjpath,'rb').read()
    cnt,nt=struct.unpack_from('<II',m,0); p=8+8*nt
    p+=4; nd=struct.unpack_from('<I',m,p)[0]; p+=4+16*nd
    ents={}
    for k in range(cnt):
        name,ext,off,comp=struct.unpack_from('<8s4sI4s',m,p+k*20)
        n=name.split(b'\0')[0].decode('latin1'); e=ext.split(b'\0')[0].decode('latin1')
        ents[(n+'.'+e).upper()]=off
    return d,ents
def get(d,off):
    assert d[off:off+7]==b'C:\\HG2\\', d[off:off+12]
    comp,us,cs=struct.unpack_from('<HII',d,off+12)
    data=d[off+22:off+22+cs]
    if comp==1: data=zlib.decompress(data)
    assert len(data)==us,(len(data),us)
    return data
if __name__=='__main__':
    d,ents=load(sys.argv[1],sys.argv[2]); out=sys.argv[3]; os.makedirs(out,exist_ok=True)
    for k,o in ents.items(): open(os.path.join(out,k),'wb').write(get(d,o))
    print(len(ents))
