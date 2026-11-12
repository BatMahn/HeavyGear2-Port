import struct
def decompress(d):
    out=bytearray(); p=0
    while p<len(d):
        t=d[p]; n=struct.unpack_from('<h',d,p+1)[0]
        if t==0x80: break
        blk=d[p+3:p+n]; p+=n
        if t==0: out+=blk; continue
        i=0
        while i<len(blk):
            b=blk[i]
            if b&0x80:
                off=(((b<<8)|blk[i+1])&0x7ff)+1; cnt=(b>>3)-13; i+=2
                for _ in range(cnt): out.append(out[-off])
            elif b&0x40==0:
                cnt=b+1; out+=blk[i+1:i+1+cnt]; i+=1+cnt
            elif b&0x20:
                off=(((b<<8)|blk[i+1])&0x1fff)+1; cnt=blk[i+2]+4; i+=3
                for _ in range(cnt): out.append(out[-off])
            elif b&0x10==0:
                cnt=(b-0x3d)&0xff; out+=bytes([blk[i+1]])*cnt; i+=2
            elif b&8==0:
                cnt=(((b<<8)|blk[i+1])+0xb041)&0xffff; out+=blk[i+2:i+2+cnt]; i+=2+cnt
            else:
                cnt=(((b<<8)|blk[i+1])+0xa813)&0xffff; out+=bytes([blk[i+2]])*cnt; i+=3
    return bytes(out)
if __name__=='__main__':
    import sys,glob,os
    for f in sorted(glob.glob(sys.argv[1])):
        o=decompress(open(f,'rb').read()); print(os.path.basename(f),len(o))
        if len(sys.argv)>2: open(os.path.join(sys.argv[2],os.path.basename(f)[:-1]+'M'),'wb').write(o)
