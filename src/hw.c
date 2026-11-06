/* PC hardware and BIOS for the port.
 *
 * Timing model: the game runs against an emulated clock driven by executed
 * instructions (cycles_per_ms, same idea as DOSBox "cycles"). The PIT,
 * vertical retrace and BIOS tick all derive from that emulated clock, and the
 * main loop sleeps whenever emulated time gets ahead of wall time. The game
 * therefore runs at the speed of the machine it was written for no matter how
 * fast the host is, and the host CPU only does that much work per ms. */
#include "machine.h"
#include "runtime.h"
#include "pc98.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int cycles_per_ms = 30000;
int hle_retry;
static void poll_note(int nothing);

double emu_time_ms(void) { return (double)cycles_done / cycles_per_ms; }

/* ---------------- PIT ---------------- */
typedef struct { u16 reload; u8 mode, rw, latch_flip, write_flip; u16 latched; int latched_valid; double start_ms; } PitCh;
static PitCh pit[3];
static double pit0_next_ms;
static double PIT_HZ = 1193182.0;

static double pit_period_ms(int ch) { return (pit[ch].reload ? pit[ch].reload : 65536) * 1000.0 / PIT_HZ; }
static u16 pit_count(int ch)
{
    double el = (emu_time_ms() - pit[ch].start_ms) * PIT_HZ / 1000.0;
    u32 r = pit[ch].reload ? pit[ch].reload : 65536;
    u32 c = r - ((u32)el % r);
    if (pit[ch].mode == 3) c = (c * 2) % r;  /* square wave counts down by 2 */
    return c;
}
static void pit_write(int ch, u8 v)
{
    PitCh *p = &pit[ch];
    int done = 0;
    switch (p->rw) {
    case 1: p->reload = v; done = 1; break;
    case 2: p->reload = v << 8; done = 1; break;
    default: if (!p->write_flip) { p->reload = (p->reload & 0xFF00) | v; p->write_flip = 1; } else { p->reload = (p->reload & 0xFF) | (v << 8); p->write_flip = 0; done = 1; } break;
    }
    if (done) {
        p->start_ms = emu_time_ms();
        if (ch == 0) { pit0_next_ms = p->start_ms + pit_period_ms(0); logmsg("PIT0 reload %u (%.2f Hz)", p->reload, 1000.0 / pit_period_ms(0)); }
    }
}
static u8 pit_read(int ch)
{
    PitCh *p = &pit[ch];
    u16 v = p->latched_valid ? p->latched : pit_count(ch);
    u8 r;
    switch (p->rw) {
    case 1: r = v & 0xFF; p->latched_valid = 0; break;
    case 2: r = v >> 8; p->latched_valid = 0; break;
    default: if (!p->latch_flip) { r = v & 0xFF; p->latch_flip = 1; } else { r = v >> 8; p->latch_flip = 0; p->latched_valid = 0; } break;
    }
    return r;
}

/* ---------------- PIC ---------------- */
static u16 pic_mask = 0xFFB8, pic_isr, pic_irr;
long irq_count[16];
static int pic_read_isr[2] = { 1, 1 };   /* OCW3 read select; the PC-98 BIOS leaves both PICs reading ISR */
long opn_wcount;
long audio_mpu_count(void), audio_adlib_count(void);
void pic_report(void) { fprintf(stderr, "opn writes %ld, midi bytes %ld, adlib writes %ld\n", opn_wcount, audio_mpu_count(), audio_adlib_count()); fprintf(stderr, "pic irr=%04x isr=%04x mask=%04x IF=%d irqs:", pic_irr, pic_isr, pic_mask, (cpu.flags & F_IF) != 0); for (int i = 0; i < 16; i++) if (irq_count[i]) fprintf(stderr, " %d:%ld", i, irq_count[i]); fprintf(stderr, " vec08=%04x:%04x vec15=%04x:%04x\n", rw(0x22), rw(0x20), rw(0x56), rw(0x54)); }
static void pic_raise(int irq) { pic_irr |= 1 << irq; }
static void pic_check(void)
{
    if (!(cpu.flags & F_IF)) return;
    for (int j = 0; j < 16; j++) {
        /* priority order: master IRQ0..1, then the cascade line's slave IRQs, then the rest */
        int cas = machine_pc98 ? 7 : 2;
        int i = j < cas ? j : (j < cas + 8 ? j - cas + 8 : j - 8);
        if (i < 8 && i == cas) continue;
        if (pic_isr & (1 << i)) return;     /* higher priority in service */
        if ((pic_irr & ~pic_mask) & (1 << i)) {
            pic_irr &= ~(1 << i); pic_isr |= 1 << i;
            if (i >= 8) pic_isr |= 1 << (machine_pc98 ? 7 : 2);   /* cascade input in service on the master */
            cpu.halted = 0;
            irq_count[i]++;
            cpu_interrupt(i < 8 ? 8 + i : (machine_pc98 ? 0x10 + i - 8 : 0x70 + i - 8));
            return;
        }
    }
}

/* ---------------- keyboard ---------------- */
static u8 kbd_queue[64]; static int kq_head, kq_tail;
static u8 port60, port61 = 0x30;
static double kbd_next_ms;
static u8 kb_shift;

void kbd_host_event(int sc, int down, int unused)
{
    if (machine_pc98) { pc98_key_event(sc, down); return; }
    (void)unused;
    u8 code = sc & 0x7F;
    if (sc & 0x100) { int n = (kq_tail + 1) & 63; if (n != kq_head) { kbd_queue[kq_tail] = 0xE0; kq_tail = n; } }
    int n = (kq_tail + 1) & 63;
    if (n != kq_head) { kbd_queue[kq_tail] = down ? code : code | 0x80; kq_tail = n; }
}

/* BIOS keyboard buffer at 0040:001E */
static void bios_buf_put(u16 key)
{
    u16 head = rw(0x41A), tail = rw(0x41C), nt = tail + 2;
    if (nt >= 0x3E) nt = 0x1E;
    if (nt == head) return;
    ww(0x400 + tail, key); ww(0x41C, nt);
}
int kbd_peek(void) { u16 head = rw(0x41A), tail = rw(0x41C); if (head == tail) return -1; return rw(0x400 + head); }
int kbd_getkey(void)
{
    int k = kbd_peek(); if (k < 0) return -1;
    u16 head = rw(0x41A) + 2; if (head >= 0x3E) head = 0x1E; ww(0x41A, head); return k;
}
int kbd_getkey_blocking(void)
{
    int k = kbd_getkey();
    if (k < 0) { hle_retry = 1; return 0; }
    return k;
}
void kbd_flush(void) { ww(0x41A, rw(0x41C)); }

static const char sc_lo[] = "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char sc_hi[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static void bios_int9(void)
{
    u8 sc = port60;
    static int e0;
    
    if (sc == 0xE0) { e0 = 1; return; }
    int up = sc & 0x80; u8 c = sc & 0x7F;
    if (c == 0x2A || c == 0x36) { if (up) kb_shift &= ~3; else kb_shift |= (c == 0x2A ? 2 : 1); }
    else if (c == 0x1D) { if (up) kb_shift &= ~4; else kb_shift |= 4; }
    else if (c == 0x38) { if (up) kb_shift &= ~8; else kb_shift |= 8; }
    else if (!up) {
        u8 ascii = 0;
        if (!e0 && c < sizeof sc_lo) ascii = (kb_shift & 3) ? sc_hi[c] : sc_lo[c];
        if ((kb_shift & 4) && ascii >= 'a' && ascii <= 'z') ascii -= 0x60;
        if (c >= 0x3B && c <= 0x44 && (kb_shift & 3)) c += 0x19;  /* shift-F1..F10 */
        bios_buf_put((c << 8) | ascii);
    }
    mem[0x417] = kb_shift;
    e0 = 0;
}

/* ---------------- ports ---------------- */
static int mpu_ack;
static void port_out_pit_ctl(u8 v)
{
    int ch = v >> 6; if (ch == 3) return;
    if (((v >> 4) & 3) == 0) { pit[ch].latched = pit_count(ch); pit[ch].latched_valid = 1; pit[ch].latch_flip = 0; return; }
    pit[ch].rw = (v >> 4) & 3; pit[ch].mode = (v >> 1) & 7; pit[ch].write_flip = 0; pit[ch].latch_flip = 0;
}
static u8 adlib_idx, adlib_status;
static double adlib_t1_ms, adlib_t2_ms; static u8 adlib_t1, adlib_t2, adlib_ctl;


u8 opn_read(int off); void opn_write(int off, u8 v); void opn_service(double now_ms); int opn_irq(void);
static u8 pc98_port_in(u16 port)
{
    int h; u8 v = pc98_video_in(port, &h); if (h) return v;
    switch (port) {
    case 0x00: return pic_read_isr[0] ? (pic_isr & 0xFF) : (pic_irr & 0xFF);
    case 0x02: return pic_mask & 0xFF;
    case 0x08: return pic_read_isr[1] ? (pic_isr >> 8) : (pic_irr >> 8);
    case 0x0A: return pic_mask >> 8;
    case 0x41: return port60;
    case 0x43: return 0x85 | (pc98_kbd_pending() ? 0x02 : 0);
    case 0x71: return pit_read(0);
    case 0x73: return pit_read(1);
    case 0x75: return pit_read(2);
    case 0x188: return opn_read(0);
    case 0x18A: return opn_read(1);
    case 0xA460: return 0xFF;                /* no PC-9801-86 */
    }
    logmsg("pc98 in %04x", port);
    return 0xFF;
}
static void pc98_port_out(u16 port, u8 v);

u8 port_in(u16 port)
{
    if (machine_pc98) return pc98_port_in(port);
    switch (port) {
    case 0x20: return pic_irr;
    case 0x21: return pic_mask;
    case 0x40: case 0x41: case 0x42: return pit_read(port - 0x40);
    case 0x60: return port60;
    case 0x61: { static int t; t ^= 0x10; return (port61 & ~0x30) | t | ((pit_count(2) > (pit[2].reload ? pit[2].reload : 65536) / 2) ? 0x20 : 0); }
    case 0x64: return 0x1C;
    case 0x201: return 0xFF; /* no joystick */
    case 0x388: {
        u8 s = 0; double t = emu_time_ms();
        if ((adlib_ctl & 1) && t >= adlib_t1_ms) s |= 0xC0;
        if ((adlib_ctl & 2) && t >= adlib_t2_ms) s |= 0xA0;
        return s | adlib_status;
    }
    case 0x331: return mpu_ack ? 0x00 : 0x80;  /* MPU-401 status: bit7 clear = data waiting */
    case 0x330: if (mpu_ack) { mpu_ack = 0; return 0xFE; } return 0xFF;
    }
    if (port >= 0x3C0 && port <= 0x3DF) return video_port_in(port);
    return 0xFF;
}

static void pic_eoi(int slave)
{
    for (int i = slave ? 8 : 0; i < (slave ? 16 : 8); i++) if (pic_isr & (1 << i)) { pic_isr &= ~(1 << i); break; }
}
static void pc98_port_out(u16 port, u8 v)
{
    if (pc98_video_out(port, v)) return;
    switch (port) {
    case 0x00: if ((v & 0x18) == 0x08) { if (v & 2) pic_read_isr[0] = v & 1; return; } if ((v & 0xE0) == 0x20 || (v & 0xE0) == 0x60) pic_eoi(0); return;
    case 0x02: pic_mask = (pic_mask & 0xFF00) | v; return;
    case 0x08: if ((v & 0x18) == 0x08) { if (v & 2) pic_read_isr[1] = v & 1; return; } if ((v & 0xE0) == 0x20 || (v & 0xE0) == 0x60) pic_eoi(1); return;
    case 0x0A: pic_mask = (pic_mask & 0x00FF) | (v << 8); if (log_enabled > 1) logmsg("slave mask %02x at %04x:%04x", v, cpu.s[CS], cpu.ip); return;
    case 0x71: pit_write(0, v); return;
    case 0x73: pit_write(1, v); return;
    case 0x75: pit_write(2, v); return;
    case 0x77: port_out_pit_ctl(v); return;
    case 0x188: opn_write(0, v); opn_wcount++; if (log_enabled > 1) logmsg("OPN reg %02x", v); return;
    case 0x18A: opn_write(1, v); if (log_enabled > 1) logmsg("OPN data %02x", v); return;
    case 0x37: case 0x35: case 0x43: case 0x5F: return;
    }
    logmsg("pc98 out %04x <- %02x", port, v);
}

void port_out(u16 port, u8 v)
{
    if (machine_pc98) { pc98_port_out(port, v); return; }
    switch (port) {
    case 0x20: if (v == 0x20) pic_eoi(0); return;
    case 0x21: pic_mask = (pic_mask & 0xFF00) | v; return;
    case 0x40: case 0x41: case 0x42: pit_write(port - 0x40, v);
        if (port == 0x42) audio_speaker((port61 & 3) == 3, PIT_HZ / (pit[2].reload ? pit[2].reload : 65536));
        return;
    case 0x43: port_out_pit_ctl(v); return;
    case 0x4300: {
        int ch = v >> 6; if (ch == 3) return;
        if (((v >> 4) & 3) == 0) { pit[ch].latched = pit_count(ch); pit[ch].latched_valid = 1; pit[ch].latch_flip = 0; return; }
        pit[ch].rw = (v >> 4) & 3; pit[ch].mode = (v >> 1) & 7; pit[ch].write_flip = 0; pit[ch].latch_flip = 0;
        return; }
    case 0x61: port61 = v; audio_speaker((v & 3) == 3, PIT_HZ / (pit[2].reload ? pit[2].reload : 65536)); return;
    case 0x388: adlib_idx = v; return;
    case 0x389:
        if (adlib_idx == 2) adlib_t1 = v;
        else if (adlib_idx == 3) adlib_t2 = v;
        else if (adlib_idx == 4) {
            if (v & 0x80) { adlib_status = 0; return; }
            adlib_ctl = v;
            if (v & 1) adlib_t1_ms = emu_time_ms() + (256 - adlib_t1) * 0.080;
            if (v & 2) adlib_t2_ms = emu_time_ms() + (256 - adlib_t2) * 0.320;
            return;
        }
        audio_adlib_write(adlib_idx, v);
        return;
    case 0xC0: audio_tandy_write(v); return;
    case 0x330: if (getenv("MW_MPULOG")) fprintf(stderr, "mpu data %02x\n", v); audio_mpu_data(v); return;
    case 0x331: if (getenv("MW_MPULOG")) fprintf(stderr, "mpu cmd %02x\n", v); if (v == 0xFF || v == 0x3F) mpu_ack = 1; return;  /* reset / UART mode -> ACK */
    }
    if (port >= 0x3C0 && port <= 0x3DF) { video_port_out(port, v); return; }
}
u16 port_inw(u16 p) { return port_in(p) | (port_in(p + 1) << 8); }
void port_outw(u16 p, u16 v) { port_out(p, v & 0xFF); port_out(p + 1, v >> 8); }

/* ---------------- video retrace (EGA: 60 Hz in 200-line modes) ---------------- */
int video_in_retrace(void)
{
    double t = emu_time_ms(), period = 1000.0 / 60.0;
    double ph = t - (long long)(t / period) * period;
    return ph >= period - 1.25;
}

/* ---------------- BIOS ---------------- */
static void int10(void)
{
    u8 ah = cpu.r[AX] >> 8, al = cpu.r[AX] & 0xFF;
    switch (ah) {
    case 0x00: video_set_mode(al); break;
    case 0x0B: break;
    case 0x0F: cpu.r[AX] = (mem[0x44A] << 8) | video_get_mode(); cpu.r[BX] &= 0x00FF; break;
    case 0x10:
        if (al == 0) video_set_palette_reg(cpu.r[BX] & 0xFF, cpu.r[BX] >> 8);
        else if (al == 1) video_set_palette_reg(0x11, cpu.r[BX] >> 8);
        else if (al == 2) { for (int i = 0; i < 17; i++) video_set_palette_reg(i < 16 ? i : 0x11, rb(lin(cpu.s[ES], cpu.r[DX] + i))); }
        break;
    case 0x11: break;
    case 0x12: if ((cpu.r[BX] & 0xFF) == 0x10) { cpu.r[BX] = 0x0003; cpu.r[CX] = 0x0009; } break; /* EGA, 256K, colour */
    case 0x1A: break; /* not VGA: leave AL unchanged */
    default: bios_text_int10(); break;
    }
}

static void int16(void)
{
    u8 ah = cpu.r[AX] >> 8;
    switch (ah & 0xEF) {
    case 0x00: { int k = kbd_getkey(); if (k < 0) { hle_retry = 1; break; } cpu.r[AX] = k; logmsg("int16 got %04x", k); } break;
    case 0x01: { int k = kbd_peek(); poll_note(k < 0); if (k < 0) cpu.flags |= F_ZF; else { cpu.flags &= ~F_ZF; cpu.r[AX] = k; } } break;
    case 0x02: cpu.r[AX] = (cpu.r[AX] & 0xFF00) | kb_shift; break;
    }
}

/* ---------------- idle detection ----------------
 * When the game sits in a tight loop polling the keyboard/mouse with nothing
 * arriving, nothing it does can change until the next interrupt. We then skip
 * emulated time straight to that interrupt instead of executing the empty
 * polls. The game sees exactly the same timer and input timing. */
int idle_hint; static int64_t last_poll; static int poll_run;
void poll_note_ext(int nothing) { poll_note(nothing); }
static void poll_note(int nothing)
{
    if (nothing && cycles_done - last_poll < 400) { if (++poll_run > 24) idle_hint = 1; }
    else poll_run = 0;
    last_poll = cycles_done;
}

/* ---------------- mouse driver (INT 33h), as MOUSE.COM provided ---------------- */
static int ms_x = 320, ms_y = 100, ms_btn, ms_mick_x, ms_mick_y, ms_rel[3], ms_press[3];
void mouse_host_motion(int dx, int dy)
{
    if (machine_pc98) { pc98_mouse_motion(dx, dy); return; }
    ms_mick_x += dx; ms_mick_y += dy * 2;
    ms_x += dx; ms_y += dy;
    if (ms_x < 0) ms_x = 0; if (ms_x > 639) ms_x = 639; if (ms_y < 0) ms_y = 0; if (ms_y > 199) ms_y = 199;
}
void mouse_host_button(int b, int down)
{
    if (machine_pc98) { pc98_mouse_button(b, down); return; }
    if (b < 0 || b > 2) return;
    if (down) { ms_btn |= 1 << b; ms_press[b]++; } else { ms_btn &= ~(1 << b); ms_rel[b]++; }
}
static void int33(void)
{
    switch (cpu.r[AX]) {
    case 0: cpu.r[AX] = 0xFFFF; cpu.r[BX] = 2; ms_btn = 0; break;
    case 3: cpu.r[BX] = ms_btn; cpu.r[CX] = ms_x; cpu.r[DX] = ms_y; poll_note(1); break;
    case 4: ms_x = cpu.r[CX]; ms_y = cpu.r[DX]; break;
    case 5: { int b = cpu.r[BX] & 3; cpu.r[AX] = ms_btn; cpu.r[BX] = ms_press[b]; ms_press[b] = 0; cpu.r[CX] = ms_x; cpu.r[DX] = ms_y; } break;
    case 6: { int b = cpu.r[BX] & 3; poll_note(!ms_rel[b]); cpu.r[AX] = ms_btn; cpu.r[BX] = ms_rel[b]; ms_rel[b] = 0; cpu.r[CX] = ms_x; cpu.r[DX] = ms_y; } break;
    case 11: poll_note(!ms_mick_x && !ms_mick_y); cpu.r[CX] = ms_mick_x; cpu.r[DX] = ms_mick_y; ms_mick_x = ms_mick_y = 0; break;
    default: break;
    }
}

void runtime_irq_service(void);

void hle_callback(u8 n)
{
    u16 start_ip = cpu.ip - 2;
    hle_retry = 0;
    switch (n) {
    case 0x08: { u32 t = rw(0x46C) | (rw(0x46E) << 16); t++; ww(0x46C, t); ww(0x46E, t >> 16); } break;
    case 0x09: if (machine_pc98) pc98_bios_int9(); else bios_int9(); break;
    case 0x18: if (machine_pc98) pc98_int18(); break;
    case 0x0A: if (machine_pc98) { pic_eoi(0); } else port_out(0x20, 0x20); break;   /* EOI helper used by the stubs */
    case 0xF0: pic_eoi(0); break;                 /* PC-98 default IRQ stub: master EOI */
    case 0xF1: pic_eoi(1); pic_eoi(0); break;     /* PC-98 default IRQ stub: slave + cascade EOI */
    case 0x10: int10(); break;
    case 0x11: cpu.r[AX] = 0x0063; break;     /* equipment: 80x25 colour, 1 floppy, no FPU */
    case 0x12: cpu.r[AX] = 640; break;
    case 0x13: cpu.flags &= ~F_CF; cpu.r[AX] &= 0xFF; break;
    case 0x15: if ((cpu.r[AX] >> 8) == 0xC0) { cpu.flags |= F_CF; cpu.r[AX] = 0x8600; } else { cpu.flags |= F_CF; cpu.r[AX] = 0x8600; } break;
    case 0x16: int16(); break;
    case 0x33: int33(); break;
    case 0x1A: if ((cpu.r[AX] >> 8) == 0) { cpu.r[CX] = rw(0x46E); cpu.r[DX] = rw(0x46C); cpu.r[AX] &= 0xFF00; } break;
    case 0x20: cpu.r[AX] = 0x4C00; int21(); break;
    case 0x21: int21(); break;
    case 0x1B: case 0x1C: case 0x23: case 0x24: case 0x28: case 0x2F: case 0x67: break;
    default: break;
    }
    if (n != 0x08 && n != 0x09 && n != 0x0A) {
        /* service results travel back in CF/ZF: patch them into the flags the stub's IRET will pop */
        u32 fl = lin(cpu.s[SS], cpu.r[SP] + 4);
        ww(fl, (rw(fl) & ~(F_CF | F_ZF)) | (cpu.flags & (F_CF | F_ZF)));
    }
    if (hle_retry) {
        /* nothing to return yet (waiting for a key): enable interrupts and re-run the call */
        cpu.ip = start_ip;
        cpu.flags |= F_IF;
        cycles_done += 20;
        /* the iret in the stub would restore old IF; set it in the saved flags too */
        u32 fl = lin(cpu.s[SS], cpu.r[SP] + 4);
        ww(fl, rw(fl) | F_IF);
    }
}

/* Interrupt stub table at F000:0000: each vector n -> F000:n*8 : F1 n CF.
 * Hardware IRQ stubs also issue EOI and chain to INT 1Ch like the real BIOS. */
void bios_init(void)
{
    for (int n = 0; n < 256; n++) {
        u32 a = 0xF0000 + n * 8;
        mem[a] = 0xF1; mem[a + 1] = n; mem[a + 2] = 0xCF;
        ww(n * 4, n * 8); ww(n * 4 + 2, 0xF000);
    }
    /* INT 8: tick, INT 1Ch, EOI, IRET */
    u8 s8[] = { 0xF1, 0x08, 0xCD, 0x1C, 0xF1, 0x0A, 0xCF };
    memcpy(mem + 0xF0000 + 8 * 8, s8, sizeof s8);
    u8 s9[] = { 0xF1, 0x09, 0xF1, 0x0A, 0xCF };
    memcpy(mem + 0xF0000 + 9 * 8, s9, sizeof s9);
    /* BIOS data area */
    ww(0x41A, 0x1E); ww(0x41C, 0x1E);
    ww(0x410, 0x0063); ww(0x413, 640);
    ww(0x463, 0x3D4);
    mem[0x487] = 0x60; mem[0x488] = 0x09;  /* EGA info: 256K, colour monitor */
    mem[0xFFFFE] = 0xFC;  /* machine id: AT. Tandy detection is driven by the launcher choice instead */
    if (machine_pc98) {
        pc98_bios_init(); PIT_HZ = 2457600.0; pic_mask = 0xFFFF & ~0x0007;
        /* like the PC-98 BIOS, unclaimed hardware interrupts are acknowledged and ignored */
        if (!getenv("MW_OLDSTUB")) for (int n = 0x0B; n <= 0x17; n++) {
            if (n == 0x0A) continue;
            u32 a = 0xF0000 + n * 8;
            mem[a] = 0xF1; mem[a + 1] = n >= 0x10 ? 0xF1 : 0xF0; mem[a + 2] = 0xCF;
        }
        if (!getenv("MW_OLDSTUB")) { u32 a = 0xF0000 + 0x08 * 8; u8 s8[] = { 0xF1, 0xF0, 0xCF }; memcpy(mem + a, s8, 3); }   /* timer: no IBM tick/1Ch chain */
    }
    pit[0].reload = 0; pit0_next_ms = pit_period_ms(0);
}

/* run due hardware interrupts; called between CPU slices */
static double next_vs;
void runtime_irq_service(void)
{
    double t = emu_time_ms();
    if (t >= pit0_next_ms) {
        pic_raise(0);
        pit0_next_ms += pit_period_ms(0);
        if (pit0_next_ms < t - 50) pit0_next_ms = t + pit_period_ms(0);
    }
    if (machine_pc98) {
        opn_service(t);
        { static double next_ms; if (t >= next_ms) { next_ms = t + pc98_mouse_period_ms(); if (pc98_mouse_irq_enabled()) pic_raise(13); } }
        { static int prev; int now = opn_irq();   /* edge-triggered PIC: raise on the rising edge */
          if (now != prev && log_enabled > 1) logmsg("OPN irq line %d (isr %04x irr %04x mask %04x)", now, pic_isr, pic_irr, pic_mask);
          if (now && !prev) pic_raise(12);           /* 26K board on INT5 = IRQ12 (INT 14h) */
          prev = now; }
        if (t >= next_vs) { next_vs = t + 1000.0 / 56.42; if (pc98_take_vsync_irq()) pic_raise(2); }
        if (pc98_kbd_pending() && t >= kbd_next_ms && !(pic_irr & 2) && !(pic_isr & 2)) { port60 = pc98_kbd_next(); pic_raise(1); kbd_next_ms = t + 2.0; }
    } else
    if (kq_head != kq_tail && t >= kbd_next_ms && !(pic_irr & 2) && !(pic_isr & 2)) {
        port60 = kbd_queue[kq_head]; kq_head = (kq_head + 1) & 63;
        pic_raise(1);
        logmsg("kbd irq sc=%02x isr=%02x mask=%02x IF=%d vec9=%04x:%04x", port60, pic_isr, pic_mask, (cpu.flags & F_IF) != 0, rw(0x26), rw(0x24));
        kbd_next_ms = t + 2.0;
    }
    pic_check();
}

double next_irq_ms(void) { return pit0_next_ms; }

#include "snap.h"
void hw_snap(FILE *f, int save)
{
    S(pit); S(pit0_next_ms); S(PIT_HZ); S(pic_mask); S(pic_isr); S(pic_irr); S(pic_read_isr);
    S(kbd_queue); S(kq_head); S(kq_tail); S(port60); S(port61); S(kbd_next_ms); S(kb_shift);
    S(mpu_ack); S(adlib_idx); S(adlib_status); S(adlib_t1_ms); S(adlib_t2_ms); S(adlib_t1); S(adlib_t2); S(adlib_ctl);
    S(ms_x); S(ms_y); S(ms_btn); S(ms_mick_x); S(ms_mick_y); S(ms_rel); S(ms_press);
    S(last_poll); S(poll_run); S(next_vs); S(cycles_per_ms);
    if (!save && getenv("MW_SNAPDBG")) fprintf(stderr, "pic mask %04x isr %04x irr %04x kbd_next %.1f kq %d/%d\n", pic_mask, pic_isr, pic_irr, kbd_next_ms, kq_head, kq_tail);
}
