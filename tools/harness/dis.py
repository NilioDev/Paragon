import sys
from capstone import *
d=open(sys.argv[1],'rb').read(); a=int(sys.argv[2],16); n=int(sys.argv[3])
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB)
for i in md.disasm(d[a:a+n],a): print('%x: %s %s'%(i.address,i.mnemonic,i.op_str))
