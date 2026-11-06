import struct, sys
from capstone import *
def load(f):
    d=open(f,'rb').read(); hp=struct.unpack_from('<H',d,8)[0]*16
    lp,pg=struct.unpack_from('<HH',d,2); size=(pg-1)*512+(lp or 512)
    ip,cs=struct.unpack_from('<HH',d,0x14); return d[hp:size],cs,ip
img,cs,ip=load(sys.argv[1]); md=Cs(CS_ARCH_X86,CS_MODE_16); md.detail=False
seen=set(); funcs={(cs,ip)}; work=[(cs,ip)]; insns=0; MN={}; IO={}; ind=0; ints={}
while work:
    s,o=work.pop(); lin=s*16+o
    if lin in seen or lin>=len(img): continue
    while lin<len(img) and lin not in seen:
        seen.add(lin)
        try: i=next(md.disasm(img[lin:lin+16],o))
        except StopIteration: break
        insns+=1; m=i.mnemonic; op=i.op_str
        for b in range(1,i.size): seen.add(lin+b)
        if m=='int': ints[op]=ints.get(op,0)+1
        MN[m]=MN.get(m,0)+1
        if m in('out','in'): IO[op]=IO.get(op,0)+1
        if m in('call','jmp') or m.startswith('j') or m=='loop':
            if op.startswith('0x') and ':' not in op:
                t=int(op,16)&0xffff; work.append((s,t))
                if m=='call': funcs.add((s,t))
            elif ', ' in op and op.startswith('0x') and m in('lcall','ljmp'):
                a,b=op.split(', '); t=(int(a,16)+0,int(b,16)); work.append(t)
                if m.startswith('call') or m=='lcall': funcs.add(t)
            else: ind+=1
        if m in('ret','retf','iret','jmp','ljmp'): break
        o=(o+i.size)&0xffff; lin=s*16+o
print(sys.argv[1],'image',len(img),'reached bytes',len(seen),'insns',insns,'funcs',len(funcs),'indirect jmps/calls',ind)
print(' ints',sorted(ints.items(),key=lambda x:-x[1])[:12])
print(' io',IO)
print(' mn',sorted(MN))
