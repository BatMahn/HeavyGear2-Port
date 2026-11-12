import os
# Heavy Gear II compiled script (.o) disassembler — instruction set from hg2sim.dll
# (linker FUN_100c6700, descriptor lists at 0x101608b8, handlers FUN_100c8390..FUN_100cb3a0).
import struct,sys,glob
TSIZE=[0,4,4,12,4,4]
NAMES={0:'nop',1:'call',2:'callsym',3:'callnative',4:'ret',5:'retv',6:'movr0',7:'nop7',8:'arg',9:'sp',
 0xa:'jz',0xc:'jnz',0xd:'jmp',0xf:'switch',0x10:'case',0x11:'endswitch',0x12:'mul',0x13:'div',0x14:'mod',
 0x15:'add',0x16:'sub',0x17:'shl',0x18:'shr',0x19:'lt',0x1a:'gt',0x1b:'le',0x1c:'ge',0x1d:'ne',0x1e:'eq',
 0x1f:'band',0x20:'bxor',0x21:'bor',0x22:'land',0x23:'lor',0x24:'store',0x25:'line'}
SPACE=['imp','glob','blk2','fp','sp','reg','?6','?7']
def load(d):
    h=struct.unpack_from('<25I',d,0); blob=d[100:100+h[2]]
    return h,blob
def tsize(t,types):
    return TSIZE[t] if t<6 else types.get(t,0)
def disasm(d,types=None,out=None):
    h,b=load(d); types=types or {}
    # struct types: h[17]/h[18] table {char* (offset), u16 id, u32 size} 12 bytes
    for i in range(h[17]):
        o=h[18]+12*i; tid=struct.unpack_from('<H',b,o+4)[0]; types[tid]=struct.unpack_from('<I',b,o+8)[0]
    p=0; code=h[3]; ins=[]
    def addr(q):
        w,x=struct.unpack_from('<II',b,q); return f'{SPACE[(w>>28)&7]}[{w&0xffff}+{x&0xffff}]:t{(w>>16)&0xfff}', (w>>16)&0xfff
    def imm(q):
        t=struct.unpack_from('<I',b,q)[0]; s=tsize(t,types); raw=b[q+4:q+4+s]
        if t==2: v=f'{struct.unpack_from("<f",raw)[0]:g}f'
        elif t==3: v='vec(%g,%g,%g)'%struct.unpack_from('<3f',raw)
        elif s==4: v=str(struct.unpack_from('<i',raw)[0])
        else: v=raw.hex()
        return f'#t{t}:{v}', 4+s
    while p<code:
        w=struct.unpack_from('<I',b,p)[0]; op=(w>>2)&0x7f; v=w&3; q=p+4; s=''
        if op in (0,7,0x11): pass
        elif op in (1,2,3,5,9,0xd,0x25): s=str(struct.unpack_from('<i',b,q)[0]); q+=4
        elif op==6: s=f'r{b[q]&15}'; q+=1
        elif op in (4,0xa,0xc,0x10):
            n=struct.unpack_from('<i',b,q)[0]; q+=4
            if v==0: t,k=imm(q); q+=k
            else: t,_=addr(q); q+=8
            s=f'{n} {t}'
        elif op==8 or op==0xf:
            if v==0 and op==8: t,k=imm(q); q+=k
            else: t,_=addr(q); q+=8
            s=t
        elif 0x12<=op<=0x23:
            fl=b[q]; q+=1; dst=f'r{fl&15}' if not fl&0x10 else 'inplace'
            if v==0:
                a,_=addr(q); q+=8; t,k=imm(q); q+=k
                s=f'{dst} {"imm,mem" if fl&0x20 else "mem,imm"} {a} {t}'
            else:
                a,_=addr(q); a2,_=addr(q+8); q+=16; s=f'{dst} {a} {a2}'
        elif op==0x24:
            a,_=addr(q); q+=8
            if v==0: t,k=imm(q); q+=k
            else: t,_=addr(q); q+=8
            s=f'{a} <- {t}'
        else:
            raise Exception(f'bad op {op:#x} at {p}')
        ins.append((p,NAMES.get(op,hex(op))+('.m' if v else ''),s)); p=q
    return h,b,ins,p
if __name__=='__main__':
    files=sys.argv[1:] or sorted(glob.glob(os.environ.get('HG2_DATA','.')+'/Scripting/*.o'))
    bad=0
    for f in files:
        d=open(f,'rb').read()
        try:
            h,b,ins,end=disasm(d)
            if end!=h[3]: bad+=1; print(f,'END MISMATCH',end,h[3])
            if len(files)==1:
                strs=[];o=h[20]
                for i in range(h[19]): n=b[o]; strs.append(b[o+1:o+n].rstrip(b'\0').decode('latin1')); o+=1+n
                cs=lambda i: strs[i] if i<len(strs) else '?'
                syms=[]
                for i in range(h[9]):
                    o=h[10]+16*i; syms.append(cs(struct.unpack_from('<I',b,o)[0])+('/%d'%b[o+11] if b[o+10] else ''))
                for p,n,s in ins:
                    if n=='callsym' and 0<=int(s)<len(syms): s+=' '+syms[int(s)]
                    if '#t4:' in s:
                        try: k=int(s.split('#t4:')[1].split()[0]); s+=' "%s"'%cs(k)
                        except: pass
                    print(f'{p:5} {n:10} {s}')
        except Exception as e: bad+=1; print(f,'ERR',e)
    print('files',len(files),'bad',bad)
