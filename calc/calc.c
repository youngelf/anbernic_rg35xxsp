/*
 * calc.c - Native 640x480 Desk & Scientific Calculator (BASIC / SCI)
 *          for Anbernic RG 35xxSP (Allwinner H700 aarch64) & Linux Desktop (x86_64 X11)
 *
 * Controls (RG 35xxSP / Desktop X11):
 *   D-Pad (Arrows)       : Navigate keypad grid (wraps around edges)
 *   A (A / Space / Z)    : Press highlighted key
 *   B (B / Backspace)    : Backspace / Delete last digit (or close Help)
 *   X (X)                : Evaluate '='
 *   Y (Y)                : Toggle English Help overlay
 *   SELECT (Tab)         : Switch BASIC (5x5) <-> SCI (5x8) mode
 *   L1 (L / Q / [ / PgUp): Toggle Degrees (DEG) <-> Radians (RAD)
 *   R1 (R / E / ] / PgDn): Clear All (AC)
 *   L2 / R2 (1 / 2)      : Open '(' / Close ')' Parenthesis
 *   MENU (Esc)           : Quit Calculator (sole exit key)
 */

#include "../rg35xxsp_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dlfcn.h>

#include "calc_font.h"

#define NATIVE_W 640
#define NATIVE_H 480

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E  2.71828182845904523536
#endif

typedef struct {
    uint32_t held;
    uint32_t pressed;
    uint32_t released;
    int      direct_char;
} input_state_t;

static uint32_t g_fb[NATIVE_W * NATIVE_H];
static volatile sig_atomic_t g_sig_quit = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_sig_quit = 1;
}

static void delay_ms(uint32_t ms) {
    usleep((useconds_t)ms * 1000u);
}

#define RGB(r, g, b) (0xFF000000u | (((uint32_t)(r) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu))

/* ============================================================================
 * 1. PLATFORM BACKEND: HOST_X11 vs TARGET_RG35XXSP (SDL2 dlopen + /dev/fb0 + evdev)
 * ============================================================================ */

static uint32_t g_prev_buttons = 0;
static uint32_t g_cur_buttons  = 0;

#if defined(HOST_X11)

#include <X11/Xutil.h>
#include <X11/XKBlib.h>

static Display *g_x11_dpy = NULL;
static Window   g_x11_win = 0;
static GC       g_x11_gc  = 0;
static XImage  *g_x11_img = NULL;
static Atom     g_wm_delete_window = 0;
static char    *g_x11_buf = NULL;

static bool platform_init(const char *title) {
    g_x11_dpy = XOpenDisplay(NULL);
    if (!g_x11_dpy) return false;

    Bool supported = False;
    XkbSetDetectableAutoRepeat(g_x11_dpy, True, &supported);

    int scr = DefaultScreen(g_x11_dpy);
    g_x11_win = XCreateSimpleWindow(
        g_x11_dpy, RootWindow(g_x11_dpy, scr),
        100, 100, NATIVE_W, NATIVE_H, 1,
        BlackPixel(g_x11_dpy, scr), BlackPixel(g_x11_dpy, scr)
    );

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width  = hints.max_width  = NATIVE_W;
    hints.min_height = hints.max_height = NATIVE_H;
    XSetWMNormalHints(g_x11_dpy, g_x11_win, &hints);

    XWMHints wm_hints;
    memset(&wm_hints, 0, sizeof(wm_hints));
    wm_hints.flags         = InputHint | StateHint;
    wm_hints.input         = True;
    wm_hints.initial_state = NormalState;
    XSetWMHints(g_x11_dpy, g_x11_win, &wm_hints);

    XSelectInput(g_x11_dpy, g_x11_win,
                 KeyPressMask | KeyReleaseMask | ButtonPressMask |
                 ExposureMask | FocusChangeMask | StructureNotifyMask);
    g_wm_delete_window = XInternAtom(g_x11_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_x11_dpy, g_x11_win, &g_wm_delete_window, 1);
    XStoreName(g_x11_dpy, g_x11_win, title);
    XMapRaised(g_x11_dpy, g_x11_win);
    XFlush(g_x11_dpy);
    g_x11_gc = DefaultGC(g_x11_dpy, scr);

    g_x11_buf = (char *)calloc((size_t)NATIVE_W * NATIVE_H, 4);
    if (!g_x11_buf) return false;

    g_x11_img = XCreateImage(
        g_x11_dpy, DefaultVisual(g_x11_dpy, scr),
        (unsigned int)DefaultDepth(g_x11_dpy, scr), ZPixmap, 0,
        g_x11_buf, NATIVE_W, NATIVE_H, 32, 0
    );
    return true;
}

static void platform_present(void) {
    if (!g_x11_dpy || !g_x11_img) return;
    memcpy(g_x11_buf, g_fb, (size_t)NATIVE_W * NATIVE_H * sizeof(uint32_t));
    XPutImage(g_x11_dpy, g_x11_win, g_x11_gc, g_x11_img, 0, 0, 0, 0, NATIVE_W, NATIVE_H);
    XFlush(g_x11_dpy);
}

static uint32_t xkey_to_btn(XKeyEvent *xkey, int *direct_char) {
    char buf[8] = {0};
    KeySym sym = NoSymbol;
    XLookupString(xkey, buf, sizeof(buf) - 1, &sym, NULL);
    if (sym == NoSymbol) sym = XLookupKeysym(xkey, 0);

    uint32_t mask = rg35xxsp_map_x11_keysym(sym);
    if (!mask) {
        KeySym base = XLookupKeysym(xkey, 0);
        if (base != NoSymbol) mask = rg35xxsp_map_x11_keysym(base);
    }

    if (!mask && direct_char && *direct_char == 0) {
        switch (sym) {
            case XK_KP_0: *direct_char = '0'; break;
            case XK_KP_1: *direct_char = '1'; break;
            case XK_KP_2: *direct_char = '2'; break;
            case XK_KP_3: *direct_char = '3'; break;
            case XK_KP_4: *direct_char = '4'; break;
            case XK_KP_5: *direct_char = '5'; break;
            case XK_KP_6: *direct_char = '6'; break;
            case XK_KP_7: *direct_char = '7'; break;
            case XK_KP_8: *direct_char = '8'; break;
            case XK_KP_9: *direct_char = '9'; break;
            case XK_KP_Decimal: *direct_char = '.'; break;
            case XK_KP_Add: *direct_char = '+'; break;
            case XK_KP_Subtract: *direct_char = '-'; break;
            case XK_KP_Multiply: *direct_char = '*'; break;
            case XK_KP_Divide: *direct_char = '/'; break;
            default:
                if (buf[0] && strchr("03456789.+-*/=^%()", buf[0])) {
                    *direct_char = (unsigned char)buf[0];
                }
                break;
        }
    }
    return mask;
}

static bool platform_poll_input(input_state_t *out) {
    if (!g_x11_dpy) return false;
    uint32_t pressed_latch  = 0;
    uint32_t released_latch = 0;
    int direct_char = 0;

    while (XPending(g_x11_dpy) > 0) {
        XEvent xev;
        XNextEvent(g_x11_dpy, &xev);
        if (xev.type == ClientMessage) {
            if ((Atom)xev.xclient.data.l[0] == g_wm_delete_window) return false;
        } else if (xev.type == MapNotify || xev.type == ButtonPress) {
            XSetInputFocus(g_x11_dpy, g_x11_win, RevertToParent, CurrentTime);
        } else if (xev.type == KeyPress) {
            uint32_t mask = xkey_to_btn(&xev.xkey, &direct_char);
            if (mask) {
                pressed_latch |= (mask & ~g_cur_buttons);
                g_cur_buttons |= mask;
            }
        } else if (xev.type == KeyRelease) {
            if (XEventsQueued(g_x11_dpy, QueuedAfterReading)) {
                XEvent nev;
                XPeekEvent(g_x11_dpy, &nev);
                if (nev.type == KeyPress && nev.xkey.time == xev.xkey.time &&
                    nev.xkey.keycode == xev.xkey.keycode) {
                    XNextEvent(g_x11_dpy, &nev);
                    continue;
                }
            }
            uint32_t mask = xkey_to_btn(&xev.xkey, NULL);
            if (mask) {
                released_latch |= (mask & g_cur_buttons);
                g_cur_buttons  &= ~mask;
            }
        }
    }
    out->held        = g_cur_buttons;
    out->pressed     = pressed_latch | (g_cur_buttons & ~g_prev_buttons);
    out->released    = released_latch | (~g_cur_buttons & g_prev_buttons);
    out->direct_char = direct_char;
    g_prev_buttons   = g_cur_buttons;
    return true;
}

static void platform_shutdown(void) {
    if (g_x11_img) {
        XDestroyImage(g_x11_img);
        g_x11_img = NULL;
        g_x11_buf = NULL;
    }
    if (g_x11_dpy) {
        if (g_x11_win) XDestroyWindow(g_x11_dpy, g_x11_win);
        XCloseDisplay(g_x11_dpy);
        g_x11_dpy = NULL;
    }
}

#else /* TARGET_RG35XXSP: SDL2 dlopen + /dev/fb0 fallback + evdev */

#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define SDL2_INIT_VIDEO                0x00000020U
#define SDL2_WINDOWPOS_UNDEFINED       0x1FFF0000U
#define SDL2_WINDOW_SHOWN              0x00000004U
#define SDL2_WINDOW_FULLSCREEN_DESKTOP 0x00001001U
#define SDL2_RENDERER_ACCELERATED      0x00000002U
#define SDL2_RENDERER_PRESENTVSYNC     0x00000004U
#define SDL2_PIXELFORMAT_ARGB8888      0x16362004U
#define SDL2_TEXTUREACCESS_STREAMING   1
#define SDL2_QUIT_EVENT                0x100U

typedef union {
    uint32_t type;
    uint8_t  padding[64];
} SDL2_RawEvent;

static void *g_sdl2_lib = NULL;
static void *g_sdl2_win = NULL;
static void *g_sdl2_ren = NULL;
static void *g_sdl2_tex = NULL;
static bool  g_use_sdl2_video = false;

static int   (*fn_SDL_Init)(uint32_t) = NULL;
static void  (*fn_SDL_Quit)(void) = NULL;
static void* (*fn_SDL_CreateWindow)(const char*, int, int, int, int, uint32_t) = NULL;
static void  (*fn_SDL_DestroyWindow)(void*) = NULL;
static void* (*fn_SDL_CreateRenderer)(void*, int, uint32_t) = NULL;
static void  (*fn_SDL_DestroyRenderer)(void*) = NULL;
static void* (*fn_SDL_CreateTexture)(void*, uint32_t, int, int, int) = NULL;
static void  (*fn_SDL_DestroyTexture)(void*) = NULL;
static int   (*fn_SDL_UpdateTexture)(void*, const void*, const void*, int) = NULL;
static int   (*fn_SDL_RenderClear)(void*) = NULL;
static int   (*fn_SDL_RenderCopy)(void*, void*, const void*, const void*) = NULL;
static void  (*fn_SDL_RenderPresent)(void*) = NULL;
static int   (*fn_SDL_PollEvent)(SDL2_RawEvent*) = NULL;
static int   (*fn_SDL_ShowCursor)(int) = NULL;

#define MAX_EVDEV_FDS 32
static int g_evdev_fds[MAX_EVDEV_FDS];
static int g_num_evdevs = 0;

static int                      g_fb_fd = -1;
static uint8_t                 *g_fb_mem = NULL;
static size_t                   g_fb_map_size = 0;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;

static bool init_fbdev_fallback(void) {
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) return false;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) < 0 ||
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) < 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
        return false;
    }
    g_fb_map_size = g_finfo.smem_len ? g_finfo.smem_len : (size_t)(g_vinfo.yres_virtual * g_finfo.line_length);
    g_fb_mem = (uint8_t *)mmap(NULL, g_fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        g_fb_mem = NULL;
        close(g_fb_fd);
        g_fb_fd = -1;
        return false;
    }
    return true;
}

static bool platform_init(const char *title) {
    g_num_evdevs = 0;
    for (int i = 0; i < MAX_EVDEV_FDS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            g_evdev_fds[g_num_evdevs++] = fd;
        }
    }

    g_sdl2_lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (g_sdl2_lib) {
        fn_SDL_Init            = dlsym(g_sdl2_lib, "SDL_Init");
        fn_SDL_Quit            = dlsym(g_sdl2_lib, "SDL_Quit");
        fn_SDL_CreateWindow    = dlsym(g_sdl2_lib, "SDL_CreateWindow");
        fn_SDL_DestroyWindow   = dlsym(g_sdl2_lib, "SDL_DestroyWindow");
        fn_SDL_CreateRenderer  = dlsym(g_sdl2_lib, "SDL_CreateRenderer");
        fn_SDL_DestroyRenderer = dlsym(g_sdl2_lib, "SDL_DestroyRenderer");
        fn_SDL_CreateTexture   = dlsym(g_sdl2_lib, "SDL_CreateTexture");
        fn_SDL_DestroyTexture  = dlsym(g_sdl2_lib, "SDL_DestroyTexture");
        fn_SDL_UpdateTexture   = dlsym(g_sdl2_lib, "SDL_UpdateTexture");
        fn_SDL_RenderClear     = dlsym(g_sdl2_lib, "SDL_RenderClear");
        fn_SDL_RenderCopy      = dlsym(g_sdl2_lib, "SDL_RenderCopy");
        fn_SDL_RenderPresent   = dlsym(g_sdl2_lib, "SDL_RenderPresent");
        fn_SDL_PollEvent       = dlsym(g_sdl2_lib, "SDL_PollEvent");
        fn_SDL_ShowCursor      = dlsym(g_sdl2_lib, "SDL_ShowCursor");

        if (fn_SDL_Init && fn_SDL_CreateWindow && fn_SDL_CreateRenderer && fn_SDL_CreateTexture) {
            if (fn_SDL_Init(SDL2_INIT_VIDEO) == 0) {
                if (fn_SDL_ShowCursor) fn_SDL_ShowCursor(0);
                g_sdl2_win = fn_SDL_CreateWindow(
                    title,
                    SDL2_WINDOWPOS_UNDEFINED, SDL2_WINDOWPOS_UNDEFINED,
                    NATIVE_W, NATIVE_H,
                    SDL2_WINDOW_FULLSCREEN_DESKTOP | SDL2_WINDOW_SHOWN
                );
                if (g_sdl2_win) {
                    g_sdl2_ren = fn_SDL_CreateRenderer(
                        g_sdl2_win, -1,
                        SDL2_RENDERER_ACCELERATED | SDL2_RENDERER_PRESENTVSYNC
                    );
                    if (!g_sdl2_ren) g_sdl2_ren = fn_SDL_CreateRenderer(g_sdl2_win, -1, 0);
                    if (g_sdl2_ren) {
                        g_sdl2_tex = fn_SDL_CreateTexture(
                            g_sdl2_ren, SDL2_PIXELFORMAT_ARGB8888,
                            SDL2_TEXTUREACCESS_STREAMING, NATIVE_W, NATIVE_H
                        );
                        if (g_sdl2_tex) g_use_sdl2_video = true;
                    }
                }
            }
        }
    }

    if (!g_use_sdl2_video && !init_fbdev_fallback()) return false;
    return true;
}

static void platform_present(void) {
    if (g_use_sdl2_video) {
        fn_SDL_UpdateTexture(g_sdl2_tex, NULL, g_fb, NATIVE_W * 4);
        fn_SDL_RenderClear(g_sdl2_ren);
        fn_SDL_RenderCopy(g_sdl2_ren, g_sdl2_tex, NULL, NULL);
        fn_SDL_RenderPresent(g_sdl2_ren);
        return;
    }
    if (!g_fb_mem) return;

    int fb_w   = (int)g_vinfo.xres;
    int fb_h   = (int)g_vinfo.yres;
    int bpp    = (int)g_vinfo.bits_per_pixel;
    int stride = (int)g_finfo.line_length;
    uint8_t *base = g_fb_mem + g_vinfo.yoffset * stride;

    int copy_w = (fb_w < NATIVE_W) ? fb_w : NATIVE_W;
    int copy_h = (fb_h < NATIVE_H) ? fb_h : NATIVE_H;
    for (int y = 0; y < copy_h; y++) {
        const uint32_t *src = g_fb + y * NATIVE_W;
        if (bpp == 32) {
            memcpy(base + y * stride, src, (size_t)copy_w * 4);
        } else if (bpp == 16) {
            uint16_t *dst = (uint16_t *)(base + y * stride);
            for (int x = 0; x < copy_w; x++) {
                uint32_t c = src[x];
                dst[x] = (uint16_t)((((c >> 19) & 0x1F) << 11) |
                                    (((c >> 10) & 0x3F) << 5)  |
                                    ((c >> 3) & 0x1F));
            }
        }
    }
    ioctl(g_fb_fd, FBIOPAN_DISPLAY, &g_vinfo);
}

static bool platform_poll_input(input_state_t *out) {
    uint32_t pressed_latch  = 0;
    uint32_t released_latch = 0;

    if (g_use_sdl2_video && fn_SDL_PollEvent) {
        SDL2_RawEvent ev;
        while (fn_SDL_PollEvent(&ev)) {
            if (ev.type == SDL2_QUIT_EVENT) return false;
        }
    }

    for (int i = 0; i < g_num_evdevs; i++) {
        struct input_event ie;
        while (read(g_evdev_fds[i], &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
            if (ie.type == EV_KEY && (ie.value == 0 || ie.value == 1)) {
                uint32_t mask = rg35xxsp_map_evkey((int)ie.code);
                if (mask) {
                    if (ie.value == 1) {
                        pressed_latch |= (mask & ~g_cur_buttons);
                        g_cur_buttons |= mask;
                    } else {
                        released_latch |= (mask & g_cur_buttons);
                        g_cur_buttons  &= ~mask;
                    }
                }
            } else if (ie.type == EV_ABS) {
                uint32_t old_btns = g_cur_buttons;
                uint32_t new_btns = rg35xxsp_update_evabs(old_btns, ie.code, ie.value, 0, 0);
                pressed_latch  |= (new_btns & ~old_btns);
                released_latch |= (old_btns & ~new_btns);
                g_cur_buttons   = new_btns;
            }
        }
    }

    out->held        = g_cur_buttons;
    out->pressed     = pressed_latch | (g_cur_buttons & ~g_prev_buttons);
    out->released    = released_latch | (~g_cur_buttons & g_prev_buttons);
    out->direct_char = 0;
    g_prev_buttons   = g_cur_buttons;
    return true;
}

static void platform_shutdown(void) {
    for (int i = 0; i < g_num_evdevs; i++) {
        if (g_evdev_fds[i] >= 0) close(g_evdev_fds[i]);
    }
    g_num_evdevs = 0;

    if (g_use_sdl2_video) {
        if (g_sdl2_tex && fn_SDL_DestroyTexture)  fn_SDL_DestroyTexture(g_sdl2_tex);
        if (g_sdl2_ren && fn_SDL_DestroyRenderer) fn_SDL_DestroyRenderer(g_sdl2_ren);
        if (g_sdl2_win && fn_SDL_DestroyWindow)   fn_SDL_DestroyWindow(g_sdl2_win);
        g_use_sdl2_video = false;
    }
    if (fn_SDL_Quit) fn_SDL_Quit();
    if (g_sdl2_lib) {
        dlclose(g_sdl2_lib);
        g_sdl2_lib = NULL;
    }
    if (g_fb_mem) {
        munmap(g_fb_mem, g_fb_map_size);
        g_fb_mem = NULL;
    }
    if (g_fb_fd >= 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
    }
}

#endif /* HOST_X11 vs TARGET_RG35XXSP */

/* ============================================================================
 * 2. 640x480 ANTI-ALIASED DRAWING & TEXT PRIMITIVES
 * ============================================================================ */

static inline void put_pixel(int x, int y, uint32_t c) {
    if ((unsigned)x < NATIVE_W && (unsigned)y < NATIVE_H) {
        g_fb[y * NATIVE_W + x] = c;
    }
}

static inline void blend_pixel(int x, int y, uint32_t fg, uint8_t alpha) {
    if ((unsigned)x >= NATIVE_W || (unsigned)y >= NATIVE_H || alpha == 0) return;
    if (alpha == 255) {
        g_fb[y * NATIVE_W + x] = fg;
        return;
    }
    uint32_t bg = g_fb[y * NATIVE_W + x];
    uint32_t a  = alpha;
    uint32_t ia = 255u - a;
    uint32_t r  = (((fg >> 16) & 0xFFu) * a + ((bg >> 16) & 0xFFu) * ia) / 255u;
    uint32_t g  = (((fg >>  8) & 0xFFu) * a + ((bg >>  8) & 0xFFu) * ia) / 255u;
    uint32_t b  = (( fg        & 0xFFu) * a + ( bg        & 0xFFu) * ia) / 255u;
    g_fb[y * NATIVE_W + x] = RGB(r, g, b);
}

static uint32_t brighten_color(uint32_t c, int delta) {
    int r = (int)((c >> 16) & 0xFFu) + delta;
    int g = (int)((c >>  8) & 0xFFu) + delta;
    int b = (int)( c        & 0xFFu) + delta;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return RGB(r, g, b);
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > NATIVE_W) ? NATIVE_W : (x + w);
    int y1 = (y + h > NATIVE_H) ? NATIVE_H : (y + h);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g_fb[yy * NATIVE_W];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

static void fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > NATIVE_W) ? NATIVE_W : (x + w);
    int y1 = (y + h > NATIVE_H) ? NATIVE_H : (y + h);
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            blend_pixel(xx, yy, color, alpha);
        }
    }
}

static void fill_vgradient(int x, int y, int w, int h, uint32_t top_col, uint32_t bot_col) {
    if (h <= 0) return;
    int r0 = (top_col >> 16) & 0xFF, g0 = (top_col >> 8) & 0xFF, b0 = top_col & 0xFF;
    int r1 = (bot_col >> 16) & 0xFF, g1 = (bot_col >> 8) & 0xFF, b1 = bot_col & 0xFF;
    int denom = (h > 1) ? (h - 1) : 1;
    for (int i = 0; i < h; i++) {
        int r = r0 + ((r1 - r0) * i) / denom;
        int g = g0 + ((g1 - g0) * i) / denom;
        int b = b0 + ((b1 - b0) * i) / denom;
        fill_rect(x, y + i, w, 1, RGB(r, g, b));
    }
}

static void draw_border(int x, int y, int w, int h, int thick, uint32_t c) {
    fill_rect(x, y, w, thick, c);
    fill_rect(x, y + h - thick, w, thick, c);
    fill_rect(x, y, thick, h, c);
    fill_rect(x + w - thick, y, thick, h, c);
}

static void draw_pill(int x, int y, int w, int h, uint32_t fill_col, uint32_t border_col) {
    if (w < 6 || h < 6) return;
    fill_rect(x + 2, y + 2, w - 4, h - 4, fill_col);
    fill_rect(x + 2, y, w - 4, 2, border_col);
    fill_rect(x + 2, y + h - 2, w - 4, 2, border_col);
    fill_rect(x, y + 2, 2, h - 4, border_col);
    fill_rect(x + w - 2, y + 2, 2, h - 4, border_col);
    put_pixel(x + 1, y + 1, border_col);
    put_pixel(x + w - 2, y + 1, border_col);
    put_pixel(x + 1, y + h - 2, border_col);
    put_pixel(x + w - 2, y + h - 2, border_col);
}

/* 3D beveled keycap with rounded corners and glowing golden focus ring */
static void draw_beveled_key(int x, int y, int w, int h, uint32_t base_col, bool focused) {
    if (w <= 6 || h <= 6) return;
    uint32_t fill   = focused ? brighten_color(base_col, 32) : base_col;
    uint32_t top_hi = brighten_color(fill, 28);
    uint32_t bot_sh = brighten_color(fill, -26);

    /* Subtle drop shadow under keycap */
    fill_rect(x + 2, y + h - 1, w - 4, 2, RGB(8, 12, 20));

    /* Vertical gradient body for tactile keycap depth */
    fill_vgradient(x + 2, y + 2, w - 4, h - 4, brighten_color(fill, 8), brighten_color(fill, -8));

    /* 2px top/left highlight bevel + 2px bottom/right shadow bevel */
    fill_rect(x + 2, y,         w - 4, 2, top_hi);
    fill_rect(x,     y + 2,     2, h - 4, top_hi);
    fill_rect(x + 2, y + h - 2, w - 4, 2, bot_sh);
    fill_rect(x + w - 2, y + 2, 2, h - 4, bot_sh);
    put_pixel(x + 1, y + 1, top_hi);
    put_pixel(x + w - 2, y + 1, top_hi);
    put_pixel(x + 1, y + h - 2, bot_sh);
    put_pixel(x + w - 2, y + h - 2, bot_sh);

    if (focused) {
        uint32_t ring_outer = RGB(250, 204, 21);  /* #FACC15 */
        uint32_t ring_inner = RGB(254, 240, 138); /* #FEF08A */
        fill_rect(x + 2, y,         w - 4, 2, ring_outer);
        fill_rect(x + 2, y + h - 2, w - 4, 2, ring_outer);
        fill_rect(x,     y + 2,     2, h - 4, ring_outer);
        fill_rect(x + w - 2, y + 2, 2, h - 4, ring_outer);
        put_pixel(x + 1, y + 1, ring_outer);
        put_pixel(x + w - 2, y + 1, ring_outer);
        put_pixel(x + 1, y + h - 2, ring_outer);
        put_pixel(x + w - 2, y + h - 2, ring_outer);

        fill_rect(x + 2, y + 2,     w - 4, 1, ring_inner);
        fill_rect(x + 2, y + h - 3, w - 4, 1, ring_outer);
        fill_rect(x + 2, y + 2,     1, h - 4, ring_inner);
        fill_rect(x + w - 3, y + 2, 1, h - 4, ring_outer);
    }
}

static int text_width(int font_id, const char *s) {
    const CalcFont *f = &g_calc_fonts[font_id];
    int w = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 32 || c > 126) c = '?';
        w += f->glyphs[c - 32].advance;
    }
    return w;
}

static void draw_text(int font_id, int x, int y, const char *s, uint32_t color) {
    const CalcFont *f = &g_calc_fonts[font_id];
    int cur_x = x;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 32 || c > 126) c = '?';
        const CalcGlyph *g = &f->glyphs[c - 32];
        const uint8_t *bmp = &g_calc_font_bitmaps[g->offset];
        for (int row = 0; row < g->h; row++) {
            for (int col = 0; col < g->w; col++) {
                uint8_t a = bmp[row * g->w + col];
                if (a > 0) {
                    blend_pixel(cur_x + g->xoff + col, y + g->yoff + row, color, a);
                }
            }
        }
        cur_x += g->advance;
    }
}

static void draw_text_centered(int font_id, int bx, int by, int bw, int bh, const char *s, uint32_t color) {
    const CalcFont *f = &g_calc_fonts[font_id];
    int tw = text_width(font_id, s);
    int tx = bx + (bw - tw) / 2;
    int ty = by + (bh - f->line_height) / 2;
    draw_text(font_id, tx, ty, s, color);
}

static void draw_text_right(int font_id, int right_x, int y, const char *s, uint32_t color) {
    int tw = text_width(font_id, s);
    draw_text(font_id, right_x - tw, y, s, color);
}

static void save_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t row_bytes = NATIVE_W * 3;
    uint32_t img_bytes = row_bytes * NATIVE_H;
    uint32_t file_size = 54 + img_bytes;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)(file_size);
    hdr[3] = (uint8_t)(file_size >> 8);
    hdr[4] = (uint8_t)(file_size >> 16);
    hdr[5] = (uint8_t)(file_size >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (uint8_t)(NATIVE_W);
    hdr[19] = (uint8_t)(NATIVE_W >> 8);
    int32_t neg_h = -NATIVE_H;
    memcpy(&hdr[22], &neg_h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (int y = 0; y < NATIVE_H; y++) {
        for (int x = 0; x < NATIVE_W; x++) {
            uint32_t c = g_fb[y * NATIVE_W + x];
            uint8_t bgr[3] = {
                (uint8_t)(c & 0xFFu),
                (uint8_t)((c >> 8) & 0xFFu),
                (uint8_t)((c >> 16) & 0xFFu)
            };
            fwrite(bgr, 1, 3, f);
        }
    }
    fclose(f);
}

/* ============================================================================
 * 3. CALCULATOR STATE, PARENTHESES STACK, MEMORY, ANS & SCIENTIFIC MATH ENGINE
 * ============================================================================ */

typedef enum {
    MODE_BASIC = 0,
    MODE_SCI   = 1
} CalcMode;

typedef enum {
    OP_NONE = 0,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_POW,
    OP_MOD
} BinaryOp;

typedef enum {
    KEY_CAT_DIGIT = 0,
    KEY_CAT_OP,
    KEY_CAT_EQUALS,
    KEY_CAT_CLEAR,
    KEY_CAT_MEM,
    KEY_CAT_SCI
} KeyCategory;

#define BASIC_ROWS 5
#define BASIC_COLS 5
#define SCI_ROWS   5
#define SCI_COLS   8

static const char *const k_basic_grid[BASIC_ROWS][BASIC_COLS] = {
    { "MC",   "C",   "DEL", "%",   "/" },
    { "MR",   "7",   "8",   "9",   "*" },
    { "M+",   "4",   "5",   "6",   "-" },
    { "M-",   "1",   "2",   "3",   "+" },
    { "sqrt", "0",   ".",   "+/-", "=" }
};

static const char *const k_sci_grid[SCI_ROWS][SCI_COLS] = {
    { "sin",  "cos",  "tan",  "ANS",  "C",   "DEL", "%",   "/" },
    { "asin", "acos", "atan", "pi",   "7",   "8",   "9",   "*" },
    { "log",  "ln",   "exp",  "10^x", "4",   "5",   "6",   "-" },
    { "x^y",  "x^2",  "sqrt", "cbrt", "1",   "2",   "3",   "+" },
    { "1/x",  "n!",   "mod",  "e",    "0",   ".",   "+/-", "=" }
};

#define MAX_PAREN_DEPTH 16
typedef struct {
    double   acc;
    BinaryOp op;
    char     expr_prefix[96];
} ParenFrame;

static CalcMode   g_mode        = MODE_BASIC;
static bool       g_deg_mode    = true;
static bool       g_show_help   = false;
static int        g_cur_row     = 4;
static int        g_cur_col     = 4; /* Starts on '=' in BASIC mode */

static char       g_display[64] = "0";
static char       g_expr[128]   = "";
static bool       g_new_entry   = true;
static bool       g_error       = false;

static double     g_acc         = 0.0;
static double     g_last_rhs    = 0.0;
static BinaryOp   g_pending_op  = OP_NONE;
static BinaryOp   g_last_op     = OP_NONE;
static double     g_memory      = 0.0;
static double     g_ans         = 0.0;
static bool       g_has_ans     = false;

static ParenFrame g_paren_stack[MAX_PAREN_DEPTH];
static int        g_paren_depth = 0;

static void format_number(double val, char *buf, size_t buf_sz) {
    if (isnan(val) || isinf(val)) {
        snprintf(buf, buf_sz, "Math Error");
        g_error = true;
        g_new_entry = true;
        return;
    }
    if (fabs(val) < 1e-15) val = 0.0;

    snprintf(buf, buf_sz, "%.12g", val);

    char *e_ptr = strchr(buf, 'e');
    if (!e_ptr) e_ptr = strchr(buf, 'E');
    if (!e_ptr && strchr(buf, '.')) {
        size_t len = strlen(buf);
        while (len > 1 && buf[len - 1] == '0') {
            buf[--len] = '\0';
        }
        if (len > 1 && buf[len - 1] == '.') {
            buf[--len] = '\0';
        }
    }
}

static double parse_display(void) {
    if (g_error) return 0.0;
    return strtod(g_display, NULL);
}

static const char *op_symbol(BinaryOp op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_POW: return "^";
        case OP_MOD: return "mod";
        default:     return "";
    }
}

static void set_error(const char *msg) {
    snprintf(g_display, sizeof(g_display), "%s", msg);
    g_error       = true;
    g_new_entry   = true;
    g_pending_op  = OP_NONE;
    g_paren_depth = 0;
}

static double eval_binary(double a, double b, BinaryOp op, bool *err) {
    *err = false;
    switch (op) {
        case OP_ADD: return a + b;
        case OP_SUB: return a - b;
        case OP_MUL: return a * b;
        case OP_DIV:
            if (b == 0.0) { *err = true; return 0.0; }
            return a / b;
        case OP_POW:
            return pow(a, b);
        case OP_MOD:
            if (b == 0.0) { *err = true; return 0.0; }
            return fmod(a, b);
        default:
            return b;
    }
}

static void clear_all(void) {
    snprintf(g_display, sizeof(g_display), "0");
    g_expr[0]     = '\0';
    g_new_entry   = true;
    g_error       = false;
    g_acc         = 0.0;
    g_last_rhs    = 0.0;
    g_pending_op  = OP_NONE;
    g_last_op     = OP_NONE;
    g_paren_depth = 0;
}

static void backspace_digit(void) {
    if (g_error) {
        clear_all();
        return;
    }
    if (g_new_entry) return;
    size_t len = strlen(g_display);
    if (len <= 1 || (len == 2 && g_display[0] == '-')) {
        snprintf(g_display, sizeof(g_display), "0");
        g_new_entry = true;
    } else {
        g_display[len - 1] = '\0';
    }
}

static void input_digit(char d) {
    if (g_error) clear_all();
    if (g_new_entry) {
        if (d == '.') {
            snprintf(g_display, sizeof(g_display), "0.");
        } else {
            g_display[0] = d;
            g_display[1] = '\0';
        }
        g_new_entry = false;
        return;
    }
    if (d == '.' && strchr(g_display, '.')) return;
    size_t len = strlen(g_display);
    if (len >= 20) return;
    if (strcmp(g_display, "0") == 0 && d != '.') {
        g_display[0] = d;
        g_display[1] = '\0';
    } else {
        g_display[len]     = d;
        g_display[len + 1] = '\0';
    }
}

static void toggle_sign(void) {
    if (g_error) return;
    if (strcmp(g_display, "0") == 0) return;
    if (g_display[0] == '-') {
        memmove(g_display, g_display + 1, strlen(g_display));
    } else {
        size_t len = strlen(g_display);
        if (len + 2 < sizeof(g_display)) {
            memmove(g_display + 1, g_display, len + 1);
            g_display[0] = '-';
        }
    }
}

static void push_open_paren(void) {
    if (g_error) clear_all();
    if (g_paren_depth >= MAX_PAREN_DEPTH) return;

    ParenFrame *fr = &g_paren_stack[g_paren_depth++];
    fr->acc = g_acc;
    fr->op  = g_pending_op;
    snprintf(fr->expr_prefix, sizeof(fr->expr_prefix), "%.*s",
             (int)(sizeof(fr->expr_prefix) - 1), g_expr);

    if (g_expr[0] != '\0') {
        size_t len = strlen(g_expr);
        if (len + 3 < sizeof(g_expr)) {
            snprintf(g_expr + len, sizeof(g_expr) - len, " (");
        }
    } else {
        snprintf(g_expr, sizeof(g_expr), "(");
    }

    g_acc        = 0.0;
    g_pending_op = OP_NONE;
    g_last_op    = OP_NONE;
    snprintf(g_display, sizeof(g_display), "0");
    g_new_entry  = true;
}

static void pop_close_paren(void) {
    if (g_error || g_paren_depth <= 0) return;

    double cur = parse_display();
    if (g_pending_op != OP_NONE) {
        bool err = false;
        double res = eval_binary(g_acc, cur, g_pending_op, &err);
        if (err || isnan(res) || isinf(res)) {
            set_error(err ? "Div by 0" : "Math Error");
            return;
        }
        cur = res;
    }

    ParenFrame *fr = &g_paren_stack[--g_paren_depth];
    g_acc        = fr->acc;
    g_pending_op = fr->op;

    char val_s[32];
    format_number(cur, val_s, sizeof(val_s));
    snprintf(g_display, sizeof(g_display), "%s", val_s);
    if (fr->expr_prefix[0] != '\0') {
        snprintf(g_expr, sizeof(g_expr), "%.80s (%.32s)", fr->expr_prefix, val_s);
    } else {
        snprintf(g_expr, sizeof(g_expr), "(%.32s)", val_s);
    }
    g_new_entry = false;
}

static void start_binary_op(BinaryOp op) {
    if (g_error) return;
    double cur = parse_display();
    if (g_pending_op != OP_NONE && !g_new_entry) {
        bool err = false;
        double res = eval_binary(g_acc, cur, g_pending_op, &err);
        if (err || isnan(res) || isinf(res)) {
            set_error(err ? "Div by 0" : "Math Error");
            return;
        }
        g_acc = res;
        format_number(res, g_display, sizeof(g_display));
    } else {
        g_acc = cur;
    }
    g_pending_op = op;
    g_last_op    = OP_NONE;

    char num_buf[32];
    format_number(g_acc, num_buf, sizeof(num_buf));
    if (g_paren_depth > 0 && g_paren_stack[g_paren_depth - 1].expr_prefix[0] != '\0') {
        snprintf(g_expr, sizeof(g_expr), "%.70s (%.28s %s",
                 g_paren_stack[g_paren_depth - 1].expr_prefix, num_buf, op_symbol(op));
    } else if (g_paren_depth > 0) {
        snprintf(g_expr, sizeof(g_expr), "(%.28s %s", num_buf, op_symbol(op));
    } else {
        snprintf(g_expr, sizeof(g_expr), "%s %s", num_buf, op_symbol(op));
    }
    g_new_entry = true;
}

static void press_equals(void) {
    if (g_error) {
        clear_all();
        return;
    }
    while (g_paren_depth > 0 && !g_error) {
        pop_close_paren();
    }
    if (g_error) return;

    double cur = parse_display();
    if (g_pending_op != OP_NONE) {
        bool err = false;
        double lhs = g_acc;
        double rhs = cur;
        BinaryOp op = g_pending_op;
        double res = eval_binary(lhs, rhs, op, &err);
        char lhs_s[32], rhs_s[32];
        format_number(lhs, lhs_s, sizeof(lhs_s));
        format_number(rhs, rhs_s, sizeof(rhs_s));
        snprintf(g_expr, sizeof(g_expr), "%s %s %s =", lhs_s, op_symbol(op), rhs_s);

        g_last_op    = op;
        g_last_rhs   = rhs;
        g_pending_op = OP_NONE;
        if (err || isnan(res) || isinf(res)) {
            set_error(err ? "Div by 0" : "Math Error");
            return;
        }
        format_number(res, g_display, sizeof(g_display));
        g_ans     = res;
        g_has_ans = true;
        g_new_entry = true;
    } else if (g_last_op != OP_NONE) {
        bool err = false;
        double lhs = cur;
        double rhs = g_last_rhs;
        double res = eval_binary(lhs, rhs, g_last_op, &err);
        char lhs_s[32], rhs_s[32];
        format_number(lhs, lhs_s, sizeof(lhs_s));
        format_number(rhs, rhs_s, sizeof(rhs_s));
        snprintf(g_expr, sizeof(g_expr), "%s %s %s =", lhs_s, op_symbol(g_last_op), rhs_s);
        if (err || isnan(res) || isinf(res)) {
            set_error(err ? "Div by 0" : "Math Error");
            return;
        }
        format_number(res, g_display, sizeof(g_display));
        g_ans     = res;
        g_has_ans = true;
        g_new_entry = true;
    } else {
        g_ans     = cur;
        g_has_ans = true;
        g_new_entry = true;
    }
}

static void apply_unary(const char *fn) {
    if (g_error) return;
    double x = parse_display();
    double res = 0.0;
    char x_str[32];
    format_number(x, x_str, sizeof(x_str));

    if (strcmp(fn, "sin") == 0) {
        snprintf(g_expr, sizeof(g_expr), "sin(%s%s)", x_str, g_deg_mode ? " deg" : " rad");
        if (g_deg_mode) {
            double m = fmod(x, 180.0);
            if (fabs(m) < 1e-12) res = 0.0;
            else res = sin(x * (M_PI / 180.0));
        } else {
            res = sin(x);
        }
    } else if (strcmp(fn, "cos") == 0) {
        snprintf(g_expr, sizeof(g_expr), "cos(%s%s)", x_str, g_deg_mode ? " deg" : " rad");
        if (g_deg_mode) {
            double m = fmod(fabs(x), 180.0);
            if (fabs(m - 90.0) < 1e-12) res = 0.0;
            else res = cos(x * (M_PI / 180.0));
        } else {
            res = cos(x);
        }
    } else if (strcmp(fn, "tan") == 0) {
        snprintf(g_expr, sizeof(g_expr), "tan(%s%s)", x_str, g_deg_mode ? " deg" : " rad");
        if (g_deg_mode) {
            double m = fmod(fabs(x), 180.0);
            if (fabs(m - 90.0) < 1e-9) { set_error("Math Error"); return; }
            if (fabs(m) < 1e-12) res = 0.0;
            else res = tan(x * (M_PI / 180.0));
        } else {
            if (fabs(cos(x)) < 1e-12) { set_error("Math Error"); return; }
            res = tan(x);
        }
    } else if (strcmp(fn, "asin") == 0) {
        snprintf(g_expr, sizeof(g_expr), "asin(%s)", x_str);
        if (x < -1.0 || x > 1.0) { set_error("Math Error"); return; }
        res = asin(x);
        if (g_deg_mode) res *= (180.0 / M_PI);
    } else if (strcmp(fn, "acos") == 0) {
        snprintf(g_expr, sizeof(g_expr), "acos(%s)", x_str);
        if (x < -1.0 || x > 1.0) { set_error("Math Error"); return; }
        res = acos(x);
        if (g_deg_mode) res *= (180.0 / M_PI);
    } else if (strcmp(fn, "atan") == 0) {
        snprintf(g_expr, sizeof(g_expr), "atan(%s)", x_str);
        res = atan(x);
        if (g_deg_mode) res *= (180.0 / M_PI);
    } else if (strcmp(fn, "log") == 0) {
        snprintf(g_expr, sizeof(g_expr), "log10(%s)", x_str);
        if (x <= 0.0) { set_error("Math Error"); return; }
        res = log10(x);
    } else if (strcmp(fn, "ln") == 0) {
        snprintf(g_expr, sizeof(g_expr), "ln(%s)", x_str);
        if (x <= 0.0) { set_error("Math Error"); return; }
        res = log(x);
    } else if (strcmp(fn, "exp") == 0) {
        snprintf(g_expr, sizeof(g_expr), "exp(%s)", x_str);
        res = exp(x);
    } else if (strcmp(fn, "10^x") == 0) {
        snprintf(g_expr, sizeof(g_expr), "10^(%s)", x_str);
        res = pow(10.0, x);
    } else if (strcmp(fn, "x^2") == 0) {
        snprintf(g_expr, sizeof(g_expr), "sqr(%s)", x_str);
        res = x * x;
    } else if (strcmp(fn, "sqrt") == 0) {
        snprintf(g_expr, sizeof(g_expr), "sqrt(%s)", x_str);
        if (x < 0.0) { set_error("Math Error"); return; }
        res = sqrt(x);
    } else if (strcmp(fn, "cbrt") == 0) {
        snprintf(g_expr, sizeof(g_expr), "cbrt(%s)", x_str);
        res = cbrt(x);
    } else if (strcmp(fn, "1/x") == 0) {
        snprintf(g_expr, sizeof(g_expr), "1/(%s)", x_str);
        if (x == 0.0) { set_error("Div by 0"); return; }
        res = 1.0 / x;
    } else if (strcmp(fn, "n!") == 0) {
        snprintf(g_expr, sizeof(g_expr), "(%s)!", x_str);
        if (x < 0.0 && floor(x) == x) { set_error("Math Error"); return; }
        if (x > 170.0) { set_error("Overflow"); return; }
        if (x >= 0.0 && floor(x) == x && x <= 20.0) {
            double f = 1.0;
            for (int i = 2; i <= (int)x; i++) f *= (double)i;
            res = f;
        } else {
            res = tgamma(x + 1.0);
        }
    } else if (strcmp(fn, "%") == 0) {
        if (g_pending_op == OP_ADD || g_pending_op == OP_SUB) {
            res = g_acc * (x / 100.0);
        } else {
            res = x / 100.0;
        }
    } else {
        return;
    }

    format_number(res, g_display, sizeof(g_display));
    if (!g_error) {
        g_ans     = res;
        g_has_ans = true;
    }
    g_new_entry = true;
}

static void activate_key_label(const char *lbl) {
    if (strlen(lbl) == 1 && lbl[0] >= '0' && lbl[0] <= '9') {
        input_digit(lbl[0]);
    } else if (strcmp(lbl, ".") == 0) {
        input_digit('.');
    } else if (strcmp(lbl, "+/-") == 0) {
        toggle_sign();
    } else if (strcmp(lbl, "C") == 0) {
        clear_all();
    } else if (strcmp(lbl, "DEL") == 0) {
        backspace_digit();
    } else if (strcmp(lbl, "+") == 0) {
        start_binary_op(OP_ADD);
    } else if (strcmp(lbl, "-") == 0) {
        start_binary_op(OP_SUB);
    } else if (strcmp(lbl, "*") == 0) {
        start_binary_op(OP_MUL);
    } else if (strcmp(lbl, "/") == 0) {
        start_binary_op(OP_DIV);
    } else if (strcmp(lbl, "x^y") == 0) {
        start_binary_op(OP_POW);
    } else if (strcmp(lbl, "mod") == 0) {
        start_binary_op(OP_MOD);
    } else if (strcmp(lbl, "=") == 0) {
        press_equals();
    } else if (strcmp(lbl, "MC") == 0) {
        g_memory = 0.0;
    } else if (strcmp(lbl, "MR") == 0) {
        if (g_error) clear_all();
        format_number(g_memory, g_display, sizeof(g_display));
        g_new_entry = false;
    } else if (strcmp(lbl, "M+") == 0) {
        if (!g_error) {
            g_memory += parse_display();
            g_new_entry = true;
        }
    } else if (strcmp(lbl, "M-") == 0) {
        if (!g_error) {
            g_memory -= parse_display();
            g_new_entry = true;
        }
    } else if (strcmp(lbl, "ANS") == 0) {
        if (g_error) clear_all();
        format_number(g_ans, g_display, sizeof(g_display));
        g_new_entry = false;
    } else if (strcmp(lbl, "pi") == 0) {
        if (g_error) clear_all();
        format_number(M_PI, g_display, sizeof(g_display));
        g_new_entry = false;
    } else if (strcmp(lbl, "e") == 0) {
        if (g_error) clear_all();
        format_number(M_E, g_display, sizeof(g_display));
        g_new_entry = false;
    } else if (strcmp(lbl, "(") == 0) {
        push_open_paren();
    } else if (strcmp(lbl, ")") == 0) {
        pop_close_paren();
    } else {
        apply_unary(lbl);
    }
}

static void toggle_calc_mode(void) {
    if (g_mode == MODE_BASIC) {
        g_mode = MODE_SCI;
        if (g_cur_col >= 1) g_cur_col += 3;
        else                g_cur_col = 3;
        if (g_cur_col >= SCI_COLS) g_cur_col = SCI_COLS - 1;
    } else {
        g_mode = MODE_BASIC;
        if (g_cur_col >= 4) g_cur_col -= 3;
        else                g_cur_col = 0;
        if (g_cur_col >= BASIC_COLS) g_cur_col = BASIC_COLS - 1;
    }
}

static KeyCategory classify_key(const char *lbl) {
    if ((strlen(lbl) == 1 && lbl[0] >= '0' && lbl[0] <= '9') ||
        strcmp(lbl, ".") == 0 || strcmp(lbl, "+/-") == 0) {
        return KEY_CAT_DIGIT;
    }
    if (strcmp(lbl, "=") == 0) return KEY_CAT_EQUALS;
    if (strcmp(lbl, "C") == 0 || strcmp(lbl, "DEL") == 0) return KEY_CAT_CLEAR;
    if (strcmp(lbl, "+") == 0 || strcmp(lbl, "-") == 0 ||
        strcmp(lbl, "*") == 0 || strcmp(lbl, "/") == 0) {
        return KEY_CAT_OP;
    }
    if (strcmp(lbl, "MC") == 0 || strcmp(lbl, "MR") == 0 ||
        strcmp(lbl, "M+") == 0 || strcmp(lbl, "M-") == 0 ||
        strcmp(lbl, "%") == 0  || strcmp(lbl, "ANS") == 0) {
        return KEY_CAT_MEM;
    }
    return KEY_CAT_SCI;
}

static bool is_pending_op_key(const char *lbl) {
    if (g_pending_op == OP_ADD && strcmp(lbl, "+") == 0) return true;
    if (g_pending_op == OP_SUB && strcmp(lbl, "-") == 0) return true;
    if (g_pending_op == OP_MUL && strcmp(lbl, "*") == 0) return true;
    if (g_pending_op == OP_DIV && strcmp(lbl, "/") == 0) return true;
    if (g_pending_op == OP_POW && strcmp(lbl, "x^y") == 0) return true;
    if (g_pending_op == OP_MOD && strcmp(lbl, "mod") == 0) return true;
    return false;
}

/* ============================================================================
 * 4. NATIVE 640x480 UI RENDERING
 * ============================================================================ */

static void render_help_overlay(void) {
    fill_rect_alpha(0, 0, NATIVE_W, NATIVE_H, RGB(8, 12, 22), 210);

    int mx = 24, my = 14, mw = NATIVE_W - 48, mh = NATIVE_H - 28;
    fill_rect(mx, my, mw, mh, RGB(17, 24, 39));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    fill_rect(mx + 4, my + 4, mw - 8, 40, RGB(28, 39, 60));
    fill_rect(mx + 4, my + 44, mw - 8, 2, RGB(64, 82, 115));
    draw_pill(mx + 14, my + 10, 68, 26, RGB(37, 99, 235), RGB(147, 197, 253));
    draw_text_centered(FONT_13, mx + 14, my + 10, 68, 26, "HELP", RGB(255, 255, 255));
    draw_text(FONT_20, mx + 92, my + 11, "CALCULATOR - QUICK GUIDE & CONTROLS", RGB(248, 250, 252));

    fill_rect(mx + 4, my + 46, mw - 8, 24, RGB(14, 21, 36));
    fill_rect(mx + 4, my + 70, mw - 8, 1, RGB(45, 62, 92));
    draw_text_centered(FONT_13, mx + 8, my + 46, mw - 16, 24,
                       "Desk (5x5) & Scientific (5x8) calculator with DEG/RAD trig, MEM, ANS & ( ).",
                       RGB(186, 230, 253));

    static const struct {
        const char *btn;
        const char *desc;
    } k_rows[9] = {
        { "D-PAD",     "Navigate Keypad Grid (Wraps Around Edges)" },
        { "A Button",  "Press Highlighted Key" },
        { "B Button",  "Backspace / Delete Last Digit (DEL)" },
        { "X Button",  "Evaluate Result (=)" },
        { "SELECT",    "Switch BASIC (5x5) <-> SCI (5x8) Mode" },
        { "L1 / R1",   "L1: Toggle DEG / RAD   |   R1: Clear All (AC)" },
        { "L2 / R2",   "L2: Open '('   |   R2: Close ')' Parenthesis" },
        { "Y Button",  "Open / Close This Help Overlay" },
        { "MENU",      "Quit Calculator" }
    };

    int ky = my + 78;
    for (int i = 0; i < 9; i++) {
        int ry = ky + i * 33;
        draw_pill(mx + 16, ry, 116, 26, RGB(30, 58, 95), RGB(56, 189, 248));
        draw_text_centered(FONT_13, mx + 16, ry, 116, 26, k_rows[i].btn, RGB(254, 240, 138));
        draw_text(FONT_16, mx + 144, ry + 3, k_rows[i].desc, RGB(226, 232, 240));
    }

    fill_rect(mx + 16, my + mh - 66, mw - 32, 24, RGB(22, 33, 52));
    draw_border(mx + 16, my + mh - 66, mw - 32, 24, 1, RGB(51, 65, 85));
    draw_text_centered(FONT_13, mx + 16, my + mh - 66, mw - 32, 24,
                       "Tip: Press X anytime to evaluate (=); press R1 to Clear All (AC).",
                       RGB(254, 240, 138));

    fill_rect(mx + 4, my + mh - 34, mw - 8, 30, RGB(12, 18, 30));
    fill_rect(mx + 4, my + mh - 34, mw - 8, 1, RGB(51, 65, 85));
    draw_text_centered(FONT_13, mx, my + mh - 33, mw, 28,
                       "Press [Y] or [B] to return to the Calculator",
                       RGB(186, 230, 253));
}

static void render_ui(void) {
    /* 1. Midnight-slate chassis background */
    fill_rect(0, 0, NATIVE_W, NATIVE_H, RGB(13, 18, 29));

    /* 2. Edge-to-Edge 640x136 Top Expression & Hero Result Display */
    fill_vgradient(0, 0, NATIVE_W, 136, RGB(19, 27, 45), RGB(28, 40, 65));
    fill_rect(0, 136, NATIVE_W, 2, RGB(56, 78, 114));

    /* Top Status Bar Badges (y=10..36) */
    if (g_mode == MODE_BASIC) {
        draw_pill(14, 10, 72, 26, RGB(37, 99, 235), RGB(96, 165, 250));
        draw_text_centered(FONT_13, 14, 10, 72, 26, "BASIC", RGB(255, 255, 255));

        draw_pill(92, 10, 56, 26, RGB(15, 23, 42), RGB(56, 74, 106));
        draw_text_centered(FONT_13, 92, 10, 56, 26, "SCI", RGB(100, 116, 139));
    } else {
        draw_pill(14, 10, 72, 26, RGB(15, 23, 42), RGB(56, 74, 106));
        draw_text_centered(FONT_13, 14, 10, 72, 26, "BASIC", RGB(100, 116, 139));

        draw_pill(92, 10, 56, 26, RGB(124, 58, 237), RGB(167, 139, 250));
        draw_text_centered(FONT_13, 92, 10, 56, 26, "SCI", RGB(255, 255, 255));
    }

    /* DEG / RAD Angle Mode Pill */
    draw_pill(156, 10, 62, 26,
              g_deg_mode ? RGB(14, 74, 110) : RGB(120, 53, 15),
              g_deg_mode ? RGB(56, 189, 248) : RGB(251, 191, 36));
    draw_text_centered(FONT_13, 156, 10, 62, 26,
                       g_deg_mode ? "DEG" : "RAD",
                       g_deg_mode ? RGB(186, 230, 253) : RGB(254, 240, 138));

    int bx = 226;
    if (fabs(g_memory) > 1e-15) {
        draw_pill(bx, 10, 46, 26, RGB(180, 83, 9), RGB(251, 191, 36));
        draw_text_centered(FONT_13, bx, 10, 46, 26, "MEM", RGB(255, 255, 255));
        bx += 54;
    }
    if (g_has_ans) {
        draw_pill(bx, 10, 48, 26, RGB(15, 84, 72), RGB(52, 211, 153));
        draw_text_centered(FONT_13, bx, 10, 48, 26, "ANS", RGB(209, 250, 229));
        bx += 56;
    }
    if (g_paren_depth > 0) {
        char pstr[16];
        snprintf(pstr, sizeof(pstr), "(%d)", g_paren_depth);
        draw_pill(bx, 10, 44, 26, RGB(76, 29, 149), RGB(192, 132, 252));
        draw_text_centered(FONT_13, bx, 10, 44, 26, pstr, RGB(243, 232, 255));
    }

    /* Right-aligned Expression History Line (20px) */
    if (g_expr[0] != '\0') {
        int expr_font = (text_width(FONT_20, g_expr) > 600) ? FONT_16 : FONT_20;
        draw_text_right(expr_font, NATIVE_W - 18, 44, g_expr, RGB(148, 163, 184));
    }

    /* Main Large Numeric Readout (48px Bold Monospace, auto-scaling for long numbers) */
    int disp_font = FONT_48;
    int disp_y    = 72;
    if (text_width(FONT_48, g_display) > NATIVE_W - 36) {
        disp_font = FONT_36;
        disp_y    = 80;
    }
    if (text_width(disp_font, g_display) > NATIVE_W - 36) {
        disp_font = FONT_26;
        disp_y    = 88;
    }
    uint32_t disp_col = g_error ? RGB(248, 113, 113) : RGB(248, 250, 252);
    draw_text_right(disp_font, NATIVE_W - 18, disp_y, g_display, disp_col);

    /* 3. Color-Coded 3D Beveled Keypad Grid (y=144..444) */
    int rows      = (g_mode == MODE_BASIC) ? BASIC_ROWS : SCI_ROWS;
    int cols      = (g_mode == MODE_BASIC) ? BASIC_COLS : SCI_COLS;
    int grid_top  = 144;
    int grid_h    = 298;
    int gap       = 6;
    int cell_h    = (grid_h - (rows - 1) * gap) / rows; /* 54px */
    int total_w   = NATIVE_W - 16;                      /* 624px */
    int cell_w    = (total_w - (cols - 1) * gap) / cols;
    int grid_left = (NATIVE_W - (cols * cell_w + (cols - 1) * gap)) / 2;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const char *lbl = (g_mode == MODE_BASIC) ? k_basic_grid[r][c] : k_sci_grid[r][c];
            int kx = grid_left + c * (cell_w + gap);
            int ky = grid_top  + r * (cell_h + gap);
            bool focused   = (r == g_cur_row && c == g_cur_col);
            bool active_op = is_pending_op_key(lbl);

            KeyCategory cat = classify_key(lbl);
            uint32_t base_col = RGB(42, 54, 78);
            uint32_t txt_col  = RGB(248, 250, 252);

            switch (cat) {
                case KEY_CAT_DIGIT:
                    base_col = RGB(42, 54, 78);
                    txt_col  = RGB(248, 250, 252);
                    break;
                case KEY_CAT_OP:
                    base_col = active_op ? RGB(59, 130, 246) : RGB(29, 78, 216);
                    txt_col  = RGB(255, 255, 255);
                    break;
                case KEY_CAT_EQUALS:
                    base_col = RGB(5, 150, 105);
                    txt_col  = RGB(255, 255, 255);
                    break;
                case KEY_CAT_CLEAR:
                    base_col = (strcmp(lbl, "C") == 0) ? RGB(185, 28, 28) : RGB(145, 32, 46);
                    txt_col  = RGB(254, 242, 242);
                    break;
                case KEY_CAT_MEM:
                    base_col = RGB(30, 41, 62);
                    txt_col  = RGB(251, 191, 36);
                    break;
                case KEY_CAT_SCI:
                    base_col = active_op ? RGB(99, 102, 241) : RGB(52, 39, 96);
                    txt_col  = RGB(224, 231, 255);
                    break;
            }
            if (focused) {
                txt_col = RGB(255, 255, 255);
            }

            draw_beveled_key(kx, ky, cell_w, cell_h, base_col, focused);

            int key_font;
            if (g_mode == MODE_BASIC) {
                key_font = (strlen(lbl) <= 2) ? FONT_26 : FONT_20;
            } else {
                key_font = (strlen(lbl) <= 1) ? FONT_20 : FONT_16;
            }
            draw_text_centered(key_font, kx, ky, cell_w, cell_h, lbl, txt_col);
        }
    }

    /* 4. Bottom Legend Bar (y=448..480) */
    fill_rect(0, 448, NATIVE_W, 32, RGB(11, 15, 25));
    fill_rect(0, 448, NATIVE_W, 2,  RGB(38, 52, 76));

    draw_text(FONT_13, 12, 457,
              "A:Press  B:DEL  X:=  SEL:Basic/Sci  L1:DEG/RAD  R1:AC  MENU:Quit",
              RGB(165, 180, 202));

    draw_pill(NATIVE_W - 88, 452, 78, 24, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_13, NATIVE_W - 88, 452, 78, 24, "Y: Help", RGB(254, 240, 138));

    if (g_show_help) {
        render_help_overlay();
    }
}

/* ============================================================================
 * 5. HEADLESS SELF-TEST & SCREENSHOT GENERATOR
 * ============================================================================ */

static int run_verify_tests(void) {
    int fails = 0;

    /* Test 1: Basic arithmetic 128 * 45 = 5760 */
    clear_all();
    activate_key_label("1"); activate_key_label("2"); activate_key_label("8");
    activate_key_label("*");
    activate_key_label("4"); activate_key_label("5");
    activate_key_label("=");
    if (strcmp(g_display, "5760") != 0) {
        fprintf(stderr, "FAIL: 128*45 got %s\n", g_display);
        fails++;
    }

    /* Test 2: Parentheses (2 + 3) * 4 = 20 */
    clear_all();
    push_open_paren();
    activate_key_label("2"); activate_key_label("+"); activate_key_label("3");
    pop_close_paren();
    activate_key_label("*"); activate_key_label("4"); activate_key_label("=");
    if (strcmp(g_display, "20") != 0) {
        fprintf(stderr, "FAIL: (2+3)*4 got %s\n", g_display);
        fails++;
    }

    /* Test 3: ANS recall + Memory */
    activate_key_label("M+");
    clear_all();
    activate_key_label("ANS"); activate_key_label("+"); activate_key_label("MR"); activate_key_label("=");
    if (strcmp(g_display, "40") != 0) {
        fprintf(stderr, "FAIL: ANS+MR got %s\n", g_display);
        fails++;
    }
    activate_key_label("MC");

    /* Test 4: Trig in DEG (sin(30) = 0.5) and RAD (cos(pi) = -1) */
    g_deg_mode = true;
    clear_all();
    activate_key_label("3"); activate_key_label("0"); activate_key_label("sin");
    if (fabs(parse_display() - 0.5) > 1e-9) {
        fprintf(stderr, "FAIL: sin(30 deg) got %s\n", g_display);
        fails++;
    }
    g_deg_mode = false;
    clear_all();
    activate_key_label("pi"); activate_key_label("cos");
    if (fabs(parse_display() - (-1.0)) > 1e-9) {
        fprintf(stderr, "FAIL: cos(pi rad) got %s\n", g_display);
        fails++;
    }

    /* Test 5: Factorial 6! = 720, sqrt(81) = 9, cbrt(27) = 3, log10(1000) = 3 */
    clear_all(); activate_key_label("6"); activate_key_label("n!");
    if (strcmp(g_display, "720") != 0) { fprintf(stderr, "FAIL: 6! got %s\n", g_display); fails++; }
    clear_all(); activate_key_label("8"); activate_key_label("1"); activate_key_label("sqrt");
    if (strcmp(g_display, "9") != 0) { fprintf(stderr, "FAIL: sqrt(81) got %s\n", g_display); fails++; }
    clear_all(); activate_key_label("2"); activate_key_label("7"); activate_key_label("cbrt");
    if (strcmp(g_display, "3") != 0) { fprintf(stderr, "FAIL: cbrt(27) got %s\n", g_display); fails++; }
    clear_all(); activate_key_label("1"); activate_key_label("0"); activate_key_label("0"); activate_key_label("0");
    activate_key_label("log");
    if (strcmp(g_display, "3") != 0) { fprintf(stderr, "FAIL: log(1000) got %s\n", g_display); fails++; }

    /* Test 6: Division by zero */
    clear_all(); activate_key_label("5"); activate_key_label("/"); activate_key_label("0"); activate_key_label("=");
    if (!g_error || strcmp(g_display, "Div by 0") != 0) {
        fprintf(stderr, "FAIL: 5/0 got %s\n", g_display);
        fails++;
    }

    if (fails == 0) {
        printf("[OK] calc verification passed all 6 test suites (arithmetic, parens, ANS/MEM, DEG/RAD trig, sci, div0)\n");
    }
    return fails ? 1 : 0;
}

static int run_screenshots(const char *dir) {
    char path[512];

    /* 1. BASIC Mode Screenshot */
    g_mode      = MODE_BASIC;
    g_deg_mode  = true;
    g_show_help = false;
    clear_all();
    activate_key_label("1"); activate_key_label("2"); activate_key_label("8");
    activate_key_label("*");
    activate_key_label("4"); activate_key_label("5");
    activate_key_label("=");
    g_memory  = 5760.0;
    g_cur_row = 4;
    g_cur_col = 4; /* '=' focused */
    render_ui();
    snprintf(path, sizeof(path), "%s/calc_basic.bmp", dir);
    save_bmp(path);

    /* 2. SCI Mode Screenshot */
    toggle_calc_mode();
    g_deg_mode  = true;
    g_show_help = false;
    clear_all();
    g_memory  = 42.0;
    activate_key_label("4"); activate_key_label("5");
    activate_key_label("sin");
    g_cur_row = 0;
    g_cur_col = 0; /* 'sin' focused */
    render_ui();
    snprintf(path, sizeof(path), "%s/calc_sci.bmp", dir);
    save_bmp(path);

    /* 3. Help Overlay Screenshot */
    g_show_help = true;
    render_ui();
    snprintf(path, sizeof(path), "%s/calc_help.bmp", dir);
    save_bmp(path);

    printf("[OK] Saved calc_basic.bmp, calc_sci.bmp, calc_help.bmp to %s\n", dir);
    return 0;
}

/* ============================================================================
 * 6. MAIN ENTRY POINT & EVENT LOOP
 * ============================================================================ */

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0) {
            return run_verify_tests();
        } else if ((strcmp(argv[i], "--screenshot") == 0 ||
                    strcmp(argv[i], "--screenshot-states") == 0) && i + 1 < argc) {
            return run_screenshots(argv[i + 1]);
        }
    }

    if (!platform_init("Anbernic RG35XXSP Calculator (640x480)")) {
        fprintf(stderr, "Failed to initialize platform backend\n");
        return 1;
    }

    input_state_t input;
    while (!g_sig_quit && platform_poll_input(&input)) {
        uint32_t p = input.pressed;

        /* MENU (Esc on desktop): SOLE quit key */
        if (p & BTN_MENU) {
            break;
        }

        /* Y: Toggle English Help overlay */
        if (p & BTN_Y) {
            g_show_help = !g_show_help;
        } else if (g_show_help) {
            if (p & (BTN_B | BTN_A | BTN_SELECT)) {
                g_show_help = false;
            }
        } else {
            int rows = (g_mode == MODE_BASIC) ? BASIC_ROWS : SCI_ROWS;
            int cols = (g_mode == MODE_BASIC) ? BASIC_COLS : SCI_COLS;

            if (p & BTN_SELECT) {
                toggle_calc_mode();
            } else if (p & BTN_UP) {
                g_cur_row = (g_cur_row + rows - 1) % rows;
            } else if (p & BTN_DOWN) {
                g_cur_row = (g_cur_row + 1) % rows;
            } else if (p & BTN_LEFT) {
                g_cur_col = (g_cur_col + cols - 1) % cols;
            } else if (p & BTN_RIGHT) {
                g_cur_col = (g_cur_col + 1) % cols;
            } else if (p & BTN_A) {
                const char *lbl = (g_mode == MODE_BASIC)
                                  ? k_basic_grid[g_cur_row][g_cur_col]
                                  : k_sci_grid[g_cur_row][g_cur_col];
                activate_key_label(lbl);
            } else if (p & BTN_B) {
                backspace_digit();
            } else if (p & BTN_X) {
                press_equals();
            } else if (p & BTN_L1) {
                g_deg_mode = !g_deg_mode;
            } else if (p & BTN_R1) {
                clear_all();
            } else if (p & BTN_L2) {
                push_open_paren();
            } else if (p & BTN_R2) {
                pop_close_paren();
            } else if (input.direct_char != 0) {
                int ch = input.direct_char;
                if (ch >= '0' && ch <= '9') input_digit((char)ch);
                else if (ch == '.') input_digit('.');
                else if (ch == '+') start_binary_op(OP_ADD);
                else if (ch == '-') start_binary_op(OP_SUB);
                else if (ch == '*') start_binary_op(OP_MUL);
                else if (ch == '/') start_binary_op(OP_DIV);
                else if (ch == '^') start_binary_op(OP_POW);
                else if (ch == '%') apply_unary("%");
                else if (ch == '=') press_equals();
                else if (ch == '(') push_open_paren();
                else if (ch == ')') pop_close_paren();
            }
        }

        render_ui();
        platform_present();
        delay_ms(16);
    }

    platform_shutdown();
    return 0;
}
