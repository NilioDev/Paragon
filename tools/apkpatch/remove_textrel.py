import struct,sys
p=sys.argv[1]; d=bytearray(open(p,'rb').read())
phoff,=struct.unpack_from('<I',d,28); phentsz,phnum=struct.unpack_from('<HH',d,42)
dyn=None
for i in range(phnum):
    o=phoff+i*phentsz
    t,off,va,pa,fs,ms,fl,al=struct.unpack_from('<8I',d,o)
    if t==1 and fl==5: struct.pack_into('<I',d,o+24,7); print('LOAD',i,'R E -> RWE')
    if t==2: dyn=(off,fs)
off,fs=dyn; ents=[struct.unpack_from('<iI',d,off+k) for k in range(0,fs,8)]
new=[e for e in ents if e[0]!=0x16 and e[0]!=0]
if any(e[0]==30 for e in new): raise SystemExit('DT_FLAGS present, handle manually')
new+= [(0,0)]*(len(ents)-len(new))
for k,e in enumerate(new): struct.pack_into('<iI',d,off+k*8,*e)
print('removed TEXTREL' if len([e for e in ents if e[0]==0x16]) else 'no textrel')
open(p,'wb').write(d)
