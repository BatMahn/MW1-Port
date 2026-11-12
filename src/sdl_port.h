/* SDL 2 or SDL 3. Builds against SDL 3 when MW_SDL3 is defined (the Makefile picks SDL 3 when
 * pkg-config finds it, else SDL 2). Homebrew's "sdl2" is now sdl2-compat, a shim that loads
 * SDL 3 at run time and can't find it from inside an app bundle, so macOS builds use SDL 3. */
#ifndef SDL_PORT_H
#define SDL_PORT_H
#ifdef MW_SDL3
#include <SDL3/SDL.h>
#define EV_QUIT SDL_EVENT_QUIT
#define EV_MOUSEMOTION SDL_EVENT_MOUSE_MOTION
#define EV_MOUSEDOWN SDL_EVENT_MOUSE_BUTTON_DOWN
#define EV_MOUSEUP SDL_EVENT_MOUSE_BUTTON_UP
#define EV_KEYDOWN SDL_EVENT_KEY_DOWN
#define EV_KEYUP SDL_EVENT_KEY_UP
#define KEY_SC(e) ((e).key.scancode)
#define KEY_MOD(e) ((e).key.mod)
#define MOD_ALT SDL_KMOD_ALT
#define MOD_CTRL SDL_KMOD_CTRL
#else
#include <SDL.h>
#define EV_QUIT SDL_QUIT
#define EV_MOUSEMOTION SDL_MOUSEMOTION
#define EV_MOUSEDOWN SDL_MOUSEBUTTONDOWN
#define EV_MOUSEUP SDL_MOUSEBUTTONUP
#define EV_KEYDOWN SDL_KEYDOWN
#define EV_KEYUP SDL_KEYUP
#define KEY_SC(e) ((e).key.keysym.scancode)
#define KEY_MOD(e) ((e).key.keysym.mod)
#define MOD_ALT KMOD_ALT
#define MOD_CTRL KMOD_CTRL
#endif
#endif
