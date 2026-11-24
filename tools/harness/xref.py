import sys
from capstone import *
d=open(sys.argv[1],'rb').read(); t=sys.argv[2]
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB); md.skipdata=True
for i in md.disasm(d[0x3c278:0x3c278+0x6a1d8],0x3c278):
    if i.mnemonic in('bl','blx') and i.op_str=='#'+t: print(hex(i.address))
