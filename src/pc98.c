/* NEC PC-9801 hardware for the PC-98 edition.
 *
 * Graphics: 640x400, 4 planes (B A8000, R B0000, G B8000, I E0000), two
 * banks selected by ports A4 (display) / A6 (CPU access), GRCG for fast
 * plane writes (ports 7C/7E), 16-colour analog palette (A8/AA/AC/AE).
 * Text: VRAM at A0000 (codes) / A2000 (attributes), 80x25 over the graphics.
 * GDC status (60/A0) supplies vertical sync; IRQ2 (INT 0Ah) fires each frame
 * once armed by a write to port 64.
 * Keyboard: 8251 at 41/43, IRQ1, PC-98 scan codes; BIOS INT 18h. */
#include "machine.h"
#include "runtime.h"
#include "pc98.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../third_party/font8x8_basic.h"

int machine_pc98;
int english_art_on;
static int ms_dx, ms_dy, ms_lx, ms_ly, ms_btn;
static u8 ms_portc, ms_freq = 1;
static u8 mouse_read_a(void); static void mouse_write_c(u8 v);
static u8 cg_hi, cg_lo, cg_line;   /* character-generator window (ports A1/A3/A5/A9) */
static u8 cg_read(void);

/* character ROM stand-in: fonts/pc98font.bin (public domain glyphs) */
static u8 *font98;
const char *font98_path = "fonts/pc98font.bin";
static void font98_load(void)
{
    FILE *f = fopen(font98_path, "rb");
    if (!f) { logmsg("no %s: Japanese text will show as boxes", font98_path); return; }
    font98 = malloc(256 * 16 + 94 * 94 * 32);
    if (fread(font98, 1, 256 * 16 + 94 * 94 * 32, f) != 256 * 16 + 94 * 94 * 32) { free(font98); font98 = NULL; }
    fclose(f);
}
static const u8 *ank16(u8 c) { return font98 ? font98 + c * 16 : NULL; }
static const u8 *kanji16(u8 j1, u8 j2)
{
    /* ROM rows 29h/2Ah hold the half-width ASCII and katakana sets (left half only) */
    if (font98 && (j1 == 0x29 || j1 == 0x2A)) {
        static u8 half[32]; const u8 *g = ank16(j1 == 0x29 ? j2 : (j2 >= 0x80 ? j2 : j2 + 0x80));
        for (int i = 0; i < 16; i++) { half[i * 2] = g[i]; half[i * 2 + 1] = 0; }
        return half;
    }
    if (!font98 || j1 < 0x21 || j1 > 0x7E || j2 < 0x21 || j2 > 0x7E) return NULL;
    return font98 + 256 * 16 + ((j1 - 0x21) * 94 + (j2 - 0x21)) * 32;
}

static u8 gvram[2][4][0x8000];   /* bank, plane (B,R,G,I) */
static int disp_bank, acc_bank;
static u8 grcg_mode, grcg_tile[4], grcg_tidx;
static int analog_mode = 1;
static u8 pal_idx, pal[16][3];          /* g, r, b (0-15) */
static u8 dig_pal[4];
static int gfx_on = 1, text_on = 1;
static int vsync_armed;
static u8 mode_ff[8];
static u16 gdc_start;                   /* graphics display start (words) */
static u8 gdc_cmd, gdc_param[16]; static int gdc_np;

/* ---------------- graphics memory ---------------- */
static int plane_of(u32 a, u16 *off)
{
    if (a >= 0xA8000 && a < 0xB0000) { *off = a - 0xA8000; return 0; }
    if (a >= 0xB0000 && a < 0xB8000) { *off = a - 0xB0000; return 1; }
    if (a >= 0xB8000 && a < 0xC0000) { *off = a - 0xB8000; return 2; }
    if (a >= 0xE0000 && a < 0xE8000) { *off = a - 0xE0000; return 3; }
    return -1;
}

u8 pc98_rb(u32 a)
{
    u16 o; int p = plane_of(a, &o);
    if (p < 0) return mem[a];
    if (grcg_mode & 0x80) {
        if (!(grcg_mode & 0x40)) {
            /* TCR: bits where every enabled plane matches its tile */
            u8 r = 0xFF;
            for (int i = 0; i < 4; i++) if (!(grcg_mode & (1 << i))) r &= ~(gvram[acc_bank][i][o] ^ grcg_tile[i]);
            return r;
        }
    }
    return gvram[acc_bank][p][o];
}

void pc98_wb(u32 a, u8 v)
{
    u16 o; int p = plane_of(a, &o);
    if (p < 0) { if (a < 0xA4000 || (a >= 0xC0000 && a < 0xE0000)) mem[a] = v; return; }
    if (grcg_mode & 0x80) {
        for (int i = 0; i < 4; i++) {
            if (grcg_mode & (1 << i)) continue;
            if (grcg_mode & 0x40) gvram[acc_bank][i][o] = (gvram[acc_bank][i][o] & ~v) | (grcg_tile[i] & v);  /* RMW */
            else gvram[acc_bank][i][o] = grcg_tile[i];                                                        /* TDW */
        }
        return;
    }
    gvram[acc_bank][p][o] = v;
}

/* ---------------- ports ---------------- */
int pc98_vsync_active(void)
{
    double t = emu_time_ms(), period = 1000.0 / 56.42;
    double ph = t - (long long)(t / period) * period;
    return ph >= period - 1.1;
}

u8 pc98_video_in(u16 port, int *handled)
{
    *handled = 1;
    switch (port) {
    case 0x7FD9: return mouse_read_a();
    case 0xA9: return cg_read();
    case 0x7FDB: return 0x40;
    case 0x7FDD: return ms_portc;
    case 0x60: case 0xA0: { static int hb; hb ^= 1;   /* FIFO empty, VSYNC, HBLANK toggling as the beam scans */
        return 0x04 | (pc98_vsync_active() ? 0x20 : 0) | (hb ? 0x40 : 0); }
    case 0x62: case 0xA2: return 0;
    case 0x7C: return grcg_mode;
    case 0xA4: return disp_bank;
    case 0xA6: return acc_bank;
    case 0xA8: return pal_idx;
    case 0xAA: return analog_mode ? pal[pal_idx & 15][0] : dig_pal[1];
    case 0xAC: return analog_mode ? pal[pal_idx & 15][1] : dig_pal[2];
    case 0xAE: return analog_mode ? pal[pal_idx & 15][2] : dig_pal[3];
    }
    *handled = 0; return 0xFF;
}

static void gdc_exec_param(int slave)
{
    /* graphics GDC SCROLL command (0x70): start address of display area 1 */
    if (slave && (gdc_cmd & 0xF0) == 0x70 && gdc_np >= 2) gdc_start = gdc_param[0] | ((gdc_param[1] & 0x3F) << 8);
}

int pc98_video_out(u16 port, u8 v)
{
    switch (port) {
    case 0x7FDD: mouse_write_c(v); return 1;
    case 0xA1: cg_lo = v; { void english_bt_drawing(void); english_bt_drawing(); }
        if (getenv("MW_CGDBG")) { static int n; if (n++ < 400) { u32 sp = lin(cpu.s[SS], cpu.r[SP]); fprintf(stderr, "cg %02x%02x at %04x:%04x stk %04x %04x %04x %04x %04x %04x %04x %04x si=%04x ds=%04x di=%04x es=%04x\n", cg_hi, v, cpu.s[CS], cpu.ip, rw(sp), rw(sp+2), rw(sp+4), rw(sp+6), rw(sp+8), rw(sp+10), rw(sp+12), rw(sp+14), cpu.r[SI], cpu.s[DS], cpu.r[DI], cpu.s[ES]); } }
        return 1;
    case 0xA3: cg_hi = v; return 1;
    case 0xA5: cg_line = v; if (getenv("MW_CGLINE")) { static int n; if (n++ < 64) fprintf(stderr, "a5 %02x hi %02x lo %02x at %04x:%04x\n", v, cg_hi, cg_lo, cpu.s[CS], cpu.ip); } return 1;
    case 0x7FDF: if (!(v & 0x80)) { int bit = (v >> 1) & 7; if (v & 1) mouse_write_c(ms_portc | (1 << bit)); else mouse_write_c(ms_portc & ~(1 << bit)); } return 1;
    case 0xBFDB: ms_freq = v & 3; return 1;
    case 0x62: case 0xA2:
        gdc_cmd = v; gdc_np = 0;
        if (v == 0x0D || v == 0x6B) { if (port == 0xA2) gfx_on = 1; else text_on = 1; }
        if (v == 0x0C || v == 0x05) { if (port == 0xA2) gfx_on = 0; else text_on = 0; }
        return 1;
    case 0x60: case 0xA0: if (gdc_np < 16) gdc_param[gdc_np++] = v; gdc_exec_param(port == 0xA0); return 1;
    case 0x64: vsync_armed = 1; return 1;
    case 0x68: mode_ff[(v >> 1) & 7] = v & 1; return 1;
    case 0x6A: if ((v & 0xFE) == 0) analog_mode = v & 1; return 1;
    case 0x7C: grcg_mode = v; grcg_tidx = 0; return 1;
    case 0x7E: grcg_tile[grcg_tidx & 3] = v; grcg_tidx = (grcg_tidx + 1) & 3; return 1;
    case 0xA4: disp_bank = v & 1; return 1;
    case 0xA6: acc_bank = v & 1; return 1;
    case 0xA8: if (analog_mode) pal_idx = v & 15; else dig_pal[0] = v; return 1;
    case 0xAA: if (analog_mode) pal[pal_idx & 15][0] = v & 15; else dig_pal[1] = v; return 1;
    case 0xAC: if (analog_mode) pal[pal_idx & 15][1] = v & 15; else dig_pal[2] = v; return 1;
    case 0xAE: if (analog_mode) pal[pal_idx & 15][2] = v & 15; else dig_pal[3] = v; return 1;
    }
    return 0;
}

int pc98_take_vsync_irq(void) { if (vsync_armed) { vsync_armed = 0; return 1; } return 0; }

/* ---------------- rendering (640x400) ---------------- */
u32 fb98[640 * 400];
static inline u8 rev8(u8 b) { b = (b & 0xF0) >> 4 | (b & 0x0F) << 4; b = (b & 0xCC) >> 2 | (b & 0x33) << 2; return (b & 0xAA) >> 1 | (b & 0x55) << 1; }

/* Artwork overlays: some screens have Japanese drawn into the picture itself. Each rule
 * (textmap/pc98_art.overrides: x y w h hash text) names a rectangle of the 640x400 screen
 * and the hash of its pixels when the original label is showing; while that exact label is
 * on screen, the rectangle is repainted with the English text in the label's own colours. */
typedef struct { int x, y, w, h; u32 hash; char text[40]; int reported; } ArtRule;
static ArtRule art[64]; static int nart = -1;
const char *art_path = "textmap/pc98_art.overrides";
static void art_load(void)
{
    nart = 0; FILE *f = fopen(art_path, "r"); char line[160];
    while (f && fgets(line, sizeof line, f) && nart < 64) {
        ArtRule *r = &art[nart]; int n = 0;
        if (line[0] == '#' || sscanf(line, "%d %d %d %d %x %n", &r->x, &r->y, &r->w, &r->h, &r->hash, &n) < 5) continue;
        snprintf(r->text, sizeof r->text, "%s", line + n); char *e = strpbrk(r->text, "\r\n"); if (e) *e = 0;
        r->reported = 0; nart++;
    }
    if (f) fclose(f);
}
static void art_apply(void)
{
    if (nart < 0) art_load();
    for (int i = 0; i < nart; i++) {
        ArtRule *r = &art[i];
        u32 h = 2166136261u;
        for (int y = r->y; y < r->y + r->h; y++) for (int x = r->x; x < r->x + r->w; x++) { h ^= fb98[y * 640 + x]; h *= 16777619u; }
        if (!r->hash) { if (!r->reported && getenv("MW_ARTHASH")) { fprintf(stderr, "art %d %d %d %d hash %08x\n", r->x, r->y, r->w, r->h, h); r->reported = 1; } continue; }
        if (h != r->hash) continue;
        /* label colours: the commonest pixel is the panel, the next commonest the lettering;
         * dark lettering on a light panel gets a light bevel like the originals */
        u32 cand[16]; int cnt[16], nc = 0;
        for (int y = r->y; y < r->y + r->h; y++) for (int x = r->x; x < r->x + r->w; x++) {
            u32 c = fb98[y * 640 + x]; int k; for (k = 0; k < nc && cand[k] != c; k++);
            if (k == nc && nc < 16) { cand[nc] = c; cnt[nc++] = 0; } if (k < 16) cnt[k]++; }
        int b1 = 0, b2 = -1;
        for (int k = 1; k < nc; k++) if (cnt[k] > cnt[b1]) b1 = k;
        for (int k = 0; k < nc; k++) if (k != b1 && (b2 < 0 || cnt[k] > cnt[b2])) b2 = k;
        u32 bg = cand[b1], ink = b2 >= 0 ? cand[b2] : 0xFFFFFFFF;
        #define LUM(c) ((((c) >> 16) & 255) + (((c) >> 8) & 255) + ((c) & 255))
        int shadow = LUM(ink) < LUM(bg);
        u32 light = 0xFFFFFFFF; for (int k = 0; k < nc; k++) if (LUM(cand[k]) > LUM(light & 0xFFFFFF ? light : 0)) light = cand[k];
        for (int y = r->y; y < r->y + r->h; y++) for (int x = r->x; x < r->x + r->w; x++) fb98[y * 640 + x] = bg;
        const char *t = r->text; int left = 0; if (*t == '<') { left = 1; t++; }
        int len = strlen(t), tw = len * 8, tx = left ? r->x + 1 : r->x + (r->w - tw) / 2, ty = r->y + (r->h - 16) / 2;
        for (int pass = shadow ? 0 : 1; pass < 2; pass++)
            for (int c = 0; c < len; c++) {
                const u8 *g = ank16((u8)t[c]); if (!g) continue;
                for (int y = 0; y < 16; y++) for (int b = 0; b < 8; b++) if (g[y] & (0x80 >> b)) {
                    int px = tx + c * 8 + b + (pass ? 0 : 1), py = ty + y + (pass ? 0 : 1);
                    if (px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h) fb98[py * 640 + px] = pass ? ink : light;
                }
            }
    }
}

void pc98_render(void)
{
    u32 cp[16];
    for (int i = 0; i < 16; i++) {
        u32 g, r, b;
        if (analog_mode) { g = pal[i][0] * 17; r = pal[i][1] * 17; b = pal[i][2] * 17; }
        else { /* digital 8 colours: bit0 B, bit1 R, bit2 G */ b = (i & 1) ? 255 : 0; r = (i & 2) ? 255 : 0; g = (i & 4) ? 255 : 0; }
        cp[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    u8 (*pl)[0x8000] = gvram[disp_bank];
    for (int y = 0; y < 400; y++) {
        u32 *out = fb98 + y * 640;
        u16 o = (u16)((gdc_start * 2 + y * 80) & 0x7FFF);
        for (int xb = 0; xb < 80; xb++, o = (o + 1) & 0x7FFF) {
            u8 b = pl[0][o], r = pl[1][o], g = pl[2][o], in = pl[3][o];
            for (int k = 7; k >= 0; k--) {
                int c = ((b >> k) & 1) | (((r >> k) & 1) << 1) | (((g >> k) & 1) << 2) | (((in >> k) & 1) << 3);
                *out++ = gfx_on ? cp[c] : 0xFF000000;
            }
        }
    }
    if (!text_on) { if (english_art_on) art_apply(); return; }
    /* text layer: 80x25 cells of 8x16, attribute bits: 0 secret, 1 blink, 2 reverse, 3 underline, 5-7 GRB colour */
    static const u32 tcol[8] = { 0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF, 0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF };
    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < 80; col++) {
            u32 a = 0xA0000 + (row * 80 + col) * 2;
            u16 code = mem[a] | (mem[a + 1] << 8);
            u8 at = mem[0xA2000 + (row * 80 + col) * 2];
            if (!(at & 1)) continue;                       /* secret (hidden) */
            int rev = (at >> 2) & 1;
            if (code == 0x20 && !rev) continue;
            u32 fg = tcol[(at >> 5) & 7];
            for (int y = 0; y < 16; y++) {
                u8 bits;
                if (code < 0x100) { const u8 *g = ank16(code); bits = g ? g[y] : (code < 0x80 ? (u8)font8x8_basic[code][y >> 1] : 0); if (g) bits = rev8(bits); }
                else {
                    /* kanji: low byte = JIS first byte - 0x20 (bit 7 set in the cell holding the right
                     * half), high byte = second byte */
                    const u8 *g = kanji16((code & 0x7F) + 0x20, (code >> 8) & 0x7F);
                    int right = ((code >> 7) & 1) | ((code >> 15) & 1);
                    bits = g ? rev8(g[y * 2 + right]) : ((y == 0 || y == 15) ? 0xFF : 0x81);
                }
                if (rev) bits = ~bits;
                u32 *out = fb98 + (row * 16 + y) * 640 + col * 8;
                for (int x = 0; x < 8; x++) if (bits & (1 << x)) out[x] = fg;
            }
        }
    }
    if (english_art_on) art_apply();
}

/* ---------------- keyboard ---------------- */
static u8 kq[64]; static int kq_h, kq_t;       /* raw scancodes for the 8251 */
static u16 bq[64]; static int bq_h, bq_t;      /* BIOS key buffer (scan<<8 | char) */
static u8 kbd_data, shift_state;

/* SDL (PC/AT set-1) scan code -> PC-98 scan code */
static int at_to_98(int sc)
{
    static const u8 t[0x60] = {
        /*00*/ 0xFF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F,
        /*10*/ 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x74, 0x1D, 0x1E,
        /*20*/ 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x0D, 0x70, 0x28, 0x29, 0x2A, 0x2B, 0x2C,
        /*30*/ 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x70, 0x45, 0x73, 0x34, 0x71, 0x62, 0x63, 0x64, 0x65, 0x66,
        /*40*/ 0x67, 0x68, 0x69, 0x6A, 0x6B, 0xFF, 0xFF, 0x42, 0x3A, 0x44, 0x40, 0x3B, 0x47, 0x3C, 0x4A, 0x4A,
        /*50*/ 0x3D, 0x4B, 0x38, 0x39, 0xFF, 0xFF, 0xFF, 0x52, 0x53, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    return sc < 0x60 ? t[sc] : 0xFF;
}
static const char ank_lo[] = "\033" "1234567890-^\\\b\tqwertyuiop@[\r" "asdfghjkl;:]" "zxcvbnm,./_ ";
static const char ank_hi[] = "\033" "!\"#$%&'()0=~|\b\tQWERTYUIOP`{\r" "ASDFGHJKL+*}" "ZXCVBNM<>?_ ";

void pc98_key_event(int at_sc, int down)
{
    int sc;
    if (at_sc & 0x200) { sc = at_sc & 0x7F; int n = (kq_t + 1) & 63; if (n != kq_h) { kq[kq_t] = down ? sc : sc | 0x80; kq_t = n; } return; }   /* raw PC-98 code (testing) */
    if (at_sc & 0x100) {   /* E0-prefixed: arrows etc. */
        switch (at_sc & 0x7F) { case 0x48: sc = 0x3A; break; case 0x50: sc = 0x3D; break; case 0x4B: sc = 0x3B; break; case 0x4D: sc = 0x3C; break;
        case 0x1C: sc = 0x1C; break; case 0x52: sc = 0x38; break; case 0x53: sc = 0x39; break; default: return; }
    } else sc = at_to_98(at_sc & 0x7F);
    if (sc == 0xFF) return;
    int n = (kq_t + 1) & 63;
    if (n != kq_h) { kq[kq_t] = down ? sc : sc | 0x80; kq_t = n; }
}
int pc98_kbd_pending(void) { return kq_h != kq_t; }
u8 pc98_kbd_next(void) { u8 v = kq[kq_h]; kq_h = (kq_h + 1) & 63; kbd_data = v; return v; }

/* default IRQ1 handler: turn scan codes into BIOS keys */
void pc98_bios_int9(void)
{
    u8 sc = kbd_data, c = sc & 0x7F; int up = sc & 0x80;
    /* key-state matrix (0000:052A, 16 groups of 8 keys) read by INT 18h AH=04 */
    if (up) mem[0x52A + (c >> 3)] &= ~(1 << (c & 7)); else mem[0x52A + (c >> 3)] |= 1 << (c & 7);
    if (c == 0x70) { if (up) shift_state &= ~1; else shift_state |= 1; }
    else if (c == 0x74) { if (up) shift_state &= ~0x10; else shift_state |= 0x10; }
    else if (!up) {
        u8 ch = 0;
        if (c < 0x0E) ch = (shift_state & 1) ? ank_hi[c] : ank_lo[c];
        else if (c >= 0x0E && c <= 0x1C) ch = (shift_state & 1) ? ank_hi[c] : ank_lo[c];
        else if (c >= 0x1D && c <= 0x28) ch = (shift_state & 1) ? ank_hi[c - 0x1D + 29] : ank_lo[c - 0x1D + 29];
        else if (c >= 0x29 && c <= 0x34) ch = (shift_state & 1) ? ank_hi[c - 0x29 + 41] : ank_lo[c - 0x29 + 41];
        int n = (bq_t + 1) & 63; if (n != bq_h) { bq[bq_t] = (c << 8) | ch; bq_t = n; }
    }
    mem[0x53A] = shift_state;
}

/* character ROM read through the CG window: A3 = JIS first byte - 0x20 (0 for ANK),
 * A1 = second byte (or the ANK code), A5 = line 0-15 with bit 5 selecting the left half */
static u8 cg_read(void)
{
    if (getenv("MW_FONTDBG") && emu_time_ms() > 308900 && !(cg_line & 0x0F)) fprintf(stderr, "cg read %02x%02x\n", cg_hi, cg_lo);
    int line = cg_line & 0x0F, left = (cg_line & 0x20) != 0;
    if ((cg_hi & 0x7F) == 0) { const u8 *g = ank16(cg_lo); return g ? g[line] : 0; }
    const u8 *g = kanji16((cg_hi & 0x7F) + 0x20, cg_lo & 0x7F);
    return g ? g[line * 2 + (left ? 0 : 1)] : 0;
}

/* ---------------- bus mouse (8255 at 7FD9-7FDF, IRQ13) ---------------- */
void pc98_mouse_motion(int dx, int dy) { ms_dx += dx; ms_dy += dy; }
void pc98_mouse_button(int b, int down) { if (b < 2) { if (down) ms_btn |= 1 << b; else ms_btn &= ~(1 << b); } }
int pc98_mouse_irq_enabled(void) { return !(ms_portc & 0x10); }
double pc98_mouse_period_ms(void) { static const double hz[4] = { 120, 60, 30, 15 }; return 1000.0 / hz[ms_freq & 3]; }
static u8 mouse_read_a(void)
{
    int sel = (ms_portc >> 5) & 3;
    int v = sel == 0 ? ms_lx : sel == 1 ? ms_lx >> 4 : sel == 2 ? ms_ly : ms_ly >> 4;
    u8 r = v & 0x0F;
    if (!(ms_btn & 1)) r |= 0x80;      /* buttons read 0 while pressed */
    if (!(ms_btn & 2)) r |= 0x20;
    return r | 0x40;
}
static void mouse_write_c(u8 v)
{
    if ((v & 0x80) && !(ms_portc & 0x80)) {      /* HC rising: latch and clear the counters */
        ms_lx = ms_dx < -128 ? -128 : ms_dx > 127 ? 127 : ms_dx;
        ms_ly = ms_dy < -128 ? -128 : ms_dy > 127 ? 127 : ms_dy;
        ms_dx -= ms_lx; ms_dy -= ms_ly;
    }
    ms_portc = v;
}

/* ---------------- INT 18h BIOS ---------------- */
void pc98_int18(void)
{
    u8 ah = cpu.r[AX] >> 8;
    switch (ah) {
    case 0x00: if (bq_h == bq_t) { hle_retry = 1; break; } cpu.r[AX] = bq[bq_h]; bq_h = (bq_h + 1) & 63; break;
    case 0x01: if (bq_h == bq_t) { cpu.r[BX] &= 0x00FF; } else { cpu.r[AX] = bq[bq_h]; cpu.r[BX] = (cpu.r[BX] & 0xFF) | 0x0100; } { extern void poll_note_ext(int); poll_note_ext(bq_h == bq_t); } break;
    case 0x02: cpu.r[AX] = (cpu.r[AX] & 0xFF00) | shift_state; break;
    case 0x03: bq_h = bq_t = 0; break;
    case 0x04: cpu.r[AX] = (cpu.r[AX] & 0xFF) | ((getenv("MW_NOKEYMAT") ? 0 : mem[0x52A + (cpu.r[AX] & 0x0F)]) << 8); break;   /* key group state */
    case 0x0A: case 0x0B: break;           /* text mode set / get */
    case 0x0C: text_on = 1; break;
    case 0x0D: text_on = 0; break;
    case 0x14: { /* read character pattern: DX = code, BX:CX = buffer */
        u32 b = lin(cpu.r[BX], cpu.r[CX]); u16 dx = cpu.r[DX];
        int half_rows = !getenv("MW_OLD29") && ((dx >> 8) == 0x29 || (dx >> 8) == 0x2A);
        if ((dx >> 8) == 0x00 || (dx >> 8) == 0x80 || half_rows) {
            /* 8x16 patterns: ANK, and the half-width rows 29h (ASCII) / 2Ah (katakana) */
            u8 c = dx & 0xFF; if ((dx >> 8) == 0x2A && c < 0x80) c += 0x80;   /* half-width kana arrive either as A1-DF or 21-5F */
            const u8 *g = ank16(c);
            wb(b, 1); wb(b + 1, 2);
            for (int i = 0; i < 16; i++) wb(b + 2 + i, g ? g[i] : 0);
        } else {
            const u8 *g = kanji16(dx >> 8, dx & 0x7F);
            wb(b, 2); wb(b + 1, 2);
            for (int i = 0; i < 32; i++) wb(b + 2 + i, g ? g[i] : (i & 1 ? 0x01 : 0x80));
        }
        if (getenv("MW_FONTDBG")) { u32 sp = lin(cpu.s[SS], cpu.r[SP]); fprintf(stderr, "font read %04x stk", dx); for (int q = 0; q < 24; q++) fprintf(stderr, " %04x", rw(sp + q * 2)); fprintf(stderr, "\n"); }
    } break;
    case 0x1B: case 0x4A: break;  /* KCG access mode / misc: no state to keep */
    case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x15: break;
    case 0x16: /* fill text VRAM: DL char, DH attr */
        for (int i = 0; i < 80 * 25; i++) { mem[0xA0000 + i * 2] = cpu.r[DX] & 0xFF; mem[0xA0001 + i * 2] = 0; mem[0xA2000 + i * 2] = cpu.r[DX] >> 8; }
        break;
    case 0x40: gfx_on = 1; break;
    case 0x41: gfx_on = 0; break;
    case 0x42: if (cpu.r[CX] & 0x100) { } break;   /* display area setup: game uses 640x400 */
    case 0x43: break;
    default: logmsg("unhandled INT 18h ah=%02x", ah); break;
    }
}

void pc98_bios_init(void)
{
    machine_pc98 = 1;
    font98_load();
    mem[0x501] = 0x08;        /* 640K, 10 MHz-family clock (PIT input 2.4576 MHz) */
    mem[0x54C] = 0x04;        /* analog (16-colour) palette board present */
    mem[0x458] = 0x80;
    mem[0x584] = 0x80;              /* booted from hard disk 0 */
    mem[0x600 + 0x6C + 2] = 0x80;
    mem[0x600 + 0x6C + 3] = 0x90;  /* drive D = 2HD floppy 0 (game asks for its data disks there; all files live in the game folder) */  /* DOS DA/UA table: drive C = SASI hard disk 0 (installed copy) */
    /* default analog palette = the digital 8 colours twice */
    for (int i = 0; i < 16; i++) { int on = (i & 8) ? 15 : 7; pal[i][0] = (i & 4) ? on : 0; pal[i][1] = (i & 2) ? on : 0; pal[i][2] = (i & 1) ? on : 0; }
    for (int i = 0; i < 80 * 25; i++) { mem[0xA0000 + i * 2] = ' '; mem[0xA2000 + i * 2] = 0xE1; }
}

#include "snap.h"
void pc98_snap(FILE *f, int save)
{
    S(ms_dx); S(ms_dy); S(ms_lx); S(ms_ly); S(ms_btn); S(ms_portc); S(ms_freq); S(cg_hi); S(cg_lo); S(cg_line);
    S(gvram); S(disp_bank); S(acc_bank); S(grcg_mode); S(grcg_tile); S(grcg_tidx); S(analog_mode);
    S(pal_idx); S(pal); S(dig_pal); S(gfx_on); S(text_on); S(vsync_armed); S(mode_ff); S(gdc_start);
    S(gdc_cmd); S(gdc_param); S(gdc_np); S(kq); S(kq_h); S(kq_t); S(bq); S(bq_h); S(bq_t); S(kbd_data); S(shift_state);
}
