#!/usr/bin/env python3
"""Disassemble a function of libmerkava.so by symbol name (substring) or address, with call targets named.
Usage: disfn.py LIB NAME_OR_ADDR [MAXBYTES]"""
import sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM
from elftools.elf.elffile import ELFFile

lib = sys.argv[1]
data = open(lib, 'rb').read()
elf = ELFFile(open(lib, 'rb'))
syms = [s for s in elf.get_section_by_name('.dynsym').iter_symbols() if s.name and s['st_value']]
by_addr = {}
for s in syms:
    by_addr.setdefault(s['st_value'] & ~1, s.name)
q = sys.argv[2]
if q.startswith('0x'):
    a = int(q, 16)
    cand = [s for s in syms if (s['st_value'] & ~1) == (a & ~1)]
    size = cand[0]['st_size'] if cand else 256
    thumb = (a & 1) or (cand and cand[0]['st_value'] & 1)
    a &= ~1
    name = cand[0].name if cand else q
    todo = [(name, a, size, thumb)]
else:
    todo = [(s.name, s['st_value'] & ~1, s['st_size'], s['st_value'] & 1) for s in syms if q in s.name]
if len(sys.argv) > 3:
    todo = [(n, a, int(sys.argv[3]), t) for n, a, _, t in todo]
for name, a, size, thumb in todo:
    print('== %s @%x size %d' % (name, a, size))
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    for i in md.disasm(data[a:a + size], a):
        extra = ''
        if i.mnemonic in ('bl', 'blx', 'b') and i.op_str.startswith('#'):
            t = int(i.op_str[1:], 16)
            if t in by_addr: extra = '   ; ' + by_addr[t]
        if i.mnemonic.startswith('ldr') and 'pc' in i.op_str and '#' in i.op_str:
            off = int(i.op_str.split('#')[-1].rstrip(']'), 16)
            la = ((i.address + 4) & ~3) + off
            v = int.from_bytes(data[la:la + 4], 'little')
            extra = '   ; =%x' % v + (' ' + by_addr[v & ~1] if (v & ~1) in by_addr else '')
        print('%x: %-6s %s%s' % (i.address, i.mnemonic, i.op_str, extra))
