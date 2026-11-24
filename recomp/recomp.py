#!/usr/bin/env python3
"""Static recompiler: ARMv5TE / Thumb-1 shared library -> portable C.

Usage: recomp.py libmerkava.so OUTDIR

The output keeps a flat 32-bit guest address space: guest address A lives at host G + A.
Every guest function becomes a C function operating on global CPU state (rt.h).
Indirect control flow is resolved at run time (see the __ind block emitted per function).
"""
import os
import struct
import sys
from collections import defaultdict

from capstone import (CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs)
from capstone import arm as A
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

GBASE = 0x10000000          # where the library image lives in guest memory
IMP_BASE = 0x0F000000       # fake addresses for imported functions
DATA_IMP = 0x0F100000       # storage for imported data objects (e.g. __stack_chk_guard)

# libgcc helpers that are replaced by host implementations (rt_hle.c), keyed by symbol name.
HLE = {
    '__aeabi_uidiv', '__aeabi_uidivmod', '__aeabi_idiv', '__aeabi_idivmod', '__aeabi_ldivmod',
    '__aeabi_uldivmod', '__divdi3', '__udivdi3', '__aeabi_ldiv0', '__aeabi_idiv0',
    '__aeabi_drsub', '__aeabi_dsub', '__aeabi_dadd', '__floatunsidf', '__aeabi_i2d', '__extendsfdf2',
    '__aeabi_ul2d', '__aeabi_l2d', '__aeabi_dmul', '__aeabi_ddiv', '__aeabi_d2iz', '__truncdfsf2',
    '__aeabi_frsub', '__aeabi_fsub', '__addsf3', '__floatunsisf', '__aeabi_i2f', '__floatundisf',
    '__floatdisf', '__mulsf3', '__divsf3', '__gtsf2', '__ltsf2', '__nesf2', '__aeabi_cfrcmple',
    '__aeabi_cfcmpeq', '__aeabi_fcmpeq', '__aeabi_fcmplt', '__aeabi_fcmple', '__aeabi_fcmpge',
    '__aeabi_fcmpgt', '__aeabi_f2iz', '__gnu_uldivmod_helper', '__gnu_ldivmod_helper',
}
# ARM-mode runtime code we never expect to run (C++ unwinder): replaced by an abort.
ABORT_PREFIXES = ('_Unwind', '__gnu_Unwind', '___Unwind', '__gnu_unwind', '__aeabi_unwind',
                  'unwind_', 'get_eit_entry', 'search_EIT_table', 'selfrel_offset31',
                  'restore_non_core_regs', '__restore_core_regs', 'next_unwind_byte')

COND = {
    A.ARM_CC_EQ: 'FZ', A.ARM_CC_NE: '!FZ', A.ARM_CC_HS: 'FC', A.ARM_CC_LO: '!FC',
    A.ARM_CC_MI: 'FN', A.ARM_CC_PL: '!FN', A.ARM_CC_VS: 'FV', A.ARM_CC_VC: '!FV',
    A.ARM_CC_HI: '(FC&&!FZ)', A.ARM_CC_LS: '(!FC||FZ)', A.ARM_CC_GE: '(FN==FV)',
    A.ARM_CC_LT: '(FN!=FV)', A.ARM_CC_GT: '(!FZ&&FN==FV)', A.ARM_CC_LE: '(FZ||FN!=FV)',
}


class Image:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.elf = ELFFile(self.f)
        hi = max(s['p_vaddr'] + s['p_memsz'] for s in self.elf.iter_segments() if s['p_type'] == 'PT_LOAD')
        self.size = (hi + 0xfff) & ~0xfff
        self.mem = bytearray(self.size)
        self.filesz_end = 0
        for s in self.elf.iter_segments():
            if s['p_type'] == 'PT_LOAD':
                d = s.data()
                self.mem[s['p_vaddr']:s['p_vaddr'] + len(d)] = d
        self.imports = []          # names, index -> IMP_BASE + 4*i
        self.data_imports = {}
        self._relocate()
        self._symbols()

    def u32(self, off):
        return struct.unpack_from('<I', self.mem, off)[0]

    def _imp(self, name):
        if name == '__stack_chk_guard':
            self.data_imports[name] = DATA_IMP
            return DATA_IMP
        if name not in self.imports:
            self.imports.append(name)
        return IMP_BASE + 4 * self.imports.index(name)

    def _relocate(self):
        dynsym = self.elf.get_section_by_name('.dynsym')
        for sec in self.elf.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            for r in sec.iter_relocations():
                t = r['r_info_type']
                off = r['r_offset']
                cur = self.u32(off)
                sym = dynsym.get_symbol(r['r_info_sym'])
                if t == 23:                                  # R_ARM_RELATIVE
                    v = cur + GBASE
                elif t in (21, 22, 2):                       # GLOB_DAT, JUMP_SLOT, ABS32
                    if sym['st_shndx'] != 'SHN_UNDEF':
                        v = GBASE + sym['st_value'] + (cur if t == 2 else 0)
                    else:
                        v = self._imp(sym.name) + (cur if t == 2 else 0)
                else:
                    raise SystemExit('unsupported relocation type %d' % t)
                struct.pack_into('<I', self.mem, off, v & 0xffffffff)

    def _symbols(self):
        st = self.elf.get_section_by_name('.symtab') or self.elf.get_section_by_name('.dynsym')
        text = self.elf.get_section_by_name('.text')
        self.text_lo = text['sh_addr']
        self.text_hi = text['sh_addr'] + text['sh_size']
        self.maps = []              # (addr, 't'|'a'|'d')
        self.funcs = {}             # addr(even) -> dict(name, size, thumb)
        self.names = defaultdict(list)
        for s in st.iter_symbols():
            n = s.name
            if n in ('$a', '$t', '$d') or n[:3] in ('$a.', '$t.', '$d.'):
                if self.text_lo <= s['st_value'] < self.text_hi:
                    self.maps.append((s['st_value'], n[1]))
            elif s['st_info']['type'] == 'STT_FUNC' and self.text_lo <= (s['st_value'] & ~1) < self.text_hi:
                a = s['st_value'] & ~1
                self.names[a].append(n)
                f = self.funcs.setdefault(a, {'name': n, 'size': 0, 'thumb': bool(s['st_value'] & 1)})
                f['size'] = max(f['size'], s['st_size'])
        self.maps.sort()
        dyn = self.elf.get_section_by_name('.dynsym')
        self.exports = {}
        for s in dyn.iter_symbols():
            if s['st_shndx'] != 'SHN_UNDEF' and s['st_info']['type'] == 'STT_FUNC':
                self.exports[s.name] = s['st_value']

    def mode_at(self, a):
        lo, hi = 0, len(self.maps)
        while lo < hi:
            m = (lo + hi) // 2
            if self.maps[m][0] <= a:
                lo = m + 1
            else:
                hi = m
        return self.maps[lo - 1][1] if lo else 't'

    def region_end(self, a):
        """End of the code region (next mapping symbol of a different kind or end of text)."""
        for addr, k in self.maps:
            if addr > a and k == 'd':
                return addr
        return self.text_hi


def fname(a):
    return 'F_%06x' % a


class FuncGen:
    def __init__(self, rc, start, end, thumb, name):
        self.rc, self.img = rc, rc.img
        self.start, self.end, self.thumb, self.name = start, end, thumb, name
        self.out = []
        self.labels = set()
        self.insns = []

    # ---------- operand helpers ----------
    def pcval(self, ins):
        return ins.address + (4 if self.thumb else 8) + GBASE

    def reg(self, ins, r, align_pc=False):
        if r == A.ARM_REG_PC:
            v = self.pcval(ins)
            if align_pc:
                v &= ~3
            return '0x%xu' % v
        return 'R[%d]' % self.rn(r)

    @staticmethod
    def rn(r):
        m = {A.ARM_REG_R0: 0, A.ARM_REG_R1: 1, A.ARM_REG_R2: 2, A.ARM_REG_R3: 3, A.ARM_REG_R4: 4,
             A.ARM_REG_R5: 5, A.ARM_REG_R6: 6, A.ARM_REG_R7: 7, A.ARM_REG_R8: 8, A.ARM_REG_R9: 9,
             A.ARM_REG_R10: 10, A.ARM_REG_R11: 11, A.ARM_REG_R12: 12, A.ARM_REG_SP: 13,
             A.ARM_REG_LR: 14, A.ARM_REG_PC: 15}
        return m[r]

    def shifted(self, ins, op, want_carry):
        """Return (expr, carry_expr_or_None) for a register/immediate operand with optional shift."""
        if op.type == A.ARM_OP_IMM:
            v = op.imm & 0xffffffff
            carry = None
            if want_carry and not self.thumb and v > 0xff:
                carry = str(v >> 31)       # ARM rotated immediate: C = bit 31
            return '0x%xu' % v, carry
        base = self.reg(ins, op.reg)
        st, sv = op.shift.type, op.shift.value
        if st == A.ARM_SFT_INVALID or (st == A.ARM_SFT_LSL and sv == 0):
            return base, None
        kind = {A.ARM_SFT_LSL: 'LSL', A.ARM_SFT_LSR: 'LSR', A.ARM_SFT_ASR: 'ASR', A.ARM_SFT_ROR: 'ROR',
                A.ARM_SFT_RRX: 'RRX', A.ARM_SFT_LSL_REG: 'LSL', A.ARM_SFT_LSR_REG: 'LSR',
                A.ARM_SFT_ASR_REG: 'ASR', A.ARM_SFT_ROR_REG: 'ROR', A.ARM_SFT_RRX_REG: 'RRX'}[st]
        if st >= A.ARM_SFT_ASR_REG:
            amt = '(R[%d]&0xff)' % self.rn(sv)
        else:
            amt = str(sv)
        if kind == 'RRX':
            return '((%s>>1)|(FC<<31))' % base, '(%s&1)' % base
        if want_carry:
            self.out.append('  { uint32_t sc_; uint32_t sv_ = SH_%s_C(%s, %s, &sc_);' % (kind, base, amt))
            return 'sv_', 'sc_'
        return 'SH_%s(%s, %s)' % (kind, base, amt), None

    def memaddr(self, ins, mem_op, post_op=None):
        m = mem_op.mem
        base = self.reg(ins, m.base, align_pc=True)
        if m.index != 0:
            idx = 'R[%d]' % self.rn(m.index)
            if mem_op.shift.type != A.ARM_SFT_INVALID and mem_op.shift.value:
                idx = 'SH_%s(%s, %d)' % ({A.ARM_SFT_LSL: 'LSL', A.ARM_SFT_LSR: 'LSR', A.ARM_SFT_ASR: 'ASR',
                                          A.ARM_SFT_ROR: 'ROR'}[mem_op.shift.type], idx, mem_op.shift.value)
            off = ('-' if mem_op.subtracted else '+') + idx
        else:
            off = ('%+d' % m.disp) if m.disp else ''
        return base, off

    # ---------- emit ----------
    def emit(self, s):
        self.out.append(s)

    def jump_ind(self, expr):
        self.emit('  { T = %s; goto __ind; }' % expr)

    def call_direct(self, target, lr):
        tgt = target & ~1
        self.emit('  R[14] = 0x%xu; %s(); if (PEND) { T = PEND; PEND = 0; goto __ind; }' % (lr, self.rc.callee(tgt)))

    def branch(self, target):
        tgt = target & ~1
        if self.start <= tgt < self.end and tgt in self.rc.insn_addrs_of[self.start]:
            self.labels.add(tgt)
            return 'goto L_%x;' % tgt
        # tail jump to another function (or into one)
        self.rc.want_entry(tgt, self.rc.img.mode_at(tgt) == 't')
        return '{ %s(); if (PEND) { T = PEND; PEND = 0; goto __ind; } return; }' % self.rc.callee(tgt)

    def gen(self):
        rc = self.rc
        for ins in self.insns:
            a = ins.address
            self.emit('L_%x:' % a)
            try:
                self.translate(ins)
            except Exception as e:
                self.emit('  /* %s %s */ rt_unimpl(0x%xu, "%s %s");' % (ins.mnemonic, ins.op_str, a + GBASE,
                                                                        ins.mnemonic, ins.op_str))
                rc.unimpl[ins.mnemonic] += 1
        # fall off the end of the function: continue in whatever code follows
        nxt = self.end
        self.emit('  T = 0x%xu; goto __ind;' % ((nxt + GBASE) | (1 if self.thumb else 0)))
        body = self.out
        head = ['void %s(void) { /* %s */' % (fname(self.start), self.name),
                '  const uint32_t ret = R[14]; uint32_t T;']
        tail = ['__ind:',
                '  if (T == ret) return;',
                '  switch ((T & ~1u) - 0x%xu) {' % (GBASE + self.start)]
        for ins in self.insns:
            tail.append('    case 0x%x: goto L_%x;' % (ins.address - self.start, ins.address))
        tail.append('  }')
        tail.append('  rt_jump(T); return;')
        tail.append('}')
        return head + body + tail

    def translate(self, ins):
        I = ins.id
        ops = list(ins.operands)
        cc = ins.cc
        a = ins.address
        nxt = a + ins.size
        cond = COND.get(cc)
        if cond:
            self.emit('  if (%s) {' % cond)
        self._translate(ins, I, ops, nxt)
        if cond:
            self.emit('  }')

    def write_reg(self, ins, rd, expr):
        if rd == A.ARM_REG_PC:
            self.jump_ind(expr)
        else:
            self.emit('  R[%d] = %s;' % (self.rn(rd), expr))

    def _translate(self, ins, I, ops, nxt):
        S = ins.update_flags
        thumb = self.thumb
        lr_val = (nxt + GBASE) | (1 if thumb else 0)
        # ---- branches ----
        if I == A.ARM_INS_B:
            self.emit('  ' + self.branch(ops[0].imm))
            return
        if I in (A.ARM_INS_BL, A.ARM_INS_BLX):
            if ops[0].type == A.ARM_OP_IMM:
                tgt = ops[0].imm & ~1
                to_thumb = (I == A.ARM_INS_BL) == thumb
                self.rc.want_entry(tgt, to_thumb)
                self.call_direct(tgt, lr_val)
            else:
                self.emit('  { uint32_t t_ = R[%d]; R[14] = 0x%xu; rt_call(t_); if (PEND) { T = PEND; PEND = 0; goto __ind; } }'
                          % (self.rn(ops[0].reg), lr_val))
            return
        if I == A.ARM_INS_BX:
            self.jump_ind(self.reg(ins, ops[0].reg))
            return
        # ---- loads / stores ----
        LD = {A.ARM_INS_LDR: ('RD32', 0), A.ARM_INS_LDRB: ('RD8', 0), A.ARM_INS_LDRH: ('RD16', 0),
              A.ARM_INS_LDRSB: ('RDS8', 0), A.ARM_INS_LDRSH: ('RDS16', 0)}
        ST = {A.ARM_INS_STR: 'WR32', A.ARM_INS_STRB: 'WR8', A.ARM_INS_STRH: 'WR16'}
        if I in LD or I in ST or I in (A.ARM_INS_LDRD, A.ARM_INS_STRD):
            dual = I in (A.ARM_INS_LDRD, A.ARM_INS_STRD)
            regs = ops[:2] if dual else ops[:1]
            mop = ops[2] if dual else ops[1]
            post = ops[3] if dual and len(ops) > 3 else (ops[2] if not dual and len(ops) > 2 else None)
            base, off = self.memaddr(ins, mop)
            m = mop.mem
            # constant PC-relative load: fold to the literal value
            if I == A.ARM_INS_LDR and m.base == A.ARM_REG_PC and m.index == 0 and post is None:
                addr = ((self.pcval(ins) & ~3) + m.disp) - GBASE
                val = self.img.u32(addr)
                self.write_reg(ins, ops[0].reg, '0x%xu' % val)
                return
            if post is not None:
                pv = self.shifted(ins, post, False)[0]
                sign = '-' if post.subtracted else '+'
                ea = base
                wb = '  R[%d] = %s %s %s;' % (self.rn(m.base), base, sign, pv)
            else:
                ea = '(%s%s)' % (base, off) if off else base
                wb = ('  R[%d] = ea_;' % self.rn(m.base)) if ins.writeback else None
            self.emit('  { uint32_t ea_ = %s;' % ea)
            if I in LD:
                fn = LD[I][0]
                if ops[0].reg == A.ARM_REG_PC:
                    self.emit('    uint32_t v_ = %s(ea_);' % fn)
                    if wb:
                        self.emit('  ' + wb)
                    self.emit('    T = v_; }')
                    self.emit('  goto __ind;')
                    return
                self.emit('    uint32_t v_ = %s(ea_);' % fn)
                if wb:
                    self.emit('  ' + wb)
                self.emit('    R[%d] = v_; }' % self.rn(ops[0].reg))
            elif I in ST:
                self.emit('    %s(ea_, %s);' % (ST[I], self.reg(ins, ops[0].reg)))
                if wb:
                    self.emit('  ' + wb)
                self.emit('  }')
            elif I == A.ARM_INS_LDRD:
                self.emit('    uint32_t a_ = RD32(ea_), b_ = RD32(ea_+4);')
                if wb:
                    self.emit('  ' + wb)
                self.emit('    R[%d] = a_; R[%d] = b_; }' % (self.rn(ops[0].reg), self.rn(ops[1].reg)))
            else:
                self.emit('    WR32(ea_, %s); WR32(ea_+4, %s);' % (self.reg(ins, ops[0].reg), self.reg(ins, ops[1].reg)))
                if wb:
                    self.emit('  ' + wb)
                self.emit('  }')
            return
        # ---- multiple ----
        if I in (A.ARM_INS_PUSH, A.ARM_INS_POP, A.ARM_INS_LDM, A.ARM_INS_STM, A.ARM_INS_STMDB, A.ARM_INS_LDMDB,
                 A.ARM_INS_LDMIB, A.ARM_INS_STMIB, A.ARM_INS_LDMDA, A.ARM_INS_STMDA):
            if I in (A.ARM_INS_PUSH, A.ARM_INS_POP):
                base = 13
                regs = [self.rn(o.reg) for o in ops]
                wb = True
                kind = 'db' if I == A.ARM_INS_PUSH else 'ia'
                load = I == A.ARM_INS_POP
            else:
                base = self.rn(ops[0].reg)
                regs = [self.rn(o.reg) for o in ops[1:]]
                wb = ins.writeback
                load = I in (A.ARM_INS_LDM, A.ARM_INS_LDMDB, A.ARM_INS_LDMIB, A.ARM_INS_LDMDA)
                kind = {A.ARM_INS_LDM: 'ia', A.ARM_INS_STM: 'ia', A.ARM_INS_STMDB: 'db', A.ARM_INS_LDMDB: 'db',
                        A.ARM_INS_LDMIB: 'ib', A.ARM_INS_STMIB: 'ib', A.ARM_INS_LDMDA: 'da', A.ARM_INS_STMDA: 'da'}[I]
            regs = sorted(regs)
            n = len(regs)
            start = {'ia': 'R[%d]' % base, 'ib': 'R[%d]+4' % base,
                     'db': 'R[%d]-%d' % (base, 4 * n), 'da': 'R[%d]-%d' % (base, 4 * n - 4)}[kind]
            newb = {'ia': 'R[%d]+%d', 'ib': 'R[%d]+%d', 'db': 'R[%d]-%d', 'da': 'R[%d]-%d'}[kind] % (base, 4 * n)
            self.emit('  { uint32_t ea_ = %s, nb_ = %s;' % (start, newb))
            if load:
                for i, r in enumerate(regs):
                    if r == 15:
                        self.emit('    T = RD32(ea_+%d);' % (4 * i))
                    elif r == base and wb:
                        continue
                    else:
                        self.emit('    R[%d] = RD32(ea_+%d);' % (r, 4 * i))
                if wb and base not in regs:
                    self.emit('    R[%d] = nb_;' % base)
                elif wb and base in regs:
                    pass
                self.emit('  }')
                if 15 in regs:
                    self.emit('  goto __ind;')
            else:
                for i, r in enumerate(regs):
                    self.emit('    WR32(ea_+%d, %s);' % (4 * i, '0x%xu' % (self.pcval(ins) + 4) if r == 15 else 'R[%d]' % r))
                if wb:
                    self.emit('    R[%d] = nb_;' % base)
                self.emit('  }')
            return
        # ---- data processing ----
        if I in (A.ARM_INS_NOP,):
            return
        if I in (A.ARM_INS_MOV, A.ARM_INS_MVN, A.ARM_INS_LSL, A.ARM_INS_LSR, A.ARM_INS_ASR, A.ARM_INS_ROR,
                 A.ARM_INS_RRX):
            rd = ops[0].reg
            if I in (A.ARM_INS_MOV, A.ARM_INS_MVN):
                v, c = self.shifted(ins, ops[1], S)
                close = v == 'sv_'
                if I == A.ARM_INS_MVN:
                    v = '(~%s)' % v
            elif I == A.ARM_INS_RRX:
                src = self.reg(ins, ops[1].reg)
                v, c, close = '((%s>>1)|(FC<<31))' % src, '(%s&1)' % src, False
            else:
                kind = {A.ARM_INS_LSL: 'LSL', A.ARM_INS_LSR: 'LSR', A.ARM_INS_ASR: 'ASR', A.ARM_INS_ROR: 'ROR'}[I]
                if len(ops) == 3:
                    src = self.reg(ins, ops[1].reg)
                    amt = str(ops[2].imm) if ops[2].type == A.ARM_OP_IMM else '(R[%d]&0xff)' % self.rn(ops[2].reg)
                else:
                    src = self.reg(ins, ops[0].reg)
                    amt = str(ops[1].imm) if ops[1].type == A.ARM_OP_IMM else '(R[%d]&0xff)' % self.rn(ops[1].reg)
                if ops[-1].type == A.ARM_OP_IMM and int(amt) == 0 and kind == 'LSL':
                    v, c, close = src, None, False
                else:
                    if S:
                        self.emit('  { uint32_t sc_; uint32_t sv_ = SH_%s_C(%s, %s, &sc_);' % (kind, src, amt))
                        v, c, close = 'sv_', 'sc_', True
                    else:
                        v, c, close = 'SH_%s(%s, %s)' % (kind, src, amt), None, False
            if S:
                self.emit('  { uint32_t r_ = %s; FN = r_>>31; FZ = r_==0;%s' % (v, (' FC = %s;' % c) if c else ''))
                self.write_reg(ins, rd, 'r_')
                self.emit('  }')
            else:
                self.write_reg(ins, rd, v)
            if close:
                self.emit('  }')
            return
        ARITH = {A.ARM_INS_ADD: 'ADD', A.ARM_INS_ADC: 'ADC', A.ARM_INS_SUB: 'SUB', A.ARM_INS_SBC: 'SBC',
                 A.ARM_INS_RSB: 'RSB', A.ARM_INS_RSC: 'RSC', A.ARM_INS_CMP: 'CMP', A.ARM_INS_CMN: 'CMN'}
        LOGIC = {A.ARM_INS_AND: '&', A.ARM_INS_ORR: '|', A.ARM_INS_EOR: '^', A.ARM_INS_BIC: '&~',
                 A.ARM_INS_TST: '&', A.ARM_INS_TEQ: '^'}
        if I in ARITH or I in LOGIC:
            test = I in (A.ARM_INS_CMP, A.ARM_INS_CMN, A.ARM_INS_TST, A.ARM_INS_TEQ)
            if test:
                rd, x, yop = None, ops[0], ops[1]
            elif len(ops) == 3:
                rd, x, yop = ops[0].reg, ops[1], ops[2]
            else:
                rd, x, yop = ops[0].reg, ops[0], ops[1]
            # add rX, pc  (thumb high-reg form, unaligned pc) vs adr (aligned)
            xs = self.reg(ins, x.reg, align_pc=(yop.type == A.ARM_OP_IMM)) if x.type == A.ARM_OP_REG else '0x%xu' % (x.imm & 0xffffffff)
            want_c = I in LOGIC and (S or test)
            ys, yc = self.shifted(ins, yop, want_c)
            close = ys == 'sv_'
            setf = S or test
            if I in LOGIC:
                expr = '(%s %s %s)' % (xs, LOGIC[I], ys)
                if setf:
                    self.emit('  { uint32_t r_ = %s; FN = r_>>31; FZ = r_==0;%s' % (expr, (' FC = %s;' % yc) if yc else ''))
                    if not test:
                        self.write_reg(ins, rd, 'r_')
                    self.emit('  }')
                else:
                    self.write_reg(ins, rd, expr)
            else:
                op = ARITH[I]
                if op in ('CMP', 'CMN'):
                    self.emit('  %s(%s, %s);' % ('SUBS' if op == 'CMP' else 'ADDS', xs, ys))
                elif setf:
                    fn = {'ADD': 'ADDS(%s,%s)', 'ADC': 'ADCS(%s,%s)', 'SUB': 'SUBS(%s,%s)', 'SBC': 'SBCS(%s,%s)',
                          'RSB': 'SUBS(%s,%s)', 'RSC': 'SBCS(%s,%s)'}[op]
                    args = (ys, xs) if op in ('RSB', 'RSC') else (xs, ys)
                    self.write_reg(ins, rd, fn % args)
                else:
                    e = {'ADD': '(%s + %s)', 'ADC': '(%s + %s + FC)', 'SUB': '(%s - %s)', 'SBC': '(%s - %s - !FC)',
                         'RSB': '(%s - %s)', 'RSC': '(%s - %s - !FC)'}[op]
                    args = (ys, xs) if op in ('RSB', 'RSC') else (xs, ys)
                    self.write_reg(ins, rd, e % args)
            if close:
                self.emit('  }')
            return
        if I == A.ARM_INS_MUL:
            rd = ops[0].reg
            x, y = (ops[1], ops[2]) if len(ops) == 3 else (ops[0], ops[1])
            e = '(%s * %s)' % (self.reg(ins, x.reg), self.reg(ins, y.reg))
            if S:
                self.emit('  { uint32_t r_ = %s; FN = r_>>31; FZ = r_==0; R[%d] = r_; }' % (e, self.rn(rd)))
            else:
                self.write_reg(ins, rd, e)
            return
        if I == A.ARM_INS_MLA:
            e = '(%s * %s + %s)' % tuple(self.reg(ins, o.reg) for o in ops[1:4])
            if S:
                self.emit('  { uint32_t r_ = %s; FN = r_>>31; FZ = r_==0; R[%d] = r_; }' % (e, self.rn(ops[0].reg)))
            else:
                self.write_reg(ins, ops[0].reg, e)
            return
        if I in (A.ARM_INS_UMULL, A.ARM_INS_SMULL, A.ARM_INS_UMLAL, A.ARM_INS_SMLAL):
            lo, hi, x, y = (self.rn(o.reg) for o in ops[:4])
            signed = I in (A.ARM_INS_SMULL, A.ARM_INS_SMLAL)
            acc = I in (A.ARM_INS_UMLAL, A.ARM_INS_SMLAL)
            mul = ('(uint64_t)((int64_t)(int32_t)R[%d] * (int32_t)R[%d])' if signed else '((uint64_t)R[%d] * R[%d])') % (x, y)
            if acc:
                mul = '(%s + (((uint64_t)R[%d]<<32)|R[%d]))' % (mul, hi, lo)
            self.emit('  { uint64_t m_ = %s; R[%d] = (uint32_t)m_; R[%d] = (uint32_t)(m_>>32);%s }'
                      % (mul, lo, hi, ' FN = (uint32_t)(m_>>63); FZ = m_==0;' if S else ''))
            return
        if I == A.ARM_INS_CLZ:
            self.write_reg(ins, ops[0].reg, 'CLZ(%s)' % self.reg(ins, ops[1].reg))
            return
        raise NotImplementedError(ins.mnemonic)


class Recompiler:
    def __init__(self, path):
        self.img = Image(path)
        self.entries = {}          # addr -> thumb
        self.insn_addrs_of = {}
        self.unimpl = defaultdict(int)
        self.cs_t = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        self.cs_a = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        for c in (self.cs_t, self.cs_a):
            c.detail = True
        self.hle = {}              # addr -> name
        ovf = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'overrides.txt')
        self.overrides = set()
        if os.path.exists(ovf):
            self.overrides = {l.split('#')[0].strip() for l in open(ovf)} - {''}
        plt = self.img.elf.get_section_by_name('.plt')
        self.plt = {}              # plt entry addr -> GOT slot addr
        if plt:
            lo, hi = plt['sh_addr'], plt['sh_addr'] + plt['sh_size']
            ins = list(self.cs_a.disasm(bytes(self.img.mem[lo:hi]), lo))
            for i in range(len(ins) - 2):
                a, b, c = ins[i:i + 3]
                if a.id == A.ARM_INS_ADD and a.operands[1].reg == A.ARM_REG_PC and c.id == A.ARM_INS_LDR \
                        and c.operands[0].reg == A.ARM_REG_PC:
                    ip = a.address + 8 + a.operands[2].imm
                    ip += b.operands[2].imm
                    self.plt[a.address] = ip + c.operands[1].mem.disp
                    # old toolchains put a 4-byte Thumb 'bx pc; nop' stub in front of the entry
                    self.plt[a.address - 4] = self.plt[a.address]
        self.aborts = {}
        for a, f in self.img.funcs.items():
            names = self.img.names[a]
            hn = [n for n in names if n in HLE]
            on = [n for n in names if n in self.overrides]
            if on:
                self.hle[a] = on[0]
            elif hn:
                self.hle[a] = hn[0]
            elif any(n.startswith(ABORT_PREFIXES) for n in names):
                self.aborts[a] = names[0]
            elif a == 0:
                continue
            else:
                self.entries[a] = f['thumb']

    def callee(self, a):
        if a in self.plt:
            return 'P_%06x' % a
        if a in self.hle:
            return 'H' + self.hle[a]
        if a in self.aborts:
            return 'rt_abort_%06x' % a
        return fname(a)

    def want_entry(self, a, thumb):
        if a in self.plt:
            return
        if a in self.entries or a in self.hle or a in self.aborts:
            return
        self.entries[a] = thumb
        self.pending.append(a)

    def func_end(self, a):
        """Function range: symbol size if known, else up to the next entry/data region."""
        f = self.img.funcs.get(a)
        nxt_start = min([e for e in self.all_starts if e > a] + [self.img.text_hi])
        if f and f['size'] and a + f['size'] <= nxt_start:
            return a + f['size']
        return min(nxt_start, self.img.region_end(a))

    def disasm(self, a, end, thumb):
        cs = self.cs_t if thumb else self.cs_a
        insns = []
        pos = a
        while pos < end:
            mode = self.img.mode_at(pos)
            if mode == 'd':
                # skip literal pool up to the next code mapping symbol
                nx = [m for m, k in self.img.maps if m > pos and k != 'd']
                if not nx or nx[0] >= end:
                    break
                pos = nx[0]
                continue
            got = False
            for ins in cs.disasm(bytes(self.img.mem[pos:end]), pos, 1):
                insns.append(ins)
                pos += ins.size
                got = True
            if not got:
                pos += 2 if thumb else 4
        return insns

    def run(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        for fn in os.listdir(outdir):
            if fn.startswith('code_') and fn.endswith('.c'):
                os.unlink(os.path.join(outdir, fn))
        self.pending = list(self.entries)
        done = {}
        while self.pending:
            self.all_starts = sorted(set(self.entries) | set(self.hle) | set(self.aborts))
            batch, self.pending = self.pending, []
            for a in batch:
                if a in done:
                    continue
                thumb = self.entries[a]
                end = self.func_end(a)
                insns = self.disasm(a, end, thumb)
                self.insn_addrs_of[a] = {i.address for i in insns}
                done[a] = (thumb, end, insns)
        # generate (second pass so all entry points are known)
        self.all_starts = sorted(set(self.entries) | set(self.hle) | set(self.aborts))
        funcs = []
        for a in sorted(done):
            thumb, end, insns = done[a]
            names = self.img.names.get(a) or ['sub_%x' % a]
            g = FuncGen(self, a, end, thumb, names[0])
            g.insns = insns
            funcs.append(g.gen())
        # entries discovered during generation (jumps into mid-function) need their own functions
        while self.pending:
            self.all_starts = sorted(set(self.entries) | set(self.hle) | set(self.aborts))
            batch, self.pending = self.pending, []
            for a in batch:
                if a in done:
                    continue
                thumb = self.entries[a]
                end = self.func_end(a)
                insns = self.disasm(a, end, thumb)
                self.insn_addrs_of[a] = {i.address for i in insns}
                done[a] = (thumb, end, insns)
                g = FuncGen(self, a, end, thumb, 'sub_%x' % a)
                g.insns = insns
                funcs.append(g.gen())
        self.write(outdir, funcs, done)

    def write(self, outdir, funcs, done):
        allf = sorted(done)
        with open(os.path.join(outdir, 'funcs.h'), 'w') as h:
            h.write('#pragma once\n#include "rt.h"\n')
            for a in allf:
                h.write('void %s(void);\n' % fname(a))
            for a, n in sorted(self.hle.items()):
                h.write('void H%s(void);\n' % n)
            for a in sorted(self.aborts):
                h.write('void rt_abort_%06x(void);\n' % a)
            for a in sorted(self.plt):
                h.write('void P_%06x(void);\n' % a)
        per = 150
        for i in range(0, len(funcs), per):
            with open(os.path.join(outdir, 'code_%03d.c' % (i // per)), 'w') as f:
                f.write('#include "funcs.h"\n')
                for fn in funcs[i:i + per]:
                    f.write('\n'.join(fn))
                    f.write('\n\n')
        with open(os.path.join(outdir, 'tables.c'), 'w') as f:
            f.write('#include "funcs.h"\n')
            for a, n in sorted(self.aborts.items()):
                f.write('void rt_abort_%06x(void) { rt_fatal("unsupported runtime routine %s"); }\n' % (a, n))
            for a, slot in sorted(self.plt.items()):
                f.write('void P_%06x(void) { rt_call(RD32(0x%xu)); }\n' % (a, slot + GBASE))
            f.write('rt_fn rt_lookup(uint32_t a) {\n  switch (a) {\n')
            for a in allf:
                f.write('  case 0x%xu: return %s;\n' % (a + GBASE, fname(a)))
            for a, n in sorted(self.hle.items()):
                f.write('  case 0x%xu: return H%s;\n' % (a + GBASE, n))
            for a, n in sorted(self.aborts.items()):
                f.write('  case 0x%xu: return rt_abort_%06x;\n' % (a + GBASE, a))
            f.write('  }\n  return 0;\n}\n')
            for n in self.img.imports:
                f.write('void I_%s(void);\n' % n)
            f.write('const rt_fn rt_imports[] = {\n')
            for n in self.img.imports:
                f.write('  I_%s,\n' % n)
            f.write('  0 };\n')
            f.write('const char *const rt_import_names[] = {\n')
            for n in self.img.imports:
                f.write('  "%s",\n' % n)
            f.write('  0 };\n')
            f.write('const uint32_t rt_import_count = %d;\n' % len(self.img.imports))
            f.write('const uint32_t rt_image_base = 0x%xu, rt_image_size = 0x%xu;\n' % (GBASE, self.img.size))
            f.write('const uint32_t rt_stack_chk_guard = 0x%xu;\n' % DATA_IMP)
            f.write('const struct rt_export rt_exports[] = {\n')
            for n, v in sorted(self.img.exports.items()):
                if n.startswith('Java_') or n == 'JNI_OnLoad':
                    f.write('  { "%s", %s },\n' % (n, fname(v & ~1)))
            f.write('  { 0, 0 } };\n')
        with open(os.path.join(outdir, 'image.bin'), 'wb') as f:
            f.write(self.img.mem)
        print('functions:', len(funcs), 'hle:', len(self.hle), 'aborts:', len(self.aborts),
              'imports:', len(self.img.imports))
        if self.unimpl:
            print('unimplemented:', dict(self.unimpl))


if __name__ == '__main__':
    Recompiler(sys.argv[1]).run(sys.argv[2])
