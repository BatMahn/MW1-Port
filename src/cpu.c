/* 8086/80186 real-mode interpreter. Used to trace code coverage for the
 * static recompiler and as the fallback for any code it did not reach. */
#include "machine.h"
#include "ops.h"
#include <stdio.h>

CPU cpu;
u8 mem[MEM_SIZE];
int64_t cycles_done;


static int seg_ovr;      /* -1 none, else segment index */
static int rep_mode;     /* 0 none, 1 REPE/REP, 2 REPNE */
static u16 op_ip;        /* ip of current instruction (for faults) */
static u16 op_cs;

static inline u8 fetch8(void) { u8 v = rb(lin(cpu.s[CS], cpu.ip)); cpu.ip++; return v; }
static inline u16 fetch16(void) { u16 v = rw(lin(cpu.s[CS], cpu.ip)); cpu.ip += 2; return v; }

void cpu_push(u16 v) { cpu.r[SP] -= 2; ww(lin(cpu.s[SS], cpu.r[SP]), v); }
u16 cpu_pop(void) { u16 v = rw(lin(cpu.s[SS], cpu.r[SP])); cpu.r[SP] += 2; return v; }

/* ---- modrm ---- */
static int mod, reg, rm;
static u32 ea; static int ea_is_reg;

static void decode_modrm(void)
{
    u8 b = fetch8();
    mod = b >> 6; reg = (b >> 3) & 7; rm = b & 7;
    if (mod == 3) { ea_is_reg = 1; return; }
    ea_is_reg = 0;
    u16 off = 0; int defseg = DS;
    switch (rm) {
    case 0: off = cpu.r[BX] + cpu.r[SI]; break;
    case 1: off = cpu.r[BX] + cpu.r[DI]; break;
    case 2: off = cpu.r[BP] + cpu.r[SI]; defseg = SS; break;
    case 3: off = cpu.r[BP] + cpu.r[DI]; defseg = SS; break;
    case 4: off = cpu.r[SI]; break;
    case 5: off = cpu.r[DI]; break;
    case 6: if (mod == 0) off = fetch16(); else { off = cpu.r[BP]; defseg = SS; } break;
    case 7: off = cpu.r[BX]; break;
    }
    if (mod == 1) off += (s8)fetch8();
    else if (mod == 2) off += fetch16();
    ea = (u32)off | ((u32)(seg_ovr >= 0 ? seg_ovr : defseg) << 16);
}
static inline u32 ea_lin(int add) { return lin(cpu.s[ea >> 16], (u16)(ea + add)); }
static inline u8 get_rm8(void) { return ea_is_reg ? *r8p(rm) : rb(ea_lin(0)); }
static inline u16 get_rm16(void) { return ea_is_reg ? cpu.r[rm] : rw(ea_lin(0)); }
static inline void set_rm8(u8 v) { if (ea_is_reg) *r8p(rm) = v; else wb(ea_lin(0), v); }
static inline void set_rm16(u16 v) { if (ea_is_reg) cpu.r[rm] = v; else ww(ea_lin(0), v); }

void cpu_interrupt(u8 n)
{
    if (n == 0xEB && log_enabled) logmsg("INT EBh ax=%04x bx=%04x cx=%04x dx=%04x", cpu.r[AX], cpu.r[BX], cpu.r[CX], cpu.r[DX]);
    if (n == 0x15 && log_enabled > 1) logmsg("IRQ13/INT15");
    cpu_push(cpu.flags);
    cpu_push(cpu.s[CS]);
    cpu_push(cpu.ip);
    cpu.flags &= ~(F_IF | F_TF);
    cpu.ip = rw(n * 4);
    cpu.s[CS] = rw(n * 4 + 2);
}

void cpu_reset(void) { cpu.flags = 0x0002; seg_ovr = -1; }

/* string ops */
static void string_op(u8 op)
{
    int w = op & 1, d = (cpu.flags & F_DF) ? -1 : 1, sz = w ? 2 : 1;
    int sseg = seg_ovr >= 0 ? seg_ovr : DS;
    for (;;) {
        if (rep_mode && cpu.r[CX] == 0) break;
        switch (op & 0xFE) {
        case 0xA4: if (w) ww(lin(cpu.s[ES], cpu.r[DI]), rw(lin(cpu.s[sseg], cpu.r[SI]))); else wb(lin(cpu.s[ES], cpu.r[DI]), rb(lin(cpu.s[sseg], cpu.r[SI]))); cpu.r[SI] += d * sz; cpu.r[DI] += d * sz; break;
        case 0xA6: if (w) alu(7, rw(lin(cpu.s[sseg], cpu.r[SI])), rw(lin(cpu.s[ES], cpu.r[DI])), 1); else alu(7, rb(lin(cpu.s[sseg], cpu.r[SI])), rb(lin(cpu.s[ES], cpu.r[DI])), 0); cpu.r[SI] += d * sz; cpu.r[DI] += d * sz; break;
        case 0xAA: if (w) ww(lin(cpu.s[ES], cpu.r[DI]), cpu.r[AX]); else wb(lin(cpu.s[ES], cpu.r[DI]), cpu.r[AX] & 0xFF); cpu.r[DI] += d * sz; break;
        case 0xAC: if (w) cpu.r[AX] = rw(lin(cpu.s[sseg], cpu.r[SI])); else *r8p(0) = rb(lin(cpu.s[sseg], cpu.r[SI])); cpu.r[SI] += d * sz; break;
        case 0xAE: if (w) alu(7, cpu.r[AX], rw(lin(cpu.s[ES], cpu.r[DI])), 1); else alu(7, cpu.r[AX] & 0xFF, rb(lin(cpu.s[ES], cpu.r[DI])), 0); cpu.r[DI] += d * sz; break;
        case 0x6C: if (w) ww(lin(cpu.s[ES], cpu.r[DI]), port_inw(cpu.r[DX])); else wb(lin(cpu.s[ES], cpu.r[DI]), port_in(cpu.r[DX])); cpu.r[DI] += d * sz; break;
        case 0x6E: if (w) port_outw(cpu.r[DX], rw(lin(cpu.s[sseg], cpu.r[SI]))); else port_out(cpu.r[DX], rb(lin(cpu.s[sseg], cpu.r[SI]))); cpu.r[SI] += d * sz; break;
        }
        if (!rep_mode) break;
        cpu.r[CX]--;
        cycles_done++;
        if ((op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE) {
            int z = (cpu.flags & F_ZF) != 0;
            if (rep_mode == 1 && !z) break;
            if (rep_mode == 2 && z) break;
        }
    }
}

void string_op_ex(u8 op, int so, int rm) { seg_ovr = so; rep_mode = rm; string_op(op); seg_ovr = -1; rep_mode = 0; }

static void div_fault(void)
{
    cpu.ip = op_ip; cpu.s[CS] = op_cs;  /* 8086 pushes next ip; 286 pushes faulting ip. MSC handler aborts either way */
    cpu_interrupt(0);
}

static void grp3(int w)
{
    if (!w) {
        u8 v = get_rm8();
        switch (reg) {
        case 0: case 1: alu(4, v, fetch8(), 0); break;
        case 2: set_rm8(~v); break;
        case 3: { u8 r = alu(5, 0, v, 0); set_rm8(r); SETF(F_CF, v != 0); } break;
        case 4: { u16 r = (u16)(cpu.r[AX] & 0xFF) * v; cpu.r[AX] = r; SETF(F_CF | F_OF, r >> 8); szp8(r & 0xFF); } break;
        case 5: { s16 r = (s16)(s8)(cpu.r[AX] & 0xFF) * (s8)v; cpu.r[AX] = r; SETF(F_CF | F_OF, r != (s8)r); szp8(r & 0xFF); } break;
        case 6: { if (!v) { div_fault(); return; } u16 a = cpu.r[AX]; u16 q = a / v; if (q > 0xFF) { div_fault(); return; } cpu.r[AX] = (u16)((a % v) << 8) | q; } break;
        case 7: { if (!v) { div_fault(); return; } s16 a = cpu.r[AX]; s16 q = a / (s8)v; if (q > 127 || q < -128) { div_fault(); return; } cpu.r[AX] = (u16)(((u8)(a % (s8)v)) << 8) | (u8)q; } break;
        }
    } else {
        u16 v = get_rm16();
        switch (reg) {
        case 0: case 1: alu(4, v, fetch16(), 1); break;
        case 2: set_rm16(~v); break;
        case 3: { u16 r = alu(5, 0, v, 1); set_rm16(r); SETF(F_CF, v != 0); } break;
        case 4: { u32 r = (u32)cpu.r[AX] * v; cpu.r[AX] = r; cpu.r[DX] = r >> 16; SETF(F_CF | F_OF, cpu.r[DX]); szp16(r & 0xFFFF); } break;
        case 5: { s32 r = (s32)(s16)cpu.r[AX] * (s16)v; cpu.r[AX] = r; cpu.r[DX] = (u32)r >> 16; SETF(F_CF | F_OF, r != (s16)r); szp16(r & 0xFFFF); } break;
        case 6: { if (!v) { div_fault(); return; } u32 a = ((u32)cpu.r[DX] << 16) | cpu.r[AX]; u32 q = a / v; if (q > 0xFFFF) { div_fault(); return; } cpu.r[AX] = q; cpu.r[DX] = a % v; } break;
        case 7: { if (!v) { div_fault(); return; } s32 a = (s32)(((u32)cpu.r[DX] << 16) | cpu.r[AX]); s32 q = a / (s16)v; if (q > 32767 || q < -32768) { div_fault(); return; } cpu.r[AX] = q; cpu.r[DX] = a % (s16)v; } break;
        }
    }
}

void cpu_step(void)
{
    seg_ovr = -1; rep_mode = 0;
    op_ip = cpu.ip; op_cs = cpu.s[CS];
    u8 op;
    for (;;) {
        op = fetch8();
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) { seg_ovr = (op >> 3) & 3; continue; }
        if (op == 0xF3) { rep_mode = 1; continue; }
        if (op == 0xF2) { rep_mode = 2; continue; }
        if (op == 0xF0) continue;
        break;
    }
    cycles_done++;
    if (op < 0x40 && (op & 7) < 6) {
        int a = op >> 3, w = op & 1;
        switch (op & 7) {
        case 0: decode_modrm(); { u8 r = alu(a, get_rm8(), *r8p(reg), 0); if (a != 7) set_rm8(r); } break;
        case 1: decode_modrm(); { u16 r = alu(a, get_rm16(), cpu.r[reg], 1); if (a != 7) set_rm16(r); } break;
        case 2: decode_modrm(); { u8 r = alu(a, *r8p(reg), get_rm8(), 0); if (a != 7) *r8p(reg) = r; } break;
        case 3: decode_modrm(); { u16 r = alu(a, cpu.r[reg], get_rm16(), 1); if (a != 7) cpu.r[reg] = r; } break;
        case 4: { u8 r = alu(a, cpu.r[AX] & 0xFF, fetch8(), 0); if (a != 7) *r8p(0) = r; } break;
        case 5: { u16 r = alu(a, cpu.r[AX], fetch16(), 1); if (a != 7) cpu.r[AX] = r; } break;
        }
        (void)w;
        return;
    }
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: cpu_push(cpu.s[op >> 3]); break;
    case 0x07: case 0x17: case 0x1F: cpu.s[op >> 3] = cpu_pop(); break;
    case 0x0F: cpu.s[CS] = cpu_pop(); break;
    /* BCD adjusts, as the 8086/8088 does them (checked against hardware test vectors): the
     * second step's threshold is 9Fh when the first step adjusted, and AAA/AAS adjust AL and AH
     * separately (no carry from AL into AH) */
    case 0x27: { u8 al = cpu.r[AX] & 0xFF, old = al; int cf = cpu.flags & F_CF, af0 = cpu.flags & F_AF, af = 0;
        if ((al & 0xF) > 9 || af0) { al += 6; af = 1; }
        SETF(F_AF, af);
        if (old > (af0 ? 0x9F : 0x99) || cf) { al += 0x60; cpu.flags |= F_CF; } else cpu.flags &= ~F_CF;
        *r8p(0) = al; szp8(al); } break;
    case 0x2F: { u8 al = cpu.r[AX] & 0xFF, old = al; int cf = cpu.flags & F_CF, af0 = cpu.flags & F_AF, af = 0;
        if ((al & 0xF) > 9 || af0) { al -= 6; af = 1; }
        SETF(F_AF, af);
        if (old > (af0 ? 0x9F : 0x99) || cf) { al -= 0x60; cpu.flags |= F_CF; } else cpu.flags &= ~F_CF;
        *r8p(0) = al; szp8(al); } break;
    case 0x37: { u8 al = cpu.r[AX] & 0xFF, ah = cpu.r[AX] >> 8;
        if ((al & 0xF) > 9 || (cpu.flags & F_AF)) { al += 6; ah += 1; cpu.flags |= F_AF | F_CF; } else cpu.flags &= ~(F_AF | F_CF);
        cpu.r[AX] = (ah << 8) | (al & 0x0F); } break;
    case 0x3F: { u8 al = cpu.r[AX] & 0xFF, ah = cpu.r[AX] >> 8;
        if ((al & 0xF) > 9 || (cpu.flags & F_AF)) { al -= 6; ah -= 1; cpu.flags |= F_AF | F_CF; } else cpu.flags &= ~(F_AF | F_CF);
        cpu.r[AX] = (ah << 8) | (al & 0x0F); } break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        { int cf = cpu.flags & F_CF; cpu.r[op & 7] = incdec(cpu.r[op & 7], 0, 1); cpu.flags = (cpu.flags & ~F_CF) | cf; } break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        { int cf = cpu.flags & F_CF; cpu.r[op & 7] = incdec(cpu.r[op & 7], 1, 1); cpu.flags = (cpu.flags & ~F_CF) | cf; } break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57: cpu_push(cpu.r[op & 7]); break;
    case 0x54: { u16 v = cpu.r[SP] - 2; cpu_push(v); } break;  /* 8086 pushes decremented SP */
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: cpu.r[op & 7] = cpu_pop(); break;
    case 0x60: { u16 t = cpu.r[SP]; for (int i = 0; i < 8; i++) cpu_push(i == SP ? t : cpu.r[i]); } break;
    case 0x61: for (int i = 7; i >= 0; i--) { u16 v = cpu_pop(); if (i != SP) cpu.r[i] = v; } break;
    case 0x62: decode_modrm(); break; /* BOUND - assume in range */
    case 0x68: cpu_push(fetch16()); break;
    case 0x6A: cpu_push((u16)(s8)fetch8()); break;
    case 0x69: case 0x6B: { decode_modrm(); s16 a = get_rm16(); s16 b = op == 0x69 ? (s16)fetch16() : (s8)fetch8(); s32 r = (s32)a * b; cpu.r[reg] = r; SETF(F_CF | F_OF, r != (s16)r); } break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: string_op(op); break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        { s8 d = fetch8(); if (cond(op & 0xF)) cpu.ip += d; } break;
    case 0x80: case 0x82: decode_modrm(); { u8 v = get_rm8(); u8 r = alu(reg, v, fetch8(), 0); if (reg != 7) set_rm8(r); } break;
    case 0x81: decode_modrm(); { u16 v = get_rm16(); u16 r = alu(reg, v, fetch16(), 1); if (reg != 7) set_rm16(r); } break;
    case 0x83: decode_modrm(); { u16 v = get_rm16(); u16 r = alu(reg, v, (u16)(s8)fetch8(), 1); if (reg != 7) set_rm16(r); } break;
    case 0x84: decode_modrm(); alu(4, get_rm8(), *r8p(reg), 0); break;
    case 0x85: decode_modrm(); alu(4, get_rm16(), cpu.r[reg], 1); break;
    case 0x86: decode_modrm(); { u8 t = get_rm8(); set_rm8(*r8p(reg)); *r8p(reg) = t; } break;
    case 0x87: decode_modrm(); { u16 t = get_rm16(); set_rm16(cpu.r[reg]); cpu.r[reg] = t; } break;
    case 0x88: decode_modrm(); set_rm8(*r8p(reg)); break;
    case 0x89: decode_modrm(); set_rm16(cpu.r[reg]); break;
    case 0x8A: decode_modrm(); *r8p(reg) = get_rm8(); break;
    case 0x8B: decode_modrm(); cpu.r[reg] = get_rm16(); break;
    case 0x8C: decode_modrm(); set_rm16(cpu.s[reg & 3]); break;
    case 0x8D: decode_modrm(); cpu.r[reg] = (u16)ea; break;
    case 0x8E: decode_modrm(); cpu.s[reg & 3] = get_rm16(); break;
    case 0x8F: decode_modrm(); { u16 v = cpu_pop(); set_rm16(v); } break;
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        { u16 t = cpu.r[AX]; cpu.r[AX] = cpu.r[op & 7]; cpu.r[op & 7] = t; } break;
    case 0x98: cpu.r[AX] = (u16)(s8)(cpu.r[AX] & 0xFF); break;
    case 0x99: cpu.r[DX] = (cpu.r[AX] & 0x8000) ? 0xFFFF : 0; break;
    case 0x9A: { u16 o = fetch16(), s = fetch16(); cpu_push(cpu.s[CS]); cpu_push(cpu.ip); cpu.s[CS] = s; cpu.ip = o; } break;
    case 0x9B: break;
    case 0x9C: cpu_push(cpu.flags | 0xF002); break;
    case 0x9D: cpu.flags = (cpu_pop() & 0x0FD5) | 0x0002; break;
    case 0x9E: cpu.flags = (cpu.flags & 0xFF00) | ((cpu.r[AX] >> 8) & 0xD5) | 2; break;
    case 0x9F: *r8p(4) = cpu.flags & 0xFF; break;
    case 0xA0: *r8p(0) = rb(lin(cpu.s[seg_ovr >= 0 ? seg_ovr : DS], fetch16())); break;
    case 0xA1: cpu.r[AX] = rw(lin(cpu.s[seg_ovr >= 0 ? seg_ovr : DS], fetch16())); break;
    case 0xA2: wb(lin(cpu.s[seg_ovr >= 0 ? seg_ovr : DS], fetch16()), cpu.r[AX] & 0xFF); break;
    case 0xA3: ww(lin(cpu.s[seg_ovr >= 0 ? seg_ovr : DS], fetch16()), cpu.r[AX]); break;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: string_op(op); break;
    case 0xA8: alu(4, cpu.r[AX] & 0xFF, fetch8(), 0); break;
    case 0xA9: alu(4, cpu.r[AX], fetch16(), 1); break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: *r8p(op & 7) = fetch8(); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: cpu.r[op & 7] = fetch16(); break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int w = op & 1; decode_modrm();
        int cnt = (op >= 0xD2) ? (cpu.r[CX] & 0xFF) : 1;
        if (w) { u16 v = get_rm16(); if (op < 0xD0) cnt = fetch8(); set_rm16(shift(reg, v, cnt, 1)); }
        else { u8 v = get_rm8(); if (op < 0xD0) cnt = fetch8(); set_rm8(shift(reg, v, cnt, 0)); }
        } break;
    case 0xC2: { u16 n = fetch16(); cpu.ip = cpu_pop(); cpu.r[SP] += n; } break;
    case 0xC3: cpu.ip = cpu_pop(); break;
    case 0xC4: decode_modrm(); cpu.r[reg] = rw(ea_lin(0)); cpu.s[ES] = rw(ea_lin(2)); break;
    case 0xC5: decode_modrm(); cpu.r[reg] = rw(ea_lin(0)); cpu.s[DS] = rw(ea_lin(2)); break;
    case 0xC6: decode_modrm(); set_rm8(fetch8()); break;
    case 0xC7: decode_modrm(); set_rm16(fetch16()); break;
    case 0xC8: { u16 sz = fetch16(); u8 lv = fetch8() & 31; cpu_push(cpu.r[BP]); u16 fp = cpu.r[SP];
        for (int i = 1; i < lv; i++) { cpu.r[BP] -= 2; cpu_push(rw(lin(cpu.s[SS], cpu.r[BP]))); }
        if (lv) cpu_push(fp); cpu.r[BP] = fp; cpu.r[SP] -= sz; } break;
    case 0xC9: cpu.r[SP] = cpu.r[BP]; cpu.r[BP] = cpu_pop(); break;
    case 0xCA: { u16 n = fetch16(); cpu.ip = cpu_pop(); cpu.s[CS] = cpu_pop(); cpu.r[SP] += n; } break;
    case 0xCB: cpu.ip = cpu_pop(); cpu.s[CS] = cpu_pop(); break;
    case 0xCC: cpu_interrupt(3); break;
    case 0xCD: { u8 n = fetch8(); cpu_interrupt(n); } break;
    case 0xCE: if (cpu.flags & F_OF) cpu_interrupt(4); break;
    case 0xCF: cpu.ip = cpu_pop(); cpu.s[CS] = cpu_pop(); cpu.flags = (cpu_pop() & 0x0FD5) | 2; break;
    case 0xD4: { u8 b = fetch8(); if (!b) { szp8(0); div_fault(); break; } u8 al = cpu.r[AX] & 0xFF; cpu.r[AX] = ((al / b) << 8) | (al % b); szp8(cpu.r[AX] & 0xFF); } break;
    case 0xD5: { u8 b = fetch8(); u8 al = (cpu.r[AX] & 0xFF) + (cpu.r[AX] >> 8) * b; cpu.r[AX] = al; szp8(al); } break;
    case 0xD6: *r8p(0) = (cpu.flags & F_CF) ? 0xFF : 0; break;
    case 0xD7: *r8p(0) = rb(lin(cpu.s[seg_ovr >= 0 ? seg_ovr : DS], cpu.r[BX] + (cpu.r[AX] & 0xFF))); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode_modrm(); break; /* no FPU present: ESC is a no-op; MSC runtime detects no 8087 */
    case 0xE0: case 0xE1: case 0xE2: { s8 d = fetch8(); cpu.r[CX]--;
        int z = (cpu.flags & F_ZF) != 0;
        if (cpu.r[CX] && (op == 0xE2 || (op == 0xE1 && z) || (op == 0xE0 && !z))) cpu.ip += d; } break;
    case 0xE3: { s8 d = fetch8(); if (!cpu.r[CX]) cpu.ip += d; } break;
    case 0xE4: *r8p(0) = port_in(fetch8()); break;
    case 0xE5: cpu.r[AX] = port_inw(fetch8()); break;
    case 0xE6: port_out(fetch8(), cpu.r[AX] & 0xFF); break;
    case 0xE7: port_outw(fetch8(), cpu.r[AX]); break;
    case 0xE8: { u16 d = fetch16(); cpu_push(cpu.ip); cpu.ip += d; } break;
    case 0xE9: { u16 d = fetch16(); cpu.ip += d; } break;
    case 0xEA: { u16 o = fetch16(), s = fetch16(); cpu.ip = o; cpu.s[CS] = s; } break;
    case 0xEB: { s8 d = fetch8(); cpu.ip += d; } break;
    case 0xEC: *r8p(0) = port_in(cpu.r[DX]); break;
    case 0xED: cpu.r[AX] = port_inw(cpu.r[DX]); break;
    case 0xEE: port_out(cpu.r[DX], cpu.r[AX] & 0xFF); break;
    case 0xEF: port_outw(cpu.r[DX], cpu.r[AX]); break;
    case 0xF1: hle_callback(fetch8()); break;  /* runtime callback opcode */
    case 0xF4: cpu.halted = 1; break;
    case 0xF5: cpu.flags ^= F_CF; break;
    case 0xF6: decode_modrm(); grp3(0); break;
    case 0xF7: decode_modrm(); grp3(1); break;
    case 0xF8: cpu.flags &= ~F_CF; break;
    case 0xF9: cpu.flags |= F_CF; break;
    case 0xFA: cpu.flags &= ~F_IF; break;
    case 0xFB: cpu.flags |= F_IF; break;
    case 0xFC: cpu.flags &= ~F_DF; break;
    case 0xFD: cpu.flags |= F_DF; break;
    case 0xFE: decode_modrm(); { int cf = cpu.flags & F_CF; u8 v = get_rm8();
        if (reg < 2) set_rm8(incdec(v, reg, 0)); else fatal("bad FE /%d at %04x:%04x", reg, op_cs, op_ip);
        cpu.flags = (cpu.flags & ~F_CF) | cf; } break;
    case 0xFF: decode_modrm(); switch (reg) {
        case 0: case 1: { int cf = cpu.flags & F_CF; set_rm16(incdec(get_rm16(), reg, 1)); cpu.flags = (cpu.flags & ~F_CF) | cf; } break;
        case 2: { u16 t = get_rm16(); cpu_push(cpu.ip); cpu.ip = t; } break;
        case 3: { u16 o = rw(ea_lin(0)), s = rw(ea_lin(2)); cpu_push(cpu.s[CS]); cpu_push(cpu.ip); cpu.ip = o; cpu.s[CS] = s; } break;
        case 4: cpu.ip = get_rm16(); break;
        case 5: cpu.ip = rw(ea_lin(0)); cpu.s[CS] = rw(ea_lin(2)); break;
        case 6: cpu_push(get_rm16()); break;
        default: fatal("bad FF /7 at %04x:%04x", op_cs, op_ip);
        } break;
    default:
        fatal("unimplemented opcode %02x at %04x:%04x", op, op_cs, op_ip);
    }
}
