/* Code coverage recorder: notes every instruction executed, per program
 * image, with the code segment it ran under (relative to the image's load
 * segment). The static recompiler uses this to find all code, including code
 * reached only through jump tables and function pointers. */
#include "machine.h"
#include "runtime.h"
#include "pc98.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

int trace_enabled;
#define IMG_MAX 0x40000
typedef struct Img { char name[32]; u32 base; u16 seg; u16 *hit; struct Img *parent; } Img;
static Img imgs[16]; static int nimg;
static Img *cur;

static Img *get(const char *name, u16 seg)
{
    const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
    if (strchr(b, ':')) b = strchr(b, ':') + 1;
    for (int i = 0; i < nimg; i++) if (!strcasecmp(imgs[i].name, b)) { imgs[i].base = seg * 16; imgs[i].seg = seg; return &imgs[i]; }
    Img *m = &imgs[nimg++];
    snprintf(m->name, sizeof m->name, "%s", b);
    for (char *c = m->name; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    m->base = seg * 16; m->seg = seg; m->hit = calloc(IMG_MAX, 2);
    char fn[300]; snprintf(fn, sizeof fn, "trace/%s/%s.cov", machine_pc98 ? "pc98" : "dos", m->name);
    FILE *f = fopen(fn, "rb"); if (f) { if (fread(m->hit, 2, IMG_MAX, f)) {} fclose(f); }
    return m;
}
void trace_note_program(const char *name, u16 seg) { if (trace_enabled) cur = get(name, seg); }
void trace_note_overlay(const char *name, u16 seg) { if (trace_enabled) get(name, seg)->parent = cur; }
void trace_exec(u16 cs, u16 ip)
{
    u32 a = lin(cs, ip);
    Img *best = NULL;
    /* overlays sit inside the current program's memory: pick the image with the highest base below a */
    for (int i = 0; i < nimg; i++) {
        Img *m = &imgs[i];
        if (m != cur && (!m->parent || m->parent != cur)) continue;
        if (a >= m->base && a - m->base < IMG_MAX && (!best || m->base > best->base)) best = m;
    }
    if (best) best->hit[a - best->base] = (u16)(cs - best->seg) + 1;
}
void trace_save(void)
{
    if (!trace_enabled) return;
    for (int i = 0; i < nimg; i++) {
        char fn[300]; snprintf(fn, sizeof fn, "trace/%s/%s.cov", machine_pc98 ? "pc98" : "dos", imgs[i].name);
        FILE *f = fopen(fn, "wb"); if (f) { fwrite(imgs[i].hit, 2, IMG_MAX, f); fclose(f); }
    }
}
