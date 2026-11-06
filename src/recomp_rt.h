/* Interface between recompiled code (generated/rc_*.c) and the runtime. */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H
#include "machine.h"
#include "ops.h"
#include <stddef.h>

typedef struct {
    u32 off;            /* image offset of the block */
    u16 seg;            /* code segment, relative to load segment */
    u16 len;            /* bytes of machine code covered */
    u16 ninsn;          /* instructions in the block */
    u16 io;             /* bit0: touches ports / interrupts / interpreter; bit1: pure wait loop */
    u32 csum;           /* checksum of the code bytes (relocated words skipped) */
    const u16 *relocs;  /* offsets of relocated words inside the block, 0xFFFF-terminated */
    void (*fn)(void);
} RcBlock;

void cpu_step(void);
#endif
