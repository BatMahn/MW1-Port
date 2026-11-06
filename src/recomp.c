/* Dispatcher for recompiled code.
 *
 * Every program image the game loads (launcher, strategy game, combat engine,
 * graphics overlays) has a table of recompiled blocks. When the CPU is at an
 * address inside a loaded image, the matching block runs natively; anything
 * else (code never seen in a trace, the BIOS stubs) goes through the reference
 * interpreter. Before a block runs for the first time after its image is
 * loaded, its code bytes in memory are checksummed against the original, so
 * patched or self-modified code can never run a stale translation. */
#include "recomp_rt.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *name;
    const RcBlock *blocks;
    const int *nblocks;
    u16 **loadseg;
} RcImageDesc;
#include "../generated/images.inc"

#define IMG_MAX 0x40000
typedef struct {
    const RcImageDesc *d;
    u32 base;               /* linear load address */
    u16 seg;
    int depth;              /* exec nesting level that owns it */
    u32 *idx;               /* offset -> block index + 1 */
    u8 *state;              /* per block: 0 unchecked, 1 ok, 2 bad */
} RcImage;

static RcImage act[8]; static int nact;
static int exec_level;
int rc_enabled = 1, rc_verify, rc_misslog;
static u32 h[1 << 16][2];
long rc_hits, rc_misses, rc_bad, rc_ver_ok, rc_ver_fail, rc_ver_skip;

static const RcImageDesc *find_desc(const char *name)
{
    const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
    if (strchr(b, ':')) b = strchr(b, ':') + 1;
    extern int machine_pc98;
    const RcImageDesc *list = machine_pc98 ? rc_images_pc98 : rc_images_dos;
    for (int i = 0; list[i].name; i++) if (!strcasecmp(list[i].name, b)) return &list[i];
    return NULL;
}

static void add_image(const char *name, u16 seg)
{
    const RcImageDesc *d = find_desc(name);
    if (!d || nact >= 8) return;
    RcImage *m = &act[nact++];
    m->d = d; m->base = seg * 16; m->seg = seg; m->depth = exec_level;
    **d->loadseg = seg;
    if (!m->idx) m->idx = calloc(IMG_MAX, sizeof(u32));
    else memset(m->idx, 0, IMG_MAX * sizeof(u32));
    for (int i = 0; i < *d->nblocks; i++) m->idx[d->blocks[i].off] = i + 1;
    m->state = calloc(*d->nblocks, 1);
    logmsg("recomp: %s at %04x, %d blocks", d->name, seg, *d->nblocks);
}

void rc_program_loaded(const char *name, u16 seg) { exec_level++; add_image(name, seg); }
void rc_overlay_loaded(const char *name, u16 seg) { add_image(name, seg); }
void rc_program_exit(void)
{
    while (nact && act[nact - 1].depth == exec_level) { nact--; free(act[nact].idx); act[nact].idx = NULL; free(act[nact].state); }
    exec_level--;
    /* parent's images are active again: their load segments are unchanged */
    for (int i = 0; i < nact; i++) **act[i].d->loadseg = act[i].seg;
}

static int verify_code(RcImage *m, const RcBlock *b)
{
    u32 a = m->base + b->off, csum = 0;
    const u16 *r = b->relocs;
    for (int k = 0; k < b->len; k++) {
        int skip = 0;
        if (r) for (const u16 *q = r; *q != 0xFFFF; q++) if (k == *q || k == *q + 1) skip = 1;
        csum = skip ? csum * 31 : csum * 31 + mem[(a + k) & 0xFFFFF];
    }
    return csum == b->csum;
}

/* ---- lockstep verification against the interpreter ---- */
int wlog_on; int wlog_n; int wlog_overflow; int wlog_video;
typedef struct { u32 a; u8 old, nw; } WEnt;
static WEnt wlog[1 << 17];
void wlog_add(u32 a, u8 old, u8 nw)
{
    if (a >= 0xA0000) { wlog_video = 1; return; }
    if (wlog_n >= (1 << 17)) { wlog_overflow = 1; return; }
    wlog[wlog_n].a = a; wlog[wlog_n].old = old; wlog[wlog_n].nw = nw; wlog_n++;
}

static void lockstep(const RcBlock *b, RcImage *m)
{
    CPU s0 = cpu; int64_t c0 = cycles_done;
    wlog_on = 1; wlog_n = 0; wlog_overflow = 0; wlog_video = 0;
    b->fn();
    wlog_on = 0;
    if (wlog_video || wlog_overflow || (b->io & 1)) { rc_ver_skip++; return; }
    CPU s1 = cpu; int64_t c1 = cycles_done;
    int n1 = wlog_n;
    static WEnt w1[1 << 17];
    memcpy(w1, wlog, n1 * sizeof(WEnt));
    for (int i = n1 - 1; i >= 0; i--) mem[w1[i].a] = w1[i].old;
    cpu = s0; cycles_done = c0;
    wlog_on = 1; wlog_n = 0;
    for (int i = 0; i < b->ninsn; i++) cpu_step();
    wlog_on = 0;
    int bad = memcmp(cpu.r, s1.r, sizeof cpu.r) || memcmp(cpu.s, s1.s, sizeof cpu.s) || cpu.ip != s1.ip || cpu.flags != s1.flags;
    /* compare final memory contents at every written address */
    if (!bad) for (int i = 0; i < n1; i++) if (mem[w1[i].a] != w1[i].nw) {
        /* final value is what counts: check the last write to that address in both */
        int k = i; for (int j = i + 1; j < n1; j++) if (w1[j].a == w1[i].a) k = j;
        if (mem[w1[k].a] != w1[k].nw) { bad = 1; break; }
    }
    if (!bad && wlog_n != n1) bad = 2;
    if (bad) {
        rc_ver_fail++;
        if (rc_ver_fail < 40) {
            fprintf(stderr, "VERIFY FAIL %s block %05x (%04x:%04x) kind %d\n", m->d->name, b->off, s0.s[CS], s0.ip, bad);
            fprintf(stderr, "  rc : ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x sp=%04x ds=%04x es=%04x ss=%04x cs:ip=%04x:%04x fl=%04x w=%d\n",
                s1.r[AX], s1.r[BX], s1.r[CX], s1.r[DX], s1.r[SI], s1.r[DI], s1.r[BP], s1.r[SP], s1.s[DS], s1.s[ES], s1.s[SS], s1.s[CS], s1.ip, s1.flags, n1);
            fprintf(stderr, "  int: ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x sp=%04x ds=%04x es=%04x ss=%04x cs:ip=%04x:%04x fl=%04x w=%d\n",
                cpu.r[AX], cpu.r[BX], cpu.r[CX], cpu.r[DX], cpu.r[SI], cpu.r[DI], cpu.r[BP], cpu.r[SP], cpu.s[DS], cpu.s[ES], cpu.s[SS], cpu.s[CS], cpu.ip, cpu.flags, wlog_n);
        }
    } else rc_ver_ok++;
    (void)c1;
}

/* execute one block or one interpreted instruction */
int english_hook(u32 a); int english_hooks_active(void);
void rc_step(void)
{
    if (english_hooks_active() && english_hook(lin(cpu.s[CS], cpu.ip))) return;
    if (rc_enabled) {
        u32 a = lin(cpu.s[CS], cpu.ip);
        for (int i = nact - 1; i >= 0; i--) {
            RcImage *m = &act[i];
            if (m->depth != exec_level) continue;
            if (a < m->base || a - m->base >= IMG_MAX) continue;
            u32 k = m->idx[a - m->base];
            if (!k) continue;
            const RcBlock *b = &m->d->blocks[k - 1];
            if ((u16)(cpu.s[CS] - m->seg) != b->seg) break;
            if (m->state[k - 1] == 0) m->state[k - 1] = verify_code(m, b) ? 1 : 2;
            if (m->state[k - 1] == 2) { rc_bad++; break; }
            rc_hits++;
            if (rc_verify) lockstep(b, m); else b->fn();
            if ((b->io & 2) && cpu.ip == (u16)(b->off - b->seg * 16) && cpu.s[CS] == (u16)(m->seg + b->seg)) {
                extern int idle_hint; idle_hint = 1;   /* went round a pure wait loop */
            }
            return;
        }
    }
    rc_misses++;
    if (rc_misslog) { static int init; u32 key = ((u32)cpu.s[CS] << 16) | cpu.ip; u32 x = (key * 2654435761u) >> 16;
        for (;;) { if (!h[x][0] || h[x][0] == key) { h[x][0] = key; h[x][1]++; break; } x = (x + 1) & 0xFFFF; }
 (void)init; }
    if (trace_enabled) trace_exec(cpu.s[CS], cpu.ip);
    cpu_step();
}

void rc_report(void)
{
    if (rc_misslog) { FILE *f = fopen("misses.txt", "w"); for (int i = 0; i < 65536; i++) if (h[i][1] > 1000) fprintf(f, "%u %04x:%04x\n", h[i][1], h[i][0] >> 16, h[i][0] & 0xFFFF); fclose(f); }
    fprintf(stderr, "recomp: %ld blocks run, %ld interpreted instructions, %ld checksum rejects; verify ok %ld fail %ld skipped %ld\n",
            rc_hits, rc_misses, rc_bad, rc_ver_ok, rc_ver_fail, rc_ver_skip);
}

#include "snap.h"
void english_program_loaded(const char *name, u16 seg);
void rc_snap(FILE *f, int save)
{
    S(exec_level);
    int n = nact; S(n);
    for (int i = 0; i < n; i++) {
        char nm[32] = { 0 }; u16 seg = 0; int depth = 0;
        if (save) { snprintf(nm, sizeof nm, "%s", act[i].d->name); seg = act[i].seg; depth = act[i].depth; }
        S(nm); S(seg); S(depth);
        if (!save) {
            if (i == 0) { while (nact) { nact--; free(act[nact].idx); act[nact].idx = NULL; free(act[nact].state); } }
            int lvl = exec_level; exec_level = depth; add_image(nm, seg); exec_level = lvl;
            english_program_loaded(nm, seg);   /* English (if on) is applied fresh to the Japanese snapshot */
        }
    }
}
