#include "compat.h"
/* English text for the PC-98 edition.
 *
 * textmap/pc98_<program>.map pairs each Japanese string in a PC-98 program
 * (image offset) with the string the DOS build uses in the same place. When
 * the PC-98 program is loaded, the English is read from the player's own DOS
 * copy (--english /path/to/MECHWAR) and either written over the Japanese in
 * place (when it fits) or kept in a reserved block that the text routine is
 * pointed at. Two small hooks in the strategy program's text routine make
 * plain ASCII draw half-width (the routine otherwise only draws half-width
 * characters after an FEh prefix). */
#include "machine.h"
#include "runtime.h"
#include "pc98.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

const char *english_dir;           /* DOS game folder, NULL = Japanese */
const char *english_fallback;      /* optional TSV of translations for text the DOS version lacks */
static u32 done_lo[4096], done_hi[4096]; static int ndone;   /* ranges already given DOS English */
double english_min_conf = 2.0;
#define RESERVE_SEG 0xC000         /* English copies live above 640 KB (PC-98 C000-DFFF: no RAM, no ROM in use), so the game keeps all its conventional memory */
static u32 reserve_next = RESERVE_SEG * 16;

typedef struct { u32 from, to; u16 len; u8 *orig; int dead; int name; } Redirect;
static Redirect redir[2048]; static int nredir;
static u32 inplace_at[4096]; static int ninplace;
static int drawing_english;   /* the text routine is currently drawing a string we replaced */

static u8 *load_mz_image(const char *dir, const char *name, long *len)
{
    char path[1200]; DIR *d = opendir(dir); struct dirent *e; path[0] = 0;
    if (!d) return NULL;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(path, sizeof path, "%s/%s", dir, e->d_name); break; }
    closedir(d);
    if (!path[0]) return NULL;
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; } fclose(f);
    long hdr = (b[8] | b[9] << 8) * 16, size = ((b[4] | b[5] << 8) - 1) * 512L + ((b[2] | b[3] << 8) ? (b[2] | b[3] << 8) : 512);
    if (size > n) size = n;
    u16 ecs = b[0x16] | b[0x17] << 8;
    *len = size - hdr;
    memmove(b, b + hdr, *len);
    /* Microsoft EXEPACK (the DOS combat engine): unpack so offsets match the running image */
    u32 eh = ecs * 16;
    if (eh + 18 <= (u32)*len && b[eh + 16] == 'R' && b[eh + 17] == 'B') {
        u16 dlen = b[eh + 12] | b[eh + 13] << 8, skip = b[eh + 14] | b[eh + 15] << 8;
        long comp_end = eh - (skip - 1) * 16L, outlen = dlen * 16L;
        u8 *out = calloc(outlen > comp_end ? outlen : comp_end, 1);
        long src = comp_end - 1, dst = outlen - 1;
        while (src >= 0 && b[src] == 0xFF) src--;
        for (;;) {
            if (src < 2) break;
            u8 cmd = b[src]; u16 cnt = b[src - 2] | b[src - 1] << 8; src -= 3;
            if ((cmd & 0xFE) == 0xB0) { u8 v = b[src--]; for (u16 i = 0; i < cnt && dst >= 0; i++) out[dst--] = v; }
            else if ((cmd & 0xFE) == 0xB2) { for (u16 i = 0; i < cnt && dst >= 0 && src >= 0; i++) out[dst--] = b[src--]; }
            else break;
            if (cmd & 1) break;
        }
        if (src >= 0) memcpy(out, b, src + 1);
        free(b); b = out; *len = outlen;
    }
    return b;
}

/* Japanese records can start with binary fields (ids, coordinates) before the text: the text
 * is the shortest tail that parses cleanly as Shift-JIS / ASCII. */
static size_t jp_text_start(const u8 *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        size_t k = i; int ok = 1;
        while (k < n) {
            u8 c = p[k];
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF)) {
                if (k + 1 >= n || p[k + 1] < 0x40 || p[k + 1] == 0x7F || p[k + 1] > 0xFC) { ok = 0; break; }
                k += 2;
            } else if (c >= 0x20 && c < 0x7F) k++;
            else if (c == 0x0D) k++;
            else { ok = 0; break; }
        }
        if (ok) return i;
    }
    return n;
}
static size_t header_len(const u8 *p, size_t n)
{
    size_t i = 0;
    for (;;) {   /* (optional parameter byte, control code) pairs */
        if (i < n && p[i] < 0x20) { i++; continue; }
        if (i + 1 < n && p[i] >= 0x20 && p[i] < 0x7F && p[i + 1] < 0x20 && p[i + 1] != 0x0D) { i += 2; continue; }
        return i;
    }
}
static int need_upper = 1;   /* DOS tier: reject lower-case file names; the fallback table has lower-case continuation lines */
static int has_letters(const u8 *p, size_t n)
{
    /* real text: Latin letters, and none of the symbols that show up when the "string"
     * is actually table data that happens to be printable */
    int letters = 0, upper = 0;
    for (size_t i = 0; i < n; i++) {
        u8 c = p[i];
        if (c >= 'A' && c <= 'Z') upper++;
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') letters++;
        else if (strchr("@#$^~`{}|\\[]<>_", c)) return 0;
    }
    /* file names in the DOS data are lower case; game text never is */
    return letters >= 2 && (upper > 0 || !need_upper);
}

/* Lay English out on the Japanese line's columns. In the Japanese text every character
 * is 16 dots wide (kanji/kana, and ASCII too: the game draws ASCII full-width), while our
 * English letters are 8 dots. Each line is split into fields at runs of 2+ spaces (a field
 * that is only a colon belongs to the field before it). When both sides have the same
 * number of fields, each English field is padded out to start at the same dot position as
 * its Japanese field, so numbers the game draws later at fixed positions still line up.
 * Returns the new length (which may exceed the Japanese byte length). */
static int is_lead(u8 c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF); }
static int jp_fields(const u8 *p, size_t n, int *x)
{
    int nf = 0, px = 0; size_t k = 0;
    while (k < n && nf < 16) {
        int gap = 0;
        while (k < n) {
            if (p[k] == ' ') { gap++; px += 16; k++; }
            else if (p[k] == 0x81 && k + 1 < n && p[k + 1] == 0x40) { gap += 2; px += 16; k += 2; }
            else break;
        }
        if (k >= n) break;
        int colon_only = (p[k] == ':' && (k + 1 >= n || p[k + 1] == ' ')) || (p[k] == 0x81 && k + 1 < n && p[k + 1] == 0x46);
        if (nf == 0 || (gap >= 2 && !colon_only)) x[nf++] = px;
        while (k < n) {
            if (p[k] == ' ' && k + 1 < n && p[k + 1] == ' ') break;
            if (p[k] == 0x81 && k + 1 < n && p[k + 1] == 0x40) break;
            if (is_lead(p[k]) && k + 1 < n) k += 2; else k++;
            px += 16;
        }
    }
    return nf;
}
static size_t columnize(u8 *out, size_t cap, const u8 *en, size_t elen, const u8 *jp, size_t jlen)
{
    size_t o = 0, ei = 0, ji = 0;
    for (;;) {
        size_t ee = ei, je = ji;
        while (ee < elen && en[ee] != 0x0D) ee++;
        while (je < jlen && jp[je] != 0x0D) je++;
        int fs[16], fl[16], ne = 0, jx[16];
        for (size_t k = ei; k < ee && ne < 16; ) {
            while (k < ee && en[k] == ' ') k++;
            if (k >= ee) break;
            size_t st = k; while (k < ee && !(en[k] == ' ' && k + 1 < ee && en[k + 1] == ' ')) k++;
            while (k > st && en[k - 1] == ' ') k--;
            fs[ne] = st; fl[ne] = k - st; ne++;
            while (k < ee && en[k] == ' ') k++;
        }
        int nj = jp_fields(jp + ji, je - ji, jx);
        int ok = ne > 1 && ne == nj;
        size_t ls = o;
        for (int f = 0; ok && f < ne; f++) {
            size_t col = jx[f] / 8;
            if (f && o - ls >= col) col = o - ls + 1;   /* keep at least one space */
            while (o - ls < col && o < cap) out[o++] = ' ';
            for (int c = 0; c < fl[f] && o < cap; c++) out[o++] = en[fs[f] + c];
        }
        if (!ok) for (size_t k = ei; k < ee && o < cap; k++) out[o++] = en[k];
        if (ee >= elen) break;
        if (o < cap) out[o++] = 0x0D;
        ei = ee + 1; ji = je < jlen ? je + 1 : je;
    }
    return o;
}

static int hook_text_entry, hook_text_fe, hook_text2_entry, hook_text2_call, hook_bt_text, hook_bt_char, hook_text_join;   /* linear addresses, 0 = off */
static u16 text_cs;
static u32 patrol_lo, patrol_hi;
static double bt_loaded_ms = -1; static int bt_swapped;   /* combat engine: names swapped in place after its start-up check */   /* data segment to watch for copied names (combat engine) */

void english_program_loaded(const char *name, u16 seg)
{
    { extern int english_art_on; english_art_on = english_dir != NULL || getenv("MW_ARTHASH") != NULL; }
    need_upper = 1;
    for (int i = 0; i < nredir; i++) free(redir[i].orig);
    memset(redir, 0, sizeof redir);
    nredir = 0; ninplace = 0; ndone = 0; hook_bt_text = 0; hook_bt_char = 0; hook_text_join = 0; patrol_lo = patrol_hi = 0; bt_loaded_ms = -1; bt_swapped = 0;
    reserve_next = RESERVE_SEG * 16;   /* the previous program's copies are no longer needed */
    if (machine_pc98) memset(mem + RESERVE_SEG * 16, 0, 0xE0000 - RESERVE_SEG * 16);   /* and must not look like RAM/ROM to the next one */ hook_text_entry = hook_text_fe = hook_text2_entry = hook_text2_call = 0;
    if (getenv("MW_TXTDBG") && machine_pc98 && !strcasecmp(name, "MW_MAIN.EXE") && !english_dir) { text_cs = seg + 0x709; hook_text_entry = lin(text_cs, 0x0966); hook_text2_entry = lin(text_cs, 0x04ED); return; }
    if (!english_dir || !machine_pc98) return;
    char mapname[64], mp[200];
    snprintf(mapname, sizeof mapname, "%s", name);
    for (char *c = mapname; *c; c++) { *c = *c >= 'A' && *c <= 'Z' ? *c + 32 : *c; if (*c == '.') { *c = 0; break; } }
    snprintf(mp, sizeof mp, "textmap/pc98_%s.map", mapname);
    FILE *f = fopen(mp, "r");
    if (!f) return;
    long dlen; u8 *dos = load_mz_image(english_dir, name, &dlen);
    if (!dos) { fclose(f); logmsg("english: %s not found in %s", name, english_dir); return; }
    /* per-string overrides found in testing: r = redirect only, skip = keep Japanese */
    unsigned ov_off[256]; char ov_act[256]; char ov_text[256][32]; int nov = 0;
    { char op[200]; snprintf(op, sizeof op, "textmap/pc98_%s.overrides", mapname);
      FILE *o = fopen(op, "r"); char line[128];
      while (o && fgets(line, sizeof line, o) && nov < 256) { unsigned x; char act[16];
          if (line[0] != '#' && sscanf(line, "%x %15s", &x, act) == 2) {
              ov_off[nov] = x; ov_act[nov] = act[0]; ov_text[nov][0] = 0;
              if (act[0] == 'w' || act[0] == 'h' || act[0] == 't' || act[0] == 'l' || act[0] == 'H') { char *q = strchr(line + 5, act[0]); q = q ? q + 1 : line; while (*q == ' ') q++;
                  snprintf(ov_text[nov], sizeof ov_text[nov], "%s", q); char *e = strpbrk(ov_text[nov], "\r\n"); if (e) *e = 0; }
              nov++; } }
      if (o) fclose(o); }
    u32 base = seg * 16; unsigned a, b; double conf; int inplace = 0, moved = 0;
    int pidx = -1, rlo = 0, rhi = 1 << 30; { const char *r = getenv("MW_EN_RANGE"); if (r) sscanf(r, "%d,%d", &rlo, &rhi); }
    while (fscanf(f, "%x %x %lf", &a, &b, &conf) == 3) {
        pidx++; if (pidx < rlo || pidx >= rhi) continue;
        if (conf < english_min_conf || b >= (unsigned)dlen) continue;
        char act = 0; for (int k = 0; k < nov; k++) if (ov_off[k] == a) act = ov_act[k];
        if (act == 's') continue;
        const u8 *en = dos + b; size_t elen = strnlen((const char *)en, dlen - b);
        u32 jp = base + a; size_t jlen = 0; while (mem[jp + jlen] && jlen < 4096) jlen++;
        /* records start with layout bytes (a parameter byte and/or control code) that
         * differ between editions: keep the PC-98 header, replace only the text body */
        size_t jh = header_len(mem + jp, jlen), eh = header_len(en, elen);
        { size_t ts = jp_text_start(mem + jp, jlen); if (ts > jh && ts < jlen) jh = ts; }
        const u8 *body = en + eh; size_t blen = elen - eh;
        if (!blen || !has_letters(body, blen)) continue;
        /* keep any leading bytes both editions share (record layout data) */
        { size_t k = 0; while (k < jlen && k < elen && mem[jp + k] == en[k]) k++; if (k > jh) jh = k; if (k > eh) { eh = k; body = en + eh; blen = elen - eh; } }
        if (!blen || !has_letters(body, blen)) continue;
        if (jlen && mem[jp + jlen - 1] == '\n') {
            /* fixed-width label lists ("AAAAAABBBBBB\n"): the menu code slices them into equal
             * fields, 6 characters wide in the DOS build and 8 bytes in the PC-98 one. Re-slot
             * the DOS labels into the PC-98 field width when both split into the same count. */
            size_t jb = jlen - 1 - jh, eb = (elen > eh && en[elen - 1] == '\n') ? elen - 1 - eh : 0;
            for (int nf = 2; nf <= 8; nf++) {
                if (!eb || jb % nf || eb % nf || jb / nf < eb / nf) continue;
                size_t jw = jb / nf, ew = eb / nf;
                if (jw != 8 && jw != 6 && jw != 4) continue;
                for (int fi = 0; fi < nf; fi++)
                    for (size_t c = 0; c < jw; c++) mem[jp + jh + fi * jw + c] = c < ew ? en[eh + fi * ew + c] : ' ';
                if (ninplace < 4096) inplace_at[ninplace++] = jp;
                if (ndone < 4096) { done_lo[ndone] = jp; done_hi[ndone++] = jp + jlen; }
                inplace++;
                break;
            }
            continue;
        }
        /* a DOS string can hold more lines than the Japanese one it replaces (two labels in one
         * string where the PC-98 has one per slot): keep only as many lines as the slot has */
        { int jl_lines = 1, el = 1; for (size_t k = jh; k < jlen; k++) if (mem[jp + k] == 0x0D) jl_lines++;
          for (size_t k = 0; k < blen; k++) if (body[k] == 0x0D && ++el > jl_lines) { blen = k; break; } }
        static u8 colbuf[2048];
        if (blen < 1500) { blen = columnize(colbuf, sizeof colbuf, body, blen, mem + jp + jh, jlen - jh); body = colbuf; }   /* fixed-width label lists: widths differ between editions */
        if (getenv("MW_EN_NOINPLACE") && jh + blen <= jlen) continue;
        if (getenv("MW_EN_NOREDIR") && jh + blen > jlen) continue;
        if (jh + blen <= jlen && act != 'r') {
            /* pad with spaces up to the original terminator: some tables are walked by counting
             * terminators, so the string must end exactly where the Japanese one did */
            for (size_t i = 0; i < blen; i++) mem[jp + jh + i] = body[i];
            for (size_t i = jh + blen; i < jlen; i++) mem[jp + i] = ' ';
            inplace++;
            if (ninplace < 4096) inplace_at[ninplace++] = jp;
            if (ndone < 4096) { done_lo[ndone] = jp; done_hi[ndone++] = jp + jlen; }
        } else if (nredir < 2048 && reserve_next + jh + blen + 1 < 0xE0000) {
            Redirect *r = &redir[nredir++];
            r->from = jp; r->to = reserve_next; r->len = jlen; r->dead = 0;
            r->orig = malloc(jlen); memcpy(r->orig, mem + jp, jlen); r->name = (act == 'r');
            memcpy(mem + reserve_next, mem + jp, jh);
            memcpy(mem + reserve_next + jh, body, blen); mem[reserve_next + jh + blen] = 0;
            /* callers may hand over the text after the header instead of the record start */
            if (jh && nredir < 2048) {
                Redirect *r2 = &redir[nredir++];
                r2->from = jp + jh; r2->to = reserve_next + jh; r2->len = jlen - jh; r2->dead = 0;
                r2->orig = malloc(jlen - jh); memcpy(r2->orig, mem + jp + jh, jlen - jh);
            }
            reserve_next += jh + blen + 1; moved++;
            if (ndone < 4096) { done_lo[ndone] = jp; done_hi[ndone++] = jp + jlen; }
        }
    }
    fclose(f); free(dos);
    /* override writes: short UI text of our own (date suffixes, month names) */
    /* 'h': half-width text in the FEh-prefixed form the game itself uses (FE c FE c ...),
     * for strings drawn by routines that only understand two-byte units */
    for (int k = 0; k < nov; k++) if (ov_act[k] == 'h') {
        u32 at = base + ov_off[k]; size_t jl = 0; while (mem[at + jl] && mem[at + jl] != 0x0D && jl < 64) jl++;
        size_t tl = strlen(ov_text[k]);
        for (size_t i = 0; i + 1 < jl; i += 2) { mem[at + i] = 0xFE; mem[at + i + 1] = i / 2 < tl ? (u8)ov_text[k][i / 2] : ' '; }
    }
    /* 't': our own text, swapped in only when drawn (for strings too short to hold it) */
    for (int k = 0; k < nov; k++) if (ov_act[k] == 't' && nredir < 2048) {
        u32 at = base + ov_off[k]; size_t jl = 0; while (mem[at + jl] && jl < 64) jl++;
        size_t tl = strlen(ov_text[k]);
        Redirect *r = &redir[nredir++];
        r->from = at; r->to = reserve_next; r->len = jl; r->dead = 0; r->name = 1; r->orig = malloc(jl ? jl : 1); memcpy(r->orig, mem + at, jl);
        memcpy(mem + reserve_next, ov_text[k], tl); mem[reserve_next + tl] = 0; reserve_next += tl + 1;
        if (ndone < 4096) { done_lo[ndone] = at; done_hi[ndone++] = at + jl; }
    }
    /* 'l': "OFF l LEN TEXT" - a label of exactly LEN bytes inside a record (screen templates
     * whose labels are followed by value placeholders): written as plain half-width text */
    for (int k = 0; k < nov; k++) if (ov_act[k] == 'l') {
        int len = atoi(ov_text[k]); const char *t = strchr(ov_text[k], ' '); t = t ? t + 1 : "";
        u32 at = base + ov_off[k]; size_t tl = strlen(t);
        for (int i = 0; i < len; i++) mem[at + i] = i < (int)tl ? (u8)t[i] : ' ';
        u32 cand[3] = { at, at - 1, at - 5 };
        for (int c = 0; c < 3; c++) if (ninplace < 4096) inplace_at[ninplace++] = cand[c];
        if (ndone < 4096) { done_lo[ndone] = at; done_hi[ndone++] = at + len; }
    }
    /* 'H': "OFF H LEN TEXT" - half-width text in the game's FEh form over exactly LEN bytes */
    for (int k = 0; k < nov; k++) if (ov_act[k] == 'H') {
        int len = atoi(ov_text[k]); const char *t = strchr(ov_text[k], ' '); t = t ? t + 1 : "";
        u32 at = base + ov_off[k]; size_t tl = strlen(t);
        for (int i = 0; i + 1 < len; i += 2) { mem[at + i] = 0xFE; mem[at + i + 1] = i / 2 < (int)tl ? (u8)t[i / 2] : ' '; }
        if (ndone < 4096) { done_lo[ndone] = at; done_hi[ndone++] = at + len; }
    }
    for (int k = 0; k < nov; k++) if (ov_act[k] == 'w') {
        u32 at = base + ov_off[k]; size_t jl = 0; while (mem[at + jl] && jl < 64) jl++;
        size_t tl = strlen(ov_text[k]); if (tl > jl) tl = jl;
        for (size_t i = 0; i < jl; i++) mem[at + i] = i < tl ? (u8)ov_text[k][i] : ' ';
        if (ninplace < 4096) inplace_at[ninplace++] = at;
        if (ndone < 4096) { done_lo[ndone] = at; done_hi[ndone++] = at + jl; }
    }
    /* second tier: a translation table (file, offset, max_bytes, terminator, english, japanese)
     * for text with no DOS counterpart. Offsets are file offsets; anything that already got
     * DOS English is left alone. */
    int fb = 0, fb_mismatch = 0;
    need_upper = 0;
    if (english_fallback && !(getenv("MW_EN_NOFB") && strcasestr(name, getenv("MW_EN_NOFB")))) {
        FILE *t = fopen(english_fallback, "r");
        char exe[1200]; DIR *gd = opendir(game_dir); struct dirent *ge; exe[0] = 0;
        while (gd && (ge = readdir(gd))) if (!strcasecmp(ge->d_name, name)) snprintf(exe, sizeof exe, "%s/%s", game_dir, ge->d_name);
        if (gd) closedir(gd);
        long hdr = 0; FILE *x = exe[0] ? fopen(exe, "rb") : NULL;
        if (x) { u8 h[10]; if (fread(h, 1, 10, x) == 10) hdr = (h[8] | h[9] << 8) * 16; fclose(x); }
        char line[8192];
        while (t && hdr && fgets(line, sizeof line, t)) {
            char *col[6]; int n = 0; char *p = line;
            for (; n < 6; n++) { col[n] = p; char *tab = strchr(p, '\t'); if (!tab) { n++; break; } *tab = 0; p = tab + 1; }
            if (line[0] == '#' || n < 5 || strcasecmp(col[0], name)) continue;
            long off = strtol(col[1], NULL, 16) - hdr; int mx = atoi(col[2]);
            if (off < 0 || mx <= 0) continue;
            u32 at = base + off; int skip = 0;
            for (int k = 0; k < nov; k++) if (ov_off[k] == (unsigned)off && ov_act[k] == 's') skip = 1;
            for (int k = 0; k < ndone && !skip; k++) if (at < done_hi[k] && at + mx > done_lo[k]) skip = 1;
            if (getenv("MW_FBDBG") && (unsigned)off == strtoul(getenv("MW_FBDBG"), NULL, 16)) fprintf(stderr, "fb %lx skip=%d mx=%d\n", off, skip, mx);
            if (skip) continue;
            /* MW1-Port's own table carries a hash of the original bytes instead of the Japanese:
             * a row that doesn't match this copy of the game is not applied */
            if (n >= 6 && strlen(col[5]) >= 8 && strspn(col[5], "0123456789abcdefABCDEF") == 8) {
                u32 h = 2166136261u; for (int i = 0; i < mx; i++) { h ^= mem[at + i]; h *= 16777619u; }
                if (h != (u32)strtoul(col[5], NULL, 16)) { fb_mismatch++; continue; }
            }
            /* english column: UTF-8; U+23CE (E2 8F 8E) marks a line break */
            static u8 out[2048]; int o = 0;
            for (u8 *q = (u8 *)col[4]; *q && o < (int)sizeof out; q++) {
                if (q[0] == 0xE2 && q[1] == 0x8F && q[2] == 0x8E) { out[o++] = 0x0D; q += 2; }
                else if (*q < 0x80) out[o++] = *q;
            }
            if (o > 0) { static u8 cb[2048]; size_t n2 = columnize(cb, sizeof cb, out, o, mem + at, mx); memcpy(out, cb, n2); o = n2; }
            if (getenv("MW_FBDBG") && (unsigned)off == strtoul(getenv("MW_FBDBG"), NULL, 16)) fprintf(stderr, "fb %lx o=%d letters=%d\n", off, o, has_letters(out, o));
            if (o > 0 && !has_letters(out, o)) continue;
            /* people's names (with the Japanese middle dot) are matched against the mission data
             * by the combat engine: keep their bytes, swap them only when drawn */
            int is_name = 0; if (mx <= 24) for (int i = 0; i + 1 < mx; i++) { if (mem[at + i] == 0x0D) { is_name = 0; break; } if (mem[at + i] == 0x81 && mem[at + i + 1] == 0x45) is_name = 1; }
            if (o <= mx && !is_name) {
                for (int i = 0; i < o; i++) mem[at + i] = out[i];
                for (int i = o; i < mx; i++) mem[at + i] = ' ';
                if (ninplace < 4096) inplace_at[ninplace++] = at;
            } else {
                /* too long once laid out: the routine is handed the record start (after the
                 * previous terminator), so redirect that, keeping the record's header bytes */
                u32 rs = at; while (rs > base && mem[rs - 1] != 0 && mem[rs - 1] < 0x20 && at - rs < 16) rs--;
                size_t jh = at - rs, jl = 0; while (mem[rs + jl] && jl < 4096) jl++;
                if (nredir >= 2048 || reserve_next + jh + o + 1 >= 0xE0000) continue;
                Redirect *r = &redir[nredir++];
                r->from = rs; r->to = reserve_next; r->len = jl; r->dead = 0;
                if (getenv("MW_FBDBG")) fprintf(stderr, "fb redirect from %05x (at %05x) to %05x\n", rs, at, r->to);
                r->orig = malloc(jl); memcpy(r->orig, mem + rs, jl);
                memcpy(mem + reserve_next, mem + rs, jh); memcpy(mem + reserve_next + jh, out, o);
                mem[reserve_next + jh + o] = 0;
                /* some callers hand over the record start (header included), others the text after
                 * the header: redirect both */
                if (jh && nredir < 2048) {
                    Redirect *r2 = &redir[nredir++];
                    r2->from = at; r2->to = reserve_next + jh; r2->len = jl - jh; r2->dead = 0;
                    r2->orig = malloc(jl - jh); memcpy(r2->orig, mem + at, jl - jh);
                }
                reserve_next += jh + o + 1;
            }
            fb++;
        }
        if (t) fclose(t);
        if (fb_mismatch) fprintf(stderr, "english: %d rows of %s don't match %s (different release?) and were skipped\n", fb_mismatch, english_fallback, name);
    }
    if (!strcasecmp(name, "MW_MAIN.EXE")) {
        /* text routine of the strategy program: code segment 709h, entry at 0966h */
        text_cs = seg + 0x709;
        hook_text_entry = lin(text_cs, 0x0966);
        hook_text_fe = lin(text_cs, 0x09C6);
        /* the centred/right-aligned variants (table entries 957h, A56h, A64h, A72h) skip the entry
         * and join the routine at 971h with ES:DI on the string */
        hook_text_join = lin(text_cs, 0x0971);
        /* second text routine (one character at a time, used for headers, names and the opening) */
        hook_text2_entry = lin(text_cs, 0x04ED);
        hook_text2_call = lin(text_cs, 0x04FD);
    }
    if (!strcasecmp(name, "BTECH.EXE")) {
        /* combat engine string routine (code segment 16A2h): the string's far pointer sits at
         * [bp+14h] just before it is loaded at 228Dh */
        hook_bt_text = lin(seg + 0x16A2, 0x228D);
        patrol_lo = lin(seg + 0x1BF2, 0); patrol_hi = patrol_lo + 0x10000;
        bt_loaded_ms = emu_time_ms(); bt_swapped = 0;
        if (getenv("MW_BTCHAR")) hook_bt_char = lin(seg + 0x16A2, 0x2816);
    }
    logmsg("english: %s %d in place, %d redirected, %d from the fallback table", name, inplace, moved, fb);
    if (getenv("MW_EN_RANGES")) { FILE *rf = fopen("en_ranges.txt", "a"); for (int i = 0; i < ndone; i++) fprintf(rf, "%s %x %x\n", name, done_lo[i] - base, done_hi[i] - base); for (int i = 0; i < nredir; i++) fprintf(rf, "%s %x %x\n", name, redir[i].from - base, redir[i].from - base + redir[i].len); for (int i = 0; i < ninplace; i++) fprintf(rf, "%s %x %x\n", name, inplace_at[i] - base, inplace_at[i] - base + 1); fclose(rf); }
}

/* called before executing the instruction/block at cs:ip; returns 1 if it changed control flow */
/* find a redirect for the string at s: by address, or (for copies the game made into a buffer)
 * by its Japanese contents matching a redirected string's original bytes */
static Redirect *find_redirect(u32 s)
{
    for (int i = 0; i < nredir; i++) if (redir[i].from == s) return &redir[i];
    u8 c0 = rb(s); if (!c0) return NULL;
    for (int i = 0; i < nredir; i++) {
        Redirect *r = &redir[i];
        /* copies carry the text without the record's header bytes */
        size_t h0 = 0; while (h0 < r->len && r->orig[h0] < 0x20) h0++;
        if (r->len - h0 < 4 || r->orig[h0] != c0) continue;
        size_t k = h0; while (k < r->len && rb(s + k - h0) == r->orig[k]) k++;
        if (k != r->len) continue;
        k -= h0;
        /* fixed-width fields pad the copy with spaces (ASCII or full-width) */
        u32 q = s + k;
        for (int g = 0; g < 64; g++) {
            u8 c = rb(q);
            if (c == ' ') q++;
            else if (c == 0x81 && rb(q + 1) == 0x40) q += 2;
            else break;
        }
        if (rb(q) == 0) return r;
    }
    return NULL;
}

/* Composite lines built at run time out of several names ("mech name : pilot name"):
 * rebuild them with each known Japanese piece replaced by its English, in a scratch area,
 * and draw that instead. Returns the scratch address or 0 when nothing matched. */
#define SCRATCH (0xDF000)
static u32 compose(u32 s)
{
    u32 o = SCRATCH; int hits = 0;
    for (int guard = 0; guard < 300; guard++) {
        u8 c = rb(s); if (!c) break;
        size_t best = 0; Redirect *br = NULL;
        for (int i = 0; i < nredir; i++) {
            Redirect *r = &redir[i];
            size_t h0 = 0; while (h0 < r->len && r->orig[h0] < 0x20) h0++;
            size_t L = r->len - h0;
            if (L < 4 || L <= best || r->orig[h0] != c) continue;
            size_t k = 0; while (k < L && rb(s + k) == r->orig[h0 + k]) k++;
            if (k == L) { best = L; br = r; }
        }
        if (br) {
            u32 t = br->to; while (rb(t) && rb(t) < 0x20) t++;
            while (rb(t) && o < SCRATCH + 0x800) mem[o++] = rb(t++);
            s += best; hits++;
        } else {
            int two = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF);
            mem[o++] = c; s++; if (two) { mem[o++] = rb(s); s++; }
        }
        if (o >= SCRATCH + 0x7F0) break;
    }
    mem[o] = 0;
    return hits ? SCRATCH : 0;
}

static void decide_english(void)
{
    u32 s = lin(cpu.s[ES], cpu.r[DI]);
    { Redirect *r = find_redirect(s);
      if (getenv("MW_TXTDBG") && emu_time_ms() > atof(getenv("MW_TXTDBG"))) { fprintf(stderr, "find %05x -> %s", s, r ? "hit" : "miss"); for (int q = 0; q < 24; q++) fprintf(stderr, " %02x", rb(s + q)); fprintf(stderr, "\n"); }
      if (!r) { u32 c = compose(s); if (c) { cpu.s[ES] = c >> 4; cpu.r[DI] = c & 15; s = c; } }
      if (r) {
        /* the game fills some strings in at run time (numbers in templates): those stay as they are */
        if (!r->dead && r->from == s && memcmp(mem + r->from, r->orig, r->len)) { r->dead = 1; logmsg("english: %05x is written at run time, leaving it", r->from); }
        u32 to = r->to;
        if (r->from != s) { size_t h0 = 0; while (rb(to + h0) && rb(to + h0) < 0x20) h0++; to += h0; }
        if (!r->dead) { cpu.s[ES] = to >> 4; cpu.r[DI] = to & 15; s = to; }
      } }
    /* half-width for text with Latin letters and no Japanese in it: our English, and also
     * names the game copies into buffers before drawing. Strings mixing Japanese with
     * ASCII spaces/digits keep their full-width layout. */
    int letters = 0, sjis = 0, ctrl = 0, ours = 0;
    for (int i = 0; i < 512; i++) { u8 c = rb(s + i); if (!c) break;
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF)) { sjis = 1; break; }
        if (c < 0x20 && c != 0x0D) ctrl = 1;   /* escape codes take parameter bytes: leave those strings alone */
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') letters = 1; }
    for (int i = 0; i < ninplace && !ours; i++) if (inplace_at[i] == s) ours = 1;
    for (int i = 0; i < nredir && !ours; i++) if (redir[i].to == s) ours = 1;
    drawing_english = (letters && !sjis && !ctrl) || ours;
    if (getenv("MW_TXTDBG") && emu_time_ms() > atof(getenv("MW_TXTDBG"))) { char b[90]; int i; for (i = 0; i < 80; i++) { u8 c = rb(s + i); if (!c) break; b[i] = c >= 0x20 && c < 0x7f ? c : '.'; } b[i] = 0; fprintf(stderr, "draw %05x [%s] ret=%04x,%04x,%04x\n", s, b, rw(lin(cpu.s[SS], cpu.r[SP])), rw(lin(cpu.s[SS], cpu.r[SP] + 2)), rw(lin(cpu.s[SS], cpu.r[SP] + 4))); }
}

/* called before executing the instruction/block at cs:ip; returns 1 if it changed control flow */
int english_hook(u32 a)
{
    if (a == (u32)hook_bt_char) {
        static u32 last; u32 sp = lin(cpu.s[SS], cpu.r[SP]);
        u32 ret = (rw(sp + 2) << 16) | rw(sp);
        if (ret != last && emu_time_ms() > atof(getenv("MW_BTCHAR"))) { fprintf(stderr, "btchar dx=%04x from %04x:%04x si=%04x ds=%04x\n", cpu.r[DX], rw(sp + 2), rw(sp), cpu.r[SI], cpu.s[DS]); last = ret; }
        return 0;
    }
    if (a == (u32)hook_bt_text) {
        u32 pa = lin(cpu.s[SS], cpu.r[BP] + 0x14);
        u32 str = lin(rw(pa + 2), rw(pa));
        if (getenv("MW_TXTDBG") && emu_time_ms() > atof(getenv("MW_TXTDBG"))) { char b[40]; int i; for (i = 0; i < 32; i++) { u8 c = rb(str + i); if (!c) break; b[i] = c >= 0x20 && c < 0x7f ? c : '.'; } b[i] = 0; fprintf(stderr, "btdraw %05x [%s]\n", str, b); }
        { Redirect *r = find_redirect(str);
          if (r) {
            if (!r->dead && r->from == str && memcmp(mem + r->from, r->orig, r->len)) r->dead = 1;
            if (!r->dead) { ww(pa, r->to & 15); ww(pa + 2, r->to >> 4); }
          } }
        return 0;
    }
    if (a == (u32)hook_text_entry || a == (u32)hook_text2_entry || a == (u32)hook_text_join) { decide_english(); return 0; }
    if (a == (u32)hook_text_fe && !drawing_english) {
        /* some callers enter the loop without going through the routine's entry: decide from
         * where the current character comes from (inside a string we replaced or not) */
        u32 p1 = lin(cpu.s[ES], cpu.r[DI]) - 1, p2 = lin(cpu.s[DS], cpu.r[SI]) - 1;
        for (int i = 0; i < ndone && !drawing_english; i++)
            if ((p1 >= done_lo[i] && p1 < done_hi[i]) || (p2 >= done_lo[i] && p2 < done_hi[i])) drawing_english = 2;
    }
    if (a == (u32)hook_text_fe && drawing_english) {
        u8 cl = cpu.r[CX] & 0xFF;
        /* a Japanese character after one of our half-width ones must go back to full width */
        if (cl >= 0x81 && cl != 0xFE && cl != 0xFF) { wb(lin(cpu.s[DS], 0x965), 0); return 0; }
        if (cl >= 0x20 && cl <= 0x7A) {   /* ASCII in English text: draw half-width, as an FEh-prefixed character */
            wb(lin(cpu.s[DS], 0x965), 1);
            cpu.r[CX] = 0x2900 | cl;
            cpu.ip = 0x09EC;
            return 1;
        }
    }
    if (a == (u32)hook_text2_call && drawing_english && (cpu.r[CX] >> 8) != 0) {
        wb(lin(cpu.s[DS], 0x54F), 0);   /* back to full width for a Japanese character */
        return 0;
    }
    if (a == (u32)hook_text2_call && drawing_english && (cpu.r[CX] >> 8) == 0) {
        u8 cl = cpu.r[CX] & 0xFF;
        if (cl >= 0x20 && cl <= 0x7E) {   /* call the routine's half-width entry instead of the full-width one */
            cpu_push(0x0500);
            cpu.ip = 0x069E;
            return 1;
        }
    }
    return 0;
}
int english_hooks_active(void) { return hook_text_entry != 0 || hook_text2_entry != 0 || hook_bt_text != 0 || hook_bt_char != 0; }

/* The combat engine copies names (commander, pilots, mechs) out of the mission data into
 * working buffers and draws them from there with a character-at-a-time routine. The
 * originals must stay Japanese (the engine matches them against the mission data), but the
 * copies are only for display: every so often, find copies of redirected Japanese names in
 * its data segment and replace them with the English, padded to the same length. */
static int has_dot(const u8 *p, size_t n) { for (size_t i = 0; i + 1 < n; i++) if (p[i] == 0x81 && p[i + 1] == 0x45) return 1; return 0; }
/* The combat engine checks names in the mission data against its own name table while it
 * starts up, so both must stay Japanese until then. Once the check is done, every copy of
 * a known name anywhere in memory (its table, the mission data, working buffers) is
 * switched to English; all of them are switched back when the engine exits. */
typedef struct { u32 at; u16 len; u8 jp[32]; } NameSwap;
static NameSwap swaps[512]; static int nswaps;
static void bt_names(int english)
{
    if (!english) { for (int i = 0; i < nswaps; i++) memcpy(mem + swaps[i].at, swaps[i].jp, swaps[i].len); nswaps = 0; return; }
    for (int i = 0; i < nredir; i++) {
        Redirect *r = &redir[i];
        if (!has_dot(r->orig, r->len) && !r->name) continue;
        size_t hj = 0; while (hj < r->len && r->orig[hj] < 0x20) hj++;
        const u8 *jp = r->orig + hj; size_t jl = r->len - hj;
        if (jl < 4 || jl > 32) continue;
        const u8 *en = mem + r->to; size_t el = strnlen((const char *)en, 256), he = 0;
        while (he < el && en[he] < 0x20) he++;
        u8 *p = mem, *end = mem + 0xA0000 - jl;
        while (p < end && (p = memmem(p, end - p, jp, jl)) && nswaps < 512) {
            if (p[jl] == 0 || p[jl] == ' ') {
                NameSwap *w = &swaps[nswaps++]; w->at = p - mem; w->len = jl; memcpy(w->jp, p, jl);
                for (size_t k = 0; k < jl; k++) p[k] = he + k < el ? en[he + k] : ' ';
            }
            p += jl;
        }
    }
}
/* the combat engine has begun drawing text: its start-up checks are behind it */
void font_fix(u8 *b, long pos, size_t n);
static void font_fix_memory(void)
{
    /* the font may already be in memory (loaded before English was switched on, e.g. from a
     * save state): find the original glyph and replace it there too */
    char path[1200]; DIR *d = opendir(game_dir); struct dirent *e; path[0] = 0;
    while (d && (e = readdir(d))) if (!strcasecmp(e->d_name, "IWA.FNT")) snprintf(path, sizeof path, "%s/%s", game_dir, e->d_name);
    if (d) closedir(d);
    FILE *f = path[0] ? fopen(path, "rb") : NULL; u8 font[3072];
    if (!f) return;
    size_t n = fread(font, 1, sizeof font, f); fclose(f);
    if (n < 3072) return;
    const u8 *orig = font + ('?' - 0x20) * 32;
    u8 fixed[3072]; memcpy(fixed, font, 3072); font_fix(fixed, 0, 3072);
    u8 *p = mem, *end = mem + 0xA0000 - 32;
    while (p < end && (p = memmem(p, end - p, orig, 32))) { memcpy(p, fixed + ('?' - 0x20) * 32, 32); p += 32; }
}
void english_bt_drawing(void) { if (patrol_lo && english_dir && !bt_swapped && bt_loaded_ms >= 0) { bt_names(1); bt_swapped = 1; font_fix_memory(); } }
void english_program_exit(void) { if (bt_swapped) { bt_names(0); bt_swapped = 0; } }
void english_patrol(void)
{
    if (!patrol_lo || !english_dir) return;
    if (!bt_swapped && bt_loaded_ms >= 0 && emu_time_ms() - bt_loaded_ms > 600) { bt_names(1); bt_swapped = 1; }
    for (int i = 0; i < nredir; i++) {
        Redirect *r = &redir[i];
        if (r->len < 4) continue;
        const u8 *en = mem + r->to; size_t el = strnlen((const char *)en, 256);
        /* the copy holds the text without the record header */
        size_t hj = 0; while (hj < r->len && r->orig[hj] < 0x20) hj++;
        const u8 *jp = r->orig + hj; size_t jl = r->len - hj;
        size_t he = 0; while (he < el && en[he] < 0x20) he++;
        if (jl < 4) continue;
        u8 *p = mem + patrol_lo, *end = mem + patrol_hi - jl;
        while (p < end && (p = memmem(p, end - p, jp, jl))) {
            u32 at = p - mem;
            int is_orig = 0; for (int k = 0; k < nredir; k++) if (at >= redir[k].from && at < redir[k].from + redir[k].len) is_orig = 1;
            if (!is_orig && p[jl] == 0) {
                for (size_t k = 0; k < jl; k++) p[k] = he + k < el ? en[he + k] : ' ';
            }
            p += jl;
        }
    }
}

/* The combat engine's 16x16 Latin font (IWA.FNT) has a malformed question mark (the
 * Japanese text never used it). In English mode the glyph is replaced as the font is read. */
int font_fix_handle = -1;
void font_fix(u8 *b, long pos, size_t n)
{
    if (!english_dir) return;
    static const char *q[16] = {
        "................", "................", "....######......", "...###..###.....",
        "...###..###.....", "........###.....", ".......###......", "......###.......",
        "......###.......", "......###.......", "................", "......###.......",
        "......###.......", "................", "................", "................" };
    long g = ('?' - 0x20) * 32;
    for (int y = 0; y < 16; y++) for (int half = 0; half < 2; half++) {
        long o = g + y * 2 + half - pos;
        if (o < 0 || o >= (long)n) continue;
        u8 v = 0; for (int x = 0; x < 8; x++) if (q[y][half * 8 + x] == '#') v |= 0x80 >> x;
        b[o] = v;
    }
}
