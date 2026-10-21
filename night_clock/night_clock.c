/*
 * night_clock.c - Single-Screen Bedside & Desk Night Clock for Anbernic RG 35xxSP
 *
 * Targets:
 *   1. Anbernic RG 35xxSP (Allwinner H700 Cortex-A53 aarch64, Knulli / MuOS / Stock OS)
 *      - Single 640x480 4:3 IPS screen via SDL2 (dlopen) with /dev/fb0 mmap fallback
 *      - Direct Linux evdev (/dev/input/event*) input polling
 *   2. Linux Desktop Simulator (x86_64 -DHOST_X11)
 *      - 640x480 X11 window + PulseAudio / ALSA (dlopen) audio
 *
 * Features:
 *   - Retains the top-screen clock functionality (no date display):
 *     * 24-Hour Retro LED Dot-Matrix Digital Clock (HH:MM, constant colon, no seconds)
 *     * Minimalist High-Contrast Analog Clock (toggled with SELECT or X)
 *     * 10-Minute IPS Anti-Burn-In Orbit around the 640x480 center
 *   - Local Timezone Shift (D-Pad LEFT / RIGHT in +/-30m steps, displayed as TZ +/-HH:MM,
 *     persisted across reboots in .night_clock.cfg)
 *   - Brightness Control (D-Pad UP / DOWN, levels 1..10, hardware backlight + software dimming)
 *   - Volume Level Indicator & Confirmation (VOL+ / VOL-, levels 0..10, shows on-screen
 *     volume bar and plays confirmation alarm tone at that volume when released)
 *   - Single-Screen Alarm Editor (Press A to open on screen; press A again to save,
 *     play confirmation alarm sound, and resume the clock; press B to disable/cancel;
 *     in normal clock view, press B to quick-toggle Alarm ON/OFF)
 *   - Single-Screen Controls / Help View (Press Y to view controls on screen;
 *     press Y, A, or B to resume the clock)
 *   - Exit: Press MENU (Esc on Desktop) to save configuration and exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../rg35xxsp_input.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * 1. CONSTANTS, COLOR MACROS, & INPUT DEFINITIONS
 * ============================================================================ */

#define NATIVE_W  640
#define NATIVE_H  480

#define RGB_COLOR(r, g, b) \
    (0xFF000000u | (((uint32_t)(r) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu))

#define COLOR_BLACK   RGB_COLOR(0, 0, 0)
#define COLOR_WHITE   RGB_COLOR(255, 255, 255)

typedef struct {
    uint32_t held;
    uint32_t pressed;
    uint32_t released;
} input_state_t;

#define BURN_IN_INTERVAL_SEC      600ULL   /* 10 minutes */
#define REPEAT_INITIAL_DELAY_MS   320ULL
#define REPEAT_INTERVAL_MS         90ULL
#define VOL_OSD_DURATION_MS      2400ULL

#define AUDIO_SAMPLE_RATE         44100
#define AUDIO_CHUNK_FRAMES        512

typedef enum {
    UI_MODE_NORMAL    = 0,
    UI_MODE_ALARM_SET = 1,
    UI_MODE_HELP      = 2
} ui_mode_t;

typedef enum {
    ALARM_FIELD_HOUR   = 0,
    ALARM_FIELD_MINUTE = 1,
    ALARM_FIELD_STATE  = 2
} alarm_field_t;

/* Persisted configuration */
static int          g_tz_shift_mins = 0;      /* -1440 .. +1440 in steps of 30 mins */
static bool         g_is_analog     = false;  /* false = Retro LED Digital, true = Minimal Analog */
static int          g_brightness    = 7;      /* 1 .. 10 */
static volatile int g_volume        = 8;      /* 0 .. 10 */
static bool         g_alarm_enabled = false;
static int          g_alarm_hour    = 7;      /* 0 .. 23 (in shifted timezone) */
static int          g_alarm_min     = 0;      /* 0 .. 59 (in shifted timezone) */

/* Runtime UI state */
static ui_mode_t     g_ui_mode            = UI_MODE_NORMAL;
static ui_mode_t     g_mode_before_help   = UI_MODE_NORMAL;
static alarm_field_t g_alarm_field        = ALARM_FIELD_HOUR;
static int           g_edit_alarm_hour    = 7;
static int           g_edit_alarm_min     = 0;
static bool          g_edit_alarm_enabled = true;

/* Top transient status toast & volume indicator overlay */
static char      g_toast_text[96]         = {0};
static uint64_t  g_toast_until_ms         = 0;
static uint64_t  g_vol_osd_until_ms       = 0;

/* Alarm trigger & confirmation audio state */
static bool         g_alarm_ringing       = false;
static uint64_t     g_alarm_ring_start_ms = 0;
static int          g_last_alarm_min_key  = -1;
static volatile int g_confirm_samples_rem = 0;
static bool         g_vol_pending_confirm = false;

/* 10-minute anti-burn-in smooth orbit state */
static uint64_t  g_boot_mono_ms           = 0;
static float     g_orbit_cur_x            = 0.0f;
static float     g_orbit_cur_y            = 0.0f;

static char      g_config_path[512]       = {0};
static uint32_t  g_framebuf[NATIVE_W * NATIVE_H];
static volatile sig_atomic_t g_sig_quit   = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_sig_quit = 1;
}

static uint64_t ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void delay_ms(uint32_t ms) {
    usleep((useconds_t)ms * 1000u);
}

/* ============================================================================
 * 2. PLATFORM VIDEO & INPUT BACKEND (RG35XXSP SDL2/FBDEV + EVDEV vs HOST X11)
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
    uint32_t mask = rg35xxsp_map_x11_keysym((unsigned long)ks);
    if (mask) return mask;
    switch (ks) {
        case XK_plus:
        case XK_equal:
        case XK_KP_Add:      return BTN_VOL_UP;
        case XK_minus:
        case XK_underscore:
        case XK_KP_Subtract: return BTN_VOL_DOWN;
        default:             return 0;
    }
}

static uint32_t xkey_to_btn(XKeyEvent *xkey) {
    char buf[8] = {0};
    KeySym sym = NoSymbol;
    int n = XLookupString(xkey, buf, sizeof(buf) - 1, &sym, NULL);
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
    if (!mask && n > 0) {
        switch (buf[0]) {
            case 'a': case 'A': case ' ': case 'z': case 'Z': mask = BTN_A;        break;
            case 'b': case 'B': case '\b': case 127:          mask = BTN_B;        break;
            case 'x': case 'X':                               mask = BTN_X;        break;
            case 'y': case 'Y':                               mask = BTN_Y;        break;
            case 'r': case 'R': case 'e': case 'E': case ']': mask = BTN_R1;       break;
            case 'l': case 'L': case 'q': case 'Q': case '[': mask = BTN_L1;       break;
            case '1':                                         mask = BTN_L2;       break;
            case '2':                                         mask = BTN_R2;       break;
            case '+': case '=':                               mask = BTN_VOL_UP;   break;
            case '-': case '_':                               mask = BTN_VOL_DOWN; break;
            case '\r': case '\n':                             mask = BTN_START;    break;
            case '\t':                                        mask = BTN_SELECT;   break;
            case 27:                                          mask = BTN_MENU;     break;
            default: break;
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
                /* Latch rising edge even if KeyRelease arrives in the same XPending batch */
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

#define SDL2_INIT_AUDIO                0x00000010U
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
static int   (*fn_SDL2_InitSubSystem)(uint32_t) = NULL;
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
    /* Open all readable /dev/input/event* devices */
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
        fn_SDL2_InitSubSystem   = dlsym(g_sdl2_lib, "SDL_InitSubSystem");
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

        if (fn_SDL2_Init) {
            if (fn_SDL2_Init(SDL2_INIT_VIDEO | SDL2_INIT_AUDIO) != 0) {
                fn_SDL2_Init(SDL2_INIT_VIDEO);
            }
        }
        if (fn_SDL2_CreateWindow && fn_SDL2_CreateRenderer && fn_SDL2_CreateTexture) {
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
    return rg35xxsp_map_evkey(code);
}

static void save_config(void);

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
                if (ie.code == KEY_INSERT && ie.value == 1) {
                    /* RG35XXSP Hall sensor lid close: flush config to SD card */
                    save_config();
                    continue;
                }
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
 * 3. HARDWARE BACKLIGHT & VOLUME CONTROL (ALLWINNER H700 / KNULLI / MUOS)
 * ============================================================================ */

#ifndef HOST_X11
static int sysfs_read_int(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    char buf[32] = {0};
    int val = fallback;
    if (fgets(buf, sizeof(buf), f)) {
        val = atoi(buf);
    }
    fclose(f);
    return val;
}

static bool sysfs_write_int(const char *path, int val) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%d\n", val);
    fclose(f);
    return true;
}
#endif

static void apply_hardware_brightness(int level_1_to_10) {
    if (level_1_to_10 < 1)  level_1_to_10 = 1;
    if (level_1_to_10 > 10) level_1_to_10 = 10;
#ifndef HOST_X11
    /*
     * 1. Allwinner H700 sunxi display backlight attribute (0..255)
     * 2. Standard Linux /sys/class/backlight/backlight/brightness
     */
    int raw_255 = 15 + ((level_1_to_10 - 1) * 240) / 9;
    if (access("/sys/class/disp/disp/attr/lcdbl", W_OK) == 0) {
        sysfs_write_int("/sys/class/disp/disp/attr/lcdbl", raw_255);
    }
    const char *bl_dirs[] = {
        "/sys/class/backlight/backlight",
        "/sys/class/backlight/lcd0",
        NULL
    };
    for (int i = 0; bl_dirs[i]; i++) {
        char max_p[128], cur_p[128];
        snprintf(max_p, sizeof(max_p), "%s/max_brightness", bl_dirs[i]);
        snprintf(cur_p, sizeof(cur_p), "%s/brightness",     bl_dirs[i]);
        if (access(cur_p, W_OK) == 0) {
            int max_b = sysfs_read_int(max_p, 255);
            if (max_b <= 0) max_b = 255;
            int min_b = (max_b >= 20) ? (max_b / 16) : 1;
            int target = min_b + ((level_1_to_10 - 1) * (max_b - min_b)) / 9;
            sysfs_write_int(cur_p, target);
        }
    }
#endif
}

static void apply_hardware_volume(int level_0_to_10) {
    if (level_0_to_10 < 0)  level_0_to_10 = 0;
    if (level_0_to_10 > 10) level_0_to_10 = 10;
#ifndef HOST_X11
    int pct = level_0_to_10 * 10;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "(amixer -q sset 'Master' %d%% || amixer -q sset 'lineout volume' %d%% || amixer -q sset 'DAC' %d%%) >/dev/null 2>&1 &",
             pct, pct, pct);
    int r = system(cmd);
    (void)r;
#endif
}

static void show_toast(const char *msg, uint64_t now_ms) {
    snprintf(g_toast_text, sizeof(g_toast_text), "%s", msg);
    g_toast_until_ms = now_ms + 2200ULL;
}

/* ============================================================================
 * 4. PERSISTENT CONFIGURATION (.night_clock.cfg)
 * ============================================================================ */

static void resolve_config_path(void) {
    struct stat st;
    const char *dirs[] = {
        "/userdata/roms/ports/night_clock",
        "/userdata/system/configs",
        "/userdata",
        "/mnt/sdcard/Roms/PORTS/night_clock",
        "/mnt/mmc/Roms/PORTS/night_clock",
        "/mnt/sdcard",
        "/mnt/mmc",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && access(dirs[i], W_OK) == 0) {
            snprintf(g_config_path, sizeof(g_config_path), "%s/.night_clock.cfg", dirs[i]);
            return;
        }
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "%s/.night_clock.cfg", home);
    } else {
        snprintf(g_config_path, sizeof(g_config_path), "./.night_clock.cfg");
    }
}

static void load_config(void) {
    resolve_config_path();
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int val = 0;
        if (sscanf(line, "%63[^=]=%d", key, &val) == 2) {
            if (strcmp(key, "tz_shift_mins") == 0) {
                if (val >= -1440 && val <= 1440) {
                    g_tz_shift_mins = (val / 30) * 30;
                }
            } else if (strcmp(key, "is_analog") == 0) {
                g_is_analog = (val != 0);
            } else if (strcmp(key, "brightness") == 0) {
                if (val >= 1 && val <= 10) g_brightness = val;
            } else if (strcmp(key, "volume") == 0) {
                if (val >= 0 && val <= 10) g_volume = val;
            } else if (strcmp(key, "alarm_enabled") == 0) {
                g_alarm_enabled = (val != 0);
            } else if (strcmp(key, "alarm_hour") == 0) {
                if (val >= 0 && val <= 23) g_alarm_hour = val;
            } else if (strcmp(key, "alarm_min") == 0) {
                if (val >= 0 && val <= 59) g_alarm_min = val;
            }
        }
    }
    fclose(f);
}

static void save_config(void) {
    if (!g_config_path[0]) resolve_config_path();
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "tz_shift_mins=%d\n", g_tz_shift_mins);
    fprintf(f, "is_analog=%d\n",     g_is_analog ? 1 : 0);
    fprintf(f, "brightness=%d\n",    g_brightness);
    fprintf(f, "volume=%d\n",        g_volume);
    fprintf(f, "alarm_enabled=%d\n", g_alarm_enabled ? 1 : 0);
    fprintf(f, "alarm_hour=%d\n",    g_alarm_hour);
    fprintf(f, "alarm_min=%d\n",     g_alarm_min);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* ============================================================================
 * 5. ALARM & CONFIRMATION AUDIO ENGINE (SDL2 / PulseAudio / ALSA)
 * ============================================================================ */

static volatile bool g_audio_running   = false;
static pthread_t     g_audio_thread;
static bool          g_use_sdl_audio   = false;
static uint32_t      g_tone_sample_idx = 0;

typedef struct {
    int      freq;
    uint16_t format;
    uint8_t  channels;
    uint8_t  silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void   (*callback)(void *userdata, uint8_t *stream, int len);
    void    *userdata;
} sdl_audio_spec_t;

#define SDL_AUDIO_S16SYS_FMT 0x8010u
static void (*pSDL_CloseAudio)(void) = NULL;

/*
 * Synthesize loud, unmistakable dual-harmonic bedside alarm pulses:
 *   - Used when confirming an alarm with A (~1.6s confirmation burst),
 *     when releasing VOL+ / VOL- after adjusting volume, and when the
 *     alarm time is reached (continuous repeating klaxon pattern).
 */
static void render_alarm_samples(int16_t *out, int num_samples) {
    int vol = g_volume;
    if (vol < 0)  vol = 0;
    if (vol > 10) vol = 10;
    float vol_gain = (float)vol / 10.0f;

    for (int i = 0; i < num_samples; i++) {
        int rem_confirm = g_confirm_samples_rem;
        bool active_confirm = (rem_confirm > 0);
        if (active_confirm) {
            g_confirm_samples_rem = rem_confirm - 1;
        }

        if ((!active_confirm && !g_alarm_ringing) || vol == 0) {
            out[i] = 0;
            continue;
        }

        uint32_t idx = g_tone_sample_idx++;
        /*
         * 4-pulse alarm cadence:
         * Each cycle is 0.80s (35280 samples @ 44100 Hz) split into 4 fast pulses
         * of 0.14s with a brief pause at the end of the group.
         */
        uint32_t pos_in_group = idx % 35280u;
        uint32_t pulse_idx    = pos_in_group / 6615u;
        uint32_t pos_in_pulse = pos_in_group % 6615u;

        bool gate_on = (pulse_idx < 4) && (pos_in_pulse < 4600u);
        if (!gate_on) {
            out[i] = 0;
            continue;
        }

        /* Alternating high/higher alarm pitch (880 Hz A5 / 1174.66 Hz D6) + harmonics */
        float base_freq = (pulse_idx % 2 == 0) ? 880.0f : 1174.66f;
        float t  = (float)idx / (float)AUDIO_SAMPLE_RATE;
        float s1 = sinf(2.0f * (float)M_PI * base_freq * t);
        float s2 = 0.55f * sinf(2.0f * (float)M_PI * (base_freq * 2.0f) * t);
        float s3 = 0.30f * sinf(2.0f * (float)M_PI * (base_freq * 3.0f) * t);

        /* Soft attack/decay envelope on each pulse to prevent speaker pop */
        float env = 1.0f;
        if (pos_in_pulse < 220u) {
            env = (float)pos_in_pulse / 220.0f;
        } else if (pos_in_pulse > 4100u) {
            env = (float)(4600u - pos_in_pulse) / 500.0f;
        }

        float wave = (s1 + s2 + s3) * (1.0f / 1.85f) * env;
        int32_t sample = (int32_t)(wave * 28000.0f * vol_gain);
        if (sample >  32000) sample =  32000;
        if (sample < -32000) sample = -32000;
        out[i] = (int16_t)sample;
    }
}

#ifndef HOST_X11
static void sdl_audio_cb(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    render_alarm_samples((int16_t *)stream, len / (int)sizeof(int16_t));
}
#endif

static void *audio_thread_fn(void *arg) {
    (void)arg;
    int16_t buf[AUDIO_CHUNK_FRAMES];

    /* 1. Try PulseAudio Simple (libpulse-simple.so.0) */
    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    if (pa_lib) {
        typedef struct { int format; uint32_t rate; uint8_t channels; } pa_ss_t;
        typedef void *(*pa_new_fn)(const char *, const char *, int, const char *,
                                   const char *, const pa_ss_t *, const void *,
                                   const void *, int *);
        typedef int   (*pa_write_fn)(void *, const void *, size_t, int *);
        typedef void  (*pa_free_fn)(void *);

        pa_new_fn   fn_new   = (pa_new_fn)dlsym(pa_lib, "pa_simple_new");
        pa_write_fn fn_write = (pa_write_fn)dlsym(pa_lib, "pa_simple_write");
        pa_free_fn  fn_free  = (pa_free_fn)dlsym(pa_lib, "pa_simple_free");
        if (fn_new && fn_write && fn_free) {
            pa_ss_t ss = { 3 /* PA_SAMPLE_S16LE */, AUDIO_SAMPLE_RATE, 1 };
            int err = 0;
            void *pa = fn_new(NULL, "NightClock", 1 /* PLAYBACK */, NULL, "Alarm", &ss, NULL, NULL, &err);
            if (pa) {
                while (g_audio_running) {
                    render_alarm_samples(buf, AUDIO_CHUNK_FRAMES);
                    fn_write(pa, buf, sizeof(buf), &err);
                }
                fn_free(pa);
                dlclose(pa_lib);
                return NULL;
            }
        }
        dlclose(pa_lib);
    }

    /* 2. Fallback to ALSA (libasound.so.2) */
    void *alsa_lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (alsa_lib) {
        typedef int  (*snd_open_fn)(void **, const char *, int, int);
        typedef int  (*snd_set_params_fn)(void *, int, int, unsigned int, unsigned int, int, unsigned int);
        typedef long (*snd_writei_fn)(void *, const void *, unsigned long);
        typedef int  (*snd_recover_fn)(void *, int, int);
        typedef int  (*snd_close_fn)(void *);

        snd_open_fn       fn_open    = (snd_open_fn)dlsym(alsa_lib, "snd_pcm_open");
        snd_set_params_fn fn_params  = (snd_set_params_fn)dlsym(alsa_lib, "snd_pcm_set_params");
        snd_writei_fn     fn_writei  = (snd_writei_fn)dlsym(alsa_lib, "snd_pcm_writei");
        snd_recover_fn    fn_recover = (snd_recover_fn)dlsym(alsa_lib, "snd_pcm_recover");
        snd_close_fn      fn_close   = (snd_close_fn)dlsym(alsa_lib, "snd_pcm_close");

        if (fn_open && fn_params && fn_writei && fn_close) {
            void *pcm = NULL;
            if (fn_open(&pcm, "default", 0, 0) == 0 && pcm) {
                if (fn_params(pcm, 2 /* S16_LE */, 3 /* RW_INTERLEAVED */, 1, AUDIO_SAMPLE_RATE, 1, 50000) == 0) {
                    while (g_audio_running) {
                        render_alarm_samples(buf, AUDIO_CHUNK_FRAMES);
                        long r = fn_writei(pcm, buf, AUDIO_CHUNK_FRAMES);
                        if (r < 0 && fn_recover) fn_recover(pcm, (int)r, 1);
                    }
                    fn_close(pcm);
                    dlclose(alsa_lib);
                    return NULL;
                }
                fn_close(pcm);
            }
        }
        dlclose(alsa_lib);
    }

    /* 3. Silent pacing fallback if no audio device is available */
    while (g_audio_running) {
        render_alarm_samples(buf, AUDIO_CHUNK_FRAMES);
        delay_ms(10);
    }
    return NULL;
}

static void audio_init(void) {
#ifndef HOST_X11
    if (g_sdl2_lib) {
        int  (*fn_OpenAudio)(sdl_audio_spec_t *, sdl_audio_spec_t *) = dlsym(g_sdl2_lib, "SDL_OpenAudio");
        void (*fn_PauseAudio)(int) = dlsym(g_sdl2_lib, "SDL_PauseAudio");
        pSDL_CloseAudio = dlsym(g_sdl2_lib, "SDL_CloseAudio");

        if (fn_OpenAudio && fn_PauseAudio) {
            if (fn_SDL2_InitSubSystem) {
                fn_SDL2_InitSubSystem(SDL2_INIT_AUDIO);
            }
            sdl_audio_spec_t want;
            memset(&want, 0, sizeof(want));
            want.freq     = AUDIO_SAMPLE_RATE;
            want.format   = SDL_AUDIO_S16SYS_FMT;
            want.channels = 1;
            want.samples  = AUDIO_CHUNK_FRAMES;
            want.callback = sdl_audio_cb;
            if (fn_OpenAudio(&want, NULL) == 0) {
                fn_PauseAudio(0);
                g_use_sdl_audio = true;
                return;
            }
        }
    }
#endif
    g_audio_running = true;
    pthread_create(&g_audio_thread, NULL, audio_thread_fn, NULL);
}

static void audio_shutdown(void) {
    if (g_use_sdl_audio) {
        if (pSDL_CloseAudio) pSDL_CloseAudio();
        g_use_sdl_audio = false;
    } else if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
}

static void trigger_confirmation_alarm_sound(void) {
    apply_hardware_volume(g_volume);
    g_tone_sample_idx = 0;
    /* Play 2 full 4-pulse groups (~1.6 seconds at 44100 Hz) */
    g_confirm_samples_rem = 35280 * 2;
}

/* ============================================================================
 * 6. NATIVE 640x480 DRAWING PRIMITIVES, 5x7 FONT, & 7x11 LED DOT-MATRIX FONT
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

static void clear_screen(uint32_t color) {
    for (int i = 0; i < NATIVE_W * NATIVE_H; i++) {
        g_framebuf[i] = color;
    }
}

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

static void draw_text_centered(int y, const char *str, int scale, uint32_t color) {
    int w = text_width(str, scale);
    draw_text((NATIVE_W - w) / 2, y, str, scale, color);
}

/* Apply brightness dimming curve (22% at level 1 .. 100% at level 10) */
static uint32_t dim_color(uint32_t col, int brightness_1_to_10) {
    if (brightness_1_to_10 < 1)  brightness_1_to_10 = 1;
    if (brightness_1_to_10 > 10) brightness_1_to_10 = 10;
    float scale = 0.22f + 0.78f * ((float)(brightness_1_to_10 - 1) / 9.0f);
    uint32_t r = (uint32_t)((float)((col >> 16) & 0xFFu) * scale);
    uint32_t g = (uint32_t)((float)((col >>  8) & 0xFFu) * scale);
    uint32_t b = (uint32_t)((float)( col        & 0xFFu) * scale);
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return RGB_COLOR(r, g, b);
}

static inline void blend_pixel(int x, int y, uint32_t color, float alpha) {
    if (x < 0 || x >= NATIVE_W || y < 0 || y >= NATIVE_H || alpha <= 0.0f) return;
    if (alpha >= 1.0f) {
        g_framebuf[y * NATIVE_W + x] = color;
        return;
    }
    uint32_t dst = g_framebuf[y * NATIVE_W + x];
    float inv = 1.0f - alpha;
    uint32_t sr = (color >> 16) & 0xFFu;
    uint32_t sg = (color >>  8) & 0xFFu;
    uint32_t sb =  color        & 0xFFu;
    uint32_t dr = (dst   >> 16) & 0xFFu;
    uint32_t dg = (dst   >>  8) & 0xFFu;
    uint32_t db =  dst          & 0xFFu;
    uint32_t r  = (uint32_t)(sr * alpha + dr * inv);
    uint32_t g  = (uint32_t)(sg * alpha + dg * inv);
    uint32_t b  = (uint32_t)(sb * alpha + db * inv);
    g_framebuf[y * NATIVE_W + x] = RGB_COLOR(r, g, b);
}

/* Anti-aliased filled disc using signed distance field */
static void draw_aa_disc(float cx, float cy, float radius, uint32_t color) {
    int x0 = (int)floorf(cx - radius - 1.5f);
    int x1 = (int)ceilf (cx + radius + 1.5f);
    int y0 = (int)floorf(cy - radius - 1.5f);
    int y1 = (int)ceilf (cy + radius + 1.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= NATIVE_W) x1 = NATIVE_W - 1;
    if (y1 >= NATIVE_H) y1 = NATIVE_H - 1;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)y - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = radius + 0.5f - dist;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                blend_pixel(x, y, color, cov);
            }
        }
    }
}

/* Anti-aliased ring outline (inner_r .. outer_r) */
static void draw_aa_ring(float cx, float cy, float inner_r, float outer_r, uint32_t color) {
    int x0 = (int)floorf(cx - outer_r - 1.5f);
    int x1 = (int)ceilf (cx + outer_r + 1.5f);
    int y0 = (int)floorf(cy - outer_r - 1.5f);
    int y1 = (int)ceilf (cy + outer_r + 1.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= NATIVE_W) x1 = NATIVE_W - 1;
    if (y1 >= NATIVE_H) y1 = NATIVE_H - 1;

    float min_r_bound = (inner_r > 2.0f) ? (inner_r - 1.5f) : 0.0f;
    float min_r_sq    = min_r_bound * min_r_bound;
    float max_r_bound = outer_r + 1.5f;
    float max_r_sq    = max_r_bound * max_r_bound;

    for (int y = y0; y <= y1; y++) {
        float dy  = (float)y - cy;
        float dy2 = dy * dy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float d2 = dx * dx + dy2;
            if (d2 < min_r_sq || d2 > max_r_sq) continue;
            float dist  = sqrtf(d2);
            float a_out = outer_r + 0.5f - dist;
            float a_in  = dist - (inner_r - 0.5f);
            float cov   = (a_out < a_in) ? a_out : a_in;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                blend_pixel(x, y, color, cov);
            }
        }
    }
}

/* Anti-aliased capsule / baton line segment from (x0, y0) to (x1, y1) with thickness */
static void draw_aa_segment(float x0, float y0, float x1, float y1,
                            float thickness, bool round_caps, uint32_t color) {
    float half_w = thickness * 0.5f;
    float min_x = ((x0 < x1) ? x0 : x1) - half_w - 2.0f;
    float max_x = ((x0 > x1) ? x0 : x1) + half_w + 2.0f;
    float min_y = ((y0 < y1) ? y0 : y1) - half_w - 2.0f;
    float max_y = ((y0 > y1) ? y0 : y1) + half_w + 2.0f;

    int ix0 = (int)floorf(min_x);
    int ix1 = (int)ceilf (max_x);
    int iy0 = (int)floorf(min_y);
    int iy1 = (int)ceilf (max_y);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 >= NATIVE_W) ix1 = NATIVE_W - 1;
    if (iy1 >= NATIVE_H) iy1 = NATIVE_H - 1;

    float vx  = x1 - x0;
    float vy  = y1 - y0;
    float len = sqrtf(vx * vx + vy * vy);
    if (len < 0.001f) {
        draw_aa_disc(x0, y0, half_w, color);
        return;
    }
    float ux = vx / len;
    float uy = vy / len;

    for (int y = iy0; y <= iy1; y++) {
        float py = (float)y - y0;
        for (int x = ix0; x <= ix1; x++) {
            float px   = (float)x - x0;
            float proj = px * ux + py * uy;
            float perp = fabsf(-px * uy + py * ux);
            float dist;
            if (round_caps) {
                float clamped = (proj < 0.0f) ? 0.0f : (proj > len ? len : proj);
                float dx = px - clamped * ux;
                float dy = py - clamped * uy;
                dist = sqrtf(dx * dx + dy * dy) - half_w;
            } else {
                float d_along = (proj < 0.0f) ? -proj : (proj > len ? (proj - len) : 0.0f);
                float d_perp  = perp - half_w;
                if (d_along <= 0.0f && d_perp <= 0.0f) {
                    dist = (d_along > d_perp) ? d_along : d_perp;
                } else if (d_along > 0.0f && d_perp > 0.0f) {
                    dist = sqrtf(d_along * d_along + d_perp * d_perp);
                } else {
                    dist = (d_along > d_perp) ? d_along : d_perp;
                }
            }
            float cov = 0.5f - dist;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                blend_pixel(x, y, color, cov);
            }
        }
    }
}

/*
 * Custom 7x11 High-Contrast Retro LED Dot-Matrix Bit-Font for Digital Clock Numerals (0..9)
 */
static const uint8_t k_led_digits_7x11[10][11] = {
    /* 0 */ { 0x1C, 0x3E, 0x63, 0x63, 0x67, 0x6B, 0x73, 0x63, 0x63, 0x3E, 0x1C },
    /* 1 */ { 0x0C, 0x1C, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x3F },
    /* 2 */ { 0x3E, 0x7F, 0x63, 0x03, 0x0E, 0x1C, 0x38, 0x70, 0x60, 0x7F, 0x7F },
    /* 3 */ { 0x3E, 0x7F, 0x63, 0x03, 0x1E, 0x1E, 0x03, 0x03, 0x63, 0x7F, 0x3E },
    /* 4 */ { 0x06, 0x0E, 0x1E, 0x36, 0x66, 0x66, 0x7F, 0x7F, 0x06, 0x06, 0x06 },
    /* 5 */ { 0x7F, 0x7F, 0x60, 0x60, 0x7E, 0x7F, 0x03, 0x03, 0x63, 0x7F, 0x3E },
    /* 6 */ { 0x1E, 0x3E, 0x60, 0x60, 0x7E, 0x7F, 0x63, 0x63, 0x63, 0x3E, 0x1C },
    /* 7 */ { 0x7F, 0x7F, 0x63, 0x06, 0x0C, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x18 },
    /* 8 */ { 0x3E, 0x7F, 0x63, 0x63, 0x3E, 0x3E, 0x63, 0x63, 0x63, 0x7F, 0x3E },
    /* 9 */ { 0x1C, 0x3E, 0x63, 0x63, 0x63, 0x7F, 0x3F, 0x03, 0x03, 0x3E, 0x3C }
};

static void draw_led_digit_7x11(int x0, int y0, int digit,
                                int pitch, float dot_r,
                                uint32_t on_col, uint32_t core_col, uint32_t off_col) {
    if (digit < 0 || digit > 9) return;
    for (int r = 0; r < 11; r++) {
        uint8_t row_bits = k_led_digits_7x11[digit][r];
        float cy = (float)(y0 + r * pitch) + (float)pitch * 0.5f;
        for (int c = 0; c < 7; c++) {
            float cx  = (float)(x0 + c * pitch) + (float)pitch * 0.5f;
            bool  lit = (row_bits & (1u << (6 - c))) != 0;
            if (lit) {
                draw_aa_disc(cx, cy, dot_r, on_col);
                if (dot_r >= 3.5f) {
                    draw_aa_disc(cx, cy, dot_r * 0.45f, core_col);
                }
            } else if (off_col != 0) {
                draw_aa_disc(cx, cy, dot_r * 0.52f, off_col);
            }
        }
    }
}

static void draw_led_colon(int x0, int y0, int pitch, float dot_r,
                           bool lit, uint32_t on_col, uint32_t core_col, uint32_t off_col) {
    int rows[4] = { 2, 3, 7, 8 };
    for (int i = 0; i < 4; i++) {
        float cy = (float)(y0 + rows[i] * pitch) + (float)pitch * 0.5f;
        for (int c = 0; c < 2; c++) {
            float cx = (float)(x0 + c * pitch) + (float)pitch * 0.5f;
            if (lit) {
                draw_aa_disc(cx, cy, dot_r, on_col);
                if (dot_r >= 3.5f) {
                    draw_aa_disc(cx, cy, dot_r * 0.45f, core_col);
                }
            } else if (off_col != 0) {
                draw_aa_disc(cx, cy, dot_r * 0.52f, off_col);
            }
        }
    }
}

/* ============================================================================
 * 7. SINGLE-SCREEN RENDERERS (CLOCK, ALARM EDITOR, & CONTROLS HELP)
 * ============================================================================ */

static void update_burn_in_orbit(uint64_t now_ms) {
    /*
     * 9-point bounded orbit stepped every 10 minutes (600,000 ms) scaled for 640x480
     * so no static pixel remains fixed on the IPS panel.
     */
    static const float k_orbit_pts[9][2] = {
        {   0.0f,   0.0f },
        {  36.0f, -22.0f },
        { -34.0f,  24.0f },
        {  38.0f,  20.0f },
        { -36.0f, -20.0f },
        {   0.0f,  26.0f },
        { -38.0f,   0.0f },
        {   0.0f, -26.0f },
        {  38.0f,   0.0f }
    };
    uint64_t elapsed_ms = (now_ms >= g_boot_mono_ms) ? (now_ms - g_boot_mono_ms) : 0;
    int slot = (int)((elapsed_ms / (BURN_IN_INTERVAL_SEC * 1000ULL)) % 9ULL);
    float tx = k_orbit_pts[slot][0];
    float ty = k_orbit_pts[slot][1];

    g_orbit_cur_x += (tx - g_orbit_cur_x) * 0.08f;
    g_orbit_cur_y += (ty - g_orbit_cur_y) * 0.08f;
}

static void format_tz_shift(char *buf, size_t sz, int shift_mins) {
    char sign = (shift_mins >= 0) ? '+' : '-';
    int abs_m = (shift_mins >= 0) ? shift_mins : -shift_mins;
    int hh = abs_m / 60;
    int mm = abs_m % 60;
    snprintf(buf, sz, "TZ %c%02d:%02d", sign, hh, mm);
}

static void draw_pill_button(int x, int y, int w, int h,
                             const char *key_lbl, const char *desc_lbl,
                             uint32_t badge_col, uint32_t border_col) {
    fill_rect(x, y, w, h, RGB_COLOR(20, 28, 44));
    draw_rect(x, y, w, h, 2, border_col);

    int kw = text_width(key_lbl, 2) + 14;
    fill_rect(x + 6, y + 6, kw, h - 12, badge_col);
    draw_text(x + 13, y + (h - 14) / 2, key_lbl, 2, RGB_COLOR(12, 18, 28));
    draw_text(x + kw + 14, y + (h - 14) / 2, desc_lbl, 1, RGB_COLOR(225, 235, 250));
}

static void render_digital_clock(const struct tm *shifted_tm, int cx, int cy, uint64_t now_ms) {
    /*
     * Retro LED Dot-Matrix 24-Hour Digital Clock (HH:MM only, constant colon) for 640x480:
     *   pitch = 15px -> each 7x11 digit is 105x165 px, total width = 514 px
     */
    int   pitch_main = 15;
    float dot_r_main = 5.6f;
    int   digit_w    = 7 * pitch_main;   /* 105 */
    int   digit_h    = 11 * pitch_main;  /* 165 */
    int   gap_d      = 16;
    int   colon_w    = 2 * pitch_main;   /* 30  */

    int total_w = digit_w * 4 + gap_d * 4 + colon_w;
    int start_x = cx - total_w / 2;
    int start_y = cy - digit_h / 2 - 12;

    uint32_t on_col   = dim_color(RGB_COLOR(88, 224, 255),  g_brightness);
    uint32_t core_col = dim_color(RGB_COLOR(215, 250, 255), g_brightness);
    uint32_t off_col  = dim_color(RGB_COLOR(14, 24, 36),    g_brightness);

    if (g_alarm_ringing && ((now_ms / 250ULL) % 2ULL == 0ULL)) {
        on_col   = RGB_COLOR(255, 90, 82);
        core_col = RGB_COLOR(255, 225, 220);
    }

    int hr = shifted_tm->tm_hour % 24;
    int mn = shifted_tm->tm_min  % 60;

    int x = start_x;
    /* H1, H2 */
    draw_led_digit_7x11(x, start_y, hr / 10, pitch_main, dot_r_main, on_col, core_col, off_col);
    x += digit_w + gap_d;
    draw_led_digit_7x11(x, start_y, hr % 10, pitch_main, dot_r_main, on_col, core_col, off_col);
    x += digit_w + gap_d;

    /* Constant LED Colon separating Hours and Minutes */
    draw_led_colon(x, start_y, pitch_main, dot_r_main, true, on_col, core_col, off_col);
    x += colon_w + gap_d;

    /* M1, M2 */
    draw_led_digit_7x11(x, start_y, mn / 10, pitch_main, dot_r_main, on_col, core_col, off_col);
    x += digit_w + gap_d;
    draw_led_digit_7x11(x, start_y, mn % 10, pitch_main, dot_r_main, on_col, core_col, off_col);

    /* Subtle metadata strip underneath the digital clock */
    char tz_buf[32];
    format_tz_shift(tz_buf, sizeof(tz_buf), g_tz_shift_mins);
    char meta_line[128];
    if (g_alarm_enabled) {
        snprintf(meta_line, sizeof(meta_line), "24H   |   %s   |   ALARM %02d:%02d [ON]",
                 tz_buf, g_alarm_hour, g_alarm_min);
    } else {
        snprintf(meta_line, sizeof(meta_line), "24H   |   %s   |   ALARM OFF", tz_buf);
    }
    uint32_t meta_col = dim_color(g_alarm_enabled ? RGB_COLOR(110, 210, 155) : RGB_COLOR(95, 120, 155), g_brightness);
    int meta_w = text_width(meta_line, 2);
    draw_text(cx - meta_w / 2, start_y + digit_h + 26, meta_line, 2, meta_col);
}

static void render_analog_clock(const struct tm *shifted_tm, int cx, int cy, uint64_t now_ms) {
    float fcx    = (float)cx;
    float fcy    = (float)cy;
    float dial_r = 178.0f;

    uint32_t ring_col  = dim_color(RGB_COLOR(62, 86, 122),   g_brightness);
    uint32_t major_col = dim_color(RGB_COLOR(238, 244, 252), g_brightness);
    uint32_t minor_col = dim_color(RGB_COLOR(165, 185, 215), g_brightness);
    uint32_t tick_col  = dim_color(RGB_COLOR(78, 102, 138),  g_brightness);
    uint32_t hand_col  = dim_color(RGB_COLOR(244, 248, 255), g_brightness);
    uint32_t sec_col   = dim_color(RGB_COLOR(246, 88, 78),   g_brightness);
    uint32_t alarm_col = dim_color(RGB_COLOR(248, 212, 108), g_brightness);
    uint32_t pin_col   = dim_color(RGB_COLOR(18, 24, 36),    g_brightness);

    if (g_alarm_ringing && ((now_ms / 250ULL) % 2ULL == 0ULL)) {
        ring_col = RGB_COLOR(255, 88, 78);
    }

    /* 1. Outer minimalist bezel ring */
    draw_aa_ring(fcx, fcy, dial_r - 2.0f, dial_r + 1.0f, ring_col);

    /* 2. 60 Minute ticks & 12 Bold Baton Hour Markers */
    for (int m = 0; m < 60; m++) {
        float ang = ((float)m * 6.0f - 90.0f) * ((float)M_PI / 180.0f);
        float ca  = cosf(ang);
        float sa  = sinf(ang);
        if (m % 5 == 0) {
            bool is_cardinal = (m % 15 == 0);
            float r_out = dial_r - 8.0f;
            float r_in  = dial_r - (is_cardinal ? 30.0f : 23.0f);
            float thick = is_cardinal ? 6.5f : 4.2f;
            uint32_t c  = is_cardinal ? major_col : minor_col;
            draw_aa_segment(fcx + ca * r_in,  fcy + sa * r_in,
                            fcx + ca * r_out, fcy + sa * r_out,
                            thick, false, c);
        } else {
            float r_out = dial_r - 8.0f;
            float r_in  = dial_r - 14.0f;
            draw_aa_segment(fcx + ca * r_in,  fcy + sa * r_in,
                            fcx + ca * r_out, fcy + sa * r_out,
                            1.6f, false, tick_col);
        }
    }

    /* 3. Optional Alarm Indicator Hand (when alarm is enabled) */
    if (g_alarm_enabled) {
        float alrm_deg = ((float)(g_alarm_hour % 12) + (float)g_alarm_min / 60.0f) * 30.0f - 90.0f;
        float alrm_rad = alrm_deg * ((float)M_PI / 180.0f);
        float ax = fcx + cosf(alrm_rad) * 122.0f;
        float ay = fcy + sinf(alrm_rad) * 122.0f;
        draw_aa_segment(fcx, fcy, ax, ay, 2.4f, true, alarm_col);
        draw_aa_disc(ax, ay, 3.8f, alarm_col);
    }

    /* 4. Hour Hand (wide high-contrast baton) */
    float min_f  = (float)(shifted_tm->tm_min % 60);
    float hour_f = (float)(shifted_tm->tm_hour % 12) + min_f / 60.0f;

    float hr_rad = (hour_f * 30.0f - 90.0f) * ((float)M_PI / 180.0f);
    float hr_ca  = cosf(hr_rad);
    float hr_sa  = sinf(hr_rad);
    draw_aa_segment(fcx - hr_ca * 20.0f,  fcy - hr_sa * 20.0f,
                    fcx + hr_ca * 102.0f, fcy + hr_sa * 102.0f,
                    9.8f, true, hand_col);

    /* 5. Minute Hand (long high-contrast baton) */
    float mn_rad = (min_f * 6.0f - 90.0f) * ((float)M_PI / 180.0f);
    float mn_ca  = cosf(mn_rad);
    float mn_sa  = sinf(mn_rad);
    draw_aa_segment(fcx - mn_ca * 24.0f,  fcy - mn_sa * 24.0f,
                    fcx + mn_ca * 148.0f, fcy + mn_sa * 148.0f,
                    6.8f, true, hand_col);

    /* Center pivot cap */
    draw_aa_disc(fcx, fcy, 8.0f, hand_col);
    draw_aa_disc(fcx, fcy, 3.5f, sec_col);
    draw_aa_disc(fcx, fcy, 1.8f, pin_col);
}

static void render_volume_osd(void) {
    int cw = 360, ch = 64;
    int cx = (NATIVE_W - cw) / 2;
    int cy = NATIVE_H - ch - 16;

    fill_rect(cx, cy, cw, ch, RGB_COLOR(16, 23, 36));
    draw_rect(cx, cy, cw, ch, 2, RGB_COLOR(248, 212, 108));

    char vol_lbl[48];
    snprintf(vol_lbl, sizeof(vol_lbl), "VOLUME: %d / 10", g_volume);
    draw_text_centered(cy + 12, vol_lbl, 2,
                       (g_volume > 0) ? RGB_COLOR(248, 212, 108) : RGB_COLOR(145, 165, 195));

    /* 10-segment volume bar */
    int bar_total_w = 10 * 24 + 9 * 6; /* 294 */
    int bx0 = cx + (cw - bar_total_w) / 2;
    int by0 = cy + 36;
    for (int i = 0; i < 10; i++) {
        int bx = bx0 + i * 30;
        uint32_t bc = (i < g_volume) ? RGB_COLOR(248, 212, 108) : RGB_COLOR(34, 46, 68);
        fill_rect(bx, by0, 24, 16, bc);
    }
}

static void render_normal_clock_screen(const struct tm *shifted_tm, uint64_t now_ms) {
    clear_screen(COLOR_BLACK);

    update_burn_in_orbit(now_ms);
    int cx = (NATIVE_W / 2) + (int)roundf(g_orbit_cur_x);
    int cy = (NATIVE_H / 2) + (int)roundf(g_orbit_cur_y);

    if (g_is_analog) {
        render_analog_clock(shifted_tm, cx, cy, now_ms);
    } else {
        render_digital_clock(shifted_tm, cx, cy, now_ms);
    }

    /* Alarm Ringing Banner */
    if (g_alarm_ringing) {
        bool flash = ((now_ms / 300ULL) % 2ULL == 0ULL);
        uint32_t bg = flash ? RGB_COLOR(235, 65, 60) : RGB_COLOR(40, 16, 20);
        uint32_t fg = flash ? COLOR_WHITE            : RGB_COLOR(255, 120, 110);
        fill_rect(40, 14, NATIVE_W - 80, 44, bg);
        draw_rect(40, 14, NATIVE_W - 80, 44, 2, COLOR_WHITE);
        draw_text_centered(29, "ALARM RINGING! PRESS ANY BUTTON TO DISMISS", 2, fg);
    } else if (now_ms < g_toast_until_ms && g_toast_text[0]) {
        int tw = text_width(g_toast_text, 2);
        int bw = tw + 36;
        int bx = (NATIVE_W - bw) / 2;
        fill_rect(bx, 14, bw, 36, RGB_COLOR(20, 30, 48));
        draw_rect(bx, 14, bw, 36, 2, RGB_COLOR(88, 165, 235));
        draw_text_centered(25, g_toast_text, 2, RGB_COLOR(220, 240, 255));
    }

    /* On-screen Volume Level Indicator when VOL+ / VOL- is pressed or confirming */
    if (now_ms < g_vol_osd_until_ms || g_vol_pending_confirm || g_confirm_samples_rem > 0) {
        render_volume_osd();
    }
}

static void render_alarm_set_screen(const struct tm *shifted_tm) {
    clear_screen(RGB_COLOR(12, 16, 26));

    /* Top Header */
    fill_rect(0, 0, NATIVE_W, 46, RGB_COLOR(24, 36, 58));
    fill_rect(0, 44, NATIVE_W, 2, RGB_COLOR(88, 205, 255));
    draw_text_centered(16, "SET ALARM  (EVALUATED IN SHIFTED TIMEZONE)", 2, RGB_COLOR(248, 212, 108));

    char tz_buf[32];
    format_tz_shift(tz_buf, sizeof(tz_buf), g_tz_shift_mins);
    char cur_str[96];
    snprintf(cur_str, sizeof(cur_str), "CURRENT CLOCK TIME (%s):  %02d:%02d",
             tz_buf, shifted_tm->tm_hour % 24, shifted_tm->tm_min % 60);
    draw_text_centered(62, cur_str, 2, RGB_COLOR(155, 185, 225));

    /* Three interactive selector boxes: HOUR | MINUTE | ON/OFF */
    int box_y = 96, box_h = 166;
    int box_w = 164;
    int hr_x  = 44;
    int mn_x  = 238;
    int st_x  = 432;

    /* Box 0: HOUR (00..23) */
    bool sel_hr = (g_alarm_field == ALARM_FIELD_HOUR);
    fill_rect(hr_x, box_y, box_w, box_h, sel_hr ? RGB_COLOR(28, 46, 76) : RGB_COLOR(16, 23, 36));
    draw_rect(hr_x, box_y, box_w, box_h, sel_hr ? 3 : 2,
              sel_hr ? RGB_COLOR(88, 215, 255) : RGB_COLOR(58, 86, 132));
    draw_text(hr_x + (box_w - text_width("HOUR (24H)", 2)) / 2, box_y + 16, "HOUR (24H)", 2, RGB_COLOR(150, 180, 220));
    char hr_str[16];
    snprintf(hr_str, sizeof(hr_str), "%02d", g_edit_alarm_hour);
    draw_text(hr_x + (box_w - text_width(hr_str, 7)) / 2, box_y + 62, hr_str, 7, COLOR_WHITE);
    if (sel_hr) {
        draw_text(hr_x + (box_w - text_width("[UP/DN] ADJUST", 1)) / 2, box_y + 138, "[UP/DN] ADJUST", 1, RGB_COLOR(88, 215, 255));
    }

    /* Colon between Hour and Minute */
    draw_text(hr_x + box_w + 6, box_y + 66, ":", 6, RGB_COLOR(150, 180, 220));

    /* Box 1: MINUTE (00..59) */
    bool sel_mn = (g_alarm_field == ALARM_FIELD_MINUTE);
    fill_rect(mn_x, box_y, box_w, box_h, sel_mn ? RGB_COLOR(28, 46, 76) : RGB_COLOR(16, 23, 36));
    draw_rect(mn_x, box_y, box_w, box_h, sel_mn ? 3 : 2,
              sel_mn ? RGB_COLOR(88, 215, 255) : RGB_COLOR(58, 86, 132));
    draw_text(mn_x + (box_w - text_width("MINUTE", 2)) / 2, box_y + 16, "MINUTE", 2, RGB_COLOR(150, 180, 220));
    char mn_str[16];
    snprintf(mn_str, sizeof(mn_str), "%02d", g_edit_alarm_min);
    draw_text(mn_x + (box_w - text_width(mn_str, 7)) / 2, box_y + 62, mn_str, 7, COLOR_WHITE);
    if (sel_mn) {
        draw_text(mn_x + (box_w - text_width("[UP/DN] ADJUST", 1)) / 2, box_y + 138, "[UP/DN] ADJUST", 1, RGB_COLOR(88, 215, 255));
    }

    /* Box 2: STATUS (ON / OFF) */
    bool sel_st = (g_alarm_field == ALARM_FIELD_STATE);
    fill_rect(st_x, box_y, box_w, box_h, sel_st ? RGB_COLOR(28, 46, 76) : RGB_COLOR(16, 23, 36));
    draw_rect(st_x, box_y, box_w, box_h, sel_st ? 3 : 2,
              sel_st ? RGB_COLOR(88, 215, 255) : RGB_COLOR(58, 86, 132));
    draw_text(st_x + (box_w - text_width("STATUS", 2)) / 2, box_y + 16, "STATUS", 2, RGB_COLOR(150, 180, 220));
    const char *st_lbl = g_edit_alarm_enabled ? "ON" : "OFF";
    draw_text(st_x + (box_w - text_width(st_lbl, 6)) / 2, box_y + 66,
              st_lbl, 6,
              g_edit_alarm_enabled ? RGB_COLOR(72, 219, 136) : RGB_COLOR(246, 88, 78));
    if (sel_st) {
        draw_text(st_x + (box_w - text_width("[UP/DN] TOGGLE", 1)) / 2, box_y + 138, "[UP/DN] TOGGLE", 1, RGB_COLOR(88, 215, 255));
    }

    /* Confirmation Note */
    fill_rect(44, 278, NATIVE_W - 88, 62, RGB_COLOR(20, 32, 48));
    draw_rect(44, 278, NATIVE_W - 88, 62, 2, RGB_COLOR(72, 219, 136));
    draw_text_centered(292, "PRESS [A] TO SAVE ALARM & PLAY CONFIRMATION SOUND", 1, RGB_COLOR(72, 219, 136));
    draw_text_centered(314, "PRESS [B] TO DISABLE ALARM & RETURN TO CLOCK", 1, RGB_COLOR(185, 205, 230));

    /* Bottom Controls in Alarm Set Mode (2x2 Grid) */
    int btn_y1 = 356, btn_y2 = 412, btn_w = 268, btn_h = 46;
    draw_pill_button(44,  btn_y1, btn_w, btn_h,
                     "L / R", "SELECT HOUR / MIN / ON-OFF", RGB_COLOR(88, 205, 255), RGB_COLOR(58, 110, 155));
    draw_pill_button(328, btn_y1, btn_w, btn_h,
                     "UP/DN", "CHANGE VALUE (HOLD=FAST)", RGB_COLOR(248, 212, 108), RGB_COLOR(130, 115, 65));
    draw_pill_button(44,  btn_y2, btn_w, btn_h,
                     "A", "CONFIRM & TEST ALARM SOUND", RGB_COLOR(72, 219, 136), RGB_COLOR(58, 125, 95));
    draw_pill_button(328, btn_y2, btn_w, btn_h,
                     "Y", "SHOW ALL CONTROLS", RGB_COLOR(255, 155, 75), RGB_COLOR(145, 95, 55));
}

static void render_help_screen(void) {
    clear_screen(RGB_COLOR(10, 15, 24));

    /* Header */
    fill_rect(0, 0, NATIVE_W, 44, RGB_COLOR(24, 36, 58));
    fill_rect(0, 42, NATIVE_W, 2, RGB_COLOR(255, 155, 75));
    draw_text_centered(15, "NIGHT CLOCK  -  CONTROLS & BUTTON MAPPINGS", 2, RGB_COLOR(255, 185, 110));

    int table_x = 20, table_y = 52, table_w = NATIVE_W - 40;
    static const struct {
        const char *btn;
        const char *action;
    } k_help_rows[] = {
        { "D-PAD LEFT / RIGHT", "Shift local timezone (TZ -30m / +30m, saved on boot)" },
        { "D-PAD UP / DOWN",    "Adjust screen & clock brightness (1..10, hold=repeat)" },
        { "SELECT / X BUTTON",  "Switch clock mode: Retro LED Digital (24H) / Analog" },
        { "A BUTTON",           "Open Alarm Set screen; press A again to save & test" },
        { "B BUTTON",           "Quick-toggle Alarm ON/OFF (Clock) / Cancel (Alarm Set)" },
        { "Y BUTTON",           "Show Controls / Help screen; press Y again to resume" },
        { "L1 / R1 (IN ALARM)", "Step alarm minutes by -5 / +5 mins while setting" },
        { "VOL+ / VOL- (+/-)",  "Adjust volume (0..10) & play confirmation alarm at end" },
        { "MENU (ESC)",         "Save configuration & exit Night Clock" }
    };

    int num_rows = (int)(sizeof(k_help_rows) / sizeof(k_help_rows[0]));
    int row_h = 38;
    for (int i = 0; i < num_rows; i++) {
        int ry = table_y + i * row_h;
        uint32_t row_bg = (i % 2 == 0) ? RGB_COLOR(16, 24, 38) : RGB_COLOR(22, 32, 50);
        fill_rect(table_x, ry, table_w, row_h - 3, row_bg);
        draw_rect(table_x, ry, table_w, row_h - 3, 1, RGB_COLOR(48, 70, 108));

        fill_rect(table_x + 8, ry + 4, 178, row_h - 11, RGB_COLOR(32, 52, 84));
        draw_text(table_x + 16, ry + 13, k_help_rows[i].btn, 1, RGB_COLOR(248, 212, 108));
        draw_text(table_x + 198, ry + 13, k_help_rows[i].action, 1, RGB_COLOR(225, 235, 250));
    }

    /* Footer resume banner */
    fill_rect(20, 414, NATIVE_W - 40, 50, RGB_COLOR(26, 40, 64));
    draw_rect(20, 414, NATIVE_W - 40, 50, 2, RGB_COLOR(255, 155, 75));
    draw_text_centered(432, "PRESS [Y], [A], OR [B] TO RESUME CLOCK", 2, RGB_COLOR(255, 195, 120));
}

static void render_screen(const struct tm *shifted_tm, uint64_t now_ms) {
    if (g_ui_mode == UI_MODE_HELP) {
        render_help_screen();
    } else if (g_ui_mode == UI_MODE_ALARM_SET) {
        render_alarm_set_screen(shifted_tm);
    } else {
        render_normal_clock_screen(shifted_tm, now_ms);
    }
}

/* ============================================================================
 * 8. INPUT HANDLING & CONTINUOUS HOLD AUTO-REPEAT
 * ============================================================================ */

typedef struct {
    uint32_t mask;
    uint64_t next_repeat_ms;
} repeat_tracker_t;

static bool check_button_with_repeat(const input_state_t *in, repeat_tracker_t *rt, uint64_t now_ms) {
    if (in->pressed & rt->mask) {
        rt->next_repeat_ms = now_ms + REPEAT_INITIAL_DELAY_MS;
        return true;
    }
    if ((in->held & rt->mask) && rt->next_repeat_ms != 0 && now_ms >= rt->next_repeat_ms) {
        rt->next_repeat_ms = now_ms + REPEAT_INTERVAL_MS;
        return true;
    }
    if (!(in->held & rt->mask)) {
        rt->next_repeat_ms = 0;
    }
    return false;
}

/* ============================================================================
 * 9. MAIN LOOP
 * ============================================================================ */

int main(int argc, char **argv) {
    int dump_promo_mode = 0;
    const char *dump_prefix = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-promo") == 0 && i + 1 < argc) {
            dump_promo_mode = 1;
            dump_prefix = argv[++i];
        }
    }

    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGHUP,  handle_sig);

    load_config();

    if (!dump_promo_mode) {
        if (platform_init("Anbernic RG35XX SP Night Clock") != 0) {
            fprintf(stderr, "Failed to initialize video backend.\n");
            return 1;
        }
        apply_hardware_brightness(g_brightness);
        apply_hardware_volume(g_volume);
        audio_init();
    }

    g_boot_mono_ms = ticks_ms();

    repeat_tracker_t rep_left   = { BTN_LEFT,     0 };
    repeat_tracker_t rep_right  = { BTN_RIGHT,    0 };
    repeat_tracker_t rep_up     = { BTN_UP,       0 };
    repeat_tracker_t rep_down   = { BTN_DOWN,     0 };
    repeat_tracker_t rep_vol_up = { BTN_VOL_UP,   0 };
    repeat_tracker_t rep_vol_dn = { BTN_VOL_DOWN, 0 };

    while (!g_sig_quit) {
        input_state_t input = {0, 0, 0};
        if (!dump_promo_mode) {
            if (!platform_poll_input(&input)) {
                break;
            }
            if ((input.pressed & BTN_MENU) != 0) {
                break;
            }
        }

        uint64_t now_ms = ticks_ms();

        /* If the alarm is currently ringing, any button press dismisses it */
        if (g_alarm_ringing) {
            if (input.pressed != 0) {
                g_alarm_ringing       = false;
                g_vol_pending_confirm = false;
                g_confirm_samples_rem = 0;
                show_toast("ALARM DISMISSED", now_ms);
                input.pressed = 0;
            } else if (now_ms - g_alarm_ring_start_ms > 300000ULL) {
                g_alarm_ringing = false;
            }
        }

        /* Volume +/- handling: modify indicator immediately, play alarm at that volume when released */
        bool vol_inc  = check_button_with_repeat(&input, &rep_vol_up, now_ms);
        bool vol_dec  = check_button_with_repeat(&input, &rep_vol_dn, now_ms);
        bool vol_held = (input.held & (BTN_VOL_UP | BTN_VOL_DOWN)) != 0;

        if (vol_inc || vol_dec) {
            int next_vol = g_volume + (vol_inc ? 1 : -1);
            if (next_vol < 0)  next_vol = 0;
            if (next_vol > 10) next_vol = 10;
            g_volume = next_vol;
            apply_hardware_volume(g_volume);
            save_config();

            char vmsg[64];
            snprintf(vmsg, sizeof(vmsg), "VOLUME: %d / 10", g_volume);
            show_toast(vmsg, now_ms);
            g_vol_osd_until_ms    = now_ms + VOL_OSD_DURATION_MS;
            g_vol_pending_confirm = true;
            if (vol_held) {
                g_confirm_samples_rem = 0;
            }
        }
        if (g_vol_pending_confirm && !vol_held) {
            g_vol_pending_confirm = false;
            g_vol_osd_until_ms    = now_ms + VOL_OSD_DURATION_MS;
            trigger_confirmation_alarm_sound();
        }

        /* Y toggles the Controls / Help screen on the single display, and pressing Y again resumes the clock */
        if (input.pressed & BTN_Y) {
            if (g_ui_mode == UI_MODE_HELP) {
                g_ui_mode = g_mode_before_help;
            } else {
                g_mode_before_help = g_ui_mode;
                g_ui_mode = UI_MODE_HELP;
            }
            input.pressed &= ~BTN_Y;
        }

        if (g_ui_mode == UI_MODE_HELP) {
            if (input.pressed & (BTN_B | BTN_A)) {
                g_ui_mode = g_mode_before_help;
            }
        } else if (g_ui_mode == UI_MODE_ALARM_SET) {
            if (input.pressed & BTN_LEFT) {
                g_alarm_field = (alarm_field_t)((g_alarm_field + 2) % 3);
            } else if (input.pressed & BTN_RIGHT) {
                g_alarm_field = (alarm_field_t)((g_alarm_field + 1) % 3);
            }

            bool step_up   = check_button_with_repeat(&input, &rep_up,   now_ms);
            bool step_down = check_button_with_repeat(&input, &rep_down, now_ms);

            if (step_up || step_down) {
                int delta = step_up ? 1 : -1;
                if (g_alarm_field == ALARM_FIELD_HOUR) {
                    g_edit_alarm_hour = (g_edit_alarm_hour + delta + 24) % 24;
                } else if (g_alarm_field == ALARM_FIELD_MINUTE) {
                    g_edit_alarm_min = (g_edit_alarm_min + delta + 60) % 60;
                } else {
                    g_edit_alarm_enabled = !g_edit_alarm_enabled;
                }
            }

            /* Fast +/-5 minute step with L1/L2 or R1/R2 */
            if (input.pressed & (BTN_L1 | BTN_L2)) {
                g_edit_alarm_min = (g_edit_alarm_min - 5 + 60) % 60;
            } else if (input.pressed & (BTN_R1 | BTN_R2)) {
                g_edit_alarm_min = (g_edit_alarm_min + 5) % 60;
            }

            if (input.pressed & BTN_A) {
                /* Save & arm the alarm, return to the clock, and play confirmation alarm sound */
                g_alarm_hour    = g_edit_alarm_hour;
                g_alarm_min     = g_edit_alarm_min;
                g_alarm_enabled = g_edit_alarm_enabled;
                save_config();
                g_ui_mode = UI_MODE_NORMAL;

                if (g_alarm_enabled) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "ALARM SET FOR %02d:%02d", g_alarm_hour, g_alarm_min);
                    show_toast(msg, now_ms);
                    trigger_confirmation_alarm_sound();
                } else {
                    show_toast("ALARM TURNED OFF", now_ms);
                }
            } else if (input.pressed & BTN_B) {
                g_alarm_enabled = false;
                save_config();
                g_ui_mode = UI_MODE_NORMAL;
                show_toast("ALARM OFF", now_ms);
            }
        } else {
            /* UI_MODE_NORMAL: Clock View */

            /* (0) LEFT / RIGHT: Adjust local timezone shift in 30-minute increments */
            bool tz_dec = check_button_with_repeat(&input, &rep_left,  now_ms);
            bool tz_inc = check_button_with_repeat(&input, &rep_right, now_ms);
            if (tz_dec || tz_inc) {
                int next_tz = g_tz_shift_mins + (tz_inc ? 30 : -30);
                if (next_tz < -1440) next_tz = -1440;
                if (next_tz >  1440) next_tz =  1440;
                if (next_tz != g_tz_shift_mins) {
                    g_tz_shift_mins = next_tz;
                    save_config();
                }
                char tz_buf[32], msg[64];
                format_tz_shift(tz_buf, sizeof(tz_buf), g_tz_shift_mins);
                snprintf(msg, sizeof(msg), "TIMEZONE SHIFT: %s", tz_buf);
                show_toast(msg, now_ms);
            }

            /* UP / DOWN: Adjust brightness (1..10) */
            bool br_up   = check_button_with_repeat(&input, &rep_up,   now_ms);
            bool br_down = check_button_with_repeat(&input, &rep_down, now_ms);
            if (br_up || br_down) {
                int next_br = g_brightness + (br_up ? 1 : -1);
                if (next_br < 1)  next_br = 1;
                if (next_br > 10) next_br = 10;
                if (next_br != g_brightness) {
                    g_brightness = next_br;
                    apply_hardware_brightness(g_brightness);
                    save_config();
                }
                char msg[64];
                snprintf(msg, sizeof(msg), "BRIGHTNESS: %d / 10", g_brightness);
                show_toast(msg, now_ms);
            }

            /* SELECT Key: Switch between Retro LED Digital (24H) and Minimal Analog */
            if (input.pressed & BTN_SELECT) {
                g_is_analog = !g_is_analog;
                save_config();
                show_toast(g_is_analog ? "ANALOG CLOCK MODE" : "DIGITAL CLOCK MODE (24H)", now_ms);
            }

            /* B Key: Secondary action — Quick-toggle Alarm ON / OFF */
            if (input.pressed & BTN_B) {
                g_alarm_enabled = !g_alarm_enabled;
                save_config();
                if (g_alarm_enabled) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "ALARM ON (%02d:%02d)", g_alarm_hour, g_alarm_min);
                    show_toast(msg, now_ms);
                    trigger_confirmation_alarm_sound();
                } else {
                    show_toast("ALARM OFF", now_ms);
                }
            }

            /* A Key: Primary action — Open Alarm Set Mode on screen */
            if (input.pressed & BTN_A) {
                g_edit_alarm_hour    = g_alarm_hour;
                g_edit_alarm_min     = g_alarm_min;
                g_edit_alarm_enabled = true;
                g_alarm_field        = ALARM_FIELD_HOUR;
                g_ui_mode            = UI_MODE_ALARM_SET;
            }
        }

        /* Compute Device Time and Shifted Time (device_time + g_tz_shift_mins) */
        time_t raw_now = time(NULL);
        struct tm dev_tm;
        localtime_r(&raw_now, &dev_tm);

        struct tm shifted_tm = dev_tm;
        shifted_tm.tm_min  += g_tz_shift_mins;
        shifted_tm.tm_isdst = -1;
        mktime(&shifted_tm);

        /* Check if Alarm should trigger in the Shifted Timezone */
        int cur_min_key = (shifted_tm.tm_year * 530000) + (shifted_tm.tm_yday * 1440) +
                          (shifted_tm.tm_hour * 60) + shifted_tm.tm_min;
        if (g_alarm_enabled && g_ui_mode != UI_MODE_ALARM_SET &&
            shifted_tm.tm_hour == g_alarm_hour &&
            shifted_tm.tm_min  == g_alarm_min  &&
            cur_min_key != g_last_alarm_min_key) {
            g_last_alarm_min_key  = cur_min_key;
            g_alarm_ringing       = true;
            g_alarm_ring_start_ms = now_ms;
            apply_hardware_volume(g_volume);
        }

        /* Optional promo screenshot dump mode */
        if (dump_promo_mode && dump_prefix) {
            struct {
                bool is_analog;
                ui_mode_t mode;
                int tz_mins;
                bool alrm_en;
                bool show_vol_osd;
                const char *suffix;
            } shots[4] = {
                { false, UI_MODE_NORMAL,    330, true, false, "digital_clock.raw" },
                { true,  UI_MODE_NORMAL,    330, true, true,  "analog_clock.raw"  },
                { false, UI_MODE_ALARM_SET, 330, true, false, "alarm_set.raw"     },
                { false, UI_MODE_HELP,      330, true, false, "help_screen.raw"   },
            };
            g_volume             = 8;
            g_brightness         = 9;
            g_alarm_hour         = 7;
            g_alarm_min          = 30;
            g_edit_alarm_hour    = 7;
            g_edit_alarm_min     = 30;
            g_edit_alarm_enabled = true;
            for (int s = 0; s < 4; s++) {
                g_is_analog      = shots[s].is_analog;
                g_ui_mode        = shots[s].mode;
                g_tz_shift_mins  = shots[s].tz_mins;
                g_alarm_enabled  = shots[s].alrm_en;
                g_vol_osd_until_ms = shots[s].show_vol_osd ? (now_ms + 2000ULL) : 0;

                struct tm s_tm = dev_tm;
                s_tm.tm_min  += g_tz_shift_mins;
                s_tm.tm_isdst = -1;
                mktime(&s_tm);

                render_screen(&s_tm, now_ms);

                char out_fn[256];
                snprintf(out_fn, sizeof(out_fn), "%s_%s", dump_prefix, shots[s].suffix);
                FILE *df = fopen(out_fn, "wb");
                if (df) {
                    fwrite(g_framebuf, sizeof(uint32_t), (size_t)NATIVE_W * NATIVE_H, df);
                    fclose(df);
                }
            }
            break;
        }

        /* Render 640x480 screen and present */
        render_screen(&shifted_tm, now_ms);
        platform_present();
        delay_ms(16);
    }

    if (!dump_promo_mode) {
        save_config();
        audio_shutdown();
        platform_shutdown();
    }
    return 0;
}
