/* MechWarrior (1989) native port - machine state shared by the runtime,
 * the reference interpreter and the recompiled code. */
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;

enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS };

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

typedef struct {
    u16 r[8];
    u16 s[4];
    u16 ip;
    u16 flags;
    int halted;
} CPU;

extern CPU cpu;
#define MEM_SIZE (0x110000)
extern u8 mem[MEM_SIZE];

/* cycle accounting: one unit per instruction, like DOSBox "cycles" */
extern int64_t cycles_done;

/* memory access (EGA aperture A000-AFFF goes through the video module) */
u8   rb_slow(u32 a);
void wb_slow(u32 a, u8 v);
static inline u32 lin(u16 seg, u16 off) { return (((u32)seg << 4) + off) & 0xFFFFF; }
static inline u8 rb(u32 a) { a &= 0xFFFFF; if (a >= 0xA0000) return rb_slow(a); return mem[a]; }
extern int wlog_on; void wlog_add(u32 a, u8 old, u8 nw);
extern u32 watch_addr; void watch_hit(u32 a, u8 v);
static inline void wb(u32 a, u8 v) { a &= 0xFFFFF; if (a == watch_addr) watch_hit(a, v); if (wlog_on) wlog_add(a, a < 0xA0000 ? mem[a] : 0, v); if (a >= 0xA0000) { wb_slow(a, v); return; } mem[a] = v; }
static inline u16 rw(u32 a) { return rb(a) | (rb(a + 1) << 8); }
static inline void ww(u32 a, u16 v) { wb(a, v & 0xFF); wb(a + 1, v >> 8); }

/* interpreter */
void cpu_reset(void);
void cpu_run(int64_t ncycles);
void cpu_interrupt(u8 n);       /* push flags/cs/ip and vector */
void cpu_push(u16 v);
u16  cpu_pop(void);

/* runtime hooks */
void hle_callback(u8 n);        /* native service for callback opcode F1 nn */
u8   port_in(u16 port);
void port_out(u16 port, u8 v);
u16  port_inw(u16 port);
void port_outw(u16 port, u16 v);

/* misc */
void fatal(const char *fmt, ...);
void logmsg(const char *fmt, ...);
extern int log_enabled;

#endif
