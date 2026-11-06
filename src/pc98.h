#ifndef PC98_H
#define PC98_H
#include "machine.h"
extern int machine_pc98;
extern u32 fb98[640 * 400];
u8 pc98_rb(u32 a); void pc98_wb(u32 a, u8 v);
u8 pc98_video_in(u16 port, int *handled); int pc98_video_out(u16 port, u8 v);
int pc98_vsync_active(void); int pc98_take_vsync_irq(void);
void pc98_render(void);
void pc98_key_event(int at_sc, int down); int pc98_kbd_pending(void); u8 pc98_kbd_next(void);
void pc98_bios_int9(void); void pc98_int18(void); void pc98_bios_init(void);
void pc98_mouse_motion(int dx, int dy); void pc98_mouse_button(int b, int down);
int pc98_mouse_irq_enabled(void); double pc98_mouse_period_ms(void);
#endif
