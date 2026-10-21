/*
 * boot_logo.c - Surgical Boot Logo Modifier for Anbernic RG 35xxSP (Knulli Scarab)
 *
 * Targets:
 *   1. Anbernic RG 35xxSP (Allwinner H700 Cortex-A53 aarch64, Knulli Scarab)
 *      - Single 640x480 4:3 IPS screen via SDL2 (dlopen) with /dev/fb0 mmap fallback
 *      - Direct Linux evdev (/dev/input/event*) input polling
 *      - Surgical Option A modification: writes 1,228,938-byte 640x480 32-bit
 *        BITMAPV5HEADER BGRA .bmp to /boot/bootlogo.bmp on Partition 3 (FAT32
 *        boot-resource, LABEL=KNULLI) with automatic stock backup & SHA-256 read-back
 *        verification.
 *   2. Linux Desktop Simulator (x86_64 -DHOST_X11)
 *      - 640x480 X11 window with full keyboard (A, B, X, Y, R, L, Enter, Esc)
 *        and cursor key (Up, Down, Left, Right) support for debugging & verification.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ============================================================================
 * 1. CONSTANTS, COLOR MACROS, & INPUT DEFINITIONS
 * ============================================================================ */

#define NATIVE_W  640
#define NATIVE_H  480
#define EXPECTED_BMP_BYTES 1228938U

#define RGB_COLOR(r, g, b) \
    (0xFF000000u | (((uint32_t)(r) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu))

#include "../rg35xxsp_input.h"

typedef struct {
    uint32_t held;
    uint32_t pressed;
    uint32_t released;
} input_state_t;

static uint32_t g_framebuf[NATIVE_W * NATIVE_H];
static volatile sig_atomic_t g_sig_quit = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_sig_quit = 1;
}

static void delay_ms(uint32_t ms) {
    usleep((useconds_t)ms * 1000u);
}

/* ============================================================================
 * 2. PURE-C SHA-256 IMPLEMENTATION (FOR READ-BACK FLASH VERIFICATION)
 * ============================================================================ */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[64];
} sha256_ctx_t;

static const uint32_t k_sha256[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4 + 0] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8)  |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k_sha256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_compute(const uint8_t *data, size_t len, uint8_t out_hash[32]) {
    sha256_ctx_t ctx;
    ctx.state[0] = 0x6a09e667UL;
    ctx.state[1] = 0xbb67ae85UL;
    ctx.state[2] = 0x3c6ef372UL;
    ctx.state[3] = 0xa54ff53aUL;
    ctx.state[4] = 0x510e527fUL;
    ctx.state[5] = 0x9b05688cUL;
    ctx.state[6] = 0x1f83d9abUL;
    ctx.state[7] = 0x5be0cd19UL;
    ctx.bitcount = 0;

    size_t idx = 0;
    while (len >= 64) {
        sha256_transform(&ctx, data + idx);
        ctx.bitcount += 512;
        idx += 64;
        len -= 64;
    }
    memcpy(ctx.buffer, data + idx, len);
    ctx.bitcount += (uint64_t)len * 8ULL;

    ctx.buffer[len++] = 0x80;
    if (len > 56) {
        memset(ctx.buffer + len, 0, 64 - len);
        sha256_transform(&ctx, ctx.buffer);
        len = 0;
    }
    memset(ctx.buffer + len, 0, 56 - len);
    for (int i = 0; i < 8; i++) {
        ctx.buffer[56 + i] = (uint8_t)(ctx.bitcount >> (56 - i * 8));
    }
    sha256_transform(&ctx, ctx.buffer);

    for (int i = 0; i < 8; i++) {
        out_hash[i * 4 + 0] = (uint8_t)(ctx.state[i] >> 24);
        out_hash[i * 4 + 1] = (uint8_t)(ctx.state[i] >> 16);
        out_hash[i * 4 + 2] = (uint8_t)(ctx.state[i] >> 8);
        out_hash[i * 4 + 3] = (uint8_t)(ctx.state[i]);
    }
}

/* ============================================================================
 * 3. PLATFORM VIDEO & INPUT BACKEND (RG35XXSP SDL2/FBDEV + EVDEV vs HOST X11)
 * ============================================================================ */

static uint32_t g_prev_buttons = 0;
static uint32_t g_cur_buttons  = 0;

#if defined(HOST_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

static Display *g_x11_dpy = NULL;
static Window   g_x11_win = 0;
static GC       g_x11_gc  = 0;
static XImage  *g_x11_img = NULL;
static Atom     g_wm_delete_window = 0;
static char    *g_x11_buf = NULL;

static int platform_init(const char *title) {
    g_x11_dpy = XOpenDisplay(NULL);
    if (!g_x11_dpy) return -1;

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
    if (!g_x11_buf) return -1;

    g_x11_img = XCreateImage(
        g_x11_dpy, DefaultVisual(g_x11_dpy, scr),
        DefaultDepth(g_x11_dpy, scr), ZPixmap, 0,
        g_x11_buf, NATIVE_W, NATIVE_H, 32, 0
    );
    return 0;
}

static void platform_present(void) {
    if (!g_x11_dpy || !g_x11_img) return;
    memcpy(g_x11_buf, g_framebuf, (size_t)NATIVE_W * NATIVE_H * sizeof(uint32_t));
    XPutImage(g_x11_dpy, g_x11_win, g_x11_gc, g_x11_img, 0, 0, 0, 0, NATIVE_W, NATIVE_H);
    XFlush(g_x11_dpy);
}

static uint32_t translate_x11_keysym(KeySym ks) {
    return rg35xxsp_map_x11_keysym(ks);
}

static uint32_t xkey_to_btn(XKeyEvent *xkey) {
    char buf[8] = {0};
    KeySym sym = NoSymbol;
    XLookupString(xkey, buf, sizeof(buf) - 1, &sym, NULL);
    if (sym == NoSymbol) {
        sym = XLookupKeysym(xkey, 0);
    }
    uint32_t mask = translate_x11_keysym(sym);
    if (!mask) {
        KeySym base_sym = XLookupKeysym(xkey, 0);
        if (base_sym != NoSymbol && base_sym != sym) {
            mask = translate_x11_keysym(base_sym);
        }
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
            if ((Atom)xev.xclient.data.l[0] == g_wm_delete_window) {
                return false;
            }
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

#else /* TARGET_RG35XXSP (Allwinner H700 aarch64: SDL2 dlopen + /dev/fb0 + evdev) */

#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

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

static int   (*fn_SDL2_Init)(uint32_t) = NULL;
static void  (*fn_SDL2_Quit)(void) = NULL;
static void* (*fn_SDL2_CreateWindow)(const char*, int, int, int, int, uint32_t) = NULL;
static void  (*fn_SDL2_DestroyWindow)(void*) = NULL;
static void* (*fn_SDL2_CreateRenderer)(void*, int, uint32_t) = NULL;
static void  (*fn_SDL2_DestroyRenderer)(void*) = NULL;
static void* (*fn_SDL2_CreateTexture)(void*, uint32_t, int, int, int) = NULL;
static void  (*fn_SDL2_DestroyTexture)(void*) = NULL;
static int   (*fn_SDL2_UpdateTexture)(void*, const void*, const void*, int) = NULL;
static int   (*fn_SDL2_RenderClear)(void*) = NULL;
static int   (*fn_SDL2_RenderCopy)(void*, void*, const void*, const void*) = NULL;
static void  (*fn_SDL2_RenderPresent)(void*) = NULL;
static int   (*fn_SDL2_PollEvent)(SDL2_RawEvent*) = NULL;
static int   (*fn_SDL2_ShowCursor)(int) = NULL;

#define MAX_EVDEV_FDS 32
typedef struct {
    int fd;
    int hat_x;
    int hat_y;
} EvdevDevice;

static EvdevDevice g_evdevs[MAX_EVDEV_FDS];
static int         g_num_evdevs = 0;

static int                      g_fb_fd = -1;
static uint8_t                 *g_fb_mem = NULL;
static size_t                   g_fb_map_size = 0;
static struct fb_var_screeninfo g_fb_vinfo;
static struct fb_fix_screeninfo g_fb_finfo;

static bool init_fbdev_fallback(void) {
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) return false;

    if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_fb_finfo) < 0 ||
        ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_fb_vinfo) < 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
        return false;
    }

    g_fb_map_size = g_fb_finfo.smem_len;
    if (g_fb_map_size == 0) {
        g_fb_map_size = (size_t)g_fb_finfo.line_length * g_fb_vinfo.yres_virtual;
    }

    g_fb_mem = (uint8_t *)mmap(NULL, g_fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        g_fb_mem = NULL;
        close(g_fb_fd);
        g_fb_fd = -1;
        return false;
    }
    return true;
}

static int platform_init(const char *title) {
    g_num_evdevs = 0;
    for (int i = 0; i < MAX_EVDEV_FDS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            g_evdevs[g_num_evdevs].fd    = fd;
            g_evdevs[g_num_evdevs].hat_x = 0;
            g_evdevs[g_num_evdevs].hat_y = 0;
            g_num_evdevs++;
        }
    }

    g_sdl2_lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (g_sdl2_lib) {
        fn_SDL2_Init            = dlsym(g_sdl2_lib, "SDL_Init");
        fn_SDL2_Quit            = dlsym(g_sdl2_lib, "SDL_Quit");
        fn_SDL2_CreateWindow    = dlsym(g_sdl2_lib, "SDL_CreateWindow");
        fn_SDL2_DestroyWindow   = dlsym(g_sdl2_lib, "SDL_DestroyWindow");
        fn_SDL2_CreateRenderer  = dlsym(g_sdl2_lib, "SDL_CreateRenderer");
        fn_SDL2_DestroyRenderer = dlsym(g_sdl2_lib, "SDL_DestroyRenderer");
        fn_SDL2_CreateTexture   = dlsym(g_sdl2_lib, "SDL_CreateTexture");
        fn_SDL2_DestroyTexture  = dlsym(g_sdl2_lib, "SDL_DestroyTexture");
        fn_SDL2_UpdateTexture   = dlsym(g_sdl2_lib, "SDL_UpdateTexture");
        fn_SDL2_RenderClear     = dlsym(g_sdl2_lib, "SDL_RenderClear");
        fn_SDL2_RenderCopy      = dlsym(g_sdl2_lib, "SDL_RenderCopy");
        fn_SDL2_RenderPresent   = dlsym(g_sdl2_lib, "SDL_RenderPresent");
        fn_SDL2_PollEvent       = dlsym(g_sdl2_lib, "SDL_PollEvent");
        fn_SDL2_ShowCursor      = dlsym(g_sdl2_lib, "SDL_ShowCursor");

        if (fn_SDL2_Init && fn_SDL2_Init(SDL2_INIT_VIDEO) == 0 &&
            fn_SDL2_CreateWindow && fn_SDL2_CreateRenderer && fn_SDL2_CreateTexture) {
            if (fn_SDL2_ShowCursor) fn_SDL2_ShowCursor(0);
            g_sdl2_win = fn_SDL2_CreateWindow(
                title,
                SDL2_WINDOWPOS_UNDEFINED, SDL2_WINDOWPOS_UNDEFINED,
                NATIVE_W, NATIVE_H,
                SDL2_WINDOW_FULLSCREEN_DESKTOP | SDL2_WINDOW_SHOWN
            );
            if (g_sdl2_win) {
                g_sdl2_ren = fn_SDL2_CreateRenderer(
                    g_sdl2_win, -1,
                    SDL2_RENDERER_ACCELERATED | SDL2_RENDERER_PRESENTVSYNC
                );
                if (!g_sdl2_ren) {
                    g_sdl2_ren = fn_SDL2_CreateRenderer(g_sdl2_win, -1, 0);
                }
                if (g_sdl2_ren) {
                    g_sdl2_tex = fn_SDL2_CreateTexture(
                        g_sdl2_ren, SDL2_PIXELFORMAT_ARGB8888,
                        SDL2_TEXTUREACCESS_STREAMING,
                        NATIVE_W, NATIVE_H
                    );
                    if (g_sdl2_tex) {
                        g_use_sdl2_video = true;
                    }
                }
            }
        }
    }

    if (!g_use_sdl2_video) {
        if (!init_fbdev_fallback()) {
            return -1;
        }
    }
    return 0;
}

static void platform_present(void) {
    if (g_use_sdl2_video) {
        fn_SDL2_UpdateTexture(g_sdl2_tex, NULL, g_framebuf, NATIVE_W * 4);
        fn_SDL2_RenderClear(g_sdl2_ren);
        fn_SDL2_RenderCopy(g_sdl2_ren, g_sdl2_tex, NULL, NULL);
        fn_SDL2_RenderPresent(g_sdl2_ren);
        return;
    }

    if (!g_fb_mem) return;

    int fb_w   = (int)g_fb_vinfo.xres;
    int fb_h   = (int)g_fb_vinfo.yres;
    int stride = (int)g_fb_finfo.line_length;
    int bpp    = (int)g_fb_vinfo.bits_per_pixel;
    int pages  = 1;
    if ((int)g_fb_vinfo.yres_virtual >= fb_h * 2 &&
        (size_t)(fb_h * 2 * stride) <= g_fb_map_size) {
        pages = 2;
    }

    for (int page = 0; page < pages; page++) {
        uint8_t *page_base = g_fb_mem + (size_t)(page * fb_h) * stride;
        if (bpp == 32) {
            if (fb_w == NATIVE_W && fb_h == NATIVE_H) {
                for (int y = 0; y < fb_h; y++) {
                    memcpy(page_base + y * stride,
                           g_framebuf + y * NATIVE_W,
                           (size_t)fb_w * 4);
                }
            } else {
                for (int y = 0; y < fb_h; y++) {
                    int sy = (y * NATIVE_H) / fb_h;
                    uint32_t *dst_row = (uint32_t *)(page_base + y * stride);
                    const uint32_t *src_row = g_framebuf + sy * NATIVE_W;
                    for (int x = 0; x < fb_w; x++) {
                        dst_row[x] = src_row[(x * NATIVE_W) / fb_w];
                    }
                }
            }
        } else if (bpp == 16) {
            for (int y = 0; y < fb_h; y++) {
                int sy = (y * NATIVE_H) / fb_h;
                uint16_t *dst_row = (uint16_t *)(page_base + y * stride);
                const uint32_t *src_row = g_framebuf + sy * NATIVE_W;
                for (int x = 0; x < fb_w; x++) {
                    uint32_t c = src_row[(x * NATIVE_W) / fb_w];
                    uint16_t r = (uint16_t)((c >> 19) & 0x1F);
                    uint16_t g = (uint16_t)((c >> 10) & 0x3F);
                    uint16_t b = (uint16_t)((c >> 3)  & 0x1F);
                    dst_row[x] = (uint16_t)((r << 11) | (g << 5) | b);
                }
            }
        }
    }

    g_fb_vinfo.xoffset = 0;
    g_fb_vinfo.yoffset = 0;
    ioctl(g_fb_fd, FBIOPAN_DISPLAY, &g_fb_vinfo);
}

static uint32_t translate_rg35xxsp_evkey(uint16_t code) {
    return rg35xxsp_map_evkey((int)code);
}

static bool platform_poll_input(input_state_t *out) {
    uint32_t pressed_latch  = 0;
    uint32_t released_latch = 0;

    if (g_use_sdl2_video && fn_SDL2_PollEvent) {
        SDL2_RawEvent raw_ev;
        while (fn_SDL2_PollEvent(&raw_ev)) {
            if (raw_ev.type == SDL2_QUIT_EVENT) {
                return false;
            }
        }
    }

    for (int i = 0; i < g_num_evdevs; i++) {
        struct input_event ie;
        while (read(g_evdevs[i].fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
            if (ie.type == EV_KEY && (ie.value == 0 || ie.value == 1)) {
                uint32_t mask = translate_rg35xxsp_evkey(ie.code);
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
        if (g_evdevs[i].fd >= 0) close(g_evdevs[i].fd);
    }
    g_num_evdevs = 0;

    if (g_use_sdl2_video) {
        if (g_sdl2_tex && fn_SDL2_DestroyTexture)   fn_SDL2_DestroyTexture(g_sdl2_tex);
        if (g_sdl2_ren && fn_SDL2_DestroyRenderer)  fn_SDL2_DestroyRenderer(g_sdl2_ren);
        if (g_sdl2_win && fn_SDL2_DestroyWindow)    fn_SDL2_DestroyWindow(g_sdl2_win);
        g_sdl2_tex = NULL;
        g_sdl2_ren = NULL;
        g_sdl2_win = NULL;
        g_use_sdl2_video = false;
    }
    if (fn_SDL2_Quit) {
        fn_SDL2_Quit();
        fn_SDL2_Quit = NULL;
    }
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
 * 4. BMP LOADER, THEME DEFINITIONS, & SURGICAL /boot/bootlogo.bmp FLASHER
 * ============================================================================ */

static inline uint32_t read_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0]) | ((uint16_t)p[1] << 8));
}

typedef struct {
    const char *short_name;
    const char *title;
    const char *subtitle;
    const char *bmp_path;
    bool        is_stock;
    uint32_t   *pixels;      /* 640x480 ARGB8888 */
    uint8_t     sha256[32];
    bool        loaded;
} logo_theme_t;

#define NUM_THEMES 5

static logo_theme_t g_themes[NUM_THEMES] = {
    {
        .short_name = "1:KISS",
        .title      = "STARCROSSED KISS",
        .subtitle   = "Soulful girl blowing a glowing heart kiss from bottom left",
        .bmp_path   = "logos/theme1.bmp",
        .is_stock   = false,
    },
    {
        .short_name = "2:GBA CHEF",
        .title      = "GBA PIXEL KITCHEN",
        .subtitle   = "Wario & Tux whimsically cooking up a storm on a GBA",
        .bmp_path   = "logos/theme2.bmp",
        .is_stock   = false,
    },
    {
        .short_name = "3:TUX SKY",
        .title      = "TUX STARRY SKY",
        .subtitle   = "Tux on a glowing twilight cliff looking up at the sky",
        .bmp_path   = "logos/theme3.bmp",
        .is_stock   = false,
    },
    {
        .short_name = "4:POETRY",
        .title      = "MIDNIGHT POETRY",
        .subtitle   = "Sleek minimalist girl writing poetry under warm lamp light",
        .bmp_path   = "logos/theme4.bmp",
        .is_stock   = false,
    },
    {
        .short_name = "5:STOCK",
        .title      = "KNULLI SCARAB STOCK",
        .subtitle   = "Original factory Knulli Scarab bootlogo.bmp (Revert to Stock)",
        .bmp_path   = "logos/stock.bmp",
        .is_stock   = true,
    }
};

static uint8_t *read_entire_file(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    if (out_size) *out_size = (size_t)sz;
    return buf;
}

static bool write_entire_file(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t written = fwrite(data, 1, size, fp);
    fflush(fp);
    int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
    fclose(fp);
    return (written == size);
}

static bool copy_file_binary(const char *src_path, const char *dst_path) {
    size_t sz = 0;
    uint8_t *buf = read_entire_file(src_path, &sz);
    if (!buf) return false;
    bool ok = write_entire_file(dst_path, buf, sz);
    free(buf);
    return ok;
}

/* Load a 24-bit or 32-bit 640x480 BMP into an ARGB8888 buffer and compute its SHA-256 */
static bool load_bmp_to_argb(const char *path, uint32_t *out_pixels, uint8_t out_sha256[32]) {
    for (int i = 0; i < NATIVE_W * NATIVE_H; i++) {
        out_pixels[i] = RGB_COLOR(0, 0, 0);
    }

    size_t file_sz = 0;
    uint8_t *file_buf = read_entire_file(path, &file_sz);
    if (!file_buf || file_sz < 54 || file_buf[0] != 'B' || file_buf[1] != 'M') {
        free(file_buf);
        return false;
    }

    if (out_sha256) {
        sha256_compute(file_buf, file_sz, out_sha256);
    }

    uint32_t off_bits   = read_le32(file_buf + 10);
    int32_t  width      = (int32_t)read_le32(file_buf + 18);
    int32_t  height_raw = (int32_t)read_le32(file_buf + 22);
    uint16_t bpp        = read_le16(file_buf + 28);
    bool     top_down   = (height_raw < 0);
    int32_t  height     = top_down ? -height_raw : height_raw;

    if (width <= 0 || height <= 0 || (bpp != 24 && bpp != 32) || off_bits >= file_sz) {
        free(file_buf);
        return false;
    }

    int bytes_per_pixel = bpp / 8;
    size_t row_stride = ((size_t)width * bytes_per_pixel + 3U) & ~3U;
    int off_x = (NATIVE_W - width) / 2;
    int off_y = (NATIVE_H - height) / 2;

    for (int row = 0; row < height; row++) {
        size_t row_off = (size_t)off_bits + (size_t)row * row_stride;
        if (row_off + row_stride > file_sz) break;
        const uint8_t *row_buf = file_buf + row_off;
        int dst_y = off_y + (top_down ? row : (height - 1 - row));
        if (dst_y < 0 || dst_y >= NATIVE_H) continue;
        for (int col = 0; col < width; col++) {
            int dst_x = off_x + col;
            if (dst_x < 0 || dst_x >= NATIVE_W) continue;
            uint8_t b = row_buf[col * bytes_per_pixel + 0];
            uint8_t g = row_buf[col * bytes_per_pixel + 1];
            uint8_t r = row_buf[col * bytes_per_pixel + 2];
            out_pixels[dst_y * NATIVE_W + dst_x] = RGB_COLOR(r, g, b);
        }
    }

    free(file_buf);
    return true;
}

/*
 * Resolve the boot partition mount directory & bootlogo.bmp path.
 * On RG35XXSP hardware: /boot (Partition 3 FAT32 boot-resource, LABEL=KNULLI).
 * On HOST_X11 desktop simulator: /tmp/rg35xxsp_boot_logo_sim/boot
 */
static const char *get_boot_dir(void) {
#if defined(HOST_X11)
    const char *env = getenv("RG35XXSP_BOOT_DIR");
    if (env && env[0]) return env;
    mkdir("/tmp/rg35xxsp_boot_logo_sim", 0755);
    mkdir("/tmp/rg35xxsp_boot_logo_sim/boot", 0755);
    if (access("/tmp/rg35xxsp_boot_logo_sim/boot/bootlogo.bmp", F_OK) != 0) {
        copy_file_binary("logos/stock.bmp", "/tmp/rg35xxsp_boot_logo_sim/boot/bootlogo.bmp");
    }
    return "/tmp/rg35xxsp_boot_logo_sim/boot";
#else
    const char *env = getenv("RG35XXSP_BOOT_DIR");
    if (env && env[0]) return env;
    return "/boot";
#endif
}

static void resolve_bootlogo_target_path(char *out_path, size_t out_sz) {
    const char *boot_dir = get_boot_dir();
    char upper_path[512];
    char lower_path[512];
    snprintf(upper_path, sizeof(upper_path), "%s/BOOTLOGO.BMP", boot_dir);
    snprintf(lower_path, sizeof(lower_path), "%s/bootlogo.bmp", boot_dir);
    if (access(lower_path, F_OK) == 0) {
        snprintf(out_path, out_sz, "%s", lower_path);
    } else if (access(upper_path, F_OK) == 0) {
        snprintf(out_path, out_sz, "%s", upper_path);
    } else {
        snprintf(out_path, out_sz, "%s", lower_path);
    }
}

static void remount_boot_rw(bool rw) {
#if !defined(HOST_X11)
    if (rw) {
        /* Ensure /boot is mounted and remounted read-write */
        int r1 = system("mount -o remount,rw /boot >/dev/null 2>&1 || mount -t vfat -o rw /dev/mmcblk0p3 /boot >/dev/null 2>&1");
        (void)r1;
    } else {
        int r2 = system("mount -o remount,ro /boot >/dev/null 2>&1");
        (void)r2;
    }
#else
    (void)rw;
#endif
}

/* Check which theme (0..NUM_THEMES-1) currently matches /boot/bootlogo.bmp, or -1 */
static int detect_active_bootlogo_theme(void) {
    char target_path[512];
    resolve_bootlogo_target_path(target_path, sizeof(target_path));
    size_t sz = 0;
    uint8_t *buf = read_entire_file(target_path, &sz);
    if (!buf) return -1;
    uint8_t live_sha[32];
    sha256_compute(buf, sz, live_sha);
    free(buf);

    for (int i = 0; i < NUM_THEMES; i++) {
        if (g_themes[i].loaded && memcmp(live_sha, g_themes[i].sha256, 32) == 0) {
            return i;
        }
    }
    return -1;
}

static bool has_stock_backup(void) {
    return (access("backup/bootlogo.device_orig.bmp", R_OK) == 0 ||
            access("backup/bootlogo.stock.bmp", R_OK) == 0 ||
            access("logos/stock.bmp", R_OK) == 0);
}

/*
 * Surgically flash the selected theme (or restore the stock image) to /boot/bootlogo.bmp:
 *   1. Ensures ./backup/bootlogo.stock.bmp and ./backup/bootlogo.device_orig.bmp exist
 *      before modifying /boot/bootlogo.bmp.
 *   2. Remounts /boot read-write (on RG35XXSP hardware).
 *   3. Verifies the source BMP is exactly 1,228,938 bytes (138-byte BITMAPV5HEADER +
 *      640x480x4 BGRA).
 *   4. Writes /boot/bootlogo.bmp, flushes with fsync() + sync(), and reads back
 *      /boot/bootlogo.bmp to verify its SHA-256 digest matches.
 *   5. Remounts /boot read-only.
 */
static bool flash_bootlogo(
    int theme_idx,
    bool force_restore_stock,
    char *status_headline, size_t headline_sz,
    char *status_detail,   size_t detail_sz)
{
    mkdir("backup", 0755);

    char target_path[512];
    resolve_bootlogo_target_path(target_path, sizeof(target_path));

    remount_boot_rw(true);

    /* 1. Back up the live /boot/bootlogo.bmp before the very first modification */
    if (access("backup/bootlogo.device_orig.bmp", F_OK) != 0 && access(target_path, R_OK) == 0) {
        copy_file_binary(target_path, "backup/bootlogo.device_orig.bmp");
    }
    if (access("backup/bootlogo.stock.bmp", F_OK) != 0) {
        if (access("logos/stock.bmp", R_OK) == 0) {
            copy_file_binary("logos/stock.bmp", "backup/bootlogo.stock.bmp");
        } else if (access(target_path, R_OK) == 0) {
            copy_file_binary(target_path, "backup/bootlogo.stock.bmp");
        }
    }

    /* Also store a backup copy on /boot/bootlogo.stock.bmp if not present */
    char boot_bak_path[512];
    snprintf(boot_bak_path, sizeof(boot_bak_path), "%s/bootlogo.stock.bmp", get_boot_dir());
    if (access(boot_bak_path, F_OK) != 0 && access("backup/bootlogo.stock.bmp", R_OK) == 0) {
        copy_file_binary("backup/bootlogo.stock.bmp", boot_bak_path);
    }

    /* 2. Choose the source .bmp file */
    const char *src_path = g_themes[theme_idx].bmp_path;
    if (force_restore_stock || g_themes[theme_idx].is_stock) {
        if (access("backup/bootlogo.stock.bmp", R_OK) == 0) {
            src_path = "backup/bootlogo.stock.bmp";
        } else if (access("backup/bootlogo.device_orig.bmp", R_OK) == 0) {
            src_path = "backup/bootlogo.device_orig.bmp";
        } else {
            src_path = "logos/stock.bmp";
        }
    }

    size_t src_sz = 0;
    uint8_t *src_buf = read_entire_file(src_path, &src_sz);
    if (!src_buf || src_sz != EXPECTED_BMP_BYTES || src_buf[0] != 'B' || src_buf[1] != 'M') {
        free(src_buf);
        remount_boot_rw(false);
        snprintf(status_headline, headline_sz, "ERROR: INVALID SOURCE BMP");
        snprintf(status_detail, detail_sz, "Expected %u bytes in %s (got %zu)",
                 EXPECTED_BMP_BYTES, src_path, src_sz);
        return false;
    }

    uint8_t expected_sha[32];
    sha256_compute(src_buf, src_sz, expected_sha);

    /* 3. Write to /boot/bootlogo.bmp */
    bool write_ok = write_entire_file(target_path, src_buf, src_sz);
    free(src_buf);
    sync();

    if (!write_ok) {
        remount_boot_rw(false);
        snprintf(status_headline, headline_sz, "ERROR: WRITE TO /boot FAILED");
        snprintf(status_detail, detail_sz, "Could not write %.80s", target_path);
        return false;
    }

    /* 4. Read back /boot/bootlogo.bmp and verify SHA-256 digest */
    size_t verify_sz = 0;
    uint8_t *verify_buf = read_entire_file(target_path, &verify_sz);
    bool verified = false;
    if (verify_buf && verify_sz == EXPECTED_BMP_BYTES) {
        uint8_t actual_sha[32];
        sha256_compute(verify_buf, verify_sz, actual_sha);
        if (memcmp(actual_sha, expected_sha, 32) == 0) {
            verified = true;
        }
    }
    free(verify_buf);
    remount_boot_rw(false);

    if (!verified) {
        snprintf(status_headline, headline_sz, "ERROR: SHA-256 VERIFY FAILED");
        snprintf(status_detail, detail_sz, "Read-back mismatch on %.80s", target_path);
        return false;
    }

    if (force_restore_stock || g_themes[theme_idx].is_stock) {
        snprintf(status_headline, headline_sz, "STOCK BOOT LOGO RESTORED!");
    } else {
        snprintf(status_headline, headline_sz, "BOOT LOGO FLASHED & VERIFIED!");
    }
    snprintf(status_detail, detail_sz,
             "/boot/bootlogo.bmp (%u B) | SHA256 %02x%02x%02x%02x..",
             EXPECTED_BMP_BYTES,
             expected_sha[0], expected_sha[1], expected_sha[2], expected_sha[3]);
    return true;
}

/* ============================================================================
 * 5. NATIVE 640x480 DRAWING PRIMITIVES & 5x7 BITMAP FONT
 * ============================================================================ */

static const uint8_t font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* 32 ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* 33 '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* 34 '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* 35 '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* 36 '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* 37 '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* 38 '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* 39 ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* 40 '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* 41 ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* 42 '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* 43 '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* 44 ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* 45 '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* 46 '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* 47 '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 48 '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 49 '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 50 '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 51 '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 52 '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 53 '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 54 '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 55 '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 56 '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 57 '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* 58 ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* 59 ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* 60 '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* 61 '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* 62 '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* 63 '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* 64 '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 65 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 66 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 67 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 68 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 69 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 70 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 71 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 72 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 73 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 74 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 75 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 76 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 77 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 78 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 79 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 80 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 81 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 82 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 83 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 84 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 85 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 86 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 87 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 88 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 89 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 90 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* 91 '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* 92 '\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* 93 ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* 94 '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* 95 '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* 96 '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 97 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 98 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 99 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 100 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 101 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 102 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* 103 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 104 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 105 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 106 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* 107 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 108 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* 109 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 110 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 111 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 112 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 113 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 114 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 115 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 116 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 117 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 118 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* 119 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 120 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 121 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 122 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* 123 '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* 124 '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* 125 '}' */
    {0x08, 0x04, 0x08, 0x10, 0x08}  /* 126 '~' */
};

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > NATIVE_W) w = NATIVE_W - x;
    if (y + h > NATIVE_H) h = NATIVE_H - y;
    if (w <= 0 || h <= 0) return;

    for (int r = y; r < y + h; r++) {
        uint32_t *row = g_framebuf + r * NATIVE_W + x;
        for (int c = 0; c < w; c++) {
            row[c] = color;
        }
    }
}

static void draw_rect(int x, int y, int w, int h, int thickness, uint32_t color) {
    fill_rect(x, y, w, thickness, color);
    fill_rect(x, y + h - thickness, w, thickness, color);
    fill_rect(x, y, thickness, h, color);
    fill_rect(x + w - thickness, y, thickness, h, color);
}

static void draw_alpha_rect(int rx, int ry, int rw, int rh, uint32_t rgb, int alpha_255) {
    uint32_t sr = (rgb >> 16) & 0xFFu;
    uint32_t sg = (rgb >>  8) & 0xFFu;
    uint32_t sb =  rgb        & 0xFFu;
    uint32_t inv_a = 255u - (uint32_t)alpha_255;

    int x0 = (rx < 0) ? 0 : rx;
    int y0 = (ry < 0) ? 0 : ry;
    int x1 = (rx + rw > NATIVE_W) ? NATIVE_W : (rx + rw);
    int y1 = (ry + rh > NATIVE_H) ? NATIVE_H : (ry + rh);

    for (int y = y0; y < y1; y++) {
        uint32_t *row = g_framebuf + y * NATIVE_W;
        for (int x = x0; x < x1; x++) {
            uint32_t dst = row[x];
            uint32_t dr = (dst >> 16) & 0xFFu;
            uint32_t dg = (dst >>  8) & 0xFFu;
            uint32_t db =  dst        & 0xFFu;
            uint32_t r = (sr * (uint32_t)alpha_255 + dr * inv_a) / 255u;
            uint32_t g = (sg * (uint32_t)alpha_255 + dg * inv_a) / 255u;
            uint32_t b = (sb * (uint32_t)alpha_255 + db * inv_a) / 255u;
            row[x] = RGB_COLOR(r, g, b);
        }
    }
}

static int text_width(const char *str, int scale) {
    if (!str || !*str) return 0;
    int len = (int)strlen(str);
    return len * 6 * scale - scale;
}

static void draw_char(int x, int y, char c, int scale, uint32_t color) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[(int)c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *str, int scale, uint32_t color) {
    if (!str) return;
    int cur_x = x;
    while (*str) {
        draw_char(cur_x, y, *str, scale, color);
        cur_x += 6 * scale;
        str++;
    }
}

static void draw_centered_text(int center_x, int y, const char *str, int scale, uint32_t color) {
    int w = text_width(str, scale);
    draw_text(center_x - w / 2, y, str, scale, color);
}

static void blit_full_screen(const uint32_t *pixels) {
    if (!pixels) return;
    memcpy(g_framebuf, pixels, (size_t)NATIVE_W * NATIVE_H * sizeof(uint32_t));
}

/* ============================================================================
 * 6. SINGLE-SCREEN 640x480 UI RENDERER (HUD, MODALS, & R1 HELP SCREEN)
 * ============================================================================ */

typedef enum {
    MODAL_NONE = 0,
    MODAL_CONFIRM_FLASH,
    MODAL_CONFIRM_STOCK,
    MODAL_RESULT,
    MODAL_HELP
} ui_modal_t;

static void render_ui_frame(
    int cur_idx,
    int active_boot_idx,
    bool full_preview_mode,
    ui_modal_t modal,
    bool last_flash_ok,
    const char *status_headline,
    const char *status_detail)
{
    /* 1. Draw the full 640x480 boot logo at 1:1 hardware resolution */
    blit_full_screen(g_themes[cur_idx].pixels);

    /* 2. Top & Bottom translucent HUD bars (hidden when full_preview_mode is active) */
    if (!full_preview_mode && modal == MODAL_NONE) {
        /* Top HUD bar (0..44) */
        draw_alpha_rect(0, 0, NATIVE_W, 44, RGB_COLOR(10, 12, 24), 210);
        fill_rect(0, 44, NATIVE_W, 2, RGB_COLOR(90, 210, 255));

        draw_text(14, 15, "RG35XXSP BOOT LOGO", 2, RGB_COLOR(255, 225, 85));

        /* Theme selector pills [1..5] in top bar */
        for (int i = 0; i < NUM_THEMES; i++) {
            int px = 252 + i * 44;
            int py = 8;
            bool sel = (i == cur_idx);
            bool act = (i == active_boot_idx);
            uint32_t bg  = sel ? RGB_COLOR(55, 130, 215) : RGB_COLOR(26, 30, 56);
            uint32_t brd = sel ? RGB_COLOR(255, 235, 95) :
                           (act ? RGB_COLOR(95, 245, 155) : RGB_COLOR(75, 85, 130));
            fill_rect(px, py, 38, 28, bg);
            draw_rect(px, py, 38, 28, 2, brd);
            char num_str[8];
            snprintf(num_str, sizeof(num_str), "#%d", i + 1);
            draw_centered_text(px + 19, py + 7, num_str, 2, RGB_COLOR(255, 255, 255));
        }

        /* Y Help badge on top right */
        fill_rect(484, 8, 144, 28, RGB_COLOR(44, 34, 78));
        draw_rect(484, 8, 144, 28, 2, RGB_COLOR(195, 155, 255));
        draw_centered_text(556, 15, "[Y] HELP", 1, RGB_COLOR(235, 220, 255));

        /* Bottom HUD bar (384..480) */
        draw_alpha_rect(0, 384, NATIVE_W, 96, RGB_COLOR(10, 12, 24), 218);
        fill_rect(0, 384, NATIVE_W, 2, RGB_COLOR(255, 205, 65));

        char title_line[96];
        snprintf(title_line, sizeof(title_line), "THEME %d/%d: %s",
                 cur_idx + 1, NUM_THEMES, g_themes[cur_idx].title);
        draw_text(16, 394, title_line, 2, RGB_COLOR(115, 230, 255));

        /* Active / Backup status badge on bottom right of title row */
        if (cur_idx == active_boot_idx) {
            fill_rect(476, 391, 150, 20, RGB_COLOR(22, 92, 54));
            draw_rect(476, 391, 150, 20, 1, RGB_COLOR(100, 255, 160));
            draw_centered_text(551, 397, "ACTIVE ON /boot", 1, RGB_COLOR(160, 255, 195));
        } else if (has_stock_backup()) {
            fill_rect(476, 391, 150, 20, RGB_COLOR(32, 42, 74));
            draw_rect(476, 391, 150, 20, 1, RGB_COLOR(115, 175, 245));
            draw_centered_text(551, 397, "STOCK BACKUP OK", 1, RGB_COLOR(190, 225, 255));
        }

        draw_text(16, 416, g_themes[cur_idx].subtitle, 1, RGB_COLOR(220, 225, 240));

        /* Action button strip (y = 434..472) */
        /* 1. D-Pad & L1/R1 Prev/Next */
        fill_rect(14, 434, 142, 38, RGB_COLOR(36, 32, 68));
        draw_rect(14, 434, 142, 38, 2, RGB_COLOR(125, 115, 195));
        draw_centered_text(85, 449, "[DPAD/L/R] THEME", 1, RGB_COLOR(245, 245, 255));

        /* 2. [A] Flash Boot Logo */
        fill_rect(166, 434, 164, 38, RGB_COLOR(26, 108, 62));
        draw_rect(166, 434, 164, 38, 2, RGB_COLOR(105, 255, 160));
        draw_centered_text(248, 449, "[A] FLASH LOGO", 1, RGB_COLOR(255, 255, 255));

        /* 3. [SELECT] 1:1 Full Screen */
        fill_rect(340, 434, 138, 38, RGB_COLOR(30, 56, 94));
        draw_rect(340, 434, 138, 38, 2, RGB_COLOR(100, 200, 255));
        draw_centered_text(409, 449, "[SEL] FULL VIEW", 1, RGB_COLOR(225, 245, 255));

        /* 4. [B] Restore Stock */
        fill_rect(488, 434, 138, 38, RGB_COLOR(88, 34, 48));
        draw_rect(488, 434, 138, 38, 2, RGB_COLOR(255, 135, 150));
        draw_centered_text(557, 449, "[B] REVERT STOCK", 1, RGB_COLOR(255, 230, 235));
    }

    /* 3. Modal Overlays (Confirm Flash, Confirm Revert Stock, Result, Y Controls Help) */
    if (modal != MODAL_NONE) {
        draw_alpha_rect(0, 0, NATIVE_W, NATIVE_H, RGB_COLOR(6, 8, 16), 215);

        if (modal == MODAL_HELP) {
            int mx = 20, my = 28, mw = 600, mh = 424;
            fill_rect(mx, my, mw, mh, RGB_COLOR(20, 22, 44));
            draw_rect(mx, my, mw, mh, 3, RGB_COLOR(110, 215, 255));

            draw_centered_text(320, my + 20, "RG35XXSP BOOT LOGO CONTROLS", 2, RGB_COLOR(255, 225, 85));
            fill_rect(mx + 24, my + 44, mw - 48, 2, RGB_COLOR(70, 95, 150));

            draw_text(mx + 24, my + 62,  "D-PAD UP / DOWN : Prev / Next Theme", 2, RGB_COLOR(235, 240, 255));
            draw_text(mx + 24, my + 92,  "LEFT/RIGHT/L1/R1: Prev / Next Theme", 2, RGB_COLOR(235, 240, 255));
            draw_text(mx + 24, my + 122, "BUTTON A        : Flash Boot Logo", 2, RGB_COLOR(115, 255, 165));
            draw_text(mx + 24, my + 152, "BUTTON B        : Revert to Stock Logo", 2, RGB_COLOR(255, 165, 145));
            draw_text(mx + 24, my + 182, "SELECT / X      : Toggle 1:1 Full View", 2, RGB_COLOR(125, 220, 255));
            draw_text(mx + 24, my + 212, "BUTTON Y        : Toggle Controls Help", 2, RGB_COLOR(215, 175, 255));
            draw_text(mx + 24, my + 242, "MENU (ESC)      : Exit Customizer", 2, RGB_COLOR(255, 215, 120));

            fill_rect(mx + 24, my + 274, mw - 48, 86, RGB_COLOR(14, 16, 32));
            draw_rect(mx + 24, my + 274, mw - 48, 86, 2, RGB_COLOR(65, 80, 125));
            draw_text(mx + 38, my + 286, "SURGICAL MODE (OPTION A):", 1, RGB_COLOR(255, 220, 95));
            draw_text(mx + 38, my + 304, "Writes 640x480 32-bit BITMAPV5HEADER BGRA (1,228,938 B)", 1, RGB_COLOR(205, 215, 235));
            draw_text(mx + 38, my + 304 + 16, "to /boot/bootlogo.bmp with SHA-256 verification and", 1, RGB_COLOR(205, 215, 235));
            draw_text(mx + 38, my + 304 + 32, "stores the original stock image in ./backup/.", 1, RGB_COLOR(140, 245, 180));

            draw_centered_text(320, my + 384, "Press [Y], [A], or [B] to Resume", 2, RGB_COLOR(255, 235, 110));
        } else {
            int mx = 48, my = 100, mw = 544, mh = 280;
            fill_rect(mx, my, mw, mh, RGB_COLOR(24, 22, 48));

            if (modal == MODAL_CONFIRM_FLASH) {
                draw_rect(mx, my, mw, mh, 3, RGB_COLOR(105, 255, 160));
                draw_centered_text(320, my + 26, "FLASH BOOT LOGO TO /boot?", 2, RGB_COLOR(105, 255, 160));

                char t_line[96];
                snprintf(t_line, sizeof(t_line), "Theme #%d: %s", cur_idx + 1, g_themes[cur_idx].title);
                draw_centered_text(320, my + 76, t_line, 2, RGB_COLOR(255, 230, 105));
                draw_centered_text(320, my + 108, g_themes[cur_idx].subtitle, 1, RGB_COLOR(215, 220, 240));

                draw_centered_text(320, my + 148,
                                   "Target: /boot/bootlogo.bmp (640x480, 1,228,938 bytes)",
                                   1, RGB_COLOR(150, 225, 255));
                draw_centered_text(320, my + 168,
                                   "Stock image is safely preserved in ./backup/bootlogo.stock.bmp",
                                   1, RGB_COLOR(165, 240, 185));

                fill_rect(mx + 44, my + 210, mw - 88, 44, RGB_COLOR(32, 48, 76));
                draw_rect(mx + 44, my + 210, mw - 88, 44, 2, RGB_COLOR(120, 215, 255));
                draw_centered_text(320, my + 225, "[A] CONFIRM & FLASH     [B] CANCEL", 2, RGB_COLOR(255, 255, 255));
            } else if (modal == MODAL_CONFIRM_STOCK) {
                draw_rect(mx, my, mw, mh, 3, RGB_COLOR(255, 155, 125));
                draw_centered_text(320, my + 26, "REVERT TO STOCK BOOT LOGO?", 2, RGB_COLOR(255, 180, 125));

                draw_centered_text(320, my + 82,
                                   "Restores the original Knulli Scarab bootlogo.bmp",
                                   2, RGB_COLOR(240, 240, 255));
                draw_centered_text(320, my + 118,
                                   "from ./backup/bootlogo.stock.bmp to /boot/bootlogo.bmp",
                                   1, RGB_COLOR(195, 210, 235));
                draw_centered_text(320, my + 142,
                                   "and verifies the restored 1,228,938-byte image with SHA-256.",
                                   1, RGB_COLOR(165, 240, 185));

                fill_rect(mx + 44, my + 210, mw - 88, 44, RGB_COLOR(68, 32, 48));
                draw_rect(mx + 44, my + 210, mw - 88, 44, 2, RGB_COLOR(255, 150, 165));
                draw_centered_text(320, my + 225, "[A] RESTORE STOCK       [B] CANCEL", 2, RGB_COLOR(255, 255, 255));
            } else if (modal == MODAL_RESULT) {
                uint32_t col = last_flash_ok ? RGB_COLOR(100, 255, 160) : RGB_COLOR(255, 105, 115);
                draw_rect(mx, my, mw, mh, 3, col);
                draw_centered_text(320, my + 32, status_headline, 2, col);
                draw_centered_text(320, my + 96, status_detail, 1, RGB_COLOR(240, 245, 255));
                draw_centered_text(320, my + 132,
                                   "Stock backup preserved in ./backup/bootlogo.stock.bmp",
                                   1, RGB_COLOR(175, 235, 195));
                draw_centered_text(320, my + 156,
                                   "Reboot your RG35XXSP to see the new boot splash screen!",
                                   1, RGB_COLOR(255, 225, 130));

                fill_rect(mx + 84, my + 210, mw - 168, 44, RGB_COLOR(38, 34, 72));
                draw_rect(mx + 84, my + 210, mw - 168, 44, 2, RGB_COLOR(255, 225, 95));
                draw_centered_text(320, my + 225, "PRESS ANY BUTTON TO CONTINUE", 2, RGB_COLOR(255, 235, 120));
            }
        }
    }
}

/* ============================================================================
 * 7. MAIN APPLICATION LOOP & PROMO SCREENSHOT GENERATOR
 * ============================================================================ */

int main(int argc, char **argv) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    bool dump_promo_mode = false;
    const char *dump_prefix = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-promo") == 0 && i + 1 < argc) {
            dump_promo_mode = true;
            dump_prefix = argv[++i];
        }
    }

    /* Load all 5 640x480 BMP themes into memory */
    for (int i = 0; i < NUM_THEMES; i++) {
        g_themes[i].pixels = (uint32_t *)malloc((size_t)NATIVE_W * NATIVE_H * sizeof(uint32_t));
        g_themes[i].loaded = load_bmp_to_argb(
            g_themes[i].bmp_path, g_themes[i].pixels, g_themes[i].sha256);
    }

    int cur_idx = 0;
    int active_boot_idx = detect_active_bootlogo_theme();
    bool full_preview_mode = false;
    ui_modal_t modal = MODAL_NONE;
    bool last_flash_ok = true;
    char status_headline[96] = "BOOT LOGO FLASHED & VERIFIED!";
    char status_detail[128]  = "/boot/bootlogo.bmp (1228938 B) | SHA256 OK";

    if (dump_promo_mode && dump_prefix) {
        struct {
            int         t_idx;
            bool        full_pv;
            ui_modal_t  m_state;
            const char *suffix;
        } shots[6] = {
            {0, false, MODAL_NONE,   "theme1_kiss.raw"},
            {1, false, MODAL_NONE,   "theme2_gba_kitchen.raw"},
            {2, false, MODAL_NONE,   "theme3_tux_sky.raw"},
            {3, false, MODAL_NONE,   "theme4_poetry.raw"},
            {1, false, MODAL_RESULT, "flash_verified.raw"},
            {1, false, MODAL_HELP,   "controls_help.raw"},
        };

        for (int s = 0; s < 6; s++) {
            cur_idx           = shots[s].t_idx;
            full_preview_mode = shots[s].full_pv;
            modal             = shots[s].m_state;
            if (modal == MODAL_RESULT) {
                last_flash_ok = flash_bootlogo(
                    cur_idx, false,
                    status_headline, sizeof(status_headline),
                    status_detail,   sizeof(status_detail));
                active_boot_idx = detect_active_bootlogo_theme();
            }
            render_ui_frame(cur_idx, active_boot_idx, full_preview_mode, modal,
                            last_flash_ok, status_headline, status_detail);

            char out_fn[256];
            snprintf(out_fn, sizeof(out_fn), "%s_%s", dump_prefix, shots[s].suffix);
            FILE *df = fopen(out_fn, "wb");
            if (df) {
                fwrite(g_framebuf, sizeof(uint32_t), (size_t)NATIVE_W * NATIVE_H, df);
                fclose(df);
            }
        }

        for (int i = 0; i < NUM_THEMES; i++) {
            free(g_themes[i].pixels);
        }
        return 0;
    }

    if (platform_init("RG35XXSP Boot Logo Customizer") != 0) {
        fprintf(stderr, "Error: Failed to initialize display backend.\n");
        for (int i = 0; i < NUM_THEMES; i++) {
            free(g_themes[i].pixels);
        }
        return 1;
    }

    input_state_t input;
    while (!g_sig_quit && platform_poll_input(&input)) {
        if (input.pressed & BTN_MENU) {
            break;
        }

        if (modal == MODAL_HELP) {
            if (input.pressed & (BTN_Y | BTN_A | BTN_B | BTN_SELECT)) {
                modal = MODAL_NONE;
            }
        } else if (modal == MODAL_CONFIRM_FLASH) {
            if (input.pressed & BTN_A) {
                last_flash_ok = flash_bootlogo(
                    cur_idx, false,
                    status_headline, sizeof(status_headline),
                    status_detail,   sizeof(status_detail));
                active_boot_idx = detect_active_bootlogo_theme();
                modal = MODAL_RESULT;
            } else if (input.pressed & (BTN_B | BTN_Y | BTN_SELECT)) {
                modal = MODAL_NONE;
            }
        } else if (modal == MODAL_CONFIRM_STOCK) {
            if (input.pressed & BTN_A) {
                cur_idx = NUM_THEMES - 1;
                last_flash_ok = flash_bootlogo(
                    cur_idx, true,
                    status_headline, sizeof(status_headline),
                    status_detail,   sizeof(status_detail));
                active_boot_idx = detect_active_bootlogo_theme();
                modal = MODAL_RESULT;
            } else if (input.pressed & (BTN_B | BTN_Y | BTN_SELECT)) {
                modal = MODAL_NONE;
            }
        } else if (modal == MODAL_RESULT) {
            if (input.pressed != 0) {
                modal = MODAL_NONE;
            }
        } else {
            /* Normal browsing state */
            if (input.pressed & (BTN_UP | BTN_LEFT | BTN_L1)) {
                cur_idx = (cur_idx + NUM_THEMES - 1) % NUM_THEMES;
            }
            if (input.pressed & (BTN_DOWN | BTN_RIGHT | BTN_R1)) {
                cur_idx = (cur_idx + 1) % NUM_THEMES;
            }
            if (input.pressed & (BTN_SELECT | BTN_X)) {
                full_preview_mode = !full_preview_mode;
            }
            if (input.pressed & BTN_Y) {
                modal = MODAL_HELP;
            }
            if (input.pressed & BTN_A) {
                modal = MODAL_CONFIRM_FLASH;
            }
            if (input.pressed & BTN_B) {
                if (full_preview_mode) {
                    full_preview_mode = false;
                } else {
                    modal = MODAL_CONFIRM_STOCK;
                }
            }
        }

        render_ui_frame(cur_idx, active_boot_idx, full_preview_mode, modal,
                        last_flash_ok, status_headline, status_detail);
        platform_present();
        delay_ms(16);
    }

    for (int i = 0; i < NUM_THEMES; i++) {
        free(g_themes[i].pixels);
    }
    platform_shutdown();
    return 0;
}
