/*
 * bard.c - Native 640x480 Lightweight EPUB & Text E-Book Reader
 *          for Anbernic RG 35xxSP (Allwinner H700 aarch64) & Linux Desktop (x86_64 X11)
 *
 * Zero Third-Party .so Dependencies:
 *   - Statically compiles ../third_party/miniz.c to read .epub ZIP containers in memory.
 *   - Built-in pure-C XML / XHTML DOM parser supporting META-INF/container.xml,
 *     OPF metadata/manifest/spine, EPUB 2 toc.ncx, EPUB 3 <nav epub:type="toc">,
 *     fragment boundaries, HTML/XML entity decoding, and UTF-8 normalization.
 *   - Also supports plain .txt e-books cleanly.
 *
 * Controls (RG 35xxSP / Desktop X11):
 *   D-Pad UP / DOWN (or L1 / R1) : Scroll one whole page backward / forward (or navigate lists)
 *   A (A / Space / Z)            : Open book / enter folder / jump to selected chapter
 *   B (B / Backspace)            : Reading: Light/Dark theme | Modals: Back | Browser: Up Dir
 *   X (X)                        : Open File Browser
 *   Y (Y)                        : Toggle English Help & Reading Progress overlay
 *   SELECT (Tab)                 : Toggle Chapter List (Table of Contents) / Return to Book
 *   MENU (Esc)                   : Save reading position & Exit (sole quit key)
 */

#include "../rg35xxsp_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <dlfcn.h>

#include "../third_party/miniz.h"
#include "bard_font.h"

#define NATIVE_W 640
#define NATIVE_H 480

typedef struct {
    uint32_t held;
    uint32_t pressed;
    uint32_t released;
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

static uint32_t xkey_to_btn(XKeyEvent *xkey) {
    char buf[8] = {0};
    KeySym sym = NoSymbol;
    XLookupString(xkey, buf, sizeof(buf) - 1, &sym, NULL);
    if (sym == NoSymbol) sym = XLookupKeysym(xkey, 0);
    uint32_t mask = rg35xxsp_map_x11_keysym(sym);
    if (!mask) {
        KeySym base = XLookupKeysym(xkey, 0);
        if (base != NoSymbol) mask = rg35xxsp_map_x11_keysym(base);
    }
    return mask;
}

static bool platform_poll_input(input_state_t *out) {
    if (!g_x11_dpy) return false;
    uint32_t pressed_latch  = 0;
    uint32_t released_latch = 0;

    while (XPending(g_x11_dpy) > 0) {
        XEvent xev;
        XNextEvent(g_x11_dpy, &xev);
        if (xev.type == ClientMessage) {
            if ((Atom)xev.xclient.data.l[0] == g_wm_delete_window) return false;
        } else if (xev.type == MapNotify || xev.type == ButtonPress) {
            XSetInputFocus(g_x11_dpy, g_x11_win, RevertToParent, CurrentTime);
        } else if (xev.type == KeyPress) {
            uint32_t mask = xkey_to_btn(&xev.xkey);
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
            uint32_t mask = xkey_to_btn(&xev.xkey);
            if (mask) {
                released_latch |= (mask & g_cur_buttons);
                g_cur_buttons  &= ~mask;
            }
        }
    }
    out->held      = g_cur_buttons;
    out->pressed   = pressed_latch | (g_cur_buttons & ~g_prev_buttons);
    out->released  = released_latch | (~g_cur_buttons & g_prev_buttons);
    g_prev_buttons = g_cur_buttons;
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

#else /* TARGET_RG35XXSP */

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

    out->held      = g_cur_buttons;
    out->pressed   = pressed_latch | (g_cur_buttons & ~g_prev_buttons);
    out->released  = released_latch | (~g_cur_buttons & g_prev_buttons);
    g_prev_buttons = g_cur_buttons;
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
 * 2. 640x480 ANTI-ALIASED DRAWING & TYPOGRAPHY PRIMITIVES
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

static void draw_border(int x, int y, int w, int h, int thick, uint32_t color) {
    fill_rect(x, y, w, thick, color);
    fill_rect(x, y + h - thick, w, thick, color);
    fill_rect(x, y, thick, h, color);
    fill_rect(x + w - thick, y, thick, h, color);
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

static int char_advance(int font_id, char c) {
    const BardFont *f = &g_bard_fonts[font_id];
    unsigned char uc = (unsigned char)c;
    if (uc < 32 || uc > 126) uc = '?';
    return f->glyphs[uc - 32].advance;
}

static int text_width(int font_id, const char *s) {
    int w = 0;
    while (*s) w += char_advance(font_id, *s++);
    return w;
}

static void draw_text(int font_id, int x, int y, const char *s, uint32_t color) {
    const BardFont *f = &g_bard_fonts[font_id];
    int cur_x = x;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 32 || c > 126) c = '?';
        const BardGlyph *g = &f->glyphs[c - 32];
        const uint8_t *bmp = &g_bard_font_bitmaps[g->offset];
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

static void draw_text_clipped(int font_id, int x, int y, int max_w, const char *s, uint32_t color) {
    if (text_width(font_id, s) <= max_w) {
        draw_text(font_id, x, y, s, color);
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    size_t len = strlen(buf);
    while (len > 3) {
        buf[len - 3] = '.';
        buf[len - 2] = '.';
        buf[len - 1] = '\0';
        if (text_width(font_id, buf) <= max_w) break;
        len--;
    }
    draw_text(font_id, x, y, buf, color);
}

static void draw_text_centered(int font_id, int bx, int by, int bw, int bh, const char *s, uint32_t color) {
    const BardFont *f = &g_bard_fonts[font_id];
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
 * 3. PURE-C XML / XHTML DOM PARSER & HTML ENTITY DECODER (ZERO DEPENDENCIES)
 * ============================================================================ */

typedef struct XmlAttr {
    char *name;
    char *value;
    struct XmlAttr *next;
} XmlAttr;

typedef struct XmlNode {
    bool is_text;
    char *name; /* Element local tag name (namespace prefix stripped), or NULL for text */
    char *text; /* Decoded text content for text nodes */
    XmlAttr *attrs;
    struct XmlNode *first_child;
    struct XmlNode *last_child;
    struct XmlNode *next_sibling;
    struct XmlNode *parent;
} XmlNode;

static void xml_free_node(XmlNode *n) {
    while (n) {
        XmlNode *next = n->next_sibling;
        if (n->first_child) xml_free_node(n->first_child);
        XmlAttr *a = n->attrs;
        while (a) {
            XmlAttr *an = a->next;
            free(a->name);
            free(a->value);
            free(a);
            a = an;
        }
        free(n->name);
        free(n->text);
        free(n);
        n = next;
    }
}

static void append_utf8_cp(char **buf, size_t *len, size_t *cap, uint32_t cp) {
    if (*len + 8 >= *cap) {
        size_t ncap = (*cap == 0) ? 128 : (*cap * 2);
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb) return;
        *buf = nb;
        *cap = ncap;
    }
    if (cp == 0) return;
    if (cp < 0x80) {
        (*buf)[(*len)++] = (char)cp;
    } else if (cp < 0x800) {
        (*buf)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        (*buf)[(*len)++] = (char)(0xE0 | (cp >> 12));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        (*buf)[(*len)++] = (char)(0xF0 | (cp >> 18));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
    (*buf)[*len] = '\0';
}

static uint32_t lookup_named_entity(const char *ent, size_t elen) {
    static const struct { const char *name; uint32_t cp; } k_ents[] = {
        { "amp",    '&' },    { "lt",     '<' },    { "gt",     '>' },
        { "quot",   '"' },    { "apos",   '\'' },   { "nbsp",   0x00A0 },
        { "mdash",  0x2014 }, { "ndash",  0x2013 }, { "ldquo",  0x201C },
        { "rdquo",  0x201D }, { "lsquo",  0x2018 }, { "rsquo",  0x2019 },
        { "hellip", 0x2026 }, { "bull",   0x2022 }, { "middot", 0x00B7 },
        { "copy",   0x00A9 }, { "reg",    0x00AE }, { "trade",  0x2122 },
        { "laquo",  0x00AB }, { "raquo",  0x00BB }, { "deg",    0x00B0 },
        { "sect",   0x00A7 }, { "times",  'x' },    { "divide", '/' },
        { "eacute", 0x00E9 }, { "egrave", 0x00E8 }, { "ecirc",  0x00EA },
        { "aacute", 0x00E1 }, { "agrave", 0x00E0 }, { "acirc",  0x00E2 },
        { "auml",   0x00E4 }, { "iacute", 0x00ED }, { "icirc",  0x00EE },
        { "oacute", 0x00F3 }, { "ocirc",  0x00F4 }, { "ouml",   0x00F6 },
        { "uacute", 0x00FA }, { "uuml",   0x00FC }, { "ntilde", 0x00F1 },
        { "ccedil", 0x00E7 }, { "Eacute", 0x00C9 }, { "Aacute", 0x00C1 },
        { "Oacute", 0x00D3 }, { "Ntilde", 0x00D1 }, { "szlig",  's' },
        { "shy",    0 },      { "thinsp", ' ' },    { "ensp",   ' ' },
        { "emsp",   ' ' },    { NULL, 0 }
    };
    for (int i = 0; k_ents[i].name; i++) {
        if (strlen(k_ents[i].name) == elen && strncmp(k_ents[i].name, ent, elen) == 0) {
            return k_ents[i].cp;
        }
    }
    /* Case-insensitive fallback for HTML entities */
    for (int i = 0; k_ents[i].name; i++) {
        if (strlen(k_ents[i].name) == elen && strncasecmp(k_ents[i].name, ent, elen) == 0) {
            return k_ents[i].cp;
        }
    }
    return 0xFFFFFFFFu;
}

static char *xml_decode_entities(const char *src, size_t slen) {
    size_t cap = slen + 16;
    size_t len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    size_t i = 0;
    while (i < slen) {
        if (src[i] == '&') {
            size_t semi = i + 1;
            while (semi < slen && semi - i <= 16 && src[semi] != ';' &&
                   src[semi] != '&' && !isspace((unsigned char)src[semi])) {
                semi++;
            }
            if (semi < slen && src[semi] == ';' && semi > i + 1) {
                const char *ent = src + i + 1;
                size_t elen = semi - (i + 1);
                bool decoded = false;
                if (ent[0] == '#' && elen >= 2) {
                    uint32_t cp = 0;
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        for (size_t k = 2; k < elen; k++) {
                            char c = ent[k];
                            cp <<= 4;
                            if (c >= '0' && c <= '9') cp |= (uint32_t)(c - '0');
                            else if (c >= 'a' && c <= 'f') cp |= (uint32_t)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') cp |= (uint32_t)(c - 'A' + 10);
                            else { cp = 0; break; }
                        }
                    } else {
                        for (size_t k = 1; k < elen; k++) {
                            if (ent[k] >= '0' && ent[k] <= '9') {
                                cp = cp * 10u + (uint32_t)(ent[k] - '0');
                            } else { cp = 0; break; }
                        }
                    }
                    if (cp > 0 && cp <= 0x10FFFF) {
                        append_utf8_cp(&out, &len, &cap, cp);
                        decoded = true;
                    }
                } else {
                    uint32_t cp = lookup_named_entity(ent, elen);
                    if (cp != 0xFFFFFFFFu) {
                        if (cp > 0) append_utf8_cp(&out, &len, &cap, cp);
                        decoded = true;
                    }
                }
                if (decoded) {
                    i = semi + 1;
                    continue;
                }
            }
        }
        if (len + 4 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(out, cap);
            if (!nb) { free(out); return NULL; }
            out = nb;
        }
        out[len++] = src[i++];
        out[len] = '\0';
    }
    return out;
}

static bool is_html_void_tag(const char *name) {
    static const char *const k_void[] = {
        "br", "hr", "img", "meta", "link", "input", "area", "base",
        "col", "embed", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; k_void[i]; i++) {
        if (strcasecmp(name, k_void[i]) == 0) return true;
    }
    return false;
}

static char *dup_local_name(const char *s, size_t len) {
    const char *colon = memchr(s, ':', len);
    if (colon && (size_t)(colon - s) + 1 < len) {
        size_t off = (size_t)(colon - s) + 1;
        s += off;
        len -= off;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void xml_append_child(XmlNode *parent, XmlNode *child) {
    child->parent = parent;
    if (!parent->first_child) {
        parent->first_child = parent->last_child = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child = child;
    }
}

static XmlNode *xml_parse_document(const char *data, size_t size) {
    XmlNode *root = (XmlNode *)calloc(1, sizeof(XmlNode));
    if (!root) return NULL;
    root->name = strdup("#document");

    XmlNode *cur = root;
    size_t i = 0;

    /* Skip UTF-8 BOM if present */
    if (size >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        i = 3;
    }

    while (i < size) {
        if (data[i] == '<') {
            /* Comment <!-- ... --> */
            if (i + 4 <= size && strncmp(data + i, "<!--", 4) == 0) {
                const char *end = strstr(data + i + 4, "-->");
                i = end ? (size_t)(end - data) + 3 : size;
                continue;
            }
            /* CDATA <![CDATA[ ... ]]> */
            if (i + 9 <= size && strncmp(data + i, "<![CDATA[", 9) == 0) {
                const char *end = strstr(data + i + 9, "]]>");
                size_t cstart = i + 9;
                size_t cend = end ? (size_t)(end - data) : size;
                if (cend > cstart) {
                    XmlNode *tn = (XmlNode *)calloc(1, sizeof(XmlNode));
                    if (tn) {
                        tn->is_text = true;
                        tn->text = (char *)malloc(cend - cstart + 1);
                        if (tn->text) {
                            memcpy(tn->text, data + cstart, cend - cstart);
                            tn->text[cend - cstart] = '\0';
                        }
                        xml_append_child(cur, tn);
                    }
                }
                i = end ? (cend + 3) : size;
                continue;
            }
            /* Processing instruction <? ... ?> */
            if (i + 2 <= size && data[i + 1] == '?') {
                const char *end = strstr(data + i + 2, "?>");
                i = end ? (size_t)(end - data) + 2 : size;
                continue;
            }
            /* Declaration <!DOCTYPE ...> or <!...> */
            if (i + 2 <= size && data[i + 1] == '!') {
                int depth = 1;
                i += 2;
                while (i < size && depth > 0) {
                    if (data[i] == '<') depth++;
                    else if (data[i] == '>') depth--;
                    i++;
                }
                continue;
            }
            /* Closing tag </name> */
            if (i + 2 <= size && data[i + 1] == '/') {
                i += 2;
                while (i < size && isspace((unsigned char)data[i])) i++;
                size_t nstart = i;
                while (i < size && data[i] != '>' && !isspace((unsigned char)data[i])) i++;
                char *cname = dup_local_name(data + nstart, i - nstart);
                while (i < size && data[i] != '>') i++;
                if (i < size && data[i] == '>') i++;

                if (cname) {
                    /* Unwind stack to matching ancestor if found */
                    XmlNode *anc = cur;
                    while (anc && anc != root) {
                        if (anc->name && strcasecmp(anc->name, cname) == 0) {
                            cur = anc->parent ? anc->parent : root;
                            break;
                        }
                        anc = anc->parent;
                    }
                    free(cname);
                }
                continue;
            }

            /* Opening tag <name attr="val" ... [/]> */
            i++; /* skip '<' */
            while (i < size && isspace((unsigned char)data[i])) i++;
            size_t nstart = i;
            while (i < size && data[i] != '>' && data[i] != '/' &&
                   !isspace((unsigned char)data[i])) {
                i++;
            }
            if (i == nstart) {
                continue;
            }
            XmlNode *el = (XmlNode *)calloc(1, sizeof(XmlNode));
            if (!el) break;
            el->name = dup_local_name(data + nstart, i - nstart);

            bool self_close = false;
            XmlAttr *attr_tail = NULL;
            while (i < size) {
                while (i < size && isspace((unsigned char)data[i])) i++;
                if (i >= size) break;
                if (data[i] == '>') {
                    i++;
                    break;
                }
                if (data[i] == '/') {
                    self_close = true;
                    i++;
                    if (i < size && data[i] == '>') i++;
                    break;
                }
                /* Parse attribute name */
                size_t astart = i;
                while (i < size && data[i] != '=' && data[i] != '>' &&
                       data[i] != '/' && !isspace((unsigned char)data[i])) {
                    i++;
                }
                size_t alen = i - astart;
                while (i < size && isspace((unsigned char)data[i])) i++;
                char *aval = NULL;
                if (i < size && data[i] == '=') {
                    i++;
                    while (i < size && isspace((unsigned char)data[i])) i++;
                    if (i < size && (data[i] == '"' || data[i] == '\'')) {
                        char q = data[i++];
                        size_t vstart = i;
                        while (i < size && data[i] != q) i++;
                        aval = xml_decode_entities(data + vstart, i - vstart);
                        if (i < size && data[i] == q) i++;
                    } else {
                        size_t vstart = i;
                        while (i < size && !isspace((unsigned char)data[i]) &&
                               data[i] != '>' && data[i] != '/') {
                            i++;
                        }
                        aval = xml_decode_entities(data + vstart, i - vstart);
                    }
                } else {
                    aval = strdup("");
                }

                if (alen > 0) {
                    XmlAttr *at = (XmlAttr *)calloc(1, sizeof(XmlAttr));
                    if (at) {
                        at->name  = dup_local_name(data + astart, alen);
                        at->value = aval ? aval : strdup("");
                        if (!el->attrs) el->attrs = attr_tail = at;
                        else { attr_tail->next = at; attr_tail = at; }
                    } else {
                        free(aval);
                    }
                } else {
                    free(aval);
                }
            }

            xml_append_child(cur, el);
            if (!self_close && el->name && !is_html_void_tag(el->name)) {
                /* For raw-text <style>/<script> tags, skip directly to closing tag */
                if (strcasecmp(el->name, "style") == 0 || strcasecmp(el->name, "script") == 0) {
                    char close_pat[32];
                    snprintf(close_pat, sizeof(close_pat), "</%s", el->name);
                    size_t plen = strlen(close_pat);
                    while (i + plen < size) {
                        if (strncasecmp(data + i, close_pat, plen) == 0) {
                            while (i < size && data[i] != '>') i++;
                            if (i < size) i++;
                            break;
                        }
                        i++;
                    }
                } else {
                    cur = el;
                }
            }
        } else {
            /* Text content up to next '<' */
            size_t tstart = i;
            while (i < size && data[i] != '<') i++;
            char *decoded = xml_decode_entities(data + tstart, i - tstart);
            if (decoded && decoded[0] != '\0') {
                XmlNode *tn = (XmlNode *)calloc(1, sizeof(XmlNode));
                if (tn) {
                    tn->is_text = true;
                    tn->text = decoded;
                    xml_append_child(cur, tn);
                } else {
                    free(decoded);
                }
            } else {
                free(decoded);
            }
        }
    }

    return root;
}

static const char *xml_get_attr(const XmlNode *node, const char *attr_name) {
    if (!node || !attr_name) return NULL;
    for (const XmlAttr *a = node->attrs; a; a = a->next) {
        if (a->name && strcasecmp(a->name, attr_name) == 0) {
            return a->value;
        }
    }
    return NULL;
}

static void xml_collect_text_rec(const XmlNode *node, char *out, size_t out_sz, size_t *pos) {
    for (const XmlNode *cur = node; cur && *pos + 1 < out_sz; cur = cur->next_sibling) {
        if (cur->is_text && cur->text) {
            size_t tlen = strlen(cur->text);
            size_t rem  = out_sz - 1 - *pos;
            if (tlen > rem) tlen = rem;
            memcpy(out + *pos, cur->text, tlen);
            *pos += tlen;
            out[*pos] = '\0';
        } else if (cur->first_child) {
            xml_collect_text_rec(cur->first_child, out, out_sz, pos);
        }
    }
}

static void xml_node_text(const XmlNode *el, char *out, size_t out_sz) {
    out[0] = '\0';
    if (!el) return;
    size_t pos = 0;
    xml_collect_text_rec(el->first_child, out, out_sz, &pos);
}

/* ============================================================================
 * 4. BOOK DATA STRUCTURES, UTF-8 NORMALIZATION & 640x480 WORD-WRAP PAGINATOR
 * ============================================================================ */

typedef enum {
    STATE_FILE_BROWSER = 0,
    STATE_READING      = 1,
    STATE_CHAPTER_LIST = 2,
    STATE_HELP         = 3
} AppState;

#define MAX_FILES    512
#define BROWSER_ROWS 11
#define TOC_ROWS     10

typedef struct {
    char  name[256];
    bool  is_dir;
    off_t size;
} FileEntry;

static char      g_cwd[PATH_MAX];
static FileEntry g_files[MAX_FILES];
static int       g_file_count  = 0;
static int       g_file_sel    = 0;
static int       g_file_scroll = 0;

#define MAX_MANIFEST 512
typedef struct {
    char id[128];
    char href[256];
    char media_type[64];
    bool is_nav;
} ManifestItem;

#define MAX_CHAPTERS 256
typedef struct {
    char title[128];
    char href[256];
    char frag[64];
    char end_frag[64];
    int  start_spine;
    int  end_spine;
    bool use_start_frag;
    int  page_count;
} EpubChapter;

typedef struct {
    char *text;
    bool  is_heading;
    bool  is_blank;
} WrappedLine;

/* Native 640x480 book layout: 20px serif font, 26px line step, 17 lines/page */
#define LINES_PER_PAGE 17
#define TEXT_MARGIN_X  28
#define TEXT_MAX_WIDTH (NATIVE_W - 2 * TEXT_MARGIN_X)

static mz_zip_archive g_epub_zip;
static bool           g_epub_zip_open = false;
static bool           g_is_plain_txt  = false;
static char           g_current_book_path[PATH_MAX] = "";
static char           g_state_file_path[PATH_MAX]   = "";
static char           g_book_title[128]  = "";
static char           g_book_author[128] = "";
static char           g_spine_hrefs[MAX_MANIFEST][256];
static int            g_spine_count = 0;
static EpubChapter    g_chapters[MAX_CHAPTERS];
static int            g_chapter_count    = 0;
static int            g_cur_chapter      = 0;
static int            g_book_total_pages = 1;
static int            g_toc_sel          = 0;
static int            g_toc_scroll       = 0;

static WrappedLine   *g_lines       = NULL;
static int            g_line_count  = 0;
static int            g_line_cap    = 0;
static int            g_cur_page    = 0;
static int            g_total_pages = 1;
static bool           g_dark_theme  = false;
static AppState       g_state       = STATE_FILE_BROWSER;
static AppState       g_help_prev_state = STATE_READING;

static void free_wrapped_lines(void) {
    if (g_lines) {
        for (int i = 0; i < g_line_count; i++) {
            free(g_lines[i].text);
        }
        free(g_lines);
        g_lines = NULL;
    }
    g_line_count  = 0;
    g_line_cap    = 0;
    g_cur_page    = 0;
    g_total_pages = 1;
}

static void append_wrapped_line(const char *s, size_t len, bool is_heading, bool is_blank) {
    if (is_blank && g_line_count == 0) return;
    if (is_blank && g_line_count > 0 && g_lines[g_line_count - 1].is_blank) return;
    if (is_blank && (g_line_count % LINES_PER_PAGE) == 0) return;

    if (g_line_count >= g_line_cap) {
        int new_cap = (g_line_cap == 0) ? 256 : (g_line_cap * 2);
        WrappedLine *nl = (WrappedLine *)realloc(g_lines, (size_t)new_cap * sizeof(WrappedLine));
        if (!nl) return;
        g_lines = nl;
        g_line_cap = new_cap;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) return;
    memcpy(copy, s, len);
    copy[len] = '\0';
    g_lines[g_line_count].text       = copy;
    g_lines[g_line_count].is_heading = is_heading;
    g_lines[g_line_count].is_blank   = is_blank;
    g_line_count++;
}

static void normalize_utf8_text(const char *in, char *out, size_t out_sz) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    bool last_space = true;

    while (*p && o + 4 < out_sz) {
        if (*p < 0x80) {
            char c = (char)*p++;
            if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
                if (!last_space) {
                    out[o++] = ' ';
                    last_space = true;
                }
            } else if (c >= 32 && c <= 126) {
                out[o++] = c;
                last_space = false;
            }
        } else if (p[0] == 0xC2 && p[1] == 0xA0) {
            if (!last_space) {
                out[o++] = ' ';
                last_space = true;
            }
            p += 2;
        } else if ((p[0] == 0xC2 || p[0] == 0xC3) && p[1]) {
            unsigned int cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
            char mapped = '?';
            if (cp >= 0xC0 && cp <= 0xC5) mapped = 'A';
            else if (cp == 0xC7) mapped = 'C';
            else if (cp >= 0xC8 && cp <= 0xCB) mapped = 'E';
            else if (cp >= 0xCC && cp <= 0xCF) mapped = 'I';
            else if (cp == 0xD1) mapped = 'N';
            else if (cp >= 0xD2 && cp <= 0xD6) mapped = 'O';
            else if (cp >= 0xD9 && cp <= 0xDC) mapped = 'U';
            else if (cp >= 0xE0 && cp <= 0xE5) mapped = 'a';
            else if (cp == 0xE7) mapped = 'c';
            else if (cp >= 0xE8 && cp <= 0xEB) mapped = 'e';
            else if (cp >= 0xEC && cp <= 0xEF) mapped = 'i';
            else if (cp == 0xF1) mapped = 'n';
            else if (cp >= 0xF2 && cp <= 0xF6) mapped = 'o';
            else if (cp >= 0xF9 && cp <= 0xFC) mapped = 'u';
            else if (cp == 0xA9) mapped = 'c';
            else if (cp == 0xAB || cp == 0xBB) mapped = '"';
            else if (cp == 0xB7) mapped = '*';
            if (mapped != '?') {
                out[o++] = mapped;
                last_space = false;
            }
            p += 2;
        } else if (p[0] == 0xE2 && p[1] == 0x80 && p[2]) {
            unsigned char b2 = p[2];
            if (b2 == 0x93 || b2 == 0x94) {
                out[o++] = '-'; out[o++] = '-';
                last_space = false;
            } else if (b2 == 0x98 || b2 == 0x99) {
                out[o++] = '\'';
                last_space = false;
            } else if (b2 == 0x9C || b2 == 0x9D) {
                out[o++] = '"';
                last_space = false;
            } else if (b2 == 0xA6) {
                out[o++] = '.'; out[o++] = '.'; out[o++] = '.';
                last_space = false;
            } else if (b2 == 0xA2) {
                out[o++] = '*';
                last_space = false;
            }
            p += 3;
        } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
            p += 3;
        } else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            p += 4;
        } else {
            p++;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

static void wrap_paragraph_ex(const char *raw_text, bool is_heading, bool add_blank_after) {
    if (!raw_text || !*raw_text) return;
    size_t raw_len = strlen(raw_text);
    size_t norm_sz = raw_len * 2 + 16;
    char *norm = (char *)malloc(norm_sz);
    if (!norm) return;
    normalize_utf8_text(raw_text, norm, norm_sz);
    if (norm[0] == '\0') {
        free(norm);
        return;
    }

    int font_id = is_heading ? FONT_BOOK_BOLD22 : FONT_BOOK_20;
    int space_w = char_advance(font_id, ' ');

    char line_buf[512];
    size_t line_len = 0;
    int line_w = 0;

    const char *p = norm;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        const char *w_start = p;
        int word_w = 0;
        while (*p && *p != ' ') {
            word_w += char_advance(font_id, *p);
            p++;
        }
        size_t word_len = (size_t)(p - w_start);

        if (word_w > TEXT_MAX_WIDTH) {
            if (line_len > 0) {
                append_wrapped_line(line_buf, line_len, is_heading, false);
                line_len = 0;
                line_w   = 0;
            }
            size_t wi = 0;
            while (wi < word_len) {
                line_len = 0;
                line_w   = 0;
                while (wi < word_len) {
                    int cw = char_advance(font_id, w_start[wi]);
                    if (line_w + cw > TEXT_MAX_WIDTH && line_len > 0) break;
                    if (line_len + 1 < sizeof(line_buf)) {
                        line_buf[line_len++] = w_start[wi];
                    }
                    line_w += cw;
                    wi++;
                }
                append_wrapped_line(line_buf, line_len, is_heading, false);
            }
            line_len = 0;
            line_w   = 0;
            continue;
        }

        int needed_w = (line_len == 0) ? word_w : (line_w + space_w + word_w);
        if (needed_w <= TEXT_MAX_WIDTH && (line_len + 1 + word_len < sizeof(line_buf))) {
            if (line_len > 0) {
                line_buf[line_len++] = ' ';
                line_w += space_w;
            }
            memcpy(line_buf + line_len, w_start, word_len);
            line_len += word_len;
            line_w   += word_w;
        } else {
            append_wrapped_line(line_buf, line_len, is_heading, false);
            if (word_len >= sizeof(line_buf)) word_len = sizeof(line_buf) - 1;
            memcpy(line_buf, w_start, word_len);
            line_len = word_len;
            line_w   = word_w;
        }
    }

    if (line_len > 0) {
        append_wrapped_line(line_buf, line_len, is_heading, false);
    }
    if (add_blank_after) {
        append_wrapped_line("", 0, false, true);
    }

    free(norm);
}

static void wrap_paragraph(const char *raw_text, bool is_heading) {
    wrap_paragraph_ex(raw_text, is_heading, true);
}

/* ============================================================================
 * 5. EPUB CONTAINER (miniz) & TOC / XHTML EXTRACTION
 * ============================================================================ */

static void close_epub_zip(void) {
    if (g_epub_zip_open) {
        mz_zip_reader_end(&g_epub_zip);
        g_epub_zip_open = false;
    }
}

static char *read_zip_entry(const char *entry_path, size_t *out_size) {
    if (!g_epub_zip_open || !entry_path || !*entry_path) return NULL;
    int idx = mz_zip_reader_locate_file(&g_epub_zip, entry_path, NULL, 0);
    if (idx < 0) {
        /* Explicit case-insensitive scan fallback */
        mz_uint n = mz_zip_reader_get_num_files(&g_epub_zip);
        char fname[512];
        for (mz_uint i = 0; i < n; i++) {
            if (mz_zip_reader_get_filename(&g_epub_zip, i, fname, sizeof(fname)) > 0) {
                if (strcasecmp(fname, entry_path) == 0) {
                    idx = (int)i;
                    break;
                }
            }
        }
    }
    if (idx < 0) return NULL;

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&g_epub_zip, (mz_uint)idx, &st)) return NULL;
    size_t sz = (size_t)st.m_uncomp_size;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return NULL;
    if (sz > 0 && !mz_zip_reader_extract_to_mem(&g_epub_zip, (mz_uint)idx, buf, sz, 0)) {
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    if (out_size) *out_size = sz;
    return buf;
}

static void extract_fragment(const char *href, char *frag_out, size_t frag_sz) {
    frag_out[0] = '\0';
    if (!href) return;
    const char *hash = strchr(href, '#');
    if (hash && hash[1] != '\0') {
        snprintf(frag_out, frag_sz, "%s", hash + 1);
    }
}

static void resolve_relative_path(const char *base_dir, const char *href, char *out, size_t out_sz) {
    if (!href) {
        out[0] = '\0';
        return;
    }
    char clean_href[256];
    size_t ci = 0;
    for (size_t i = 0; href[i] && href[i] != '#' && ci + 1 < sizeof(clean_href); i++) {
        if (href[i] == '%' && isxdigit((unsigned char)href[i + 1]) && isxdigit((unsigned char)href[i + 2])) {
            char hex[3] = { href[i + 1], href[i + 2], '\0' };
            clean_href[ci++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            clean_href[ci++] = href[i];
        }
    }
    clean_href[ci] = '\0';

    char combined[512];
    if (base_dir && base_dir[0] != '\0' && clean_href[0] != '/') {
        snprintf(combined, sizeof(combined), "%s/%s", base_dir, clean_href);
    } else {
        const char *s = (clean_href[0] == '/') ? (clean_href + 1) : clean_href;
        snprintf(combined, sizeof(combined), "%s", s);
    }

    char *parts[64];
    int nparts = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(combined, "/", &saveptr);
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (nparts > 0) nparts--;
        } else if (nparts < 64) {
            parts[nparts++] = tok;
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }

    out[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < nparts; i++) {
        int written = snprintf(out + pos, out_sz - pos, "%s%s", (i > 0) ? "/" : "", parts[i]);
        if (written < 0 || (size_t)written >= out_sz - pos) break;
        pos += (size_t)written;
    }
}

static void extract_dirname(const char *path, char *dir_out, size_t dir_sz) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        dir_out[0] = '\0';
    } else {
        size_t len = (size_t)(slash - path);
        if (len >= dir_sz) len = dir_sz - 1;
        memcpy(dir_out, path, len);
        dir_out[len] = '\0';
    }
}

typedef struct {
    char       *buf;
    size_t      len;
    size_t      cap;
    const char *start_frag;
    const char *end_frag;
    bool        active;
    bool        stopped;
} StrBuf;

static void sb_append(StrBuf *sb, const char *s) {
    if (!sb->active || sb->stopped || !s || !*s) return;
    size_t slen = strlen(s);
    if (sb->len + slen + 2 >= sb->cap) {
        size_t ncap = (sb->cap == 0) ? 1024 : (sb->cap * 2 + slen);
        char *nb = (char *)realloc(sb->buf, ncap);
        if (!nb) return;
        sb->buf = nb;
        sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

static void sb_flush_ex(StrBuf *sb, bool is_heading, bool add_blank_after) {
    if (sb->len > 0 && sb->buf) {
        wrap_paragraph_ex(sb->buf, is_heading, add_blank_after);
        sb->len = 0;
        sb->buf[0] = '\0';
    }
}

static void sb_flush(StrBuf *sb, bool is_heading) {
    sb_flush_ex(sb, is_heading, true);
}

static bool is_ignored_tag(const char *name) {
    return (strcasecmp(name, "head") == 0 ||
            strcasecmp(name, "style") == 0 ||
            strcasecmp(name, "script") == 0 ||
            strcasecmp(name, "img") == 0 ||
            strcasecmp(name, "svg") == 0 ||
            strcasecmp(name, "image") == 0 ||
            strcasecmp(name, "audio") == 0 ||
            strcasecmp(name, "video") == 0);
}

static bool is_heading_tag(const char *name) {
    return (strcasecmp(name, "h1") == 0 ||
            strcasecmp(name, "h2") == 0 ||
            strcasecmp(name, "h3") == 0 ||
            strcasecmp(name, "h4") == 0);
}

static bool is_block_tag(const char *name) {
    return (is_heading_tag(name) ||
            strcasecmp(name, "h5") == 0 ||
            strcasecmp(name, "h6") == 0 ||
            strcasecmp(name, "p") == 0 ||
            strcasecmp(name, "div") == 0 ||
            strcasecmp(name, "li") == 0 ||
            strcasecmp(name, "blockquote") == 0 ||
            strcasecmp(name, "pre") == 0 ||
            strcasecmp(name, "tr") == 0 ||
            strcasecmp(name, "section") == 0 ||
            strcasecmp(name, "article") == 0 ||
            strcasecmp(name, "header") == 0);
}

static bool node_matches_frag(const XmlNode *node, const char *frag) {
    if (!node || !frag || !*frag || node->is_text) return false;
    const char *id = xml_get_attr(node, "id");
    if (id && strcmp(id, frag) == 0) return true;
    const char *nm = xml_get_attr(node, "name");
    if (nm && strcmp(nm, frag) == 0) return true;
    return false;
}

static bool dom_has_frag(const XmlNode *node, const char *frag) {
    if (!frag || !*frag) return false;
    for (const XmlNode *cur = node; cur; cur = cur->next_sibling) {
        if (node_matches_frag(cur, frag)) return true;
        if (cur->first_child && dom_has_frag(cur->first_child, frag)) return true;
    }
    return false;
}

static void walk_xhtml_nodes(const XmlNode *node, StrBuf *sb, bool in_heading) {
    for (const XmlNode *cur = node; cur && !sb->stopped; cur = cur->next_sibling) {
        if (cur->is_text) {
            if (cur->text) sb_append(sb, cur->text);
        } else if (cur->name) {
            if (sb->active && sb->end_frag && sb->end_frag[0] != '\0' &&
                node_matches_frag(cur, sb->end_frag)) {
                sb_flush(sb, in_heading);
                sb->stopped = true;
                return;
            }
            if (!sb->active && sb->start_frag && sb->start_frag[0] != '\0' &&
                node_matches_frag(cur, sb->start_frag)) {
                sb->active = true;
            }

            if (is_ignored_tag(cur->name)) continue;
            if (strcasecmp(cur->name, "br") == 0) {
                sb_flush_ex(sb, in_heading, false);
                continue;
            }
            if (strcasecmp(cur->name, "hr") == 0) {
                sb_flush_ex(sb, in_heading, true);
                continue;
            }
            bool block   = is_block_tag(cur->name);
            bool heading = is_heading_tag(cur->name) || in_heading;
            if (block) {
                sb_flush(sb, in_heading);
                if (strcasecmp(cur->name, "li") == 0) {
                    sb_append(sb, "* ");
                }
            }
            if (cur->first_child) {
                walk_xhtml_nodes(cur->first_child, sb, heading);
            }
            if (block) {
                sb_flush(sb, heading);
            }
        }
    }
}

static void find_first_heading_in_dom(const XmlNode *node, char *out, size_t out_sz) {
    for (const XmlNode *cur = node; cur && out[0] == '\0'; cur = cur->next_sibling) {
        if (!cur->is_text && cur->name) {
            if (is_heading_tag(cur->name) || strcasecmp(cur->name, "title") == 0) {
                char raw[256];
                xml_node_text(cur, raw, sizeof(raw));
                normalize_utf8_text(raw, out, out_sz);
                if (out[0] != '\0') return;
            }
        }
        if (cur->first_child) {
            find_first_heading_in_dom(cur->first_child, out, out_sz);
        }
    }
}

static void parse_ncx_navpoints(const XmlNode *node, const char *ncx_dir) {
    for (const XmlNode *cur = node; cur; cur = cur->next_sibling) {
        if (!cur->is_text && cur->name && strcasecmp(cur->name, "navPoint") == 0) {
            char label[128] = "";
            char src[256]   = "";
            char frag[64]   = "";
            for (const XmlNode *ch = cur->first_child; ch; ch = ch->next_sibling) {
                if (ch->is_text || !ch->name) continue;
                if (strcasecmp(ch->name, "navLabel") == 0) {
                    char raw[256];
                    xml_node_text(ch, raw, sizeof(raw));
                    normalize_utf8_text(raw, label, sizeof(label));
                } else if (strcasecmp(ch->name, "content") == 0) {
                    const char *s = xml_get_attr(ch, "src");
                    if (s) {
                        extract_fragment(s, frag, sizeof(frag));
                        resolve_relative_path(ncx_dir, s, src, sizeof(src));
                    }
                }
            }
            if (label[0] != '\0' && src[0] != '\0' && g_chapter_count < MAX_CHAPTERS) {
                if (g_chapter_count == 0 ||
                    strcmp(g_chapters[g_chapter_count - 1].href, src) != 0 ||
                    strcmp(g_chapters[g_chapter_count - 1].frag, frag) != 0) {
                    memset(&g_chapters[g_chapter_count], 0, sizeof(EpubChapter));
                    snprintf(g_chapters[g_chapter_count].title, sizeof(g_chapters[0].title), "%s", label);
                    snprintf(g_chapters[g_chapter_count].href, sizeof(g_chapters[0].href), "%s", src);
                    snprintf(g_chapters[g_chapter_count].frag, sizeof(g_chapters[0].frag), "%s", frag);
                    g_chapters[g_chapter_count].start_spine = -1;
                    g_chapters[g_chapter_count].end_spine   = -1;
                    g_chapter_count++;
                }
            }
            if (cur->first_child) parse_ncx_navpoints(cur->first_child, ncx_dir);
        } else if (cur->first_child) {
            parse_ncx_navpoints(cur->first_child, ncx_dir);
        }
    }
}

static void parse_epub3_nav_links(const XmlNode *node, const char *nav_dir, bool inside_toc_nav) {
    for (const XmlNode *cur = node; cur; cur = cur->next_sibling) {
        bool now_in_nav = inside_toc_nav;
        if (!cur->is_text && cur->name) {
            if (strcasecmp(cur->name, "nav") == 0) {
                const char *tp = xml_get_attr(cur, "type");
                if (!tp || strcasecmp(tp, "toc") == 0) {
                    now_in_nav = true;
                }
            } else if (inside_toc_nav && strcasecmp(cur->name, "a") == 0) {
                const char *href = xml_get_attr(cur, "href");
                char raw[256];
                xml_node_text(cur, raw, sizeof(raw));
                if (href && raw[0] != '\0' && g_chapter_count < MAX_CHAPTERS) {
                    char label[128]    = "";
                    char resolved[256] = "";
                    char frag[64]      = "";
                    normalize_utf8_text(raw, label, sizeof(label));
                    extract_fragment(href, frag, sizeof(frag));
                    resolve_relative_path(nav_dir, href, resolved, sizeof(resolved));
                    if (label[0] != '\0' && resolved[0] != '\0') {
                        memset(&g_chapters[g_chapter_count], 0, sizeof(EpubChapter));
                        snprintf(g_chapters[g_chapter_count].title, sizeof(g_chapters[0].title), "%s", label);
                        snprintf(g_chapters[g_chapter_count].href, sizeof(g_chapters[0].href), "%s", resolved);
                        snprintf(g_chapters[g_chapter_count].frag, sizeof(g_chapters[0].frag), "%s", frag);
                        g_chapters[g_chapter_count].start_spine = -1;
                        g_chapters[g_chapter_count].end_spine   = -1;
                        g_chapter_count++;
                    }
                }
            }
        }
        if (cur->first_child) {
            parse_epub3_nav_links(cur->first_child, nav_dir, now_in_nav);
        }
    }
}

static void parse_xhtml_entry_into_lines(const char *entry_href, const char *start_frag, const char *end_frag) {
    size_t xml_sz = 0;
    char *xml_buf = read_zip_entry(entry_href, &xml_sz);
    if (!xml_buf) return;

    XmlNode *doc = xml_parse_document(xml_buf, xml_sz);
    if (doc) {
        bool wait_for_start = (start_frag && start_frag[0] != '\0' && dom_has_frag(doc->first_child, start_frag));
        StrBuf sb;
        memset(&sb, 0, sizeof(sb));
        sb.start_frag = wait_for_start ? start_frag : NULL;
        sb.end_frag   = (end_frag && end_frag[0] != '\0') ? end_frag : NULL;
        sb.active     = !wait_for_start;
        sb.stopped    = false;

        walk_xhtml_nodes(doc->first_child, &sb, false);
        sb_flush(&sb, false);
        free(sb.buf);
        xml_free_node(doc);
    }
    free(xml_buf);
}

static void load_chapter(int chapter_idx) {
    if (g_is_plain_txt) return;
    if (chapter_idx < 0 || chapter_idx >= g_chapter_count) return;

    g_cur_chapter = chapter_idx;
    free_wrapped_lines();

    EpubChapter *ch = &g_chapters[chapter_idx];
    if (ch->start_spine >= 0 && ch->end_spine >= ch->start_spine) {
        for (int s = ch->start_spine; s <= ch->end_spine; s++) {
            const char *s_frag = (s == ch->start_spine && ch->use_start_frag) ? ch->frag : NULL;
            const char *e_frag = (s == ch->end_spine && ch->end_frag[0] != '\0') ? ch->end_frag : NULL;
            parse_xhtml_entry_into_lines(g_spine_hrefs[s], s_frag, e_frag);
        }
    } else {
        const char *s_frag = ch->use_start_frag ? ch->frag : NULL;
        const char *e_frag = (ch->end_frag[0] != '\0') ? ch->end_frag : NULL;
        parse_xhtml_entry_into_lines(ch->href, s_frag, e_frag);
    }

    while (g_line_count > 0 && g_lines[g_line_count - 1].is_blank) {
        free(g_lines[g_line_count - 1].text);
        g_line_count--;
    }
    if (g_line_count == 0) {
        wrap_paragraph(ch->title, true);
    }

    g_total_pages = (g_line_count + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
    if (g_total_pages < 1) g_total_pages = 1;
    ch->page_count = g_total_pages;
    g_cur_page     = 0;
}

static void find_rootfile_in_dom(const XmlNode *node, char *out, size_t out_sz) {
    for (const XmlNode *cur = node; cur && out[0] == '\0'; cur = cur->next_sibling) {
        if (!cur->is_text && cur->name && strcasecmp(cur->name, "rootfile") == 0) {
            const char *fp = xml_get_attr(cur, "full-path");
            if (fp && fp[0] != '\0') {
                snprintf(out, out_sz, "%s", fp);
                return;
            }
        }
        if (cur->first_child) {
            find_rootfile_in_dom(cur->first_child, out, out_sz);
        }
    }
}

static void walk_opf_dom(const XmlNode *node, const char *opf_dir,
                         ManifestItem *manifest, int *manifest_count,
                         char spine_ids[MAX_MANIFEST][128], int *spine_id_count,
                         char *toc_id, size_t toc_id_sz) {
    for (const XmlNode *cur = node; cur; cur = cur->next_sibling) {
        if (!cur->is_text && cur->name) {
            if (strcasecmp(cur->name, "metadata") == 0) {
                for (const XmlNode *m = cur->first_child; m; m = m->next_sibling) {
                    if (m->is_text || !m->name) continue;
                    if (strcasecmp(m->name, "title") == 0 && g_book_title[0] == '\0') {
                        char raw[256];
                        xml_node_text(m, raw, sizeof(raw));
                        normalize_utf8_text(raw, g_book_title, sizeof(g_book_title));
                    } else if (strcasecmp(m->name, "creator") == 0 && g_book_author[0] == '\0') {
                        char raw[256];
                        xml_node_text(m, raw, sizeof(raw));
                        normalize_utf8_text(raw, g_book_author, sizeof(g_book_author));
                    }
                }
            } else if (strcasecmp(cur->name, "manifest") == 0) {
                for (const XmlNode *it = cur->first_child; it && *manifest_count < MAX_MANIFEST; it = it->next_sibling) {
                    if (it->is_text || !it->name || strcasecmp(it->name, "item") != 0) continue;
                    const char *id    = xml_get_attr(it, "id");
                    const char *href  = xml_get_attr(it, "href");
                    const char *mt    = xml_get_attr(it, "media-type");
                    const char *props = xml_get_attr(it, "properties");
                    if (id && href) {
                        ManifestItem *mi = &manifest[*manifest_count];
                        snprintf(mi->id, sizeof(mi->id), "%s", id);
                        resolve_relative_path(opf_dir, href, mi->href, sizeof(mi->href));
                        if (mt) snprintf(mi->media_type, sizeof(mi->media_type), "%s", mt);
                        if (props && strstr(props, "nav")) mi->is_nav = true;
                        (*manifest_count)++;
                    }
                }
            } else if (strcasecmp(cur->name, "spine") == 0) {
                const char *ta = xml_get_attr(cur, "toc");
                if (ta && ta[0] != '\0') {
                    snprintf(toc_id, toc_id_sz, "%s", ta);
                }
                for (const XmlNode *ir = cur->first_child; ir && *spine_id_count < MAX_MANIFEST; ir = ir->next_sibling) {
                    if (ir->is_text || !ir->name || strcasecmp(ir->name, "itemref") != 0) continue;
                    const char *idref = xml_get_attr(ir, "idref");
                    if (idref) {
                        snprintf(spine_ids[(*spine_id_count)++], 128, "%s", idref);
                    }
                }
            } else if (cur->first_child) {
                walk_opf_dom(cur->first_child, opf_dir, manifest, manifest_count,
                             spine_ids, spine_id_count, toc_id, toc_id_sz);
            }
        }
    }
}

static bool open_epub_book(const char *filepath) {
    close_epub_zip();
    free_wrapped_lines();
    g_chapter_count    = 0;
    g_cur_chapter      = 0;
    g_spine_count      = 0;
    g_book_total_pages = 1;
    g_is_plain_txt     = false;
    g_current_book_path[0] = '\0';
    g_book_title[0]        = '\0';
    g_book_author[0]       = '\0';

    char resolved_book[PATH_MAX];
    const char *canonical_path = realpath(filepath, resolved_book) ? resolved_book : filepath;

    /* Plain .txt book support (paragraphs joined across soft-wrapped lines, split on blank lines) */
    const char *ext = strrchr(filepath, '.');
    if (ext && strcasecmp(ext, ".txt") == 0) {
        FILE *f = fopen(filepath, "rb");
        if (!f) return false;
        g_is_plain_txt = true;
        snprintf(g_current_book_path, sizeof(g_current_book_path), "%s", canonical_path);
        const char *base = strrchr(filepath, '/');
        snprintf(g_book_title, sizeof(g_book_title), "%s", base ? (base + 1) : filepath);
        memset(&g_chapters[0], 0, sizeof(EpubChapter));
        snprintf(g_chapters[0].title, sizeof(g_chapters[0].title), "%s", g_book_title);
        g_chapter_count = 1;

        StrBuf para;
        memset(&para, 0, sizeof(para));
        para.active = true;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n')) line[--l] = '\0';
            bool blank = true;
            for (size_t k = 0; k < l; k++) {
                if (!isspace((unsigned char)line[k])) { blank = false; break; }
            }
            if (blank) {
                sb_flush(&para, false);
            } else {
                if (para.len > 0) sb_append(&para, " ");
                sb_append(&para, line);
            }
        }
        sb_flush(&para, false);
        free(para.buf);
        fclose(f);

        while (g_line_count > 0 && g_lines[g_line_count - 1].is_blank) {
            free(g_lines[g_line_count - 1].text);
            g_line_count--;
        }
        if (g_line_count == 0) wrap_paragraph("(Empty text file)", false);

        g_total_pages = (g_line_count + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
        if (g_total_pages < 1) g_total_pages = 1;
        g_chapters[0].page_count = g_total_pages;
        g_book_total_pages       = g_total_pages;
        g_cur_page               = 0;
        return true;
    }

    memset(&g_epub_zip, 0, sizeof(g_epub_zip));
    if (!mz_zip_reader_init_file(&g_epub_zip, filepath, 0)) {
        return false;
    }
    g_epub_zip_open = true;

    /* 1. Locate .opf rootfile from META-INF/container.xml */
    char opf_path[256] = "";
    size_t cont_sz = 0;
    char *cont_xml = read_zip_entry("META-INF/container.xml", &cont_sz);
    if (cont_xml) {
        XmlNode *cdoc = xml_parse_document(cont_xml, cont_sz);
        if (cdoc) {
            find_rootfile_in_dom(cdoc->first_child, opf_path, sizeof(opf_path));
            xml_free_node(cdoc);
        }
        free(cont_xml);
    }

    /* Fallback scan for *.opf if container.xml was missing */
    if (opf_path[0] == '\0') {
        mz_uint n = mz_zip_reader_get_num_files(&g_epub_zip);
        char fname[512];
        for (mz_uint i = 0; i < n; i++) {
            if (mz_zip_reader_get_filename(&g_epub_zip, i, fname, sizeof(fname)) > 0) {
                if (strstr(fname, ".opf")) {
                    snprintf(opf_path, sizeof(opf_path), "%.*s", (int)(sizeof(opf_path) - 1), fname);
                    break;
                }
            }
        }
    }
    if (opf_path[0] == '\0') {
        close_epub_zip();
        return false;
    }

    char opf_dir[256] = "";
    extract_dirname(opf_path, opf_dir, sizeof(opf_dir));

    /* 2. Parse .opf Package Document */
    size_t opf_sz = 0;
    char *opf_xml = read_zip_entry(opf_path, &opf_sz);
    if (!opf_xml) {
        close_epub_zip();
        return false;
    }

    ManifestItem *manifest = (ManifestItem *)calloc(MAX_MANIFEST, sizeof(ManifestItem));
    int manifest_count = 0;
    char spine_ids[MAX_MANIFEST][128];
    int spine_id_count = 0;
    char toc_id[128] = "";

    XmlNode *odoc = xml_parse_document(opf_xml, opf_sz);
    if (odoc) {
        walk_opf_dom(odoc->first_child, opf_dir, manifest, &manifest_count,
                     spine_ids, &spine_id_count, toc_id, sizeof(toc_id));
        xml_free_node(odoc);
    }
    free(opf_xml);

    for (int s = 0; s < spine_id_count && g_spine_count < MAX_MANIFEST; s++) {
        for (int m = 0; m < manifest_count; m++) {
            if (strcmp(spine_ids[s], manifest[m].id) == 0) {
                snprintf(g_spine_hrefs[g_spine_count++], sizeof(g_spine_hrefs[0]), "%s", manifest[m].href);
                break;
            }
        }
    }

    if (g_book_title[0] == '\0') {
        const char *base = strrchr(filepath, '/');
        snprintf(g_book_title, sizeof(g_book_title), "%s", base ? (base + 1) : filepath);
    }

    /* 3. Parse Table of Contents (EPUB 2 NCX or EPUB 3 nav) */
    char ncx_href[256] = "";
    char nav_href[256] = "";
    for (int i = 0; i < manifest_count; i++) {
        if ((toc_id[0] != '\0' && strcmp(manifest[i].id, toc_id) == 0) ||
            strstr(manifest[i].media_type, "dtbncx") != NULL) {
            snprintf(ncx_href, sizeof(ncx_href), "%s", manifest[i].href);
        }
        if (manifest[i].is_nav) {
            snprintf(nav_href, sizeof(nav_href), "%s", manifest[i].href);
        }
    }

    if (ncx_href[0] != '\0') {
        size_t ncx_sz = 0;
        char *ncx_xml = read_zip_entry(ncx_href, &ncx_sz);
        if (ncx_xml) {
            char ncx_dir[256] = "";
            extract_dirname(ncx_href, ncx_dir, sizeof(ncx_dir));
            XmlNode *ndoc = xml_parse_document(ncx_xml, ncx_sz);
            if (ndoc) {
                parse_ncx_navpoints(ndoc->first_child, ncx_dir);
                xml_free_node(ndoc);
            }
            free(ncx_xml);
        }
    }

    if (g_chapter_count == 0 && nav_href[0] != '\0') {
        size_t nav_sz = 0;
        char *nav_xml = read_zip_entry(nav_href, &nav_sz);
        if (nav_xml) {
            char nav_dir[256] = "";
            extract_dirname(nav_href, nav_dir, sizeof(nav_dir));
            XmlNode *ndoc = xml_parse_document(nav_xml, nav_sz);
            if (ndoc) {
                parse_epub3_nav_links(ndoc->first_child, nav_dir, false);
                xml_free_node(ndoc);
            }
            free(nav_xml);
        }
    }

    /* 4. Fallback to spine items if TOC was empty */
    if (g_chapter_count == 0) {
        for (int s = 0; s < g_spine_count && g_chapter_count < MAX_CHAPTERS; s++) {
            char heading[128] = "";
            size_t ch_sz = 0;
            char *ch_xml = read_zip_entry(g_spine_hrefs[s], &ch_sz);
            if (ch_xml) {
                XmlNode *cdoc = xml_parse_document(ch_xml, ch_sz);
                if (cdoc) {
                    find_first_heading_in_dom(cdoc->first_child, heading, sizeof(heading));
                    xml_free_node(cdoc);
                }
                free(ch_xml);
            }
            if (heading[0] == '\0') {
                snprintf(heading, sizeof(heading), "Chapter %d", g_chapter_count + 1);
            }
            memset(&g_chapters[g_chapter_count], 0, sizeof(EpubChapter));
            snprintf(g_chapters[g_chapter_count].title, sizeof(g_chapters[0].title), "%s", heading);
            memcpy(g_chapters[g_chapter_count].href, g_spine_hrefs[s], sizeof(g_chapters[0].href));
            g_chapters[g_chapter_count].start_spine = s;
            g_chapters[g_chapter_count].end_spine   = s;
            g_chapter_count++;
        }
    }

    free(manifest);

    if (g_chapter_count == 0) {
        close_epub_zip();
        return false;
    }

    /* Map chapters to spine ranges and fragment boundaries */
    for (int i = 0; i < g_chapter_count; i++) {
        for (int s = 0; s < g_spine_count; s++) {
            if (strcasecmp(g_chapters[i].href, g_spine_hrefs[s]) == 0) {
                g_chapters[i].start_spine = s;
                break;
            }
        }
    }
    for (int i = 0; i < g_chapter_count; i++) {
        if (i > 0 && strcmp(g_chapters[i].href, g_chapters[i - 1].href) == 0 &&
            g_chapters[i].frag[0] != '\0') {
            g_chapters[i].use_start_frag = true;
        }
        if (i + 1 < g_chapter_count && strcmp(g_chapters[i].href, g_chapters[i + 1].href) == 0 &&
            g_chapters[i + 1].frag[0] != '\0') {
            snprintf(g_chapters[i].end_frag, sizeof(g_chapters[i].end_frag), "%s", g_chapters[i + 1].frag);
            g_chapters[i].end_spine = g_chapters[i].start_spine;
        } else if (g_chapters[i].start_spine >= 0) {
            if (i + 1 < g_chapter_count && g_chapters[i + 1].start_spine > g_chapters[i].start_spine) {
                g_chapters[i].end_spine = g_chapters[i + 1].start_spine - 1;
            } else if (i + 1 == g_chapter_count && g_spine_count > g_chapters[i].start_spine) {
                g_chapters[i].end_spine = g_spine_count - 1;
            } else {
                g_chapters[i].end_spine = g_chapters[i].start_spine;
            }
        }
    }

    /* Pre-paginate all chapters for accurate overall book progress */
    int total_book_pgs = 0;
    for (int i = 0; i < g_chapter_count; i++) {
        load_chapter(i);
        total_book_pgs += g_chapters[i].page_count;
    }
    g_book_total_pages = (total_book_pgs > 0) ? total_book_pgs : 1;

    snprintf(g_current_book_path, sizeof(g_current_book_path), "%s", canonical_path);
    load_chapter(0);
    return true;
}

/* ============================================================================
 * 6. FILE BROWSER & PERSISTENT READING STATE (.bard_rg35xxsp.cfg)
 * ============================================================================ */

static int cmp_file_entry(const void *a, const void *b) {
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;
    if (strcmp(fa->name, "..") == 0) return -1;
    if (strcmp(fb->name, "..") == 0) return 1;
    if (fa->is_dir != fb->is_dir) return fa->is_dir ? -1 : 1;
    return strcasecmp(fa->name, fb->name);
}

static bool has_book_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".epub") == 0 || strcasecmp(dot, ".txt") == 0);
}

static void scan_directory(const char *dir_path) {
    char resolved[PATH_MAX];
    if (realpath(dir_path, resolved)) {
        snprintf(g_cwd, sizeof(g_cwd), "%s", resolved);
    } else {
        snprintf(g_cwd, sizeof(g_cwd), "%s", dir_path);
    }

    g_file_count  = 0;
    g_file_sel    = 0;
    g_file_scroll = 0;

    if (strcmp(g_cwd, "/") != 0) {
        snprintf(g_files[g_file_count].name, sizeof(g_files[0].name), "..");
        g_files[g_file_count].is_dir = true;
        g_files[g_file_count].size   = 0;
        g_file_count++;
    }

    DIR *d = opendir(g_cwd);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_file_count < MAX_FILES) {
        if (de->d_name[0] == '.') continue;
        char full[PATH_MAX + 512];
        snprintf(full, sizeof(full), "%s/%s", g_cwd, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !has_book_extension(de->d_name)) continue;

        snprintf(g_files[g_file_count].name, sizeof(g_files[0].name), "%s", de->d_name);
        g_files[g_file_count].is_dir = is_dir;
        g_files[g_file_count].size   = st.st_size;
        g_file_count++;
    }
    closedir(d);

    qsort(g_files, (size_t)g_file_count, sizeof(FileEntry), cmp_file_entry);
}

static void resolve_state_file_path(void) {
    const char *env_path = getenv("BARD_CFG");
    if (!env_path || env_path[0] == '\0') env_path = getenv("BARD_STATE_FILE");
    if (env_path && env_path[0] != '\0') {
        snprintf(g_state_file_path, sizeof(g_state_file_path), "%s", env_path);
        return;
    }
    const char *dirs[] = {
        "/userdata/roms/ports/bard",
        "/userdata/system/configs",
        "/userdata",
        NULL
    };
    struct stat st;
    for (int i = 0; dirs[i]; i++) {
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && access(dirs[i], W_OK) == 0) {
            snprintf(g_state_file_path, sizeof(g_state_file_path), "%s/.bard_rg35xxsp.cfg", dirs[i]);
            return;
        }
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_state_file_path, sizeof(g_state_file_path), "%s/.bard_rg35xxsp.cfg", home);
    } else {
        snprintf(g_state_file_path, sizeof(g_state_file_path), "./.bard_rg35xxsp.cfg");
    }
}

static void save_reader_state(void) {
    if (g_state_file_path[0] == '\0' || g_current_book_path[0] == '\0') return;
    FILE *f = fopen(g_state_file_path, "w");
    if (!f) return;
    fprintf(f, "book_path=%s\n", g_current_book_path);
    fprintf(f, "chapter=%d\n", g_cur_chapter);
    fprintf(f, "page=%d\n", g_cur_page);
    fprintf(f, "dark_theme=%d\n", g_dark_theme ? 1 : 0);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static bool load_and_resume_reader_state(void) {
    if (g_state_file_path[0] == '\0') return false;
    FILE *f = fopen(g_state_file_path, "r");
    if (!f) return false;

    char saved_path[PATH_MAX] = "";
    int saved_chapter = -1;
    int saved_page    = -1;
    int saved_dark    = 0;
    bool has_chapter  = false;
    bool has_page     = false;

    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (strncmp(line, "book_path=", 10) == 0) {
            snprintf(saved_path, sizeof(saved_path), "%.*s", (int)(sizeof(saved_path) - 1), line + 10);
        } else if (strncmp(line, "chapter=", 8) == 0) {
            char *endp = NULL;
            long v = strtol(line + 8, &endp, 10);
            if (endp && endp != line + 8 && *endp == '\0') {
                saved_chapter = (int)v;
                has_chapter = true;
            }
        } else if (strncmp(line, "page=", 5) == 0) {
            char *endp = NULL;
            long v = strtol(line + 5, &endp, 10);
            if (endp && endp != line + 5 && *endp == '\0') {
                saved_page = (int)v;
                has_page = true;
            }
        } else if (strncmp(line, "dark_theme=", 11) == 0) {
            saved_dark = atoi(line + 11);
        }
    }
    fclose(f);

    g_dark_theme = (saved_dark != 0);
    if (saved_path[0] == '\0') return false;

    struct stat st;
    if (stat(saved_path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    char dir_buf[PATH_MAX];
    extract_dirname(saved_path, dir_buf, sizeof(dir_buf));
    if (dir_buf[0] != '\0' && stat(dir_buf, &st) == 0 && S_ISDIR(st.st_mode)) {
        scan_directory(dir_buf);
        const char *base = strrchr(saved_path, '/');
        const char *fname = base ? (base + 1) : saved_path;
        for (int i = 0; i < g_file_count; i++) {
            if (!g_files[i].is_dir && strcmp(g_files[i].name, fname) == 0) {
                g_file_sel = i;
                if (g_file_sel >= BROWSER_ROWS) {
                    g_file_scroll = g_file_sel - BROWSER_ROWS + 1;
                }
                break;
            }
        }
    }

    if (!open_epub_book(saved_path)) return false;

    if (has_chapter && has_page && saved_chapter >= 0 && saved_chapter < g_chapter_count) {
        load_chapter(saved_chapter);
        if (saved_page >= 0 && saved_page < g_total_pages) {
            g_cur_page = saved_page;
        } else {
            load_chapter(0);
            g_cur_page = 0;
        }
    } else {
        load_chapter(0);
        g_cur_page = 0;
    }

    g_state = STATE_READING;
    return true;
}

/* ============================================================================
 * 7. NATIVE 640x480 UI RENDERING
 * ============================================================================ */

static void render_file_browser(void) {
    fill_rect(0, 0, NATIVE_W, NATIVE_H, RGB(15, 21, 33));

    /* Top Header (y=0..46) */
    fill_rect(0, 0, NATIVE_W, 46, RGB(24, 34, 54));
    fill_rect(0, 46, NATIVE_W, 2, RGB(56, 78, 112));
    draw_pill(12, 9, 80, 28, RGB(37, 99, 235), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_16, 12, 9, 80, 28, "BARD", RGB(255, 255, 255));

    int max_pw = NATIVE_W - 196;
    if (text_width(FONT_UI_16, g_cwd) <= max_pw) {
        draw_text(FONT_UI_16, 102, 13, g_cwd, RGB(226, 232, 240));
    } else {
        char pbuf[256];
        size_t clen = strlen(g_cwd);
        size_t off = 1;
        while (off < clen) {
            snprintf(pbuf, sizeof(pbuf), "...%.*s", (int)(sizeof(pbuf) - 4), g_cwd + off);
            if (text_width(FONT_UI_16, pbuf) <= max_pw) break;
            off++;
        }
        draw_text(FONT_UI_16, 102, 13, pbuf, RGB(226, 232, 240));
    }

    draw_pill(NATIVE_W - 88, 10, 76, 26, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, NATIVE_W - 88, 10, 76, 26, "Y: Help", RGB(254, 240, 138));

    /* File List Rows (y=54..438, 11 rows x 35px) */
    int row_h = 35;
    if (g_file_count == 0) {
        draw_text_centered(FONT_UI_20, 0, 180, NATIVE_W, 40,
                           "No .epub or .txt files in this folder", RGB(148, 163, 184));
    }
    for (int i = 0; i < BROWSER_ROWS; i++) {
        int idx = g_file_scroll + i;
        if (idx >= g_file_count) break;
        int ry = 54 + i * row_h;
        bool sel = (idx == g_file_sel);

        if (sel) {
            fill_rect(10, ry, NATIVE_W - 20, row_h - 3, RGB(37, 62, 102));
            draw_border(10, ry, NATIVE_W - 20, row_h - 3, 2, RGB(250, 204, 21));
        } else if (i % 2 == 1) {
            fill_rect(10, ry, NATIVE_W - 20, row_h - 3, RGB(19, 27, 42));
        }

        if (g_files[idx].is_dir) {
            draw_pill(18, ry + 5, 58, 22, RGB(30, 58, 95), RGB(56, 189, 248));
            draw_text_centered(FONT_UI_13, 18, ry + 5, 58, 22, "DIR", RGB(125, 211, 252));
            draw_text_clipped(FONT_UI_16, 88, ry + 6, NATIVE_W - 110,
                              g_files[idx].name, sel ? RGB(255, 255, 255) : RGB(226, 232, 240));
        } else {
            const char *ext = strrchr(g_files[idx].name, '.');
            bool is_txt = (ext && strcasecmp(ext, ".txt") == 0);
            draw_pill(18, ry + 5, 58, 22,
                      is_txt ? RGB(15, 84, 72) : RGB(88, 40, 136),
                      is_txt ? RGB(52, 211, 153) : RGB(192, 132, 252));
            draw_text_centered(FONT_UI_13, 18, ry + 5, 58, 22,
                               is_txt ? "TXT" : "EPUB",
                               is_txt ? RGB(209, 250, 229) : RGB(233, 213, 255));

            char sz_str[24];
            if (g_files[idx].size >= 1024 * 1024) {
                snprintf(sz_str, sizeof(sz_str), "%.1f MB", (double)g_files[idx].size / (1024.0 * 1024.0));
            } else {
                snprintf(sz_str, sizeof(sz_str), "%.1f KB", (double)g_files[idx].size / 1024.0);
            }
            int sz_w = text_width(FONT_UI_13, sz_str);
            draw_text_right(FONT_UI_13, NATIVE_W - 22, ry + 8, sz_str, RGB(148, 163, 184));
            draw_text_clipped(FONT_UI_16, 88, ry + 6, NATIVE_W - 124 - sz_w,
                              g_files[idx].name, sel ? RGB(255, 255, 255) : RGB(241, 245, 249));
        }
    }

    /* Bottom Controls Bar (y=446..480) */
    fill_rect(0, 446, NATIVE_W, 34, RGB(10, 14, 23));
    fill_rect(0, 446, NATIVE_W, 2, RGB(38, 52, 76));
    draw_text_centered(FONT_UI_13, 0, 448, NATIVE_W, 30,
                       "UP/DN: Move   |   A: Open Book/Dir   |   B: Up Dir   |   Y: Help   |   MENU: Quit",
                       RGB(165, 180, 202));
}

static void render_reading_view(void) {
    uint32_t bg_col   = g_dark_theme ? RGB(20, 24, 34)    : RGB(250, 246, 236);
    uint32_t txt_col  = g_dark_theme ? RGB(228, 224, 214) : RGB(28, 33, 41);
    uint32_t head_col = g_dark_theme ? RGB(147, 197, 253) : RGB(124, 45, 18);

    /* Full-screen distraction-free 640x480 reading canvas */
    fill_rect(0, 0, NATIVE_W, NATIVE_H, bg_col);

    int start_line = g_cur_page * LINES_PER_PAGE;
    int line_step  = 26;
    int y = 14;
    for (int i = 0; i < LINES_PER_PAGE; i++) {
        int lidx = start_line + i;
        if (lidx >= g_line_count) break;
        if (!g_lines[lidx].is_blank && g_lines[lidx].text[0] != '\0') {
            int fid      = g_lines[lidx].is_heading ? FONT_BOOK_BOLD22 : FONT_BOOK_20;
            uint32_t col = g_lines[lidx].is_heading ? head_col : txt_col;
            draw_text(fid, TEXT_MARGIN_X, y, g_lines[lidx].text, col);
        }
        y += line_step;
    }

    /* Discrete "Y: Help" badge in bottom-right corner */
    int hw = 72, hh = 21;
    int hx = NATIVE_W - hw - 10;
    int hy = NATIVE_H - hh - 6;
    uint32_t h_fill   = g_dark_theme ? RGB(32, 38, 52)    : RGB(238, 232, 218);
    uint32_t h_border = g_dark_theme ? RGB(64, 76, 98)    : RGB(202, 194, 176);
    uint32_t h_txt    = g_dark_theme ? RGB(160, 174, 192) : RGB(108, 100, 88);
    draw_pill(hx, hy, hw, hh, h_fill, h_border);
    draw_text_centered(FONT_UI_13, hx, hy, hw, hh, "Y: Help", h_txt);
}

static void render_help_modal(void) {
    if (g_help_prev_state == STATE_FILE_BROWSER || g_chapter_count == 0) {
        render_file_browser();
    } else {
        render_reading_view();
    }

    fill_rect_alpha(0, 0, NATIVE_W, NATIVE_H, RGB(8, 12, 22), 205);

    int mx = 26, my = 20, mw = NATIVE_W - 52, mh = NATIVE_H - 40;
    fill_rect(mx, my, mw, mh, RGB(17, 24, 39));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    /* Modal Header */
    fill_rect(mx + 4, my + 4, mw - 8, 44, RGB(30, 41, 59));
    fill_rect(mx + 4, my + 48, mw - 8, 2, RGB(71, 85, 105));
    draw_pill(mx + 14, my + 11, 74, 28, RGB(37, 99, 235), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, mx + 14, my + 11, 74, 28, "HELP", RGB(255, 255, 255));
    const char *hdr_title = (g_book_title[0] != '\0') ? g_book_title : "BARD EPUB & TEXT READER";
    draw_text_clipped(FONT_UI_20, mx + 100, my + 13, mw - 114, hdr_title, RGB(248, 250, 252));

    /* Chapter Name & Reading Progress Card */
    int cy = my + 56;
    fill_rect(mx + 14, cy, mw - 28, 102, RGB(24, 33, 52));
    draw_border(mx + 14, cy, mw - 28, 102, 2, RGB(51, 65, 85));

    char ch_hdr[192];
    if (g_chapter_count > 0) {
        if (g_book_author[0] != '\0') {
            snprintf(ch_hdr, sizeof(ch_hdr), "CHAPTER %d OF %d   |   %.120s",
                     g_cur_chapter + 1, g_chapter_count, g_book_author);
        } else {
            snprintf(ch_hdr, sizeof(ch_hdr), "CHAPTER %d OF %d", g_cur_chapter + 1, g_chapter_count);
        }
    } else {
        snprintf(ch_hdr, sizeof(ch_hdr), "EPUB & PLAIN-TEXT E-BOOK READER");
    }
    draw_text_clipped(FONT_UI_13, mx + 26, cy + 8, mw - 52, ch_hdr, RGB(125, 211, 252));

    const char *ch_title = (g_chapter_count > 0 && g_cur_chapter < g_chapter_count)
                           ? g_chapters[g_cur_chapter].title
                           : "Select an .epub or .txt book with [A]";
    draw_text_clipped(FONT_UI_20, mx + 26, cy + 28, mw - 52, ch_title, RGB(255, 255, 255));

    int pages_before = 0;
    for (int i = 0; i < g_cur_chapter && i < g_chapter_count; i++) {
        pages_before += (g_chapters[i].page_count > 0) ? g_chapters[i].page_count : 1;
    }
    int book_cur_pg = pages_before + g_cur_page + 1;
    int book_tot_pg = (g_book_total_pages > 0) ? g_book_total_pages : g_total_pages;
    if (book_cur_pg > book_tot_pg) book_tot_pg = book_cur_pg;
    int ch_pct   = (int)(((g_cur_page + 1) * 100) / (g_total_pages > 0 ? g_total_pages : 1));
    int book_pct = (int)((book_cur_pg * 100) / (book_tot_pg > 0 ? book_tot_pg : 1));

    char prog_str[128];
    snprintf(prog_str, sizeof(prog_str), "Chapter Pg %d / %d (%d%%)     |     Book Pg %d / %d (%d%%)",
             g_cur_page + 1, g_total_pages, ch_pct, book_cur_pg, book_tot_pg, book_pct);
    draw_text(FONT_UI_13, mx + 26, cy + 58, prog_str, RGB(250, 204, 21));

    /* Progress Bar */
    int bx = mx + 26, by = cy + 79, bw = mw - 52, bh = 13;
    fill_rect(bx, by, bw, bh, RGB(15, 23, 42));
    draw_border(bx, by, bw, bh, 1, RGB(71, 85, 105));
    int fill_w = ((bw - 4) * book_cur_pg) / (book_tot_pg > 0 ? book_tot_pg : 1);
    if (fill_w < 4) fill_w = 4;
    if (fill_w > bw - 4) fill_w = bw - 4;
    fill_rect(bx + 2, by + 2, fill_w, bh - 4, RGB(56, 189, 248));

    /* Key Mappings */
    int ky = cy + 110;
    draw_text(FONT_UI_13, mx + 18, ky, "QUICK GUIDE & CONTROLS", RGB(148, 163, 184));
    ky += 20;

    static const struct {
        const char *key;
        const char *desc;
    } kmaps[7] = {
        { "DPAD / L1/R1", "Turn Whole Page (Reading) / Scroll Lists" },
        { "A Button",     "Open Highlighted Book, Folder, or Chapter" },
        { "B Button",     "Toggle Light/Dark Theme (Read) / Back (Lists)" },
        { "SELECT",       "Open Chapter List (TOC) / Return to Book" },
        { "X Button",     "Open File Browser (/userdata/books & media)" },
        { "Y Button",     "Toggle This Help & Reading Progress Screen" },
        { "MENU",         "Save Reading Position & Quit Reader" }
    };

    for (int i = 0; i < 7; i++) {
        int ry = ky + i * 29;
        draw_pill(mx + 18, ry, 118, 24, RGB(30, 58, 95), RGB(56, 189, 248));
        draw_text_centered(FONT_UI_13, mx + 18, ry, 118, 24, kmaps[i].key, RGB(254, 240, 138));
        draw_text(FONT_UI_16, mx + 148, ry + 2, kmaps[i].desc, RGB(226, 232, 240));
    }

    /* Modal Footer */
    fill_rect(mx + 4, my + mh - 36, mw - 8, 32, RGB(15, 23, 42));
    fill_rect(mx + 4, my + mh - 36, mw - 8, 1, RGB(51, 65, 85));
    draw_text_centered(FONT_UI_13, mx, my + mh - 34, mw, 28,
                       "Press [Y] or [B] to close Help and return to reading",
                       RGB(186, 230, 253));
}

static void render_chapter_modal(void) {
    render_reading_view();
    fill_rect_alpha(0, 0, NATIVE_W, NATIVE_H, RGB(8, 12, 22), 195);

    int mx = 22, my = 20, mw = NATIVE_W - 44, mh = NATIVE_H - 40;
    fill_rect(mx, my, mw, mh, RGB(17, 24, 39));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    /* Modal Header */
    fill_rect(mx + 4, my + 4, mw - 8, 44, RGB(30, 41, 59));
    fill_rect(mx + 4, my + 48, mw - 8, 2, RGB(71, 85, 105));
    draw_pill(mx + 14, my + 11, 106, 28, RGB(124, 58, 237), RGB(167, 139, 250));
    draw_text_centered(FONT_UI_13, mx + 14, my + 11, 106, 28, "CHAPTERS", RGB(255, 255, 255));
    draw_text_clipped(FONT_UI_20, mx + 132, my + 13, mw - 146, g_book_title, RGB(248, 250, 252));

    /* Chapter Rows (10 rows x 34px) */
    int list_y = my + 58;
    int row_h  = 34;
    for (int i = 0; i < TOC_ROWS; i++) {
        int cidx = g_toc_scroll + i;
        if (cidx >= g_chapter_count) break;
        int ry = list_y + i * row_h;
        bool sel    = (cidx == g_toc_sel);
        bool active = (cidx == g_cur_chapter);

        if (sel) {
            fill_rect(mx + 12, ry, mw - 24, row_h - 3, RGB(37, 99, 235));
            draw_border(mx + 12, ry, mw - 24, row_h - 3, 2, RGB(250, 204, 21));
        } else if (i % 2 == 1) {
            fill_rect(mx + 12, ry, mw - 24, row_h - 3, RGB(24, 33, 52));
        }

        char num_badge[16];
        snprintf(num_badge, sizeof(num_badge), "%2d.", cidx + 1);
        draw_text(FONT_UI_16, mx + 22, ry + 6, num_badge,
                  sel ? RGB(254, 240, 138) : (active ? RGB(56, 189, 248) : RGB(148, 163, 184)));

        char pg_badge[24];
        snprintf(pg_badge, sizeof(pg_badge), "%d pg", g_chapters[cidx].page_count);
        int pg_w = text_width(FONT_UI_13, pg_badge);
        draw_text_right(FONT_UI_13, mx + mw - 24, ry + 8, pg_badge,
                        sel ? RGB(254, 240, 138) : RGB(148, 163, 184));

        draw_text_clipped(FONT_UI_16, mx + 60, ry + 6, mw - 96 - pg_w,
                          g_chapters[cidx].title,
                          sel ? RGB(255, 255, 255) : (active ? RGB(125, 211, 252) : RGB(226, 232, 240)));
    }

    /* Modal Footer */
    fill_rect(mx + 4, my + mh - 36, mw - 8, 32, RGB(15, 23, 42));
    fill_rect(mx + 4, my + mh - 36, mw - 8, 1, RGB(51, 65, 85));
    draw_text_centered(FONT_UI_13, mx, my + mh - 34, mw, 28,
                       "UP/DN: Select Chapter   |   A: Jump to Chapter   |   B / SELECT: Close",
                       RGB(186, 230, 253));
}

static void render_ui(void) {
    switch (g_state) {
        case STATE_FILE_BROWSER: render_file_browser();  break;
        case STATE_READING:      render_reading_view();  break;
        case STATE_CHAPTER_LIST: render_chapter_modal(); break;
        case STATE_HELP:         render_help_modal();    break;
    }
}

/* ============================================================================
 * 8. HEADLESS VERIFICATION & SCREENSHOT GENERATION
 * ============================================================================ */

static int run_verify_epub(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "FAIL: Path not found: %s\n", path);
        return 1;
    }

    int checked = 0;
    if (S_ISDIR(st.st_mode)) {
        scan_directory(path);
        for (int i = 0; i < g_file_count; i++) {
            if (g_files[i].is_dir) continue;
            char full[PATH_MAX + 512];
            snprintf(full, sizeof(full), "%.4000s/%.255s", g_cwd, g_files[i].name);
            if (!open_epub_book(full)) {
                fprintf(stderr, "FAIL: Could not open %s\n", full);
                return 1;
            }
            if (g_chapter_count < 1 || g_line_count < 1) {
                fprintf(stderr, "FAIL: Empty book/chapters in %s\n", full);
                return 1;
            }
            printf("[OK] %s: \"%s\" by \"%s\" (%d chapters, %d total pages, ch1=%d lines)\n",
                   g_files[i].name, g_book_title, g_book_author,
                   g_chapter_count, g_book_total_pages, g_line_count);
            checked++;
        }
    } else {
        if (!open_epub_book(path)) {
            fprintf(stderr, "FAIL: Could not open %s\n", path);
            return 1;
        }
        printf("[OK] \"%s\" by \"%s\" (%d chapters, %d total pages)\n",
               g_book_title, g_book_author, g_chapter_count, g_book_total_pages);
        checked++;
    }

    if (checked == 0) {
        fprintf(stderr, "FAIL: No books found to verify\n");
        return 1;
    }

    /* Verify persistent state save & resume round-trip */
    snprintf(g_state_file_path, sizeof(g_state_file_path), "/tmp/.bard_verify_test.cfg");
    int target_ch = (g_chapter_count > 2) ? 2 : 0;
    load_chapter(target_ch);
    int target_pg = (g_total_pages > 1) ? 1 : 0;
    g_cur_page = target_pg;
    g_dark_theme = true;
    save_reader_state();

    close_epub_zip();
    free_wrapped_lines();
    g_chapter_count = 0;
    g_dark_theme = false;

    if (!load_and_resume_reader_state() ||
        g_cur_chapter != target_ch || g_cur_page != target_pg || !g_dark_theme) {
        fprintf(stderr, "FAIL: State resume mismatch (ch=%d/%d, pg=%d/%d, dark=%d)\n",
                g_cur_chapter, target_ch, g_cur_page, target_pg, g_dark_theme);
        unlink(g_state_file_path);
        return 1;
    }
    unlink(g_state_file_path);
    printf("[OK] State persistence & resume verified (chapter %d, page %d, dark_theme=1)\n",
           g_cur_chapter, g_cur_page);

    close_epub_zip();
    free_wrapped_lines();
    return 0;
}

static int run_screenshots(const char *out_dir, const char *media_dir) {
    char path[512];
    scan_directory(media_dir);

    /* 1. File Browser screenshot */
    g_state = STATE_FILE_BROWSER;
    g_file_sel = 0;
    for (int i = 0; i < g_file_count; i++) {
        if (!g_files[i].is_dir) { g_file_sel = i; break; }
    }
    render_ui();
    snprintf(path, sizeof(path), "%s/bard_files.bmp", out_dir);
    save_bmp(path);

    /* 2. Open first book for Reading, Chapters, and Help screenshots */
    if (g_file_sel < g_file_count && !g_files[g_file_sel].is_dir) {
        char full_book[PATH_MAX + 512];
        snprintf(full_book, sizeof(full_book), "%s/%s", g_cwd, g_files[g_file_sel].name);
        if (!open_epub_book(full_book)) {
            fprintf(stderr, "FAIL: Could not open %s for screenshots\n", full_book);
            return 1;
        }
        printf("[OK] Opened \"%s\" (%d chapters, %d total pages)\n",
               g_book_title, g_chapter_count, g_book_total_pages);

        g_dark_theme = false;
        g_state = STATE_READING;
        render_ui();
        snprintf(path, sizeof(path), "%s/bard_reading.bmp", out_dir);
        save_bmp(path);

        g_state = STATE_CHAPTER_LIST;
        g_toc_sel = (g_chapter_count > 1) ? 1 : 0;
        g_toc_scroll = 0;
        render_ui();
        snprintf(path, sizeof(path), "%s/bard_chapters.bmp", out_dir);
        save_bmp(path);

        g_help_prev_state = STATE_READING;
        g_state = STATE_HELP;
        render_ui();
        snprintf(path, sizeof(path), "%s/bard_help.bmp", out_dir);
        save_bmp(path);
    }

    close_epub_zip();
    free_wrapped_lines();
    printf("[OK] Saved bard_files.bmp, bard_reading.bmp, bard_chapters.bmp, bard_help.bmp to %s\n", out_dir);
    return 0;
}

/* ============================================================================
 * 9. MAIN ENTRY POINT & EVENT LOOP
 * ============================================================================ */

static void open_chapter_list(void) {
    if (g_chapter_count <= 0) return;
    g_state   = STATE_CHAPTER_LIST;
    g_toc_sel = g_cur_chapter;
    if (g_toc_sel < g_toc_scroll) {
        g_toc_scroll = g_toc_sel;
    } else if (g_toc_sel >= g_toc_scroll + TOC_ROWS) {
        g_toc_scroll = g_toc_sel - TOC_ROWS + 1;
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    const char *cli_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            return run_verify_epub(argv[i + 1]);
        } else if ((strcmp(argv[i], "--screenshot") == 0 ||
                    strcmp(argv[i], "--screenshot-states") == 0) && i + 1 < argc) {
            const char *out_dir = argv[i + 1];
            const char *media_dir = (i + 2 < argc && argv[i + 2][0] != '-') ? argv[i + 2] : "./media";
            return run_screenshots(out_dir, media_dir);
        } else if (argv[i][0] != '-') {
            cli_path = argv[i];
        }
    }

    resolve_state_file_path();

    struct stat st;
    bool opened_from_arg = false;
    if (cli_path && stat(cli_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            scan_directory(cli_path);
        } else {
            char dir_buf[PATH_MAX];
            extract_dirname(cli_path, dir_buf, sizeof(dir_buf));
            scan_directory(dir_buf[0] ? dir_buf : ".");
            if (open_epub_book(cli_path)) {
                g_state = STATE_READING;
                opened_from_arg = true;
            }
        }
    } else if (stat("/userdata/books", &st) == 0 && S_ISDIR(st.st_mode)) {
        scan_directory("/userdata/books");
    } else if (stat("/userdata/media", &st) == 0 && S_ISDIR(st.st_mode)) {
        scan_directory("/userdata/media");
    } else if (stat("./media", &st) == 0 && S_ISDIR(st.st_mode)) {
        scan_directory("./media");
    } else if (stat("/userdata", &st) == 0 && S_ISDIR(st.st_mode)) {
        scan_directory("/userdata");
    } else {
        scan_directory(".");
    }

    if (!opened_from_arg && !cli_path) {
        if (!load_and_resume_reader_state()) {
            g_state = STATE_FILE_BROWSER;
        }
    }

    if (!platform_init("Anbernic RG35XXSP EPUB Reader (Bard)")) {
        fprintf(stderr, "Failed to initialize platform backend\n");
        close_epub_zip();
        free_wrapped_lines();
        return 1;
    }

    input_state_t input;
    while (!g_sig_quit && platform_poll_input(&input)) {
        uint32_t p = input.pressed;

        /* MENU (Esc on desktop): SOLE quit key */
        if (p & BTN_MENU) {
            break;
        }

        if (g_state == STATE_FILE_BROWSER) {
            if (p & BTN_UP) {
                if (g_file_sel > 0) g_file_sel--;
                if (g_file_sel < g_file_scroll) g_file_scroll = g_file_sel;
            } else if (p & BTN_DOWN) {
                if (g_file_sel + 1 < g_file_count) g_file_sel++;
                if (g_file_sel >= g_file_scroll + BROWSER_ROWS) {
                    g_file_scroll = g_file_sel - BROWSER_ROWS + 1;
                }
            } else if (p & BTN_L1) {
                g_file_sel -= BROWSER_ROWS;
                if (g_file_sel < 0) g_file_sel = 0;
                if (g_file_sel < g_file_scroll) g_file_scroll = g_file_sel;
            } else if (p & BTN_R1) {
                g_file_sel += BROWSER_ROWS;
                if (g_file_sel >= g_file_count) g_file_sel = (g_file_count > 0) ? (g_file_count - 1) : 0;
                if (g_file_sel >= g_file_scroll + BROWSER_ROWS) {
                    g_file_scroll = g_file_sel - BROWSER_ROWS + 1;
                }
            } else if (p & BTN_A) {
                if (g_file_sel >= 0 && g_file_sel < g_file_count) {
                    char target[PATH_MAX + 512];
                    snprintf(target, sizeof(target), "%s/%s", g_cwd, g_files[g_file_sel].name);
                    if (g_files[g_file_sel].is_dir) {
                        scan_directory(target);
                    } else if (open_epub_book(target)) {
                        g_state = STATE_READING;
                        save_reader_state();
                    }
                }
            } else if (p & (BTN_B | BTN_LEFT)) {
                if (strcmp(g_cwd, "/") != 0) {
                    char parent[PATH_MAX + 32];
                    snprintf(parent, sizeof(parent), "%s/..", g_cwd);
                    scan_directory(parent);
                }
            } else if (p & BTN_SELECT) {
                if (g_chapter_count > 0) {
                    g_state = STATE_READING;
                }
            } else if (p & BTN_Y) {
                g_help_prev_state = STATE_FILE_BROWSER;
                g_state = STATE_HELP;
            }
        } else if (g_state == STATE_READING) {
            if (p & (BTN_DOWN | BTN_R1 | BTN_RIGHT)) {
                if (g_cur_page + 1 < g_total_pages) {
                    g_cur_page++;
                    save_reader_state();
                } else if (g_cur_chapter + 1 < g_chapter_count) {
                    load_chapter(g_cur_chapter + 1);
                    save_reader_state();
                }
            } else if (p & (BTN_UP | BTN_L1 | BTN_LEFT)) {
                if (g_cur_page > 0) {
                    g_cur_page--;
                    save_reader_state();
                } else if (g_cur_chapter > 0) {
                    load_chapter(g_cur_chapter - 1);
                    g_cur_page = g_total_pages - 1;
                    save_reader_state();
                }
            } else if (p & BTN_SELECT) {
                open_chapter_list();
            } else if (p & BTN_X) {
                g_state = STATE_FILE_BROWSER;
            } else if (p & BTN_B) {
                g_dark_theme = !g_dark_theme;
                save_reader_state();
            } else if (p & BTN_Y) {
                g_help_prev_state = STATE_READING;
                g_state = STATE_HELP;
            }
        } else if (g_state == STATE_HELP) {
            if (p & BTN_SELECT) {
                if (g_chapter_count > 0) open_chapter_list();
                else g_state = STATE_FILE_BROWSER;
            } else if (p & BTN_X) {
                g_state = STATE_FILE_BROWSER;
            } else if (p & (BTN_Y | BTN_B | BTN_A)) {
                g_state = (g_chapter_count > 0) ? g_help_prev_state : STATE_FILE_BROWSER;
            }
        } else if (g_state == STATE_CHAPTER_LIST) {
            if (p & BTN_UP) {
                if (g_toc_sel > 0) g_toc_sel--;
                if (g_toc_sel < g_toc_scroll) g_toc_scroll = g_toc_sel;
            } else if (p & BTN_DOWN) {
                if (g_toc_sel + 1 < g_chapter_count) g_toc_sel++;
                if (g_toc_sel >= g_toc_scroll + TOC_ROWS) {
                    g_toc_scroll = g_toc_sel - TOC_ROWS + 1;
                }
            } else if (p & BTN_L1) {
                g_toc_sel -= TOC_ROWS;
                if (g_toc_sel < 0) g_toc_sel = 0;
                if (g_toc_sel < g_toc_scroll) g_toc_scroll = g_toc_sel;
            } else if (p & BTN_R1) {
                g_toc_sel += TOC_ROWS;
                if (g_toc_sel >= g_chapter_count) g_toc_sel = g_chapter_count - 1;
                if (g_toc_sel >= g_toc_scroll + TOC_ROWS) {
                    g_toc_scroll = g_toc_sel - TOC_ROWS + 1;
                }
            } else if (p & BTN_A) {
                load_chapter(g_toc_sel);
                g_state = STATE_READING;
                save_reader_state();
            } else if (p & (BTN_B | BTN_SELECT)) {
                g_state = STATE_READING;
            } else if (p & BTN_Y) {
                g_help_prev_state = STATE_READING;
                g_state = STATE_HELP;
            }
        }

        render_ui();
        platform_present();
        delay_ms(16);
    }

    save_reader_state();
    close_epub_zip();
    free_wrapped_lines();
    platform_shutdown();
    return 0;
}
