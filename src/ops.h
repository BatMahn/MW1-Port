/* Instruction semantics shared by the interpreter and the recompiled code,
 * so both behave identically. */
#ifndef OPS_H
#define OPS_H
#include "machine.h"

/* ---- flags ---- */
static const u8 parity_tab[256] = {
#define P2(n) n, n^1, n^1, n
#define P4(n) P2(n), P2(n^1), P2(n^1), P2(n)
#define P6(n) P4(n), P4(n^1), P4(n^1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
};
#define SETF(f, c) do { if (c) cpu.flags |= (f); else cpu.flags &= ~(f); } while (0)
static inline void szp8(u8 v) { SETF(F_ZF, v == 0); SETF(F_SF, v & 0x80); SETF(F_PF, parity_tab[v]); }
static inline void szp16(u16 v) { SETF(F_ZF, v == 0); SETF(F_SF, v & 0x8000); SETF(F_PF, parity_tab[v & 0xFF]); }

static inline u32 alu(int op, u32 a, u32 b, int w)
{
    u32 m = w ? 0xFFFF : 0xFF, sb = w ? 0x8000 : 0x80, r = 0;
    int cf = cpu.flags & F_CF;
    switch (op) {
    case 0: r = a + b; SETF(F_CF, r > m); SETF(F_OF, (~(a ^ b) & (a ^ r)) & sb); SETF(F_AF, (a ^ b ^ r) & 0x10); break;
    case 1: r = a | b; cpu.flags &= ~(F_CF | F_OF | F_AF); break;
    case 2: r = a + b + cf; SETF(F_CF, r > m); SETF(F_OF, (~(a ^ b) & (a ^ r)) & sb); SETF(F_AF, (a ^ b ^ r) & 0x10); break;
    case 3: r = a - b - cf; SETF(F_CF, (u32)b + cf > a); SETF(F_OF, ((a ^ b) & (a ^ r)) & sb); SETF(F_AF, (a ^ b ^ r) & 0x10); break;
    case 4: r = a & b; cpu.flags &= ~(F_CF | F_OF | F_AF); break;
    case 5: case 7: r = a - b; SETF(F_CF, b > a); SETF(F_OF, ((a ^ b) & (a ^ r)) & sb); SETF(F_AF, (a ^ b ^ r) & 0x10); break;
    case 6: r = a ^ b; cpu.flags &= ~(F_CF | F_OF | F_AF); break;
    }
    r &= m;
    if (w) szp16(r); else szp8(r);
    return r;
}

static inline u32 incdec(u32 a, int dec, int w)
{
    u32 m = w ? 0xFFFF : 0xFF, sb = w ? 0x8000 : 0x80, r;
    if (!dec) { r = (a + 1) & m; SETF(F_OF, r == sb); SETF(F_AF, (r & 0xF) == 0); }
    else { r = (a - 1) & m; SETF(F_OF, a == sb); SETF(F_AF, (a & 0xF) == 0); }
    if (w) szp16(r); else szp8(r);
    return r;
}

static inline u32 shift(int op, u32 v, int cnt, int w)
{
    u32 m = w ? 0xFFFF : 0xFF, sb = w ? 0x8000 : 0x80;
    int bits = w ? 16 : 8;
    cnt &= 0x1F;  /* 186+ behaviour; 8086 used the full count, harmless here */
    if (!cnt) return v;
    u32 r = v; int cf = cpu.flags & F_CF;
    switch (op) {
    case 0: for (int i = 0; i < cnt; i++) { cf = (r & sb) != 0; r = ((r << 1) | cf) & m; } SETF(F_CF, cf); SETF(F_OF, ((r & sb) != 0) ^ cf); return r;
    case 1: for (int i = 0; i < cnt; i++) { cf = r & 1; r = (r >> 1) | (cf ? sb : 0); } SETF(F_CF, cf); SETF(F_OF, ((r ^ (r << 1)) & sb) != 0); return r;
    case 2: for (int i = 0; i < cnt; i++) { int nc = (r & sb) != 0; r = ((r << 1) | cf) & m; cf = nc; } SETF(F_CF, cf); SETF(F_OF, ((r & sb) != 0) ^ cf); return r;
    case 3: for (int i = 0; i < cnt; i++) { int nc = r & 1; r = (r >> 1) | (cf ? sb : 0); cf = nc; } SETF(F_CF, cf); SETF(F_OF, ((r ^ (r << 1)) & sb) != 0); return r;
    case 4: case 6:
        cf = cnt <= bits ? ((v << (cnt - 1)) & sb) != 0 : 0;
        r = (v << cnt) & m; SETF(F_CF, cf); SETF(F_OF, ((r & sb) != 0) ^ cf); break;
    case 5:
        cf = cnt <= bits ? (v >> (cnt - 1)) & 1 : 0;
        r = cnt < 32 ? v >> cnt : 0; SETF(F_CF, cf); SETF(F_OF, cnt == 1 && (v & sb)); break;
    case 7: {
        s32 sv = w ? (s16)v : (s8)v;
        if (cnt >= bits) cnt = bits;
        cf = (sv >> (cnt - 1)) & 1;
        r = (u32)(sv >> cnt) & m; SETF(F_CF, cf); cpu.flags &= ~F_OF; break; }
    }
    if (w) szp16(r); else szp8(r);
    return r;
}

static inline int cond(int c)
{
    u16 f = cpu.flags; int r;
    switch (c >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return (c & 1) ? !r : r;
}


static inline u8 *r8p(int i) { return ((u8 *)&cpu.r[i & 3]) + ((i >> 2) & 1); }
#define AL8 (*r8p(0))

/* multiply / divide; div helpers return nonzero on divide fault */
static inline void op_mul8(u8 v) { u16 r = (u16)(cpu.r[AX] & 0xFF) * v; cpu.r[AX] = r; SETF(F_CF | F_OF, r >> 8); szp8(r & 0xFF); }
static inline void op_imul8(u8 v) { s16 r = (s16)(s8)(cpu.r[AX] & 0xFF) * (s8)v; cpu.r[AX] = r; SETF(F_CF | F_OF, r != (s8)r); szp8(r & 0xFF); }
static inline void op_mul16(u16 v) { u32 r = (u32)cpu.r[AX] * v; cpu.r[AX] = r; cpu.r[DX] = r >> 16; SETF(F_CF | F_OF, cpu.r[DX]); szp16(r & 0xFFFF); }
static inline void op_imul16(u16 v) { s32 r = (s32)(s16)cpu.r[AX] * (s16)v; cpu.r[AX] = r; cpu.r[DX] = (u32)r >> 16; SETF(F_CF | F_OF, r != (s16)r); szp16(r & 0xFFFF); }
static inline int op_div8(u8 v) { if (!v) return 1; u16 a = cpu.r[AX]; u16 q = a / v; if (q > 0xFF) return 1; cpu.r[AX] = (u16)((a % v) << 8) | q; return 0; }
static inline int op_idiv8(u8 v) { if (!v) return 1; s16 a = cpu.r[AX]; s16 q = a / (s8)v; if (q > 127 || q < -128) return 1; cpu.r[AX] = (u16)(((u8)(a % (s8)v)) << 8) | (u8)q; return 0; }
static inline int op_div16(u16 v) { if (!v) return 1; u32 a = ((u32)cpu.r[DX] << 16) | cpu.r[AX]; u32 q = a / v; if (q > 0xFFFF) return 1; cpu.r[AX] = q; cpu.r[DX] = a % v; return 0; }
static inline int op_idiv16(u16 v) { if (!v) return 1; s32 a = (s32)(((u32)cpu.r[DX] << 16) | cpu.r[AX]); s32 q = a / (s16)v; if (q > 32767 || q < -32768) return 1; cpu.r[AX] = q; cpu.r[DX] = a % (s16)v; return 0; }
static inline u8 op_neg8(u8 v) { u8 r = alu(5, 0, v, 0); SETF(F_CF, v != 0); return r; }
static inline u16 op_neg16(u16 v) { u16 r = alu(5, 0, v, 1); SETF(F_CF, v != 0); return r; }
static inline u8 op_inc8(u8 v, int dec) { int cf = cpu.flags & F_CF; u8 r = incdec(v, dec, 0); cpu.flags = (cpu.flags & ~F_CF) | cf; return r; }
static inline u16 op_inc16(u16 v, int dec) { int cf = cpu.flags & F_CF; u16 r = incdec(v, dec, 1); cpu.flags = (cpu.flags & ~F_CF) | cf; return r; }

void string_op_ex(u8 op, int seg_ovr, int rep_mode);
#endif
