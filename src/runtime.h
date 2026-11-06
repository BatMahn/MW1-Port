#ifndef RUNTIME_H
#define RUNTIME_H
#include "machine.h"

/* dos.c */
extern char game_dir[1024];
extern char prog_name[64];
void dos_init(void);
int  dos_exec(const char *dospath, const char *tail, int nested);
void int21(void);

/* keyboard (hw.c) */
int  kbd_peek(void);            /* next BIOS key (scan<<8|ascii) or -1 */
int  kbd_getkey(void);
int  kbd_getkey_blocking(void); /* sets hle_retry if nothing pending */
void kbd_flush(void);
void kbd_host_event(int scancode, int down, int ascii_unused);
extern int hle_retry;

/* console text output via BIOS teletype */
void console_putc(u8 c);

/* video.c */
#define FB_W 640
#define FB_H 200
extern u32 framebuffer[FB_W * FB_H];
extern int fb_width;               /* 320 for graphics, 640 for text */
void video_init(void);
void video_set_mode(u8 mode);
u8   video_get_mode(void);
void video_render(void);
u8   video_port_in(u16 port);
void video_port_out(u16 port, u8 v);
void video_set_palette_reg(int idx, u8 val);
void bios_teletype(u8 c, u8 attr);
void bios_text_int10(void);
int  video_in_retrace(void);

/* timing (hw.c) */
extern int cycles_per_ms;
u32  runtime_ms(void);
double emu_time_ms(void);
void runtime_quit(int code);

/* audio (audio.c) */
void audio_init(void);
void audio_speaker(int on, double freq);
void audio_adlib_write(int reg, u8 val);
void audio_tandy_write(u8 val);
void audio_mpu_data(u8 v);
void audio_advance(double emu_ms);

/* code coverage trace for the recompiler (trace.c) */
void trace_note_program(const char *name, u16 load_seg);
void trace_note_overlay(const char *name, u16 seg);
void trace_exec(u16 cs, u16 ip);
void trace_save(void);
extern int trace_enabled;

#endif
