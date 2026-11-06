#include "compat.h"
/* MechWarrior (1989) native port - entry point, SDL front end and main loop. */
#include "machine.h"
#include "runtime.h"
#include "pc98.h"
#include <unistd.h>
#include <strings.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

int log_enabled;
u32 watch_addr = 0xFFFFFFFF;
void watch_hit(u32 a, u8 v) { fprintf(stderr, "[%9.1f] write %05x <- %02x at %04x:%04x si=%04x di=%04x ds=%04x es=%04x\n", emu_time_ms(), a, v, cpu.s[CS], cpu.ip, cpu.r[SI], cpu.r[DI], cpu.s[DS], cpu.s[ES]); }
void bios_init(void);
void runtime_irq_service(void);
double next_irq_ms(void);
void cpu_step(void);
void rc_step(void); void rc_report(void); extern int rc_enabled, rc_verify;
extern int audio_device;
void mouse_host_motion(int dx, int dy);
void mouse_host_button(int b, int down);

static int quit_flag, quit_code;
static SDL_Window *win; static SDL_Renderer *ren; static SDL_Texture *tex;
static int headless;
static const char *save_state_file, *load_state_file; static double save_after = -1;
static char anchor_file[64]; static double anchor_t = -1;   /* --anchor FILE: later --key times count from when the game opens FILE */
/* fast-forward: --skip-intro runs the PC-98 opening sequence unthrottled and
 * silent until the game loads its first non-opening picture; holding F12
 * fast-forwards anywhere. Game timing is unaffected (it's emulated time). */
static int skip_intro, intro_turbo, key_turbo;
int audio_muted;
void intro_file_opened(const char *p)
{
    if (anchor_file[0] && anchor_t < 0 && strcasestr(p, anchor_file)) { anchor_t = emu_time_ms(); logmsg("anchor %s at %.0f", anchor_file, anchor_t); }
    const char *b = strrchr(p, '\\'); b = b ? b + 1 : p;
    int opening = !strncasecmp(b, "OPENLAY", 7) || !strncasecmp(b, "OP_", 3) || !strncasecmp(b, "VIC", 3);
    if (skip_intro && opening && !intro_turbo) { intro_turbo = 1; logmsg("skipping the opening"); }
    else if (intro_turbo && !opening) intro_turbo = 0;
}
static int getenv_dbg;
extern int idle_hint; int64_t idle_skipped; static int no_idle;
int ring_on; u32 ring[4096]; unsigned ring_i;
static char dos_tail[128];
static Uint64 t0;

void fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); fprintf(stderr, "FATAL: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
    trace_save(); exit(1);
}
void logmsg(const char *fmt, ...)
{
    if (!log_enabled) return;
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[%9.1f] ", emu_time_ms()); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
}
void runtime_quit(int code) { quit_flag = 1; quit_code = code; }
u32 runtime_ms(void) { return (u32)emu_time_ms(); }

/* SDL scancode -> PC XT set-1 scancode (bit 8 = E0 prefix) */
static int pc_scancode(SDL_Scancode s)
{
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const u8 t[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
        return t[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return 0x02 + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return 0x3B + (s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_BACKSLASH: return 0x2B; case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x1D; case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x38;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_7: case SDL_SCANCODE_HOME: return 0x47; case SDL_SCANCODE_KP_8: case SDL_SCANCODE_UP: return 0x48;
    case SDL_SCANCODE_KP_9: case SDL_SCANCODE_PAGEUP: return 0x49; case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_KP_4: case SDL_SCANCODE_LEFT: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: case SDL_SCANCODE_RIGHT: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_KP_1: case SDL_SCANCODE_END: return 0x4F; case SDL_SCANCODE_KP_2: case SDL_SCANCODE_DOWN: return 0x50;
    case SDL_SCANCODE_KP_3: case SDL_SCANCODE_PAGEDOWN: return 0x51; case SDL_SCANCODE_KP_0: case SDL_SCANCODE_INSERT: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: case SDL_SCANCODE_DELETE: return 0x53; case SDL_SCANCODE_KP_ENTER: return 0x1C;
    default: return 0;
    }
}

/* headless test harness: scripted keys "t_ms:scancode" and screenshots */
typedef struct { double t; int sc; int down; int dx, dy; int rel; } KeyEv;
static KeyEv script[1024]; static int nscript, iscript;
static double shots[64]; static int shots_rel[64]; static int nshots, ishot;

static void write_ppm(const char *fn)
{
    if (machine_pc98) {
        pc98_render();
        FILE *f = fopen(fn, "wb"); if (!f) return;
        fprintf(f, "P6 640 400 255\n");
        for (int i = 0; i < 640 * 400; i++) { u32 p = fb98[i]; fputc(p >> 16, f); fputc(p >> 8, f); fputc(p, f); }
        fclose(f); return;
    }
    video_render();
    FILE *f = fopen(fn, "wb"); if (!f) return;
    int w = fb_width; fprintf(f, "P6 %d %d 255\n", w, FB_H);
    for (int y = 0; y < FB_H; y++) for (int x = 0; x < w; x++) { u32 p = framebuffer[y * FB_W + x]; fputc(p >> 16, f); fputc(p >> 8, f); fputc(p, f); }
    fclose(f);
}

static SDL_Texture *tex98;
static void present(void)
{
    if (machine_pc98) {
        pc98_render();
        if (!tex98) tex98 = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 400);
        SDL_UpdateTexture(tex98, NULL, fb98, 640 * 4);
        SDL_RenderClear(ren); SDL_RenderCopy(ren, tex98, NULL, NULL); SDL_RenderPresent(ren);
        return;
    }
    video_render();
    SDL_UpdateTexture(tex, NULL, framebuffer, FB_W * 4);
    SDL_Rect src = { 0, 0, fb_width, FB_H };
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, &src, NULL);
    SDL_RenderPresent(ren);
}


#include "snap.h"
static void state_io(const char *fn, int save)
{
    extern const char *english_dir;
    FILE *f = fopen(fn, save ? "wb" : "rb");
    if (!f) { fprintf(stderr, "can't open state %s\n", fn); return; }
    int eng = english_dir != NULL; S(eng);
    if (!save && eng) fprintf(stderr, "warning: state was saved with --english; save states in Japanese mode\n");
    S(cpu); S(cycles_done);
    if (save) fwrite(mem, 1, MEM_SIZE, f); else if (fread(mem, 1, MEM_SIZE, f) != MEM_SIZE) fprintf(stderr, "short state\n");
    hw_snap(f, save); pc98_snap(f, save); dos_snap(f, save); audio_snap(f, save); rc_snap(f, save);
    fclose(f);
    fprintf(stderr, "%s state %s at %.0f ms\n", save ? "saved" : "loaded", fn, emu_time_ms());
}

/* ---- folder layout -------------------------------------------------------------------
 * The program looks for its data next to itself (or under --data DIR):
 *   DOS/   files of the DOS MechWarrior (English text source; also playable with --dos)
 *   PC98/  the PC-98 disk images (BTECHSYS.FDI, BTECHGAM.FDI, BTECHDEM.FDI) or their files
 *   MT32/  Roland MT-32 control and PCM ROMs (optional, DOS edition)
 *   mechwarrior_pc98_translation.tsv  (optional second translation source)
 * textmap/ and fonts/ ship with the program. */
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#define CHDIR _chdir
#else
#define CHDIR chdir
#endif
int fdi_extract(const char *image, const char *outdir);
static int find_ci(const char *dir, const char *name, char *out, size_t n)
{
    DIR *d = opendir(dir); struct dirent *e; int ok = 0;
    while (d && (e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    if (d) closedir(d);
    return ok;
}
static int has_file(const char *dir, const char *name) { char p[1200]; return find_ci(dir, name, p, sizeof p); }
static char lay_dos[1100], lay_pc98[1100], lay_mt32[1100], lay_tsv[1200], lay_pc98_files[1200];
static int setup_layout(const char *data_root, int game_dir_given, int want_dos, int japanese)
{
    char base[1024];
    if (data_root) snprintf(base, sizeof base, "%s", data_root);
    else { char *b = SDL_GetBasePath(); snprintf(base, sizeof base, "%s", b ? b : "."); if (b) SDL_free(b); }
    size_t bl = strlen(base); while (bl > 1 && (base[bl - 1] == '/' || base[bl - 1] == '\\')) base[--bl] = 0;
    /* inside a macOS app bundle the program's own files (textmap/, fonts/) are in
     * Contents/Resources and the game data folders sit next to the .app */
    char res[1024]; snprintf(res, sizeof res, "%s", base);
    if (!data_root) { char *app = strstr(base, ".app/Contents");
        if (app) { *app = 0; char *sl = strrchr(base, '/'); if (sl && sl != base) *sl = 0; else snprintf(base, sizeof base, "."); } }
    /* normal play runs from the program's folder (textmap/, fonts/, saved data); with an explicit
     * --dir the current folder is kept (development runs) */
    if (!game_dir_given && CHDIR(res) != 0) fprintf(stderr, "can't enter %s\n", res);
    int dos_folder = find_ci(base, "DOS", lay_dos, sizeof lay_dos);
    int have_dos = dos_folder && has_file(lay_dos, "MW.EXE");                                   /* playable */
    int have_dos_text = dos_folder && has_file(lay_dos, "MW_MAIN.EXE") && has_file(lay_dos, "BTECH.EXE");   /* English source */
    int have_pc98 = find_ci(base, "PC98", lay_pc98, sizeof lay_pc98);
    if (find_ci(base, "MT32", lay_mt32, sizeof lay_mt32)) { extern const char *mt32_dir; mt32_dir = lay_mt32; }
    if (!find_ci(base, "mechwarrior_pc98_translation.tsv", lay_tsv, sizeof lay_tsv) && have_pc98)
        find_ci(lay_pc98, "mechwarrior_pc98_translation.tsv", lay_tsv, sizeof lay_tsv);
    if (game_dir_given) return 1;
    if (have_pc98 && !want_dos) {
        if (has_file(lay_pc98, "IO98.SYS")) snprintf(game_dir, sizeof game_dir, "%s", lay_pc98);
        else {
            snprintf(lay_pc98_files, sizeof lay_pc98_files, "%s/files", lay_pc98);
            if (!has_file(lay_pc98_files, "IO98.SYS")) {
                /* unpack every disk image found, in name order (system disk, game disk, demo disk) */
                DIR *d = opendir(lay_pc98); struct dirent *e; char names[16][256]; int n = 0;
                while (d && (e = readdir(d)) && n < 16) {
                    const char *x = strrchr(e->d_name, '.');
                    if (x && (!strcasecmp(x, ".fdi") || !strcasecmp(x, ".hdm"))) snprintf(names[n++], 256, "%s", e->d_name);
                }
                if (d) closedir(d);
                for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (strcasecmp(names[b], names[a]) > 0 ? 0 : 1) { char t[256]; memcpy(t, names[a], 256); memcpy(names[a], names[b], 256); memcpy(names[b], t, 256); }
                for (int a = 0; a < n; a++) {
                    char img[1400]; snprintf(img, sizeof img, "%s/%s", lay_pc98, names[a]);
                    int c = fdi_extract(img, lay_pc98_files);
                    fprintf(stderr, "unpacked %s: %d files\n", names[a], c);
                }
            }
            if (!has_file(lay_pc98_files, "IO98.SYS")) { fprintf(stderr, "PC98/ has no usable disk images (see README: folder setup)\n"); if (!have_dos) return 0; goto dos; }
            snprintf(game_dir, sizeof game_dir, "%s", lay_pc98_files);
        }
        if (!japanese && have_dos_text) {
            /* the text maps match one DOS release: say so if this copy differs */
            const char *chk[2] = { "MW_MAIN.EXE", "BTECH.EXE" }; long want[2] = { 137871, 174533 };
            for (int k = 0; k < 2; k++) { char p[1200]; if (find_ci(lay_dos, chk[k], p, sizeof p)) { FILE *f = fopen(p, "rb"); if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
                if (sz != want[k]) fprintf(stderr, "warning: DOS/%s is %ld bytes, the English maps expect %ld; text may be wrong\n", chk[k], sz, want[k]); } } }
            extern const char *english_dir, *english_fallback;
            if (!english_dir) english_dir = lay_dos;
            /* second English source: a table you supply, else the one that ships in textmap/ */
            if (!english_fallback) english_fallback = lay_tsv[0] ? lay_tsv : "textmap/pc98_english.tsv";
        }
        return 1;
    }
dos:
    if (have_dos) {
        /* the files of the original install (see README); report any that are missing */
        static const char *need[] = { "MW.EXE", "MW_MAIN.EXE", "BTECH.EXE", "MW_EGA.EXE", "MW_TANDY.EXE", "MW_CPICS.EXE",
            "MW_PICS.BIN", "MW_1PICS.BIN", "MW_2PICS.BIN", "MW_APICS.BIN", "MW_CPICS.BIN", "MW_FPICS.BIN", "MW_GPICS.BIN", "MW_TPICS.BIN",
            "6X6.FNT", "6X6B.FNT", "8X8B.FNT", "FOX88.FNT", "DEATH.MUS", "LAUNCH.MUS", "MECH.MUS", "MECHBAR.MUS", "VICTORY.MUS", "BTECHSND.SND",
            "ARCTIC.PAL", "DESERT.PAL", "TROPIC.PAL", "DMGPAL.PAL", "BAT.BMP", "COCKPIT.BMP", "DIGITS.BMP", "DOTS.BMP", "HUD_CYAN.BMP", "HUD_NUMS.BMP",
            "JEN.BMP", "LOC.BMP", "MAR.BMP", "PHO.BMP", "POINT.BMP", "PROMPT.BMP", "RIF.BMP", "SHA.BMP", "SM_MECHS.BMP", "STRUTS.BMP", "WAR.BMP",
            "HEAVY.SCR", "LIGHT.SCR", "MEDIUM.SCR", "MAP.SCR", "STATUS.SCR", "BMAPCK.TBL", "HAMPCK.TBL", "JENPCK.TBL", "LOCPCK.TBL", "MARPCK.TBL",
            "OTHPCK.TBL", "PHAPCK.TBL", "RIFPCK.TBL", "SHAPCK.TBL", "TERPCK.TBL", "TERPCK.GI", "SNARIO.DAT", NULL };
        int missing = 0;
        for (int i = 0; need[i]; i++) if (!has_file(lay_dos, need[i])) { fprintf(stderr, "%s %s", missing++ ? "," : "DOS/ is missing:", need[i]); }
        for (int t = 0; t < 16; t++) for (int b = 0; b < 2; b++) for (int k = 0; k < 2; k++) {
            char n[16]; snprintf(n, sizeof n, "TILE%d%s.%s", t, b ? "B" : "", k ? "WLD" : "GRD");
            if (!has_file(lay_dos, n)) fprintf(stderr, "%s %s", missing++ ? "," : "DOS/ is missing:", n);
        }
        if (missing) fprintf(stderr, "\n");
        snprintf(game_dir, sizeof game_dir, "%s", lay_dos); return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    double run_for = 0; const char *data_root = NULL; int game_dir_given = 0, want_dos = 0, japanese = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) { snprintf(game_dir, sizeof game_dir, "%s", argv[++i]); game_dir_given = 1; }
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data_root = argv[++i];
        else if (!strcmp(argv[i], "--dos")) want_dos = 1;
        else if (!strcmp(argv[i], "--japanese")) japanese = 1;
        else if (!strcmp(argv[i], "--mt32") && i + 1 < argc) { extern const char *mt32_dir; mt32_dir = argv[++i]; }
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles_per_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log")) log_enabled++;
        else if (!strcmp(argv[i], "--ring")) ring_on = 1;
        else if (!strcmp(argv[i], "--interp")) rc_enabled = 0;
        else if (!strcmp(argv[i], "--no-idle")) no_idle = 1;
        else if (!strcmp(argv[i], "--skip-intro")) skip_intro = 1;
        else if (!strcmp(argv[i], "--english") && i + 1 < argc) { extern const char *english_dir; english_dir = argv[++i]; }
        else if (!strcmp(argv[i], "--english-fallback") && i + 1 < argc) { extern const char *english_fallback; english_fallback = argv[++i]; }
        else if (!strcmp(argv[i], "--english-conf") && i + 1 < argc) { extern double english_min_conf; english_min_conf = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) { extern const char *font98_path; font98_path = argv[++i]; }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { extern FILE *wav_out; wav_out = fopen(argv[++i], "wb"); }
        else if (!strcmp(argv[i], "--verify")) rc_verify = 1;
        else if (!strcmp(argv[i], "--misses")) { extern int rc_misslog; rc_misslog = 1; }
        else if (!strcmp(argv[i], "--trace")) trace_enabled = 1;
        else if (!strcmp(argv[i], "--headless") && i + 1 < argc) { headless = 1; run_for = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--save-state") && i + 2 < argc) { save_state_file = argv[++i]; save_after = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) load_state_file = argv[++i];
        else if (!strcmp(argv[i], "--anchor") && i + 1 < argc) snprintf(anchor_file, sizeof anchor_file, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) { double t, h = 0; int sc; if (sscanf(argv[++i], "%lf:%x:%lf", &t, &sc, &h) >= 2) {
                script[nscript].rel = anchor_file[0] != 0; script[nscript].t = t; script[nscript].sc = sc; script[nscript++].down = 1;
                script[nscript].rel = anchor_file[0] != 0; script[nscript].t = t + (h > 0 ? h : 0.01); script[nscript].sc = sc; script[nscript++].down = 0; } }
        else if (!strcmp(argv[i], "--poke16") && i + 1 < argc) { double t; unsigned a, v; if (sscanf(argv[++i], "%lf:%x:%x", &t, &a, &v) == 3) { script[nscript].rel = anchor_file[0] != 0; script[nscript].t = t; script[nscript].sc = -3; script[nscript].dx = a; script[nscript++].dy = v; } }
        else if (!strcmp(argv[i], "--mouse") && i + 1 < argc) { double t; int dx, dy, b; if (sscanf(argv[++i], "%lf:%d:%d:%d", &t, &dx, &dy, &b) == 4) {
                /* move by dx,dy pixels (in 50 small steps), then optionally click button b-1 */
                for (int k = 0; k < 50; k++) { script[nscript].t = t + k * 10; script[nscript].sc = -1; script[nscript].dx = dx / 50 + (k < abs(dx) % 50 ? (dx > 0 ? 1 : -1) : 0); script[nscript++].dy = dy / 50 + (k < abs(dy) % 50 ? (dy > 0 ? 1 : -1) : 0); }
                if (b) { script[nscript].t = t + 600; script[nscript].sc = -2; script[nscript].dx = b - 1; script[nscript++].down = 1;
                         script[nscript].t = t + 700; script[nscript].sc = -2; script[nscript].dx = b - 1; script[nscript++].down = 0; } } }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) { shots_rel[nshots] = anchor_file[0] != 0; shots[nshots++] = atof(argv[++i]); }
        else if (argv[i][0] != '-') { size_t l = strlen(dos_tail); snprintf(dos_tail + l, sizeof dos_tail - l, " %s", argv[i]); }
        else {
            fprintf(stderr,
                "usage: %s [options] [S|T|A|R]\n"
                "  (no options)      PC-98 edition in English if PC98/ and DOS/ are present, else the DOS edition\n"
                "  --dos             play the DOS edition (sound: S speaker, T Tandy, A AdLib, R Roland MT-32)\n"
                "  --japanese        PC-98 edition with its original Japanese text\n"
                "  --skip-intro      fast-forward the PC-98 opening (F12 fast-forwards anywhere)\n"
                "  --data DIR        look for DOS/, PC98/, MT32/ in DIR instead of next to the program\n"
                "  --mt32 DIR        folder with the MT-32 ROMs (default MT32/)\n"
                "  --cycles N        emulated CPU speed (default 30000)\n"
                "  Alt+Enter toggles fullscreen.\n", argv[0]);
            return 1;
        }
    }
    for (int a = 0; a < nscript; a++) for (int b = a + 1; b < nscript; b++) if (script[b].t < script[a].t) { KeyEv x = script[a]; script[a] = script[b]; script[b] = x; }
    /* paths given on the command line are taken relative to where we were started */
    { char cwd[1024]; if (getcwd(cwd, sizeof cwd)) {
        if (game_dir_given && game_dir[0] != '/' && !(game_dir[0] && game_dir[1] == ':')) { char t[1024]; snprintf(t, sizeof t, "%s/%s", cwd, game_dir); snprintf(game_dir, sizeof game_dir, "%s", t); }
        static char abs_eng[1200], abs_fb[1200], abs_root[1200];
        extern const char *english_dir, *english_fallback;
        if (english_dir && english_dir[0] != '/') { snprintf(abs_eng, sizeof abs_eng, "%s/%s", cwd, english_dir); english_dir = abs_eng; }
        if (english_fallback && english_fallback[0] != '/') { snprintf(abs_fb, sizeof abs_fb, "%s/%s", cwd, english_fallback); english_fallback = abs_fb; }
        if (data_root && data_root[0] != '/') { snprintf(abs_root, sizeof abs_root, "%s/%s", cwd, data_root); data_root = abs_root; }
        if (load_state_file && load_state_file[0] != '/') { static char a[1200]; snprintf(a, sizeof a, "%s/%s", cwd, load_state_file); load_state_file = a; }
        if (save_state_file && save_state_file[0] != '/') { static char a[1200]; snprintf(a, sizeof a, "%s/%s", cwd, save_state_file); save_state_file = a; }
    } }
    if (!setup_layout(data_root, game_dir_given, want_dos, japanese))
        fatal("no game data found next to the program: put the DOS files in DOS/ and/or the PC-98 disk images in PC98/ (see README)");
    if (headless) { SDL_setenv("SDL_VIDEODRIVER", "dummy", 1); SDL_setenv("SDL_AUDIODRIVER", "dummy", 1); }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) fatal("SDL_Init: %s", SDL_GetError());
    if (!headless) {
        win = SDL_CreateWindow("MechWarrior", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(ren, 960, 720);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
        { /* window icon (the .app / .exe icons are set by the platform packaging) */
          #include "icon_rgba.h"
          SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)mw_icon_rgba, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32);
          if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); } }
    }
    { char p[1200]; snprintf(p, sizeof p, "%s/IO98.SYS", game_dir); if (access(p, F_OK) == 0) machine_pc98 = 1; }
    getenv_dbg = getenv("MW_DBG") != NULL;
    if (getenv("MW_WATCH")) watch_addr = strtoul(getenv("MW_WATCH"), NULL, 16);
    audio_init();
    if (!headless) SDL_SetRelativeMouseMode(SDL_TRUE);
    cpu_reset();
    bios_init();
    video_init();
    dos_init();
    /* DOS edition: the game's own default is the Roland MT-32; without MT-32 ROMs use AdLib
     * unless a sound device was asked for on the command line */
    if (!machine_pc98) {
        int given = 0; for (const char *c = dos_tail; *c; c++) if (strchr("STARstar", *c) && (c == dos_tail || c[-1] == ' ') && (c[1] == 0 || c[1] == ' ')) given = 1;
        extern int mt32_active(void);
        if (!given && !mt32_active()) { size_t l = strlen(dos_tail); snprintf(dos_tail + l, sizeof dos_tail - l, " A"); }
    }
    if (dos_exec("C:\\MW.EXE", dos_tail, 0)) fatal("could not load MW.EXE from %s", game_dir);
    if (load_state_file) { state_io(load_state_file, 0); if (anchor_file[0]) anchor_t = emu_time_ms(); }
    /* testing aid: MW_POKE32=addr:value[,addr:value...] writes 32-bit values after start-up / state load */
    if (getenv("MW_POKE32")) { char buf[256]; snprintf(buf, sizeof buf, "%s", getenv("MW_POKE32"));
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) { unsigned a, v; if (sscanf(t, "%x:%u", &a, &v) == 2) { mem[a] = v; mem[a + 1] = v >> 8; mem[a + 2] = v >> 16; mem[a + 3] = v >> 24; } } }

    t0 = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    double next_frame = 0;
    int64_t next_hw = 0;
    while (!quit_flag) {
        /* run CPU until the next millisecond boundary of emulated time */
        int64_t target = cycles_done + cycles_per_ms;
        while (cycles_done < target && !quit_flag) {
            if (cycles_done >= next_hw) { runtime_irq_service(); next_hw = cycles_done + 32; }
            if (cpu.halted) { cycles_done += 32; continue; }
            if (ring_on) { ring[ring_i++ & 4095] = ((u32)cpu.s[CS] << 16) | cpu.ip; }
            { static u32 brk[8]; static int nbrk = -1; if (nbrk < 0) { nbrk = 0; const char *e = getenv("MW_BREAK"); while (e && *e && nbrk < 8) { brk[nbrk++] = strtoul(e, (char **)&e, 16); if (*e == ',') e++; } }
              for (int b = 0; b < nbrk; b++) if (lin(cpu.s[CS], cpu.ip) == brk[b]) fprintf(stderr, "[%9.1f] break %05x ax=%04x bx=%04x\n", emu_time_ms(), brk[b], cpu.r[AX], cpu.r[BX]); }
            if (getenv_dbg && cpu.s[CS] == 0x258 && (cpu.ip == 0x4588 || cpu.ip == 0x4592 || cpu.ip == 0x45b6 || cpu.ip == 0x45c2 || cpu.ip == 0x45d9)) logmsg("FCOM %04x flag717=%02x sp=%04x", cpu.ip, mem[lin(0x258, 0x717)], cpu.r[SP]);
            rc_step();
            if (idle_hint && !no_idle) {
                extern int idle_hint; idle_hint = 0;
                if (getenv("MW_IDLEDBG")) { static int n; if (n++ % 2000 == 0) fprintf(stderr, "idle at %04x:%04x\n", cpu.s[CS], cpu.ip); }
                int64_t irq = (int64_t)(next_irq_ms() * cycles_per_ms) + 1;
                int64_t to = irq < target ? irq : target;
                if (to > cycles_done) { idle_skipped += to - cycles_done; cycles_done = to; }
            }
        }
        double t = emu_time_ms();
        audio_advance(t);
        { extern void english_patrol(void); static double pt; if (t - pt > 50) { pt = t; english_patrol(); } }
        if (save_state_file && anchor_t >= 0 && t >= anchor_t + save_after) { state_io(save_state_file, 1); save_state_file = NULL; }
        if (trace_enabled) { static double ts; if (t - ts > 30000) { ts = t; trace_save(); } }
        { extern int video_flips; static double last; static int lf; if (getenv("MW_FPS") && t - last >= 5000) { fprintf(stderr, "[%.0f] flips/s %.1f\n", t, (video_flips - lf) / ((t - last) / 1000)); last = t; lf = video_flips; } }
        while (!headless && iscript < nscript && (script[iscript].rel ? (anchor_t >= 0 && t >= anchor_t + script[iscript].t) : t >= script[iscript].t)) {
            KeyEv *e = &script[iscript++];
            if (e->sc == -1) mouse_host_motion(e->dx, e->dy);
            else if (e->sc == -2) mouse_host_button(e->dx, e->down);
            else if (e->sc == -3) { mem[(u32)e->dx] = e->dy; mem[(u32)e->dx + 1] = e->dy >> 8; }   /* testing aid: timed 16-bit poke */
            else kbd_host_event(e->sc & 0x37F, e->down, 0);
        }
        if (headless) {
            while (iscript < nscript && (script[iscript].rel ? (anchor_t >= 0 && t >= anchor_t + script[iscript].t) : t >= script[iscript].t)) {
                KeyEv *e = &script[iscript++];
                if (e->sc == -1) mouse_host_motion(e->dx, e->dy);
                else if (e->sc == -2) mouse_host_button(e->dx, e->down);
                else if (e->sc == -3) { mem[(u32)e->dx] = e->dy; mem[(u32)e->dx + 1] = e->dy >> 8; }
            else if (e->sc == -3) { mem[(u32)e->dx] = e->dy; mem[(u32)e->dx + 1] = e->dy >> 8; }   /* testing aid: timed 16-bit poke */
                else kbd_host_event(e->sc & 0x37F, e->down, 0);
            }
            while (ishot < nshots && (shots_rel[ishot] ? (anchor_t >= 0 && t >= anchor_t + shots[ishot]) : t >= shots[ishot])) { char fn[64]; snprintf(fn, sizeof fn, "shot_%05.0f.ppm", shots[ishot]); write_ppm(fn); ishot++; }
            if (anchor_file[0] ? (anchor_t >= 0 && t >= anchor_t + run_for) : t >= run_for) break;
            continue;
        }
        int turbo = intro_turbo || key_turbo;
        audio_muted = turbo;
        if (t >= next_frame) {
            next_frame = t + (turbo ? 500.0 : 1000.0 / 60.0);
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) quit_flag = 1;
                else if (e.type == SDL_MOUSEMOTION) mouse_host_motion(e.motion.xrel, e.motion.yrel);
                else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) mouse_host_button(e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2, e.type == SDL_MOUSEBUTTONDOWN);
                else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                    if (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT) && e.type == SDL_KEYDOWN) {
                        SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                        continue;
                    }
                    if (e.key.keysym.scancode == SDL_SCANCODE_F12) { key_turbo = e.type == SDL_KEYDOWN; continue; }
                    int sc = pc_scancode(e.key.keysym.scancode);
                    if (sc) kbd_host_event(sc, e.type == SDL_KEYDOWN, 0);
                }
            }
            present();
        }
        /* pace emulated time to wall-clock time: sleep instead of spinning */
        double wall = (SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
        if (turbo) { t0 = SDL_GetPerformanceCounter() - (Uint64)(t * freq / 1000.0); continue; }
        if (t > wall + 1.0) SDL_Delay((Uint32)(t - wall));
        else if (wall - t > 250) { /* host stalled: don't try to catch up in a burst */
            t0 = SDL_GetPerformanceCounter() - (Uint64)(t * freq / 1000.0);
        }
    }
    if (headless) write_ppm("shot_exit.ppm");
    if (headless && machine_pc98) { FILE *f = fopen("tvram.bin", "wb"); fwrite(mem + 0xA0000, 1, 0x4000, f); fclose(f); }
    if (getenv("MW_DUMP")) { FILE *f = fopen("mem.bin", "wb"); fwrite(mem, 1, 0xA0000, f); fclose(f); }
    { void pic_report(void); if (headless) pic_report(); }
    if (log_enabled || rc_verify || headless) { rc_report(); fprintf(stderr, "idle-skipped %.1f%% of emulated time\n", 100.0 * idle_skipped / (cycles_done ? cycles_done : 1)); }
    if (ring_on) { FILE *f = fopen("ring.txt", "w"); for (unsigned i = ring_i - 4096; i != ring_i; i++) { u32 v = ring[i & 4095]; fprintf(f, "%04x:%04x\n", v >> 16, v & 0xFFFF); } fclose(f); }
    trace_save();
    SDL_Quit();
    return quit_code;
}
