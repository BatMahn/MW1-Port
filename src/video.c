/* Video: EGA (mode 0Dh planar 320x200x16), Tandy 1000 (mode 09h, 320x200x16
 * packed, 4 interleaved banks at B800) and 80/40-column text for the
 * launcher. Renders into a 32-bit framebuffer. */
#include "machine.h"
#include "runtime.h"
#include <string.h>
#include "pc98.h"
#include "../third_party/font8x8_basic.h"

u32 framebuffer[FB_W * FB_H];
int fb_width = 640;

static u8 mode = 3;
static u8 planes[4][65536];
static u8 latch[4];
static u8 seq_idx, seq[8];
static u8 gc_idx, gc[16];
static u8 crtc_idx, crtc[32];
static u8 attr_idx, attr_ff, attr[32];
static u8 tandy_idx, tandy_pal[16], tandy_mode_ctl;
static u8 tandy_page = 0x3F; /* CRT page / CPU page register (3DF) */
static int cur_x, cur_y;
int video_flips;

static u32 ega_rgb(u8 p)
{
    /* 200-line EGA modes drive the monitor in CGA-compatible IRGB; colour 6 shows as brown */
    int i = (p >> 4) & 1, r = (p >> 2) & 1, g = (p >> 1) & 1, b = p & 1;
    u32 R = r * 0xAA + i * 0x55, G = g * 0xAA + i * 0x55, B = b * 0xAA + i * 0x55;
    if (r && g && !b && !i) G = 0x55;
    return 0xFF000000 | (R << 16) | (G << 8) | B;
}
static u32 irgb(u8 c)
{
    c &= 15;
    return ega_rgb((c & 7) | ((c & 8) << 1));
}

void video_init(void)
{
    for (int i = 0; i < 16; i++) { attr[i] = (i & 7) | ((i & 8) << 1); tandy_pal[i] = i; }
    gc[8] = 0xFF; seq[2] = 0x0F; gc[7] = 0x0F; attr[0x12] = 0x0F;
    video_set_mode(3);
}

u8 video_get_mode(void) { return mode; }

void video_set_mode(u8 m)
{
    int clear = !(m & 0x80);
    m &= 0x7F;
    mode = m;
    logmsg("video mode %02x", m);
    memset(crtc, 0, sizeof crtc);
    for (int i = 0; i < 16; i++) { attr[i] = (i & 7) | ((i & 8) << 1); tandy_pal[i] = i; }
    gc[0] = gc[1] = gc[2] = gc[3] = gc[4] = gc[5] = 0; gc[7] = 0x0F; gc[8] = 0xFF; seq[2] = 0x0F;
    if (m == 0x0D) crtc[0x13] = 20;
    if (clear) {
        if (m == 0x0D) memset(planes, 0, sizeof planes);
        else if (m <= 3 || m == 7) for (int i = 0; i < 0x4000; i += 2) { mem[0xB8000 + i] = ' '; mem[0xB8001 + i] = 7; }
        else memset(mem + 0xB8000, 0, 0x8000);
    }
    cur_x = cur_y = 0;
    mem[0x449] = m;
    mem[0x44A] = (m == 0 || m == 1) ? 40 : 80; mem[0x44B] = 0;
    mem[0x484] = 24;
    fb_width = (m == 0x0D || m == 0x09 || m == 0x04 || m == 0x05 || m == 0 || m == 1) ? 320 : 640;
}

/* ---------------- EGA memory access ---------------- */
static inline u8 rotate(u8 v) { int n = gc[3] & 7; return (u8)((v >> n) | (v << ((8 - n) & 7))); }
static inline u8 logic(u8 v, u8 l)
{
    switch ((gc[3] >> 3) & 3) { case 1: return v & l; case 2: return v | l; case 3: return v ^ l; }
    return v;
}

u8 rb_slow(u32 a)
{
    if (machine_pc98) return pc98_rb(a);
    if (a >= 0xC0000) return mem[a];
    if (a < 0xB0000) {
        if (mode != 0x0D) return 0xFF;
        u16 o = a & 0xFFFF;
        for (int p = 0; p < 4; p++) latch[p] = planes[p][o];
        if (gc[5] & 8) {
            u8 r = 0;
            for (int p = 0; p < 4; p++) {
                if (!(gc[7] & (1 << p))) continue;
                u8 x = (gc[2] & (1 << p)) ? latch[p] : (u8)~latch[p];
                r |= ~x; /* bits that differ */
            }
            return ~r;
        }
        return latch[gc[4] & 3];
    }
    return mem[a];
}

void wb_slow(u32 a, u8 v)
{
    if (machine_pc98) { pc98_wb(a, v); return; }
    if (a >= 0xB0000) { if (a < 0xC0000) mem[a] = v; return; }  /* ROM area is read-only */
    if (mode != 0x0D) return;
    u16 o = a & 0xFFFF;
    u8 mm = seq[2] & 15, bm = gc[8];
    switch (gc[5] & 3) {
    case 0:
        for (int p = 0; p < 4; p++) {
            if (!(mm & (1 << p))) continue;
            u8 x = rotate(v);
            if (gc[1] & (1 << p)) x = (gc[0] & (1 << p)) ? 0xFF : 0;
            x = logic(x, latch[p]);
            planes[p][o] = (x & bm) | (latch[p] & ~bm);
        }
        break;
    case 1:
        for (int p = 0; p < 4; p++) if (mm & (1 << p)) planes[p][o] = latch[p];
        break;
    case 2:
        for (int p = 0; p < 4; p++) {
            if (!(mm & (1 << p))) continue;
            u8 x = (v & (1 << p)) ? 0xFF : 0;
            x = logic(x, latch[p]);
            planes[p][o] = (x & bm) | (latch[p] & ~bm);
        }
        break;
    default: break;
    }
}

void video_set_palette_reg(int idx, u8 val) { if (idx < 0x14) attr[idx] = val; }

/* ---------------- ports ---------------- */
u8 video_port_in(u16 port)
{
    switch (port) {
    case 0x3C1: return attr[attr_idx & 0x1F];
    case 0x3C5: return seq[seq_idx & 7];
    case 0x3CF: return gc[gc_idx & 15];
    case 0x3D5: return crtc[crtc_idx & 31];
    case 0x3DA: {
        attr_ff = 0;
        int r = video_in_retrace();
        u8 v = r ? 0x09 : 0;
        static int hb; hb ^= 1; if (hb) v |= 1;   /* display-enable toggles while scanning */
        return v;
    }
    }
    return 0xFF;
}

void video_port_out(u16 port, u8 v)
{
    switch (port) {
    case 0x3C0:
        if (!attr_ff) attr_idx = v; else { if ((attr_idx & 0x1F) < 0x14) attr[attr_idx & 0x1F] = v; }
        attr_ff ^= 1; break;
    case 0x3C4: seq_idx = v; break;
    case 0x3C5: seq[seq_idx & 7] = v; break;
    case 0x3CE: gc_idx = v; break;
    case 0x3CF: gc[gc_idx & 15] = v; break;
    case 0x3D4: crtc_idx = v; break;
    case 0x3D5: crtc[crtc_idx & 31] = v; if ((crtc_idx & 31) == 0x0C) video_flips++; break;
    case 0x3D8: tandy_mode_ctl = v; break;
    case 0x3DA: tandy_idx = v; break;
    case 0x3DE: if ((tandy_idx & 0xF0) == 0x10) tandy_pal[tandy_idx & 15] = v & 15; break;
    case 0x3DF: tandy_page = v; break;
    }
}

/* ---------------- text output ---------------- */
static void scroll_up(void)
{
    int cols = mem[0x44A] ? mem[0x44A] : 80;
    memmove(mem + 0xB8000, mem + 0xB8000 + cols * 2, cols * 2 * 24);
    for (int i = 0; i < cols; i++) { mem[0xB8000 + (24 * cols + i) * 2] = ' '; mem[0xB8000 + (24 * cols + i) * 2 + 1] = 7; }
}

void bios_teletype(u8 c, u8 attr_v)
{
    int cols = mem[0x44A] ? mem[0x44A] : 80;
    int text = mode <= 3 || mode == 7;
    switch (c) {
    case 7: return;
    case 8: if (cur_x) cur_x--; return;
    case 10: if (++cur_y > 24) { cur_y = 24; if (text) scroll_up(); } return;
    case 13: cur_x = 0; return;
    }
    if (text) { mem[0xB8000 + (cur_y * cols + cur_x) * 2] = c; if (attr_v) mem[0xB8000 + (cur_y * cols + cur_x) * 2 + 1] = attr_v; }
    if (++cur_x >= cols) { cur_x = 0; if (++cur_y > 24) { cur_y = 24; if (text) scroll_up(); } }
}

void console_putc(u8 c) { bios_teletype(c, 0); }

void bios_text_int10(void)
{
    u8 ah = cpu.r[AX] >> 8;
    int cols = mem[0x44A] ? mem[0x44A] : 80;
    switch (ah) {
    case 0x02: cur_y = cpu.r[DX] >> 8; cur_x = cpu.r[DX] & 0xFF; if (cur_y > 24) cur_y = 24; if (cur_x >= cols) cur_x = cols - 1; break;
    case 0x03: cpu.r[DX] = (cur_y << 8) | cur_x; cpu.r[CX] = 0x0607; break;
    case 0x06: case 0x07: {
        int n = cpu.r[AX] & 0xFF, t = cpu.r[CX] >> 8, l = cpu.r[CX] & 0xFF, b = cpu.r[DX] >> 8, r = cpu.r[DX] & 0xFF;
        u8 at = cpu.r[BX] >> 8;
        if (mode > 3 && mode != 7) break;
        if (b > 24) b = 24; if (r >= cols) r = cols - 1;
        int h = b - t + 1; if (!n || n > h) n = h;
        for (int y = 0; y < h; y++) {
            int dy = ah == 6 ? t + y : b - y, sy = ah == 6 ? dy + n : dy - n;
            for (int x = l; x <= r; x++) {
                u32 d = 0xB8000 + (dy * cols + x) * 2;
                if ((ah == 6 && sy <= b) || (ah == 7 && sy >= t)) { u32 s = 0xB8000 + (sy * cols + x) * 2; mem[d] = mem[s]; mem[d + 1] = mem[s + 1]; }
                else { mem[d] = ' '; mem[d + 1] = at; }
            }
        }
    } break;
    case 0x08: { u32 a = 0xB8000 + (cur_y * cols + cur_x) * 2; cpu.r[AX] = mem[a] | (mem[a + 1] << 8); } break;
    case 0x09: case 0x0A: {
        if (mode > 3 && mode != 7) { /* graphics: draw character with game font not supported, just advance */ break; }
        for (int i = 0; i < cpu.r[CX]; i++) {
            u32 a = 0xB8000 + (cur_y * cols + cur_x + i) * 2; if (a >= 0xBC000) break;
            mem[a] = cpu.r[AX] & 0xFF; if (ah == 9) mem[a + 1] = cpu.r[BX] & 0xFF;
        }
    } break;
    case 0x0E: bios_teletype(cpu.r[AX] & 0xFF, 0); break;
    }
}

/* ---------------- rendering ---------------- */
void video_render(void)
{
    if (mode == 0x0D) {
        u16 start = (crtc[0x0C] << 8) | crtc[0x0D];
        int pitch = crtc[0x13] ? crtc[0x13] * 2 : 40;
        u32 pal[16];
        for (int i = 0; i < 16; i++) pal[i] = ega_rgb(attr[i & attr[0x12]]);
        for (int y = 0; y < 200; y++) {
            u32 *out = framebuffer + y * FB_W;
            u16 o = start + y * pitch;
            for (int xb = 0; xb < 40; xb++, o++) {
                u8 p0 = planes[0][o], p1 = planes[1][o], p2 = planes[2][o], p3 = planes[3][o];
                for (int b = 7; b >= 0; b--) {
                    int c = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3);
                    *out++ = pal[c];
                }
            }
        }
        fb_width = 320;
    } else if (mode == 0x09) {
        /* Tandy 320x200x16: 4 banks of 8 KB, 160 bytes per line, high nibble is the left pixel */
        u32 base = 0xB8000;
        for (int y = 0; y < 200; y++) {
            u32 *out = framebuffer + y * FB_W;
            u32 a = base + (y & 3) * 0x2000 + (y >> 2) * 160;
            for (int x = 0; x < 160; x++) {
                u8 b = mem[a + x];
                *out++ = irgb(tandy_pal[b >> 4]);
                *out++ = irgb(tandy_pal[b & 15]);
            }
        }
        fb_width = 320;
    } else if (mode == 0x04 || mode == 0x05) {
        static const u8 cga[4] = { 0, 3, 5, 7 };
        for (int y = 0; y < 200; y++) {
            u32 *out = framebuffer + y * FB_W;
            u32 a = 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80;
            for (int x = 0; x < 80; x++) { u8 b = mem[a + x]; for (int s = 6; s >= 0; s -= 2) { int c = (b >> s) & 3; *out++ = irgb(c ? cga[c] | 8 : 0); } }
        }
        fb_width = 320;
    } else {
        int cols = mem[0x44A] == 40 ? 40 : 80;
        fb_width = cols * 8;
        static int blink; blink++;
        for (int row = 0; row < 25; row++)
            for (int col = 0; col < cols; col++) {
                u32 a = 0xB8000 + (row * cols + col) * 2;
                u8 ch = mem[a], at = mem[a + 1];
                u32 fg = irgb(at & 15), bg = irgb((at >> 4) & 7);
                for (int y = 0; y < 8; y++) {
                    u8 bits = ch < 128 ? (u8)font8x8_basic[ch][y] : (ch == 219 ? 0xFF : ch == 196 ? (y == 4 ? 0xFF : 0) : ch == 179 ? 0x18 : 0);
                    if (row == cur_y && col == cur_x && y == 7 && (blink & 16)) bits = 0xFF;
                    u32 *out = framebuffer + (row * 8 + y) * FB_W + col * 8;
                    for (int x = 0; x < 8; x++) out[x] = (bits & (1 << x)) ? fg : bg;  /* font8x8: bit0 = leftmost */
                }
            }
    }
}
