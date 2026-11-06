#!/usr/bin/env python3
"""Static recompiler for the MechWarrior (1989) port.

Translates the 16-bit x86 code of a DOS MZ image into C. Code is found from
the runtime coverage trace (every instruction the game executed, with the code
segment it ran under) plus recursive descent along direct branches from there.
Each basic block becomes one C function operating on the shared machine state;
semantics come from src/ops.h, the same helpers the reference interpreter uses.

usage: recomp.py IMAGE.EXE NAME trace.cov out.c
"""
import struct, sys

def load_mz(path):
    d = open(path, 'rb').read()
    if d[:2] != b'MZ':
        return d, set()     # .COM: image starts at PSP:0100
    lastp, pages, nrel, hdrp = struct.unpack_from('<HHHH', d, 2)
    hdr = hdrp * 16
    size = (pages - 1) * 512 + (lastp or 512)
    size = min(size, len(d))
    relo = struct.unpack_from('<H', d, 24)[0]
    img = d[hdr:size]
    rel = set()
    for i in range(nrel):
        o, s = struct.unpack_from('<HH', d, relo + i * 4)
        rel.add(s * 16 + o)
    return img, rel

REG16 = ['AX', 'CX', 'DX', 'BX', 'SP', 'BP', 'SI', 'DI']
SEGN = ['ES', 'CS', 'SS', 'DS']

class Insn:
    pass

class Decoder:
    def __init__(self, img, rel):
        self.img, self.rel = img, rel

    def u8(self, a): return self.img[a] if a < len(self.img) else 0
    def u16(self, a): return self.u8(a) | (self.u8(a + 1) << 8)

    def imm16(self, a):
        """16-bit immediate; relocated words get the image's load segment added at run time"""
        v = self.u16(a)
        if a in self.rel:
            return '((u16)(0x%04x + LOADSEG))' % v, True
        return '0x%04x' % v, False

    def decode(self, off, ip):
        """decode one instruction at image offset off (ip = its IP). returns Insn or None"""
        I = Insn(); I.off = off; I.ip = ip
        p = off; seg = None; rep = 0
        while True:
            b = self.u8(p)
            if b in (0x26, 0x2E, 0x36, 0x3E): seg = (b >> 3) & 3; p += 1; continue
            if b == 0xF3: rep = 1; p += 1; continue
            if b == 0xF2: rep = 2; p += 1; continue
            if b == 0xF0: p += 1; continue
            break
        I.seg, I.rep = seg, rep
        I.op = op = self.u8(p); p += 1
        I.modrm = None
        self.p = p
        I.kind = 'normal'
        I.targets = []
        if p - off > 6: return None

        def modrm():
            m = self.u8(self.p); self.p += 1
            mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
            I.mod, I.reg, I.rm = mod, reg, rm
            if mod == 3:
                I.ea = None; return
            defseg = 3
            base = {0: 'cpu.r[BX] + cpu.r[SI]', 1: 'cpu.r[BX] + cpu.r[DI]', 2: 'cpu.r[BP] + cpu.r[SI]', 3: 'cpu.r[BP] + cpu.r[DI]',
                    4: 'cpu.r[SI]', 5: 'cpu.r[DI]', 6: 'cpu.r[BP]', 7: 'cpu.r[BX]'}[rm]
            if rm in (2, 3) or (rm == 6 and mod != 0): defseg = 2
            disp = 0
            if mod == 0 and rm == 6:
                base = None; disp = self.u16(self.p); self.p += 2
            elif mod == 1:
                disp = struct.unpack('b', bytes([self.u8(self.p)]))[0]; self.p += 1
            elif mod == 2:
                disp = self.u16(self.p); self.p += 2
            s = seg if seg is not None else defseg
            if base is None: o = '0x%04x' % disp
            elif disp: o = '(u16)(%s + 0x%04x)' % (base, disp & 0xFFFF)
            else: o = '(u16)(%s)' % base
            I.ea = (SEGN[s], o)

        I.modrm_fn = modrm
        return I

def ea_lin(I, add=0):
    s, o = I.ea
    if add: return 'lin(cpu.s[%s], (u16)(%s + %d))' % (s, o, add)
    return 'lin(cpu.s[%s], %s)' % (s, o)

def r8(i): return '(*r8p(%d))' % i

def get_rm(I, w):
    if I.ea is None: return ('cpu.r[%s]' % REG16[I.rm]) if w else r8(I.rm)
    return ('rw(%s)' if w else 'rb(%s)') % ea_lin(I)

def set_rm(I, w, v):
    if I.ea is None: return ('cpu.r[%s] = %s;' % (REG16[I.rm], v)) if w else ('%s = %s;' % (r8(I.rm), v))
    return ('ww(%s, %s);' if w else 'wb(%s, %s);') % (ea_lin(I), v)

def segexpr(I): return 'cpu.s[%s]' % SEGN[I.seg if I.seg is not None else 3]

class Gen:
    def __init__(self, img, rel):
        self.d = Decoder(img, rel)

    def translate(self, off, ip):
        """returns (insn, c_code, length, flow) ; flow: 'next' | 'end' with targets"""
        d = self.d
        I = d.decode(off, ip)
        if I is None: return None
        op = I.op
        c = []
        flow = 'next'
        tg = []   # direct targets: (ip, kind)

        def imm8():
            v = d.u8(d.p); d.p += 1; return v
        def imm16raw():
            v = d.u16(d.p); d.p += 2; return v
        def imm16():
            s, _ = d.imm16(d.p); d.p += 2; return s
        def nxt(): return (I.ip + (d.p - off)) & 0xFFFF

        w = op & 1
        if op < 0x40 and (op & 7) < 6:
            a = op >> 3
            k = op & 7
            if k <= 3:
                I.modrm_fn()
                w = k & 1
                regv = ('cpu.r[%s]' % REG16[I.reg]) if w else r8(I.reg)
                rmv = get_rm(I, w)
                if k < 2:
                    if a == 7: c.append('alu(7, %s, %s, %d);' % (rmv, regv, w))
                    else: c.append('{ u32 r_ = alu(%d, %s, %s, %d); %s }' % (a, rmv, regv, w, set_rm(I, w, 'r_')))
                else:
                    if a == 7: c.append('alu(7, %s, %s, %d);' % (regv, rmv, w))
                    else: c.append('%s = alu(%d, %s, %s, %d);' % (regv, a, regv, rmv, w))
            else:
                if k == 4:
                    v = imm8()
                    if a == 7: c.append('alu(7, cpu.r[AX] & 0xFF, 0x%02x, 0);' % v)
                    else: c.append('AL8 = alu(%d, cpu.r[AX] & 0xFF, 0x%02x, 0);' % (a, v))
                else:
                    v = imm16()
                    if a == 7: c.append('alu(7, cpu.r[AX], %s, 1);' % v)
                    else: c.append('cpu.r[AX] = alu(%d, cpu.r[AX], %s, 1);' % (a, v))
        elif op in (0x06, 0x0E, 0x16, 0x1E): c.append('cpu_push(cpu.s[%s]);' % SEGN[op >> 3])
        elif op in (0x07, 0x17, 0x1F): c.append('cpu.s[%s] = cpu_pop();' % SEGN[op >> 3])
        elif 0x40 <= op <= 0x47: c.append('cpu.r[%s] = op_inc16(cpu.r[%s], 0);' % (REG16[op & 7], REG16[op & 7]))
        elif 0x48 <= op <= 0x4F: c.append('cpu.r[%s] = op_inc16(cpu.r[%s], 1);' % (REG16[op & 7], REG16[op & 7]))
        elif 0x50 <= op <= 0x57 and op != 0x54: c.append('cpu_push(cpu.r[%s]);' % REG16[op & 7])
        elif 0x58 <= op <= 0x5F: c.append('cpu.r[%s] = cpu_pop();' % REG16[op & 7])
        elif op == 0x68: c.append('cpu_push(%s);' % imm16())
        elif op == 0x6A: v = imm8(); c.append('cpu_push(0x%04x);' % ((v - 256 if v > 127 else v) & 0xFFFF))
        elif 0x70 <= op <= 0x7F or op in (0xE0, 0xE1, 0xE2, 0xE3, 0xEB):
            v = imm8(); v = v - 256 if v > 127 else v
            n = nxt(); t = (n + v) & 0xFFFF
            if op == 0xEB:
                c.append('cpu.ip = 0x%04x;' % t); flow = 'end'; tg.append(t)
            else:
                if op <= 0x7F: cnd = 'cond(%d)' % (op & 0xF)
                elif op == 0xE3: cnd = '!cpu.r[CX]'
                elif op == 0xE2: cnd = '--cpu.r[CX]'
                elif op == 0xE1: cnd = '--cpu.r[CX] && (cpu.flags & F_ZF)'
                else: cnd = '--cpu.r[CX] && !(cpu.flags & F_ZF)'
                c.append('cpu.ip = (%s) ? 0x%04x : 0x%04x;' % (cnd, t, n)); flow = 'end'; tg += [t, n]
        elif op in (0x80, 0x81, 0x82, 0x83):
            I.modrm_fn(); w = op & 1
            if op == 0x81: v = imm16()
            elif op == 0x83: x = imm8(); v = '0x%04x' % ((x - 256 if x > 127 else x) & 0xFFFF)
            else: v = '0x%02x' % imm8()
            rmv = get_rm(I, w)
            if I.reg == 7: c.append('alu(7, %s, %s, %d);' % (rmv, v, w))
            else: c.append('{ u32 r_ = alu(%d, %s, %s, %d); %s }' % (I.reg, rmv, v, w, set_rm(I, w, 'r_')))
        elif op in (0x84, 0x85):
            I.modrm_fn(); w = op & 1
            c.append('alu(4, %s, %s, %d);' % (get_rm(I, w), ('cpu.r[%s]' % REG16[I.reg]) if w else r8(I.reg), w))
        elif op in (0x86, 0x87):
            I.modrm_fn(); w = op & 1
            regv = ('cpu.r[%s]' % REG16[I.reg]) if w else r8(I.reg)
            c.append('{ u16 t_ = %s; %s %s = t_; }' % (get_rm(I, w), set_rm(I, w, regv), regv))
        elif op in (0x88, 0x89):
            I.modrm_fn(); w = op & 1
            c.append(set_rm(I, w, ('cpu.r[%s]' % REG16[I.reg]) if w else r8(I.reg)))
        elif op in (0x8A, 0x8B):
            I.modrm_fn(); w = op & 1
            c.append('%s = %s;' % (('cpu.r[%s]' % REG16[I.reg]) if w else r8(I.reg), get_rm(I, w)))
        elif op == 0x8C: I.modrm_fn(); c.append(set_rm(I, 1, 'cpu.s[%s]' % SEGN[I.reg & 3]))
        elif op == 0x8D:
            I.modrm_fn()
            if I.ea is None: return None
            c.append('cpu.r[%s] = %s;' % (REG16[I.reg], I.ea[1]))
        elif op == 0x8E: I.modrm_fn(); c.append('cpu.s[%s] = %s;' % (SEGN[I.reg & 3], get_rm(I, 1)))
        elif op == 0x8F: I.modrm_fn(); c.append('{ u16 v_ = cpu_pop(); %s }' % set_rm(I, 1, 'v_'))
        elif op == 0x90: pass
        elif 0x91 <= op <= 0x97: c.append('{ u16 t_ = cpu.r[AX]; cpu.r[AX] = cpu.r[%s]; cpu.r[%s] = t_; }' % (REG16[op & 7], REG16[op & 7]))
        elif op == 0x98: c.append('cpu.r[AX] = (u16)(s8)(cpu.r[AX] & 0xFF);')
        elif op == 0x99: c.append('cpu.r[DX] = (cpu.r[AX] & 0x8000) ? 0xFFFF : 0;')
        elif op == 0x9A:
            o = imm16raw(); sg, isrel = d.imm16(d.p); sraw = d.u16(d.p); d.p += 2
            n = nxt()
            c.append('cpu_push(cpu.s[CS]); cpu_push(0x%04x); cpu.s[CS] = %s; cpu.ip = 0x%04x;' % (n, sg, o))
            flow = 'end'
            if isrel: tg.append(('far', sraw, o))
            tg.append(('ret', n))
        elif op == 0x9C: c.append('cpu_push(cpu.flags | 0xF002);')
        elif op == 0x9D: c.append('cpu.flags = (cpu_pop() & 0x0FD5) | 0x0002;')
        elif op == 0x9E: c.append('cpu.flags = (cpu.flags & 0xFF00) | ((cpu.r[AX] >> 8) & 0xD5) | 2;')
        elif op == 0x9F: c.append('*r8p(4) = cpu.flags & 0xFF;')
        elif op in (0xA0, 0xA1, 0xA2, 0xA3):
            a = imm16raw(); L = 'lin(%s, 0x%04x)' % (segexpr(I), a)
            c.append({0xA0: 'AL8 = rb(%s);', 0xA1: 'cpu.r[AX] = rw(%s);', 0xA2: 'wb(%s, cpu.r[AX] & 0xFF);', 0xA3: 'ww(%s, cpu.r[AX]);'}[op] % L)
        elif op in (0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF):
            c.append('string_op_ex(0x%02x, %d, %d);' % (op, I.seg if I.seg is not None else -1, I.rep))
        elif op == 0xA8: c.append('alu(4, cpu.r[AX] & 0xFF, 0x%02x, 0);' % imm8())
        elif op == 0xA9: c.append('alu(4, cpu.r[AX], %s, 1);' % imm16())
        elif 0xB0 <= op <= 0xB7: c.append('%s = 0x%02x;' % (r8(op & 7), imm8()))
        elif 0xB8 <= op <= 0xBF: c.append('cpu.r[%s] = %s;' % (REG16[op & 7], imm16()))
        elif op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):
            I.modrm_fn(); w = op & 1
            if op >= 0xD2: cnt = '(cpu.r[CX] & 0xFF)'
            elif op >= 0xD0: cnt = '1'
            else: cnt = '%d' % imm8()
            c.append('{ u32 r_ = shift(%d, %s, %s, %d); %s }' % (I.reg, get_rm(I, w), cnt, w, set_rm(I, w, 'r_')))
        elif op == 0xC2: n = imm16raw(); c.append('cpu.ip = cpu_pop(); cpu.r[SP] += 0x%04x;' % n); flow = 'end'
        elif op == 0xC3: c.append('cpu.ip = cpu_pop();'); flow = 'end'
        elif op in (0xC4, 0xC5):
            I.modrm_fn()
            if I.ea is None: return None
            c.append('{ u16 o_ = rw(%s), s_ = rw(%s); cpu.r[%s] = o_; cpu.s[%s] = s_; }' % (ea_lin(I), ea_lin(I, 2), REG16[I.reg], 'ES' if op == 0xC4 else 'DS'))
        elif op == 0xC6: I.modrm_fn(); c.append(set_rm(I, 0, '0x%02x' % imm8()))
        elif op == 0xC7: I.modrm_fn(); c.append(set_rm(I, 1, imm16()))
        elif op == 0xC9: c.append('cpu.r[SP] = cpu.r[BP]; cpu.r[BP] = cpu_pop();')
        elif op == 0xCA: n = imm16raw(); c.append('cpu.ip = cpu_pop(); cpu.s[CS] = cpu_pop(); cpu.r[SP] += 0x%04x;' % n); flow = 'end'
        elif op == 0xCB: c.append('cpu.ip = cpu_pop(); cpu.s[CS] = cpu_pop();'); flow = 'end'
        elif op == 0xCD:
            v = imm8(); n = nxt()
            c.append('cpu.ip = 0x%04x; cpu_interrupt(0x%02x);' % (n, v)); flow = 'end'; tg.append(('ret', n))
        elif op == 0xCF: c.append('cpu.ip = cpu_pop(); cpu.s[CS] = cpu_pop(); cpu.flags = (cpu_pop() & 0x0FD5) | 2;'); flow = 'end'
        elif op == 0xD7: c.append('AL8 = rb(lin(%s, (u16)(cpu.r[BX] + (cpu.r[AX] & 0xFF))));' % segexpr(I))
        elif op == 0xE4: c.append('AL8 = port_in(0x%02x);' % imm8())
        elif op == 0xE6: c.append('port_out(0x%02x, cpu.r[AX] & 0xFF);' % imm8())
        elif op == 0xE8:
            v = imm16raw(); n = nxt(); t = (n + v) & 0xFFFF
            c.append('cpu_push(0x%04x); cpu.ip = 0x%04x;' % (n, t)); flow = 'end'; tg += [t, ('ret', n)]
        elif op == 0xE9:
            v = imm16raw(); n = nxt(); t = (n + v) & 0xFFFF
            c.append('cpu.ip = 0x%04x;' % t); flow = 'end'; tg.append(t)
        elif op == 0xEA:
            o = imm16raw(); sg, isrel = d.imm16(d.p); sraw = d.u16(d.p); d.p += 2
            c.append('cpu.s[CS] = %s; cpu.ip = 0x%04x;' % (sg, o)); flow = 'end'
            if isrel: tg.append(('far', sraw, o))
        elif op == 0xEC: c.append('AL8 = port_in(cpu.r[DX]);')
        elif op == 0xED: c.append('cpu.r[AX] = port_inw(cpu.r[DX]);')
        elif op == 0xEE: c.append('port_out(cpu.r[DX], cpu.r[AX] & 0xFF);')
        elif op == 0xEF: c.append('port_outw(cpu.r[DX], cpu.r[AX]);')
        elif op == 0xF5: c.append('cpu.flags ^= F_CF;')
        elif op in (0xF6, 0xF7):
            I.modrm_fn(); w = op & 1
            v = get_rm(I, w)
            r = I.reg
            if r in (0, 1): c.append('alu(4, %s, %s, %d);' % (v, ('0x%02x' % imm8()) if not w else imm16(), w))
            elif r == 2: c.append(set_rm(I, w, '(%s)~%s' % ('u16' if w else 'u8', v)))
            elif r == 3: c.append(set_rm(I, w, 'op_neg%d(%s)' % (16 if w else 8, v)))
            elif r == 4: c.append('op_mul%d(%s);' % (16 if w else 8, v))
            elif r == 5: c.append('op_imul%d(%s);' % (16 if w else 8, v))
            else:
                fn = 'op_%sdiv%d' % ('i' if r == 7 else '', 16 if w else 8)
                c.append('if (%s(%s)) { cpu.ip = 0x%04x; cpu_interrupt(0); return; }' % (fn, v, I.ip))
        elif op == 0xF8: c.append('cpu.flags &= ~F_CF;')
        elif op == 0xF9: c.append('cpu.flags |= F_CF;')
        elif op == 0xFA: c.append('cpu.flags &= ~F_IF;')
        elif op == 0xFB: c.append('cpu.flags |= F_IF;')
        elif op == 0xFC: c.append('cpu.flags &= ~F_DF;')
        elif op == 0xFD: c.append('cpu.flags |= F_DF;')
        elif op == 0xFE:
            I.modrm_fn()
            if I.reg > 1: return None
            c.append(set_rm(I, 0, 'op_inc8(%s, %d)' % (get_rm(I, 0), I.reg)))
        elif op == 0xFF:
            I.modrm_fn(); r = I.reg; n = nxt()
            if r in (0, 1): c.append(set_rm(I, 1, 'op_inc16(%s, %d)' % (get_rm(I, 1), r)))
            elif r == 2: c.append('{ u16 t_ = %s; cpu_push(0x%04x); cpu.ip = t_; }' % (get_rm(I, 1), n)); flow = 'end'; tg.append(('ret', n))
            elif r == 3:
                if I.ea is None: return None
                c.append('{ u16 o_ = rw(%s), s_ = rw(%s); cpu_push(cpu.s[CS]); cpu_push(0x%04x); cpu.ip = o_; cpu.s[CS] = s_; }' % (ea_lin(I), ea_lin(I, 2), n)); flow = 'end'; tg.append(('ret', n))
            elif r == 4: c.append('cpu.ip = %s;' % get_rm(I, 1)); flow = 'end'
            elif r == 5:
                if I.ea is None: return None
                c.append('{ u16 o_ = rw(%s), s_ = rw(%s); cpu.ip = o_; cpu.s[CS] = s_; }' % (ea_lin(I), ea_lin(I, 2))); flow = 'end'
            elif r == 6: c.append('cpu_push(%s);' % get_rm(I, 1))
            else: return None
        elif op in (0x27, 0x2F, 0x37, 0x3F, 0x54, 0x60, 0x61, 0x62, 0x69, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x9B,
                    0xC8, 0xCC, 0xCE, 0xD4, 0xD5, 0xD6, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE7):
            # rare instructions: decode length here, execute through the shared interpreter
            if op in (0x62, 0x69, 0x6B) or 0xD8 <= op <= 0xDF: I.modrm_fn()
            if op == 0x69: d.p += 2
            if op in (0x6B, 0xCD, 0xD4, 0xD5, 0xE5, 0xE7): d.p += 1
            if op == 0xC8: d.p += 3
            c.append('cpu.ip = 0x%04x; cpu_step();' % I.ip)
            if op in (0xCC, 0xCE): flow = 'end'; c[-1] += ' return;'
        else:
            return None
        L = d.p - off
        return I, c, L, flow, tg

def main():
    exe, name, cov, out = sys.argv[1:5]
    img, rel = load_mz(exe)
    rel = set(rel)
    g = Gen(img, rel)
    covd = open(cov, 'rb').read()
    covw = struct.unpack('<%dH' % (len(covd) // 2), covd)
    N = len(img)
    seg_of = {}          # image offset -> code segment (relative)
    leaders = set()
    work = []
    for a in range(min(N, len(covw))):
        if covw[a]:
            s = covw[a] - 1
            seg_of[a] = s
            work.append(a)
    traced = set(work)
    # recursive descent
    insns = {}
    while work:
        a = work.pop()
        while True:
            if a in insns or a >= N or a < 0: break
            s = seg_of.get(a)
            if s is None: break
            ip = (a - s * 16) & 0xFFFF
            r = g.translate(a, ip)
            if r is None:
                break
            I, c, L, flow, tg = r
            insns[a] = (I, c, L, flow, tg, s)
            for t in tg:
                if isinstance(t, tuple):
                    if t[0] == 'ret':
                        ta = s * 16 + t[1]; leaders.add(ta)
                        if ta not in seg_of: seg_of[ta] = s; work.append(ta)
                    elif t[0] == 'far':
                        ta = t[1] * 16 + t[2]; leaders.add(ta)
                        if ta not in seg_of: seg_of[ta] = t[1]; work.append(ta)
                else:
                    ta = s * 16 + t; leaders.add(ta)
                    if ta not in seg_of: seg_of[ta] = s; work.append(ta)
            if flow == 'end': break
            na = a + L
            if na in seg_of and seg_of[na] != s: break
            seg_of.setdefault(na, s)
            a = na
    # every traced address whose predecessor isn't straight-line code is a leader
    for a in insns:
        if a in traced: pass
    prev_end = {}
    for a, (I, c, L, flow, tg, s) in insns.items():
        if flow != 'end': prev_end[a + L] = a
    for a in insns:
        if a not in prev_end: leaders.add(a)
    # emit
    o = open(out, 'w')
    o.write('/* generated by tools/recomp.py from %s - do not edit */\n' % name)
    o.write('#include "recomp_rt.h"\n#define LOADSEG (rc_loadseg)\nstatic u16 rc_loadseg;\n\n')
    blocks = []
    for a in sorted(leaders):
        if a not in insns: continue
        body = []; n = 0; x = a; csum = 0; io = 0; wr = 0; rlist = []; acc = 0
        while True:
            I, c, L, flow, tg, s = insns[x]
            body += c; n += 1
            for k in range(L):
                pos = x + k
                if pos in rel or (pos - 1) in rel:
                    if pos in rel: rlist.append(pos - a)
                    csum = (csum * 31) & 0xFFFFFFFF
                else:
                    csum = (csum * 31 + img[pos]) & 0xFFFFFFFF
            if any(('port_' in l) or ('cpu_step' in l) or ('cpu_interrupt' in l) for l in c): io = 1
            if any(('wb(' in l) or ('ww(' in l) or ('cpu_push' in l) or ('string_op' in l) or ('cpu.s[' in l and '] =' in l) for l in c): wr = 1
            # instructions that carry state from one pass to the next (counters, shifts,
            # arithmetic): a loop doing these is computing, not waiting
            p0 = x
            while img[p0] in (0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3, 0xF0): p0 += 1
            op = img[p0]; rf = (img[p0 + 1] >> 3) & 7 if p0 + 1 < len(img) else 0
            if (0xE0 <= op <= 0xE2 or 0x40 <= op <= 0x4F or op in (0xD0, 0xD1, 0xD2, 0xD3, 0xC0, 0xC1, 0x86, 0x87, 0x9C, 0x9D)
                    or 0x91 <= op <= 0x97 or (op in (0xF6, 0xF7) and rf >= 2) or (op in (0xFE, 0xFF) and rf <= 1)
                    or (op < 0x40 and (op & 7) <= 5 and (op >> 3) in (0, 2, 3, 5))
                    or (op in (0x80, 0x81, 0x83) and rf in (0, 2, 3, 5))): acc = 1
            x += L
            if flow == 'end': break
            if x in leaders or x not in insns or insns[x][5] != s:
                body.append('cpu.ip = 0x%04x;' % ((x - s * 16) & 0xFFFF)); break
        # a block that only reads memory/registers and branches back to its own start is a wait loop:
        # nothing can change until an interrupt arrives, so the runtime may skip ahead to it
        selfip = (a - s * 16) & 0xFFFF
        spin = 1 if (not io and not wr and not acc and n <= 8 and flow == 'end' and ('0x%04x' % selfip) in body[-1] and 'cpu_pop' not in body[-1]) else 0
        blocks.append((a, s, x - a, csum, n, io | (spin << 1), rlist))
        o.write('static void b_%05x(void) { /* %04x:%04x */\n    cycles_done += %d;\n' % (a, s, (a - s * 16) & 0xFFFF, n))
        for line in body: o.write('    %s\n' % line)
        o.write('}\n')
    for a, s, ln, cs, n, io, rl in blocks:
        if rl: o.write('static const u16 r_%05x[] = { %s, 0xFFFF };\n' % (a, ', '.join(str(v) for v in rl)))
    o.write('\nconst RcBlock rc_%s_blocks[] = {\n' % name)
    for a, s, ln, cs, n, io, rl in blocks:
        o.write('    { 0x%05x, 0x%04x, %d, %d, %d, 0x%08xu, %s, b_%05x },\n' % (a, s, ln, n, io, cs, ('r_%05x' % a) if rl else 'NULL', a))
    o.write('};\nconst int rc_%s_nblocks = %d;\nu16 *rc_%s_loadseg = &rc_loadseg;\n' % (name, len(blocks), name))
    o.close()
    ninsn = len(insns); ntr = len(traced)
    print('%s: %d instructions (%d from trace, %d found by descent), %d blocks' % (name, ninsn, ntr, ninsn - len([a for a in insns if a in traced]), len(blocks)))

if __name__ == '__main__':
    main()
