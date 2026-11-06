#include "compat.h"
/* DOS services (INT 21h) for the port: memory blocks, file I/O against the
 * player's game directory, EXEC of child programs and overlay loading. */
#include "machine.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

char game_dir[1024] = ".";

/* ---------- memory (MCB chain) ---------- */
#define MEM_FIRST 0x0100
#define MEM_TOP   0xA000
static void mcb_set(u16 seg, u8 type, u16 owner, u16 size)
{ wb(lin(seg, 0), type); ww(lin(seg, 1), owner); ww(lin(seg, 3), size); }
static u8 mcb_type(u16 s) { return rb(lin(s, 0)); }
static u16 mcb_owner(u16 s) { return rw(lin(s, 1)); }
static u16 mcb_size(u16 s) { return rw(lin(s, 3)); }
static u16 cur_psp;

static void mem_init(void) { mcb_set(MEM_FIRST, 'Z', 0, MEM_TOP - MEM_FIRST - 1); }

static void mem_merge(void)
{
    u16 s = MEM_FIRST;
    while (mcb_type(s) == 'M') {
        u16 n = s + mcb_size(s) + 1;
        if (mcb_owner(s) == 0 && mcb_owner(n) == 0) {
            mcb_set(s, mcb_type(n), 0, mcb_size(s) + mcb_size(n) + 1);
            continue;
        }
        s = n;
    }
}

/* returns segment of block (after MCB) or 0; *largest gets biggest free */
static u16 mem_alloc(u16 paras, u16 owner, u16 *largest)
{
    mem_merge();
    u16 s = MEM_FIRST, best = 0; *largest = 0;
    for (;;) {
        if (mcb_owner(s) == 0) {
            if (mcb_size(s) >= paras && !best) best = s;
            if (mcb_size(s) > *largest) *largest = mcb_size(s);
        }
        if (mcb_type(s) == 'Z') break;
        s += mcb_size(s) + 1;
    }
    if (!best) return 0;
    u16 sz = mcb_size(best); u8 t = mcb_type(best);
    if (sz > paras) {
        mcb_set(best + paras + 1, t, 0, sz - paras - 1);
        mcb_set(best, 'M', owner, paras);
    } else mcb_set(best, t, owner, sz);
    return best + 1;
}

static void mem_free(u16 seg) { ww(lin(seg - 1, 1), 0); mem_merge(); }

static int mem_resize(u16 seg, u16 paras, u16 *maxp)
{
    mem_merge();
    u16 m = seg - 1, sz = mcb_size(m);
    if (paras <= sz) {
        if (paras < sz) {
            u8 t = mcb_type(m);
            mcb_set(m + paras + 1, t, 0, sz - paras - 1);
            mcb_set(m, 'M', mcb_owner(m), paras);
            mem_merge();
        }
        return 1;
    }
    /* grow into following free block */
    u16 avail = sz;
    if (mcb_type(m) == 'M') { u16 n = m + sz + 1; if (mcb_owner(n) == 0) avail = sz + mcb_size(n) + 1; }
    if (paras > avail) { *maxp = avail; return 0; }
    u16 n = m + sz + 1; u8 nt = mcb_type(n);
    if (avail - paras >= 1 && avail > paras) {
        mcb_set(m + paras + 1, nt, 0, avail - paras - 1);
        mcb_set(m, 'M', mcb_owner(m), paras);
    } else mcb_set(m, nt, mcb_owner(m), avail);
    return 1;
}

static void mem_free_owner(u16 psp)
{
    u16 s = MEM_FIRST;
    for (;;) { if (mcb_owner(s) == psp) ww(lin(s, 1), 0); if (mcb_type(s) == 'Z') break; s += mcb_size(s) + 1; }
    mem_merge();
}

/* ---------- files ---------- */
#define MAXF 32
static FILE *files[MAXF];
static u32 dta;

static int resolve_path(const char *dos, char *out, size_t n)
{
    /* strip drive letter, map \ to /, then match each component case-insensitively */
    const char *p = dos;
    if (p[0] && p[1] == ':') p += 2;
    char cur[1024]; snprintf(cur, sizeof cur, "%s", game_dir);
    char comp[256];
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != '\\' && *p != '/' && k < 255) comp[k++] = *p++;
        comp[k] = 0;
        if (!strcmp(comp, ".")) continue;
        DIR *d = opendir(cur); int found = 0;
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) if (!strcasecmp(e->d_name, comp)) { snprintf(cur + strlen(cur), sizeof cur - strlen(cur), "/%s", e->d_name); found = 1; break; }
            closedir(d);
        }
        if (!found) { snprintf(cur + strlen(cur), sizeof cur - strlen(cur), "/%s", comp); snprintf(out, n, "%s", cur); return 0; }
    }
    snprintf(out, n, "%s", cur);
    return 1;
}

static void read_asciiz(u16 seg, u16 off, char *buf, int n)
{
    int i = 0;
    for (; i < n - 1; i++) { u8 c = rb(lin(seg, off + i)); if (!c) break; buf[i] = c; }
    buf[i] = 0;
}

#define CF_SET()   (cpu.flags |= F_CF)
#define CF_CLR()   (cpu.flags &= ~F_CF)
#define AL (cpu.r[AX] & 0xFF)
#define AH (cpu.r[AX] >> 8)
static void set_al(u8 v) { cpu.r[AX] = (cpu.r[AX] & 0xFF00) | v; }
static void fail(u16 err) { cpu.r[AX] = err; CF_SET(); }

/* ---------- find first/next ---------- */
static char find_dir[1024], find_pat[16];
static DIR *find_d;
static int wild_match(const char *pat, const char *name)
{
    /* DOS 8.3 wildcard match */
    char pn[9] = "", pe[4] = "", nn[9] = "", ne[4] = "";
    const char *d = strchr(pat, '.'); int i;
    for (i = 0; pat[i] && pat + i != d && i < 8; i++) pn[i] = toupper((u8)pat[i]); pn[i] = 0;
    if (d) { for (i = 0; d[1 + i] && i < 3; i++) pe[i] = toupper((u8)d[1 + i]); pe[i] = 0; }
    d = strchr(name, '.');
    if (strlen(name) > 12) return 0;
    for (i = 0; name[i] && name + i != d && i < 8; i++) nn[i] = toupper((u8)name[i]); nn[i] = 0;
    if (d) { for (i = 0; d[1 + i] && i < 3; i++) ne[i] = toupper((u8)d[1 + i]); ne[i] = 0; }
    for (int part = 0; part < 2; part++) {
        const char *a = part ? pe : pn, *b = part ? ne : nn; int len = part ? 3 : 8;
        for (i = 0; i < len; i++) {
            char c = a[i] ? a[i] : ' ', x = b[i] ? b[i] : ' ';
            if (c == '*') break;
            if (c == '?') continue;
            if (c != x) return 0;
            if (!a[i] && !b[i]) break;
        }
    }
    return 1;
}
static int find_next(void)
{
    struct dirent *e;
    while (find_d && (e = readdir(find_d))) {
        if (e->d_name[0] == '.') continue;
        if (!wild_match(find_pat, e->d_name)) continue;
        char full[2048]; struct stat st; snprintf(full, sizeof full, "%s/%s", find_dir, e->d_name);
        if (stat(full, &st) || S_ISDIR(st.st_mode)) continue;
        wb(dta + 0x15, 0x20);
        ww(dta + 0x16, 0); ww(dta + 0x18, (10 << 5) | 1 | ((1989 - 1980) << 9));
        ww(dta + 0x1A, st.st_size & 0xFFFF); ww(dta + 0x1C, st.st_size >> 16);
        int i; for (i = 0; e->d_name[i] && i < 12; i++) wb(dta + 0x1E + i, toupper((u8)e->d_name[i]));
        wb(dta + 0x1E + i, 0);
        return 1;
    }
    return 0;
}

/* ---------- program loading ---------- */
typedef struct { u16 psp, ss, sp, ret_cs, ret_ip; } ExecFrame;
static ExecFrame exec_stack[8];
static int exec_depth;
static u16 last_retcode;
char prog_name[64];

static u16 make_env(const char *path)
{
    u16 largest; u16 env = mem_alloc(8, 0xFFFF, &largest);
    u32 a = lin(env, 0);
    const char *v = "COMSPEC=C:\\COMMAND.COM";
    for (int i = 0; v[i]; i++) wb(a++, v[i]);
    wb(a++, 0); wb(a++, 0); ww(a, 1); a += 2;
    for (int i = 0; path[i]; i++) wb(a++, path[i]);
    wb(a, 0);
    return env;
}

static int load_exe_image(const char *dospath, u16 load_seg, u16 reloc, int as_overlay,
                          u16 *cs, u16 *ip, u16 *ss, u16 *sp, u16 *minpara)
{
    char host[2048];
    resolve_path(dospath, host, sizeof host);
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc(len); if (fread(d, 1, len, f) != (size_t)len) { fclose(f); free(d); return 0; }
    fclose(f);
    if (len >= 28 && d[0] == 'M' && d[1] == 'Z') {
        u16 lastp = d[2] | d[3] << 8, pages = d[4] | d[5] << 8, nrel = d[6] | d[7] << 8, hdr = (d[8] | d[9] << 8) * 16;
        u16 rel = d[24] | d[25] << 8;
        long size = (long)(pages - 1) * 512 + (lastp ? lastp : 512);
        if (size > len) size = len;
        long img = size - hdr;
        if (minpara) *minpara = (img + 15) / 16 + (d[10] | d[11] << 8);
        for (long i = 0; i < img; i++) mem[(lin(load_seg, 0) + i) & 0xFFFFF] = d[hdr + i];
        for (int i = 0; i < nrel; i++) {
            u16 o = d[rel + i * 4] | d[rel + i * 4 + 1] << 8, s = d[rel + i * 4 + 2] | d[rel + i * 4 + 3] << 8;
            u32 a = lin(load_seg + s, o); ww(a, rw(a) + reloc);
        }
        if (!as_overlay) {
            *ss = (d[14] | d[15] << 8) + reloc; *sp = d[16] | d[17] << 8;
            *ip = d[20] | d[21] << 8; *cs = (d[22] | d[23] << 8) + reloc;
        }
    } else {
        /* .COM: image goes right after the PSP, and CS = DS = ES = SS = PSP */
        u16 psp_seg = as_overlay ? load_seg : load_seg - 0x10;
        for (long i = 0; i < len; i++) mem[lin(psp_seg, 0x100 + i)] = d[i];
        if (minpara) *minpara = 0x1000;
        if (!as_overlay) { *cs = psp_seg; *ip = 0x100; *ss = psp_seg; *sp = 0xFFFE; }
    }
    free(d);
    return 1;
}

static u32 exec_fcb1, exec_fcb2;
int dos_exec(const char *dospath, const char *tail, int nested)
{
    u16 largest;
    mem_alloc(0xFFFF, 0, &largest);
    u16 envseg = make_env(dospath);
    u16 psp = mem_alloc(largest - 0x20 > 0 ? largest - 8 - 1 : largest, 0xFFFF, &largest);
    if (!psp) return 8;
    ww(lin(psp - 1, 1), psp);
    ww(lin(envseg - 1, 1), psp);
    u16 blocksz = mcb_size(psp - 1);
    /* PSP */
    for (int i = 0; i < 256; i++) wb(lin(psp, i), 0);
    ww(lin(psp, 0), 0x20CD);
    ww(lin(psp, 2), psp + blocksz);
    ww(lin(psp, 0x2C), envseg);
    ww(lin(psp, 0x16), cur_psp ? cur_psp : psp);
    wb(lin(psp, 0x50), 0xCD); wb(lin(psp, 0x51), 0x21); wb(lin(psp, 0x52), 0xCB);
    for (int i = 0; i < 20; i++) wb(lin(psp, 0x18 + i), i < 5 ? i : 0xFF);
    ww(lin(psp, 0x32), 20); ww(lin(psp, 0x34), 0x18); ww(lin(psp, 0x36), psp);
    int tl = tail ? (int)strlen(tail) : 0; if (tl > 126) tl = 126;
    wb(lin(psp, 0x80), tl);
    for (int i = 0; i < tl; i++) wb(lin(psp, 0x81 + i), tail[i]);
    wb(lin(psp, 0x81 + tl), 0x0D);
    /* DOS copies the caller's two FCBs into the child PSP (the launcher passes data this way) */
    for (int i = 0; i < 16; i++) { wb(lin(psp, 0x5C + i), exec_fcb1 ? rb(exec_fcb1 + i) : 0); wb(lin(psp, 0x6C + i), exec_fcb2 ? rb(exec_fcb2 + i) : 0); }
    exec_fcb1 = exec_fcb2 = 0;
    /* terminate address = where parent resumes */
    ww(lin(psp, 0x0A), cpu.ip); ww(lin(psp, 0x0C), cpu.s[CS]);
    u16 cs, ip, ss, sp, minp;
    if (!load_exe_image(dospath, psp + 0x10, psp + 0x10, 0, &cs, &ip, &ss, &sp, &minp)) {
        mem_free_owner(psp); return 2;
    }
    if (nested) {
        ExecFrame *fr = &exec_stack[exec_depth++];
        fr->psp = cur_psp; fr->ss = cpu.s[SS]; fr->sp = cpu.r[SP];
        fr->ret_cs = cpu.s[CS]; fr->ret_ip = cpu.ip;
    }
    const char *bn = strrchr(dospath, '\\'); bn = bn ? bn + 1 : dospath;
    if (strchr(bn, ':')) bn = strchr(bn, ':') + 1;
    snprintf(prog_name, sizeof prog_name, "%s", bn);
    for (char *c = prog_name; *c; c++) *c = toupper((u8)*c);
    logmsg("EXEC %s \"%s\" psp=%04x cs:ip=%04x:%04x", prog_name, tail ? tail : "", psp, cs, ip);
    if (getenv("MW_DUMPEXEC") && strcasestr(prog_name, getenv("MW_DUMPEXEC"))) { char fn[64]; snprintf(fn, sizeof fn, "exec_%s.bin", getenv("MW_DUMPTAG") ? getenv("MW_DUMPTAG") : "x"); FILE *f = fopen(fn, "wb"); fwrite(mem, 1, 0xA0000, f); fclose(f); }
    cur_psp = psp;
    dta = lin(psp, 0x80);
    cpu.s[DS] = cpu.s[ES] = psp; cpu.s[SS] = ss; cpu.r[SP] = sp;
    cpu.s[CS] = cs; cpu.ip = ip;
    cpu.r[AX] = 0; cpu.r[BX] = 0; cpu.r[CX] = 0xFF; cpu.r[DX] = psp;
    cpu.r[SI] = ip; cpu.r[DI] = sp; cpu.r[BP] = 0x91C;
    cpu.flags = 0x0202;
    trace_note_program(prog_name, psp + 0x10);
    { void rc_program_loaded(const char *, u16); rc_program_loaded(prog_name, psp + 0x10); }
    { void english_program_loaded(const char *, u16); english_program_loaded(prog_name, psp + 0x10); }
    return 0;
}

extern int ring_on; extern u32 ring[4096]; extern unsigned ring_i;
static void dos_terminate(u8 code)
{
    { void english_program_exit(void); english_program_exit(); }
    { extern int ring_on; extern u32 ring[4096]; extern unsigned ring_i;
      if (ring_on && getenv("MW_RINGEXIT") && strcasestr(prog_name, getenv("MW_RINGEXIT"))) {
          FILE *f = fopen("ring_exit.txt", "w"); for (unsigned i = ring_i - 4096; i != ring_i; i++) { u32 v = ring[i & 4095]; if (v) fprintf(f, "%04x:%04x\n", v >> 16, v & 0xFFFF); } fclose(f); } }

    last_retcode = code;
    for (int i = 5; i < MAXF; i++) if (files[i]) { fclose(files[i]); files[i] = NULL; }
    mem_free_owner(cur_psp);
    { void rc_program_exit(void); rc_program_exit(); }
    if (!exec_depth) { logmsg("program exit %d", code); runtime_quit(code); return; }
    ExecFrame *fr = &exec_stack[--exec_depth];
    cur_psp = fr->psp;
    dta = lin(cur_psp, 0x80);
    cpu.s[SS] = fr->ss; cpu.r[SP] = fr->sp;
    cpu.s[CS] = fr->ret_cs; cpu.ip = fr->ret_ip;
    cpu.flags &= ~F_CF;
    logmsg("child exit %d, back in parent", code);
}

void dos_init(void)
{
    mem_init();
    /* standard handles */
    files[0] = stdin; files[1] = stdout; files[2] = stderr;
}

/* ---------- INT 21h ---------- */
void int21(void)
{
    char path[256], host[2048];
    if (log_enabled > 1) logmsg("int21 ax=%04x bx=%04x cx=%04x dx=%04x", cpu.r[AX], cpu.r[BX], cpu.r[CX], cpu.r[DX]);
    switch (AH) {
    case 0x00: dos_terminate(0); break;
    case 0x01: { int k = kbd_getkey_blocking(); set_al(k & 0xFF); console_putc(k & 0xFF); } break;
    case 0x02: console_putc(cpu.r[DX] & 0xFF); break;
    case 0x06:
        if ((cpu.r[DX] & 0xFF) == 0xFF) {
            int k = kbd_peek();
            if (k < 0) { set_al(0); cpu.flags |= F_ZF; }
            else { kbd_getkey(); set_al(k & 0xFF); cpu.flags &= ~F_ZF; }
        } else console_putc(cpu.r[DX] & 0xFF);
        break;
    case 0x07: case 0x08: { int k = kbd_getkey_blocking(); set_al(k & 0xFF); } break;
    case 0x09: { u32 a = lin(cpu.s[DS], cpu.r[DX]); for (int i = 0; i < 4096; i++) { u8 c = rb(a + i); if (c == '$') break; console_putc(c); } set_al('$'); } break;
    case 0x0B: set_al(kbd_peek() >= 0 ? 0xFF : 0); break;
    case 0x0C: kbd_flush(); { u8 sub = AL; if (sub == 1 || sub == 6 || sub == 7 || sub == 8) { cpu.r[AX] = (sub << 8) | (cpu.r[AX] & 0xFF); int21(); } } break;
    case 0x0D: break;
    case 0x0E: set_al(3); break;
    case 0x19: set_al(2); break;
    case 0x1A: dta = lin(cpu.s[DS], cpu.r[DX]); break;
    case 0x25: ww(AL * 4, cpu.r[DX]); ww(AL * 4 + 2, cpu.s[DS]); break;
    case 0x2A: { time_t t = time(0); struct tm *tm = localtime(&t); cpu.r[CX] = tm->tm_year + 1900; cpu.r[DX] = ((tm->tm_mon + 1) << 8) | tm->tm_mday; set_al(tm->tm_wday); } break;
    case 0x2C: { u32 ms = runtime_ms(); time_t t = time(0); struct tm *tm = localtime(&t);
        cpu.r[CX] = (tm->tm_hour << 8) | tm->tm_min; cpu.r[DX] = (tm->tm_sec << 8) | ((ms / 10) % 100); } break;
    case 0x2F: cpu.s[ES] = dta >> 4; cpu.r[BX] = dta & 0xF; break;
    case 0x30: cpu.r[AX] = 0x1E03; cpu.r[BX] = 0; cpu.r[CX] = 0; break;   /* DOS 3.30 */
    case 0x33: if (AL == 0) cpu.r[DX] &= 0xFF00; break;
    case 0x35: cpu.r[BX] = rw(AL * 4); cpu.s[ES] = rw(AL * 4 + 2); break;
    case 0x36: cpu.r[AX] = 4; cpu.r[BX] = 0x4000; cpu.r[CX] = 512; cpu.r[DX] = 0x8000; break;
    case 0x3B: CF_CLR(); break;
    case 0x3C: case 0x3D: {
        read_asciiz(cpu.s[DS], cpu.r[DX], path, sizeof path);
        resolve_path(path, host, sizeof host);
        const char *mode = AH == 0x3C ? "w+b" : ((AL & 3) == 0 ? "rb" : "r+b");
        FILE *f = fopen(host, mode);
        if (!f && AH == 0x3D && (AL & 3)) f = fopen(host, "rb");
        logmsg("open %s -> %s %s", path, host, f ? "ok" : "FAIL");
        { void intro_file_opened(const char *); intro_file_opened(path); }
        if (!f) { fail(2); break; }
        int h; for (h = 5; h < MAXF && files[h]; h++); if (h == MAXF) { fclose(f); fail(4); break; }
        files[h] = f; cpu.r[AX] = h; CF_CLR();
        { extern int font_fix_handle; const char *bn = strrchr(path, '\\'); bn = bn ? bn + 1 : path; if (!strcasecmp(bn, "IWA.FNT")) font_fix_handle = h; }
        } break;
    case 0x3E: { int h = cpu.r[BX]; { extern int font_fix_handle; if (h == font_fix_handle) font_fix_handle = -1; } if (h >= 5 && h < MAXF && files[h]) { fclose(files[h]); files[h] = NULL; } CF_CLR(); } break;
    case 0x3F: { int h = cpu.r[BX]; u16 n = cpu.r[CX];
        if (h == 0) { /* stdin line read */
            int got = 0; while (got < n) { int k = kbd_getkey_blocking() & 0xFF; if (k == 8) { if (got) got--; continue; }
                console_putc(k); wb(lin(cpu.s[DS], cpu.r[DX] + got), k); got++; if (k == 13) { if (got < n) { wb(lin(cpu.s[DS], cpu.r[DX] + got), 10); got++; } console_putc(10); break; } }
            cpu.r[AX] = got; CF_CLR(); break; }
        if (h >= MAXF || !files[h]) { fail(6); break; }
        u8 buf[65536]; long pos0 = ftell(files[h]); size_t r = fread(buf, 1, n, files[h]);
        { extern int font_fix_handle; void font_fix(u8 *b, long pos, size_t n); if (h == font_fix_handle) font_fix(buf, pos0, r); }
        for (size_t i = 0; i < r; i++) wb(lin(cpu.s[DS], cpu.r[DX] + i), buf[i]);
        cpu.r[AX] = r; CF_CLR(); } break;
    case 0x40: { int h = cpu.r[BX]; u16 n = cpu.r[CX];
        if (h == 1 || h == 2) { for (int i = 0; i < n; i++) console_putc(rb(lin(cpu.s[DS], cpu.r[DX] + i))); cpu.r[AX] = n; CF_CLR(); break; }
        if (h >= MAXF || !files[h]) { fail(6); break; }
        if (n == 0) { fflush(files[h]); cpu.r[AX] = 0; CF_CLR(); break; }
        u8 buf[65536]; for (int i = 0; i < n; i++) buf[i] = rb(lin(cpu.s[DS], cpu.r[DX] + i));
        cpu.r[AX] = fwrite(buf, 1, n, files[h]); CF_CLR(); } break;
    case 0x41: read_asciiz(cpu.s[DS], cpu.r[DX], path, sizeof path); resolve_path(path, host, sizeof host); if (remove(host)) fail(2); else CF_CLR(); break;
    case 0x42: { int h = cpu.r[BX]; if (h >= MAXF || !files[h]) { fail(6); break; }
        long off = (s32)(((u32)cpu.r[CX] << 16) | cpu.r[DX]);
        fseek(files[h], off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END);
        long p = ftell(files[h]); cpu.r[AX] = p & 0xFFFF; cpu.r[DX] = p >> 16; CF_CLR(); } break;
    case 0x43: read_asciiz(cpu.s[DS], cpu.r[DX], path, sizeof path); if (resolve_path(path, host, sizeof host)) { cpu.r[CX] = 0x20; CF_CLR(); } else fail(2); break;
    case 0x44:
        if (AL == 0) { int h = cpu.r[BX]; cpu.r[DX] = h < 3 ? (h == 0 ? 0x81 : 0x82) | 0x80 : 0x02; CF_CLR(); }
        else if (AL == 1) CF_CLR();
        else if (AL == 8) { cpu.r[AX] = 1; CF_CLR(); }
        else { cpu.r[AX] = 0; CF_CLR(); }
        break;
    case 0x47: wb(lin(cpu.s[DS], cpu.r[SI]), 0); CF_CLR(); break;
    case 0x48: { u16 largest; u16 s = mem_alloc(cpu.r[BX], cur_psp, &largest);
        if (s) { cpu.r[AX] = s; CF_CLR(); } else { cpu.r[BX] = largest; fail(8); }
        logmsg("alloc %04x paras -> %04x", cpu.r[BX], s); } break;
    case 0x49: mem_free(cpu.s[ES]); CF_CLR(); break;
    case 0x4A: { u16 mx; if (mem_resize(cpu.s[ES], cpu.r[BX], &mx)) CF_CLR(); else { cpu.r[BX] = mx; fail(8); } } break;
    case 0x4B: {
        read_asciiz(cpu.s[DS], cpu.r[DX], path, sizeof path);
        u32 pb = lin(cpu.s[ES], cpu.r[BX]);
        if (AL == 3) {
            u16 seg = rw(pb), rel = rw(pb + 2);
            if (load_exe_image(path, seg, rel, 1, 0, 0, 0, 0, 0)) { CF_CLR(); trace_note_overlay(path, seg); { void rc_overlay_loaded(const char *, u16); rc_overlay_loaded(path, seg); } logmsg("overlay %s at %04x", path, seg); }
            else fail(2);
        } else {
            char tail[130]; u16 to = rw(pb + 2), ts = rw(pb + 4);
            u8 tl = rb(lin(ts, to)); int i; for (i = 0; i < tl && i < 127; i++) tail[i] = rb(lin(ts, to + 1 + i)); tail[i] = 0;
            exec_fcb1 = lin(rw(pb + 8), rw(pb + 6)); exec_fcb2 = lin(rw(pb + 12), rw(pb + 10));
            int e = dos_exec(path, tail, 1);
            if (e) fail(e);
        }
    } break;
    case 0x4C: dos_terminate(AL); break;
    case 0x31: { /* terminate and stay resident: keep DX paragraphs of the PSP block */
        u16 mx; mem_resize(cur_psp, cpu.r[DX], &mx);
        ww(lin(cur_psp - 1, 1), 0xFFFF);  /* mark resident so the owner sweep keeps it */
        logmsg("TSR %s keeps %04x paras", prog_name, cpu.r[DX]);
        dos_terminate(AL); } break;
    case 0x4D: cpu.r[AX] = last_retcode; CF_CLR(); break;
    case 0x4E: {
        read_asciiz(cpu.s[DS], cpu.r[DX], path, sizeof path);
        char *bs = strrchr(path, '\\'); if (!bs) bs = strrchr(path, ':');
        snprintf(find_pat, sizeof find_pat, "%s", bs ? bs + 1 : path);
        if (bs) { *bs = 0; resolve_path(path, find_dir, sizeof find_dir); } else snprintf(find_dir, sizeof find_dir, "%s", game_dir);
        if (find_d) closedir(find_d);
        find_d = opendir(find_dir);
        if (find_next()) CF_CLR(); else fail(18);
    } break;
    case 0x4F: if (find_next()) CF_CLR(); else fail(18); break;
    case 0x50: cur_psp = cpu.r[BX]; break;
    case 0x51: case 0x62: cpu.r[BX] = cur_psp; break;
    case 0x56: CF_CLR(); break;
    case 0x57: if (AL == 0) { cpu.r[CX] = 0; cpu.r[DX] = (10 << 5) | 1 | (9 << 9); } CF_CLR(); break;
    default:
        logmsg("unhandled int21 ah=%02x", AH);
        fail(1);
    }
}

#include "snap.h"
void dos_snap(FILE *f, int save)
{
    S(cur_psp); S(dta); S(exec_stack); S(exec_depth); S(last_retcode); S(prog_name); S(exec_fcb1); S(exec_fcb2);
    /* open files are not carried over: snapshots are taken on screens with nothing open */
    if (!save) for (int i = 5; i < MAXF; i++) if (files[i]) { fclose(files[i]); files[i] = NULL; }
}
