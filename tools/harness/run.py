import sys, struct, collections
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from unicorn import *
from unicorn.arm_const import *

SO=sys.argv[1]; BASE=0x10000000; STUB=0x20000000; HEAP=0x30000000; STACK=0x7f000000; ENVB=0x40000000
mu=Uc(UC_ARCH_ARM, UC_MODE_ARM)
f=open(SO,'rb'); elf=ELFFile(f)
hi=max(s['p_vaddr']+s['p_memsz'] for s in elf.iter_segments() if s['p_type']=='PT_LOAD')
mu.mem_map(BASE,(hi+0xfff)&~0xfff)
for s in elf.iter_segments():
    if s['p_type']=='PT_LOAD': mu.mem_write(BASE+s['p_vaddr'], s.data())
for a,sz in ((STUB,0x10000),(HEAP,0x4000000),(STACK-0x100000,0x100000),(ENVB,0x10000)): mu.mem_map(a,sz)
dynsym=elf.get_section_by_name('.dynsym')
imports={}; stubs={}; nstub=[0]
def stub_for(name):
    if name not in imports:
        a=STUB+nstub[0]*4; nstub[0]+=1; imports[name]=a; stubs[a]=name
        mu.mem_write(a,struct.pack('<I',0xe12fff1e))  # bx lr
    return imports[name]
DATA_IMPORTS={'__stack_chk_guard':ENVB+0x8000}
mu.mem_write(ENVB+0x8000,struct.pack('<I',0xdeadbeef))
for sec in elf.iter_sections():
    if not isinstance(sec,RelocationSection): continue
    for r in sec.iter_relocations():
        t=r['r_info_type']; off=BASE+r['r_offset']; cur,=struct.unpack('<I',mu.mem_read(off,4))
        sym=dynsym.get_symbol(r['r_info_sym'])
        if t==23: v=cur+BASE
        elif t in (21,22,2):
            if sym['st_shndx']!='SHN_UNDEF': v=BASE+sym['st_value']+(cur if t==2 else 0)
            else: v=DATA_IMPORTS.get(sym.name) or stub_for(sym.name)
        else: raise Exception('reloc %d'%t)
        mu.mem_write(off,struct.pack('<I',v&0xffffffff))
exports={s.name:BASE+s['st_value'] for s in dynsym.iter_symbols() if s['st_shndx']!='SHN_UNDEF'}
funcs=sorted((s['st_value']&~1,s.name) for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_info']['type']=='STT_FUNC') if elf.get_section_by_name('.symtab') else []
import bisect
def symat(a):
    a-=BASE; i=bisect.bisect_right(funcs,(a,'\xff'))-1
    return '%s+%x'%(funcs[i][1],a-funcs[i][0]) if i>=0 else '?'
# heap
hp=[HEAP+0x1000]; sizes={}
def malloc(n):
    a=(hp[0]+7)&~7; hp[0]=a+max(n,1); sizes[a]=n; return a
def rd(a): return struct.unpack('<I',mu.mem_read(a,4))[0]
def cstr(a):
    b=b''
    while True:
        c=mu.mem_read(a,256); i=c.find(b'\0')
        if i>=0: return (b+bytes(c[:i])).decode('latin1')
        b+=bytes(c); a+=256
def reg(i): return mu.reg_read([UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3][i]) if i<4 else rd(mu.reg_read(UC_ARM_REG_SP)+4*(i-4))
# fake JNIEnv / JavaVM
JNI_N=240
envtab=ENVB+0x100; vmtab=ENVB+0x1000; ENV=ENVB; VM=ENVB+0x10
mu.mem_write(ENV,struct.pack('<I',envtab)); mu.mem_write(VM,struct.pack('<I',vmtab))
for i in range(JNI_N): mu.mem_write(envtab+4*i,struct.pack('<I',stub_for('JNI#%d'%i)))
for i in range(8): mu.mem_write(vmtab+4*i,struct.pack('<I',stub_for('VM#%d'%i)))
calls=collections.Counter(); files={}; nf=[1]
import os
ASSETS=sys.argv[2]
SAVEDIR=os.environ.get('SAVEDIR','/tmp/zen-save')
os.makedirs(SAVEDIR,exist_ok=True)
def fmt(a, argi):
    s=cstr(a); out=''; i=0
    import re
    for m in re.finditer(r'%[-+ #0]*\d*(?:\.\d+)?l*([diuxXscp%])',s): pass
    return s
LASTTEX={}
objs={}; nobj=[0x5000]
def newobj(o):
    nobj[0]+=4; objs[nobj[0]]=o; return nobj[0]
arrmem={}
def callstatic(mid,args):
    m=objs[mid]
    if os.environ.get('JV'): print('   java',m,objs.get(args[0]) if isinstance(objs.get(args[0]),str) else args[0])
    if m=='isAssetExist':
        q=os.path.join(ASSETS,objs[args[0]]); return os.path.getsize(q) if os.path.isfile(q) else 0
    if m=='readAssete':
        pth=os.path.join(ASSETS,objs[args[0]])
        if not os.path.isfile(pth): print('  missing asset',objs[args[0]]); return 0
        b=open(pth,'rb').read(); h=malloc(16+len(b)+8)
        mu.mem_write(h,struct.pack('<IIII',0xC1A55,0,len(b),0)+b); objs[h]=bytearray(b); return h
    if m=='getAbsolueFilePath': return newobj(SAVEDIR)
    print('  JAVA call',m,args); return 0
def jni(i,r):
    if i==6: return newobj('class:'+cstr(r(1)))
    if i in(21,22,23,17): return r(1) if i==21 else 0
    if i==228: return 0
    if i in(113,33): return newobj(cstr(r(2)))
    if i==167: return newobj(cstr(r(1)))
    if i in(115,130,145, 118,121,124,127,133,136,139,142):  # CallStatic*MethodV
        va=r(3); return callstatic(r(2),[rd(va+4*k) for k in range(4)])
    if i in(114,129,144,117,120,123,126,132,135,138,141):  # CallStatic*Method varargs
        return callstatic(r(2),[r(3)]+[reg(4+k) for k in range(3)])
    if i==171: return len(objs[r(1)])
    if i==184:
        o=objs[r(1)]; a=malloc(len(o)); mu.mem_write(a,bytes(o)); arrmem[r(1)]=a
        if r(2): mu.mem_write(r(2),b'\0')
        return a
    if i==192: return 0
    if i==200:
        o=objs[r(1)]; mu.mem_write(reg(4),bytes(o[r(2):r(2)+r(3)])); return 0
    if i==169: a=malloc(len(objs[r(1)])+1); mu.mem_write(a,objs[r(1)].encode('latin1')+b'\0'); return a
    if i==170: return 0
    return None
import re
def cformat(fmt, nextarg):
    out=[]; pos=0
    for m in re.finditer(r'%([-+ #0]*)(\d*)(?:\.(\d+))?(l*|h*)([diuxXscp%])',fmt):
        out.append(fmt[pos:m.start()]); pos=m.end()
        fl,w,pr,ln,c=m.groups()
        if c=='%': out.append('%'); continue
        v=nextarg()
        if c=='s': t=cstr(v) if v else '(null)'
        elif c=='c': t=chr(v&0xff)
        elif c in 'di': v=v-(1<<32) if v&0x80000000 else v; t=str(v)
        elif c=='u': t=str(v)
        elif c in 'xXp': t=format(v,'x' if c!='X' else 'X')
        if pr and c=='s': t=t[:int(pr)]
        if w:
            w=int(w); t=(t.ljust(w) if '-' in fl else (t.rjust(w,'0') if '0' in fl and c!='s' else t.rjust(w)))
        out.append(t)
    out.append(fmt[pos:]); return ''.join(out)
def impl(name):
    r=lambda i: reg(i)
    if name=='malloc': return malloc(r(0))
    if name=='free': return 0
    if name in('memcpy','memmove'):
        mu.mem_write(r(0),bytes(mu.mem_read(r(1),r(2)))); return r(0)
    if name=='memset': mu.mem_write(r(0),bytes([r(1)&0xff])*r(2)); return r(0)
    if name=='strlen': return len(cstr(r(0)))
    if name=='strcpy': mu.mem_write(r(0),cstr(r(1)).encode('latin1')+b'\0'); return r(0)
    if name=='strcat': d=cstr(r(0)); mu.mem_write(r(0)+len(d),cstr(r(1)).encode('latin1')+b'\0'); return r(0)
    if name=='strcmp': a,b=cstr(r(0)),cstr(r(1)); return (a>b)-(a<b) &0xffffffff
    if name=='strncmp': a,b=cstr(r(0))[:r(2)],cstr(r(1))[:r(2)]; return (a>b)-(a<b) &0xffffffff
    if name=='vsprintf':
        va=[r(2)]
        def na():
            v=rd(va[0]); va[0]+=4; return v
        t=cformat(cstr(r(1)),na).encode('latin1'); mu.mem_write(r(0),t+b'\0'); return len(t)
    if name=='strstr':
        h=cstr(r(0)); n=cstr(r(1)); k=h.find(n); return 0 if k<0 else r(0)+k
    if name=='strchr':
        h=cstr(r(0)); k=h.find(chr(r(1)&0xff)); return 0 if k<0 else r(0)+k
    if name=='strncpy':
        b=cstr(r(1)).encode('latin1')[:r(2)]; mu.mem_write(r(0),b+b'\0'*(r(2)-len(b))); return r(0)
    if name=='sqrt':
        import math; x=struct.unpack('<d',struct.pack('<II',r(0),r(1)))[0]; lo,hi=struct.unpack('<II',struct.pack('<d',math.sqrt(max(x,0))))
        mu.reg_write(UC_ARM_REG_R1,hi); return lo
    if name=='atoi':
        try: return int(cstr(r(0)).strip() or 0)&0xffffffff
        except: return 0
    if name=='gettimeofday':
        import time; t=time.time(); mu.mem_write(r(0),struct.pack('<II',int(t),int((t%1)*1e6))); return 0
    if name=='time': import time; return int(time.time())
    if name=='__android_log_print': print('  LOG:',cstr(r(2))[:120]); return 0
    if name in('printf','vprintf'): return 0
    if name=='fopen':
        p=cstr(r(0)); m=cstr(r(1)); 
        lp=p
        try: fh=open(lp,m.replace('t','').replace('b','')+'b')
        except Exception as e: return 0
        h=malloc(16); files[h]=fh; return h
    if name=='fread':
        fh=files[r(3)]; d=fh.read(r(1)*r(2)); mu.mem_write(r(0),d); return len(d)//max(r(1),1)
    if name=='fwrite':
        fh=files[r(3)]; fh.write(bytes(mu.mem_read(r(0),r(1)*r(2)))); return r(2)
    if name=='fclose': files.pop(r(0)).close(); return 0
    if name=='access': return 0 if os.path.exists(cstr(r(0))) else 0xffffffff
    if name=='stat': return 0 if os.path.exists(cstr(r(0))) else 0xffffffff
    if name=='unlink':
        try: os.unlink(cstr(r(0))); return 0
        except: return 0xffffffff
    if name=='rename':
        try: os.rename(cstr(r(0)),cstr(r(1))); return 0
        except: return 0xffffffff
    if name=='localtime':
        import time; t=time.localtime(rd(r(0))); a=ENVB+0x9000
        mu.mem_write(a,struct.pack('<9i',t.tm_sec,t.tm_min,t.tm_hour,t.tm_mday,t.tm_mon-1,t.tm_year-1900,(t.tm_wday+1)%7,t.tm_yday-1,0)); return a
    if name in('glTexSubImage2D','glTexImage2D'):
        sub=name=='glTexSubImage2D'
        if sub: tgt,lvl,x,y,w,h,fm,ty,px=[reg(k) for k in range(9)]
        else: tgt,lvl,ifm,w,h,bd,fm,ty,px=[reg(k) for k in range(9)]; x=y=0
        bpp=2 if ty in(0x8363,0x8033,0x8034) else (3 if fm==0x1907 else 4)
        if px: LASTTEX.update(w=w,h=h,fm=fm,ty=ty,data=bytes(mu.mem_read(px,w*h*bpp)))
        if os.environ.get('GLV'): print('  ',name,hex(fm),hex(ty),x,y,w,h)
        return 0
    if name.startswith('gl'): return 1 if name=='glGenTextures' and mu.mem_write(r(1),struct.pack('<I',calls[name]+1)) is None else 0
    if name.startswith('JNI#'):
        i=int(name[4:])
        R=jni(i,r)
        if R is not None: return R
        # common: 6 FindClass,21 NewGlobalRef,33 GetMethodID,113 GetStaticMethodID,...
        if i in (6,21,33,113,31,94,144): pass
        print('  JNI call',i,[hex(r(k)) for k in range(4)], cstr(r(2)) if i in(113,33) else (cstr(r(1)) if i in (6,167) else ''), cstr(r(3)) if i in(113,33) else '')
        print('   ^ unhandled'); return 0x1234
    if name.startswith('VM#'):
        i=int(name[3:]); print('  VM call',i)
        if i in(4,6): mu.mem_write(r(1),struct.pack('<I',ENV)); return 0
        return 0
    if name=='__cxa_guard_acquire': g=r(0); v=rd(g); return 0 if v&1 else 1
    if name=='__cxa_guard_release': mu.mem_write(r(0),struct.pack('<I',1)); return 0
    print('  UNIMPL',name); return 0
def hook(uc,addr,size,ud):
    name=stubs.get(addr)
    if name is None: return
    calls[name]+=1
    try: v=impl(name)
    except Exception as e:
        import traceback; print('impl err',name,[hex(reg(i)) for i in range(4)],e); uc.emu_stop(); return
    uc.reg_write(UC_ARM_REG_R0,v&0xffffffff)
mu.hook_add(UC_HOOK_CODE,hook,begin=STUB,end=STUB+0x10000)
RET=STUB+0xfff0; mu.mem_write(RET,struct.pack('<I',0xe12fff1e))
icount=[0]
def call(fn,*args,limit=50_000_000):
    a=exports[fn]
    for i,v in enumerate(args[:4]): mu.reg_write([UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3][i],v)
    sp=STACK-0x100; mu.reg_write(UC_ARM_REG_SP,sp); mu.reg_write(UC_ARM_REG_LR,RET)
    try: mu.emu_start(a,RET,count=limit)
    except UcError as e:
        pc=mu.reg_read(UC_ARM_REG_PC); lr=mu.reg_read(UC_ARM_REG_LR)
        print('!! CRASH in',fn,e,'pc=',hex(pc),symat(pc),'lr=',hex(lr),symat(lr))
        print('   regs',[hex(mu.reg_read(x)) for x in [UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,UC_ARM_REG_SP]])
        raise SystemExit
    pc=mu.reg_read(UC_ARM_REG_PC)
    print('<<',fn,'returned' if pc==RET else 'STOPPED at %x'%pc, hex(mu.reg_read(UC_ARM_REG_R0)))
# init_array
for s in elf.iter_sections():
    if s.name in('.init_array','.ctors'): print('has',s.name)
import os as _o
if _o.environ.get('DBG'): exec(open(_o.environ['DBG']).read())
call('JNI_OnLoad',VM,0)
call('Java_com_gamevil_nexus2_Natives_InitializeJNIGlobalRef',ENV,0x99)
call('Java_com_gamevil_nexus2_Natives_NativeInit',ENV,0x99)
call('Java_com_gamevil_nexus2_Natives_NativeResize',ENV,0x99,800,480)
import time
FRAMEDT=float(os.environ.get('FRAMEDT','0'))
for fr in range(int(sys.argv[3]) if len(sys.argv)>3 else 5):
    t=time.time(); calls.clear(); call('Java_com_gamevil_nexus2_Natives_NativeRender',ENV,0x99)
    if fr%25==0: print('frame',fr,'%.2fs'%(time.time()-t),dict(calls.most_common(6)))
    out=os.environ.get('SHOTS')
    if out and LASTTEX and fr%int(os.environ.get('EVERY','50'))==0:
        from PIL import Image
        T=LASTTEX
        if T['ty']==0x8363:
            import array
            a=array.array('H',T['data']); im=Image.new('RGB',(T['w'],T['h'])); im.putdata([(((v>>11)&31)*255//31,((v>>5)&63)*255//63,(v&31)*255//31) for v in a])
        else: im=Image.frombytes('RGBA' if T['fm']==0x1908 else 'RGB',(T['w'],T['h']),T['data'])
        im.save(os.path.join(out,'frame%04d.png'%fr))
