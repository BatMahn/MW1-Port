/* PC-98 floppy images: unpack the FAT12 file systems of the game disks into a folder
 * the first time the PC-98 edition is started (the game then runs from that folder,
 * which is also where it keeps its saved games).
 * Formats: FDI (Anex86, 4 KB header) and raw HDM (1.2 MB, 1024-byte sectors). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

typedef struct { unsigned char *d; long len; unsigned bps, spc, res, nfat, nroot, spf, tot; long fat, root, data; } Vol;

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned fat12(Vol *v, unsigned c)
{
    const unsigned char *f = v->d + v->fat; unsigned o = c + c / 2;
    unsigned w = f[o] | f[o + 1] << 8;
    return (c & 1) ? w >> 4 : w & 0xFFF;
}

static int mkdirs(const char *path)
{
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; MKDIR(tmp); *p = '/'; }
    MKDIR(tmp);
    return 0;
}

static void walk(Vol *v, long dir_off, int nentries, unsigned dir_cluster, const char *outdir, int *count)
{
    /* a subdirectory is a cluster chain; the root is a fixed area */
    unsigned clus = dir_cluster;
    for (;;) {
        long off = dir_cluster ? v->data + (long)(clus - 2) * v->spc * v->bps : dir_off;
        int n = dir_cluster ? (int)(v->spc * v->bps / 32) : nentries;
        for (int i = 0; i < n; i++) {
            const unsigned char *e = v->d + off + i * 32;
            if (off + i * 32 + 32 > v->len || e[0] == 0) return;
            if (e[0] == 0xE5 || e[0] == '.' || (e[11] & 0x08) || e[11] == 0x0F) continue;
            char name[13]; int k = 0;
            for (int j = 0; j < 8 && e[j] != ' '; j++) name[k++] = e[j];
            if (e[8] != ' ') { name[k++] = '.'; for (int j = 8; j < 11 && e[j] != ' '; j++) name[k++] = e[j]; }
            name[k] = 0;
            for (char *c = name; *c; c++) *c = toupper((unsigned char)*c);
            char path[1200]; snprintf(path, sizeof path, "%s/%s", outdir, name);
            unsigned first = rd16(e + 26); unsigned long size = e[28] | e[29] << 8 | (unsigned long)e[30] << 16 | (unsigned long)e[31] << 24;
            if (e[11] & 0x10) { mkdirs(path); walk(v, 0, 0, first, path, count); continue; }
            FILE *f = fopen(path, "wb"); if (!f) continue;
            unsigned c = first; unsigned long left = size; unsigned csize = v->spc * v->bps;
            while (left && c >= 2 && c < 0xFF0) {
                long co = v->data + (long)(c - 2) * csize;
                unsigned long take = left < csize ? left : csize;
                if (co + (long)take > v->len) break;
                fwrite(v->d + co, 1, take, f); left -= take;
                c = fat12(v, c);
            }
            fclose(f); (*count)++;
        }
        if (!dir_cluster) return;
        clus = fat12(v, clus);
        if (clus < 2 || clus >= 0xFF0) return;
    }
}

/* returns the number of files written, or -1 if the image could not be read */
int fdi_extract(const char *image, const char *outdir)
{
    FILE *f = fopen(image, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(len); if (!buf || fread(buf, 1, len, f) != (size_t)len) { fclose(f); free(buf); return -1; }
    fclose(f);
    long hdr = 0;
    /* FDI: dword 0 is zero, dword 2 the header size, dword 3 the data size */
    if (len > 32 && rd16(buf) == 0 && rd16(buf + 2) == 0) {
        long hs = buf[8] | buf[9] << 8 | (long)buf[10] << 16, ds = buf[12] | buf[13] << 8 | (long)buf[14] << 16;
        if (hs > 0 && hs + ds <= len) hdr = hs;
    }
    Vol v = { 0 }; v.d = buf + hdr; v.len = len - hdr;
    const unsigned char *b = v.d;
    v.bps = rd16(b + 11); v.spc = b[13]; v.res = rd16(b + 14); v.nfat = b[16]; v.nroot = rd16(b + 17); v.tot = rd16(b + 19); v.spf = rd16(b + 22);
    if (v.bps != 512 && v.bps != 1024) { v.bps = 1024; v.spc = 1; v.res = 1; v.nfat = 2; v.nroot = 192; v.spf = 2; }   /* 2HD default */
    if (!v.spc) v.spc = 1;
    v.fat = (long)v.res * v.bps; v.root = v.fat + (long)v.nfat * v.spf * v.bps; v.data = v.root + (long)v.nroot * 32;
    mkdirs(outdir);
    int count = 0;
    walk(&v, v.root, v.nroot, 0, outdir, &count);
    free(buf);
    return count;
}
