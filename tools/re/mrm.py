import struct,sys
def parse(d):
    p=0
    n=struct.unpack_from('<I',d,p)[0]; p+=4
    texs=[d[p+128*i:p+128*i+128].split(b'\0')[0].decode() for i in range(n)]; p+=128*n
    hdr=struct.unpack_from('<4i',d,p); p+=16
    tiles=[]
    for t in range(128*128+1):
        if t==128*128: break
        f=struct.unpack_from('<21i',d,p); p+=84; extra=d[p:p+32]; p+=32
        cnt=f[17]  # +0x2c is index (0x2c-0x10)/4=7 within fields read starting 0x10
        tiles.append((f,extra,p)); 
        cnt=f[11]
        h=struct.unpack_from(f'<{cnt}H',d,p); p+=2*cnt
        a=struct.unpack_from(f'<{cnt}H',d,p); p+=2*cnt
        tiles[-1]=(f,extra,h,a)
    return texs,hdr,tiles,p
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    texs,hdr,tiles,p=parse(d)
    print(texs,hdr,'end',p,len(d), len(d)-p)
    for t in tiles[:2]+tiles[5000:5001]: print(t[0],t[1].hex(),t[2][:8],t[3][:8])
