/*
 * musicplayer.c - Native 640x480 Directory Music Player (MP3 / FLAC / OGG / WAV)
 *                 for Anbernic RG 35xxSP (Allwinner H700 aarch64) & Linux Desktop (x86_64 X11)
 *
 * Zero Third-Party .so Dependencies:
 *   - MP3  : Statically compiled ../third_party/dr_mp3.h + pure-C ID3v1/ID3v2 tag reader
 *   - FLAC : Statically compiled ../third_party/dr_flac.h + Vorbis comment metadata reader
 *   - OGG  : Statically compiled ../third_party/stb_vorbis.c + Vorbis comment metadata reader
 *   - WAV  : Built-in 8/16-bit PCM RIFF/WAVE reader
 *   - Audio: Background decoder thread + lock-free 44.1 kHz stereo ring buffer +
 *            linear-interpolated resampler -> dlopen(libSDL2-2.0.so.0 / libasound.so.2 / libpulse-simple.so.0)
 *
 * Controls (RG 35xxSP / Desktop X11):
 *   A (A / Space / Z)      : Browser: Resume / Play Directory | Player: Play / Pause
 *   B (B / Backspace)      : Browser: Up Directory (..)       | Player: Play Highlighted Song
 *   UP / DOWN (Arrows)     : Move cursor (hold to scroll rapidly)
 *   LEFT / RIGHT (Arrows)  : Browser: Up Dir / Enter Subdir   | Player: Page -/+ 5 Tracks
 *   L1 / R1 (L/R, [/])     : Global Seek -5s / +5s (Hold: Accelerating Rewind / Fast-Forward)
 *   SELECT (Tab) / X       : Switch Directory Browser <-> Now Playing / Playlist View
 *   Y (Y)                  : Toggle English Help Overlay
 *   MENU (Esc)             : Save Playlist + Exact Position & Quit (sole exit key)
 */

#include "../rg35xxsp_input.h"
#include "../rg35xxsp_keepawake.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>

/* Statically compiled single-file decoders with warnings suppressed for third-party headers */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wshadow"
#define DR_MP3_IMPLEMENTATION
#include "../third_party/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "../third_party/dr_flac.h"
#include "../third_party/stb_vorbis.c"
#pragma GCC diagnostic pop

#include "musicplayer_font.h"

#define NATIVE_W        640
#define NATIVE_H        480
#define AUDIO_RATE      44100
#define AUDIO_CHANNELS  2
#define AUDIO_SAMPLES   1024

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

static uint32_t get_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

static void delay_ms(uint32_t ms) {
    usleep((useconds_t)ms * 1000u);
}

#define RGB(r, g, b) (0xFF000000u | (((uint32_t)(r) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu))

/* Palette */
#define COL_BG          RGB(14, 19, 29)
#define COL_HEADER      RGB(23, 31, 47)
#define COL_PANEL       RGB(20, 27, 41)
#define COL_PANEL_ALT   RGB(29, 39, 58)
#define COL_ROW_SEL     RGB(36, 54, 84)
#define COL_BORDER      RGB(54, 71, 102)
#define COL_WHITE       RGB(244, 247, 252)
#define COL_MUTED       RGB(148, 163, 184)
#define COL_DIM         RGB(88, 104, 130)
#define COL_ACCENT      RGB(239, 68, 68)
#define COL_GOLD        RGB(250, 204, 21)
#define COL_CYAN        RGB(56, 189, 248)
#define COL_GREEN       RGB(34, 197, 94)
#define COL_ORANGE      RGB(249, 115, 22)
#define COL_PURPLE      RGB(168, 85, 247)
#define COL_TEAL        RGB(20, 184, 166)

/* Forward declaration of audio output callback used by platform audio layer */
static void audio_output_callback(void *userdata, uint8_t *stream, int len);

/* ============================================================================
 * 1. PLATFORM BACKEND: HOST_X11 vs TARGET_RG35XXSP (SDL2/FBDEV/EVDEV/ALSA)
 * ============================================================================ */

static uint32_t g_prev_buttons = 0;
static uint32_t g_cur_buttons  = 0;

#if defined(HOST_X11)

#include <X11/Xutil.h>
#include <X11/XKBlib.h>

static Display  *g_x11_dpy = NULL;
static Window    g_x11_win = 0;
static GC        g_x11_gc  = 0;
static XImage   *g_x11_img = NULL;
static Atom      g_wm_delete_window = 0;
static char     *g_x11_buf = NULL;
static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;

static void *host_audio_worker(void *arg) {
    (void)arg;
    int16_t buf[512 * AUDIO_CHANNELS];
    int chunk_bytes = (int)sizeof(buf);

    /* 1. Try PulseAudio Simple via dlopen */
    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    if (pa_lib) {
        typedef struct { int format; uint32_t rate; uint8_t channels; } pa_ss_t;
        typedef void *(*pa_new_fn)(const char*, const char*, int, const char*,
                                   const char*, const pa_ss_t*, const void*, const void*, int*);
        typedef int   (*pa_write_fn)(void*, const void*, size_t, int*);
        typedef void  (*pa_free_fn)(void*);
        pa_new_fn   fn_new   = (pa_new_fn)dlsym(pa_lib, "pa_simple_new");
        pa_write_fn fn_write = (pa_write_fn)dlsym(pa_lib, "pa_simple_write");
        pa_free_fn  fn_free  = (pa_free_fn)dlsym(pa_lib, "pa_simple_free");
        if (fn_new && fn_write && fn_free) {
            pa_ss_t ss = { 3 /* PA_SAMPLE_S16LE */, AUDIO_RATE, AUDIO_CHANNELS };
            int err = 0;
            void *pa = fn_new(NULL, "RG35XXSP MusicPlayer", 1, NULL, "Audio Playback", &ss, NULL, NULL, &err);
            if (pa) {
                while (g_audio_running) {
                    audio_output_callback(NULL, (uint8_t *)buf, chunk_bytes);
                    fn_write(pa, buf, (size_t)chunk_bytes, &err);
                }
                fn_free(pa);
                dlclose(pa_lib);
                return NULL;
            }
        }
        dlclose(pa_lib);
    }

    /* 2. Fallback to ALSA via dlopen */
    void *asound = dlopen("libasound.so.2", RTLD_LAZY);
    if (asound) {
        typedef int  (*snd_open_fn)(void**, const char*, int, int);
        typedef int  (*snd_params_fn)(void*, int, int, unsigned int, unsigned int, int, unsigned int);
        typedef long (*snd_writei_fn)(void*, const void*, unsigned long);
        typedef int  (*snd_recover_fn)(void*, int, int);
        typedef int  (*snd_close_fn)(void*);
        snd_open_fn    fn_open    = (snd_open_fn)dlsym(asound, "snd_pcm_open");
        snd_params_fn  fn_params  = (snd_params_fn)dlsym(asound, "snd_pcm_set_params");
        snd_writei_fn  fn_writei  = (snd_writei_fn)dlsym(asound, "snd_pcm_writei");
        snd_recover_fn fn_recover = (snd_recover_fn)dlsym(asound, "snd_pcm_recover");
        snd_close_fn   fn_close   = (snd_close_fn)dlsym(asound, "snd_pcm_close");
        void *pcm = NULL;
        if (fn_open && fn_params && fn_writei && fn_close && fn_open(&pcm, "default", 0, 0) >= 0) {
            fn_params(pcm, 2, 3, AUDIO_CHANNELS, AUDIO_RATE, 1, 40000);
            while (g_audio_running) {
                audio_output_callback(NULL, (uint8_t *)buf, chunk_bytes);
                long w = fn_writei(pcm, buf, 512);
                if (w < 0 && fn_recover) fn_recover(pcm, (int)w, 1);
            }
            fn_close(pcm);
            dlclose(asound);
            return NULL;
        }
        dlclose(asound);
    }

    /* 3. Silent pacing fallback (headless environment without audio daemon) */
    while (g_audio_running) {
        audio_output_callback(NULL, (uint8_t *)buf, chunk_bytes);
        usleep((512 * 1000000ULL) / AUDIO_RATE);
    }
    return NULL;
}

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

    g_audio_running = true;
    pthread_create(&g_audio_thread, NULL, host_audio_worker, NULL);
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
    if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
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

#else /* TARGET_RG35XXSP: SDL2 dlopen + /dev/fb0 fallback + evdev + ALSA fallback */

#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define SDL2_INIT_AUDIO                0x00000010U
#define SDL2_INIT_VIDEO                0x00000020U
#define SDL2_WINDOWPOS_UNDEFINED       0x1FFF0000U
#define SDL2_WINDOW_SHOWN              0x00000004U
#define SDL2_WINDOW_FULLSCREEN_DESKTOP 0x00001001U
#define SDL2_RENDERER_ACCELERATED      0x00000002U
#define SDL2_RENDERER_PRESENTVSYNC     0x00000004U
#define SDL2_PIXELFORMAT_ARGB8888      0x16362004U
#define SDL2_TEXTUREACCESS_STREAMING   1
#define SDL2_AUDIO_S16LSB              0x8010
#define SDL2_QUIT_EVENT                0x100U

typedef void (*SDL2_AudioCallback)(void *userdata, uint8_t *stream, int len);

typedef struct {
    int                freq;
    uint16_t           format;
    uint8_t            channels;
    uint8_t            silence;
    uint16_t           samples;
    uint16_t           padding;
    uint32_t           size;
    SDL2_AudioCallback callback;
    void              *userdata;
} SDL2_AudioSpec;

typedef union {
    uint32_t type;
    uint8_t  padding[64];
} SDL2_RawEvent;

static void *g_sdl2_lib = NULL;
static void *g_sdl2_win = NULL;
static void *g_sdl2_ren = NULL;
static void *g_sdl2_tex = NULL;
static bool  g_use_sdl2_video = false;
static bool  g_use_sdl2_audio = false;

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
static int   (*fn_SDL_OpenAudio)(SDL2_AudioSpec*, SDL2_AudioSpec*) = NULL;
static void  (*fn_SDL_PauseAudio)(int) = NULL;
static void  (*fn_SDL_CloseAudio)(void) = NULL;
static int   (*fn_SDL_ShowCursor)(int) = NULL;

#define MAX_EVDEV_FDS 32
static int g_evdev_fds[MAX_EVDEV_FDS];
static int g_num_evdevs = 0;

static int                      g_fb_fd = -1;
static uint8_t                 *g_fb_mem = NULL;
static size_t                   g_fb_map_size = 0;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;

static pthread_t     g_alsa_thread;
static volatile bool g_alsa_running = false;

static void *alsa_audio_worker(void *arg) {
    (void)arg;
    int16_t buf[512 * AUDIO_CHANNELS];
    void *asound = dlopen("libasound.so.2", RTLD_LAZY);
    void *pcm = NULL;

    typedef int  (*snd_open_fn)(void**, const char*, int, int);
    typedef int  (*snd_params_fn)(void*, int, int, unsigned int, unsigned int, int, unsigned int);
    typedef long (*snd_writei_fn)(void*, const void*, unsigned long);
    typedef int  (*snd_recover_fn)(void*, int, int);
    typedef int  (*snd_close_fn)(void*);

    snd_writei_fn  fn_writei  = NULL;
    snd_recover_fn fn_recover = NULL;
    snd_close_fn   fn_close   = NULL;

    if (asound) {
        snd_open_fn   fn_open   = (snd_open_fn)dlsym(asound, "snd_pcm_open");
        snd_params_fn fn_params = (snd_params_fn)dlsym(asound, "snd_pcm_set_params");
        fn_writei  = (snd_writei_fn)dlsym(asound, "snd_pcm_writei");
        fn_recover = (snd_recover_fn)dlsym(asound, "snd_pcm_recover");
        fn_close   = (snd_close_fn)dlsym(asound, "snd_pcm_close");

        if (fn_open && fn_params && fn_writei && fn_close) {
            if (fn_open(&pcm, "default", 0, 0) < 0 &&
                fn_open(&pcm, "plughw:0,0", 0, 0) < 0 &&
                fn_open(&pcm, "hw:0,0", 0, 0) < 0) {
                pcm = NULL;
            }
            if (pcm) {
                fn_params(pcm, 2 /* S16_LE */, 3 /* RW_INTERLEAVED */, AUDIO_CHANNELS, AUDIO_RATE, 1, 35000);
            }
        }
    }

    while (g_alsa_running) {
        audio_output_callback(NULL, (uint8_t *)buf, (int)sizeof(buf));
        if (pcm && fn_writei) {
            long w = fn_writei(pcm, buf, 512);
            if (w < 0 && fn_recover) fn_recover(pcm, (int)w, 1);
        } else {
            usleep(11600);
        }
    }
    if (pcm && fn_close) fn_close(pcm);
    if (asound) dlclose(asound);
    return NULL;
}

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
        fn_SDL_OpenAudio       = dlsym(g_sdl2_lib, "SDL_OpenAudio");
        fn_SDL_PauseAudio      = dlsym(g_sdl2_lib, "SDL_PauseAudio");
        fn_SDL_CloseAudio      = dlsym(g_sdl2_lib, "SDL_CloseAudio");
        fn_SDL_ShowCursor      = dlsym(g_sdl2_lib, "SDL_ShowCursor");

        if (fn_SDL_Init && fn_SDL_CreateWindow && fn_SDL_CreateRenderer && fn_SDL_CreateTexture) {
            if (fn_SDL_Init(SDL2_INIT_VIDEO | SDL2_INIT_AUDIO) == 0 ||
                fn_SDL_Init(SDL2_INIT_VIDEO) == 0) {
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
                if (fn_SDL_OpenAudio && fn_SDL_PauseAudio) {
                    SDL2_AudioSpec desired;
                    memset(&desired, 0, sizeof(desired));
                    desired.freq     = AUDIO_RATE;
                    desired.format   = SDL2_AUDIO_S16LSB;
                    desired.channels = AUDIO_CHANNELS;
                    desired.samples  = AUDIO_SAMPLES;
                    desired.callback = audio_output_callback;
                    if (fn_SDL_OpenAudio(&desired, NULL) == 0) {
                        fn_SDL_PauseAudio(0);
                        g_use_sdl2_audio = true;
                    }
                }
            }
        }
    }

    if (!g_use_sdl2_video && !init_fbdev_fallback()) return false;
    if (!g_use_sdl2_audio) {
        g_alsa_running = true;
        pthread_create(&g_alsa_thread, NULL, alsa_audio_worker, NULL);
    }
    rg35xxsp_keepawake_init("musicplayer", g_sdl2_lib);
    return true;
}

static void platform_present(void) {
    rg35xxsp_keepawake_tick_now();
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
                if (ie.value == 1) {
                    if (mask & BTN_LID_CLOSE) { rg35xxsp_keepawake_set_lid_closed(true);  continue; }
                    if (mask & BTN_LID_OPEN)  { rg35xxsp_keepawake_set_lid_closed(false); continue; }
                }
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
    rg35xxsp_keepawake_shutdown();
    if (g_alsa_running) {
        g_alsa_running = false;
        pthread_join(g_alsa_thread, NULL);
    }
    for (int i = 0; i < g_num_evdevs; i++) {
        if (g_evdev_fds[i] >= 0) close(g_evdev_fds[i]);
    }
    g_num_evdevs = 0;

    if (g_use_sdl2_audio && fn_SDL_CloseAudio) {
        fn_SDL_CloseAudio();
        g_use_sdl2_audio = false;
    }
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

static int text_width(int font_id, const char *s) {
    if (!s || font_id < 0 || font_id >= MP_FONT_COUNT) return 0;
    const MpFont *f = &g_mp_fonts[font_id];
    int w = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 32 || c > 126) c = '?';
        w += f->glyphs[c - 32].advance;
    }
    return w;
}

static void draw_text(int font_id, int x, int y, const char *s, uint32_t color) {
    if (!s || font_id < 0 || font_id >= MP_FONT_COUNT) return;
    const MpFont *f = &g_mp_fonts[font_id];
    int cur_x = x;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 32 || c > 126) c = '?';
        const MpGlyph *g = &f->glyphs[c - 32];
        const uint8_t *bmp = &g_mp_font_bitmaps[g->offset];
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
    const MpFont *f = &g_mp_fonts[font_id];
    int tw = text_width(font_id, s);
    int tx = bx + (bw - tw) / 2;
    int ty = by + (bh - f->line_height) / 2;
    draw_text(font_id, tx, ty, s, color);
}

static void draw_text_right(int font_id, int right_x, int y, const char *s, uint32_t color) {
    int tw = text_width(font_id, s);
    draw_text(font_id, right_x - tw, y, s, color);
}

static void draw_text_clipped(int font_id, int x, int y, int max_w, const char *s, uint32_t color) {
    if (!s || max_w <= 0) return;
    if (text_width(font_id, s) <= max_w) {
        draw_text(font_id, x, y, s, color);
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%.*s", (int)(sizeof(buf) - 1), s);
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

static void draw_path_tail(int font_id, int x, int y, int max_w, const char *path, uint32_t color) {
    if (!path || max_w <= 0) return;
    if (text_width(font_id, path) <= max_w) {
        draw_text(font_id, x, y, path, color);
        return;
    }
    size_t len = strlen(path);
    for (size_t off = 1; off < len; off++) {
        char buf[300];
        snprintf(buf, sizeof(buf), "...%.*s", (int)(sizeof(buf) - 4), path + off);
        if (text_width(font_id, buf) <= max_w) {
            draw_text(font_id, x, y, buf, color);
            return;
        }
    }
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
 * 3. AUDIO FORMATS, METADATA, LOCK-FREE RING BUFFER & BACKGROUND DECODER THREAD
 * ============================================================================ */

typedef enum {
    FMT_NONE = 0,
    FMT_MP3  = 1,
    FMT_OGG  = 2,
    FMT_FLAC = 3,
    FMT_WAV  = 4
} AudioFormat;

static AudioFormat detect_audio_format(const char *filename) {
    if (!filename) return FMT_NONE;
    const char *dot = strrchr(filename, '.');
    if (!dot) return FMT_NONE;
    if (strcasecmp(dot, ".mp3") == 0)  return FMT_MP3;
    if (strcasecmp(dot, ".ogg") == 0)  return FMT_OGG;
    if (strcasecmp(dot, ".flac") == 0) return FMT_FLAC;
    if (strcasecmp(dot, ".wav") == 0)  return FMT_WAV;
    return FMT_NONE;
}

static const char *format_badge_str(AudioFormat fmt) {
    switch (fmt) {
        case FMT_MP3:  return "MP3";
        case FMT_OGG:  return "OGG";
        case FMT_FLAC: return "FLAC";
        case FMT_WAV:  return "WAV";
        default:       return "---";
    }
}

static uint32_t format_badge_col(AudioFormat fmt) {
    switch (fmt) {
        case FMT_MP3:  return COL_ORANGE;
        case FMT_OGG:  return COL_PURPLE;
        case FMT_FLAC: return COL_CYAN;
        case FMT_WAV:  return COL_TEAL;
        default:       return COL_MUTED;
    }
}

/* Lock-free SPSC 44.1 kHz stereo S16LE ring buffer */
#define PCM_RING_FRAMES 65536
#define PCM_RING_MASK   (PCM_RING_FRAMES - 1)

static int16_t       g_pcm_ring[PCM_RING_FRAMES * 2];
static atomic_uint   g_ring_write_idx = 0;
static atomic_uint   g_ring_read_idx  = 0;

static inline uint32_t ring_available_to_read(void) {
    uint32_t w = atomic_load_explicit(&g_ring_write_idx, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&g_ring_read_idx, memory_order_relaxed);
    return w - r;
}

static inline uint32_t ring_available_to_write(void) {
    uint32_t w = atomic_load_explicit(&g_ring_write_idx, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&g_ring_read_idx, memory_order_acquire);
    return PCM_RING_FRAMES - (w - r);
}

static inline void ring_clear(void) {
    uint32_t w = atomic_load_explicit(&g_ring_write_idx, memory_order_relaxed);
    atomic_store_explicit(&g_ring_read_idx, w, memory_order_release);
}

/* Decoder control & shared state */
static pthread_mutex_t g_dec_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_dec_thread;

static volatile bool   g_dec_thread_running = false;
static volatile bool   g_dec_cmd_open       = false;
static char            g_dec_req_path[PATH_MAX];
static int64_t         g_dec_req_start_ms   = 0;
static volatile bool   g_dec_cmd_seek       = false;
static int64_t         g_dec_req_seek_delta = 0;

static atomic_bool     g_playing            = false;
static atomic_bool     g_track_loaded       = false;
static atomic_bool     g_track_eof          = false;
static atomic_int      g_dec_pos_ms         = 0;
static atomic_int      g_dec_dur_ms         = 0;
static atomic_int      g_dec_src_rate       = 44100;
static atomic_int      g_dec_src_channels   = 2;
static atomic_int      g_dec_bitrate_kbps   = 0;
static atomic_int      g_dec_fmt            = FMT_NONE;

static char            g_meta_title[128]    = "";
static char            g_meta_artist[128]   = "";
static pthread_mutex_t g_meta_mutex         = PTHREAD_MUTEX_INITIALIZER;

static atomic_uint_fast64_t g_played_out_frames = 0;

static void audio_output_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int frames_needed = len / (AUDIO_CHANNELS * (int)sizeof(int16_t));

    if (!atomic_load(&g_playing) || !atomic_load(&g_track_loaded)) {
        memset(stream, 0, (size_t)len);
        return;
    }

    uint32_t avail   = ring_available_to_read();
    uint32_t to_read = ((uint32_t)frames_needed < avail) ? (uint32_t)frames_needed : avail;
    uint32_t r = atomic_load_explicit(&g_ring_read_idx, memory_order_relaxed);

    for (uint32_t i = 0; i < to_read; i++) {
        uint32_t idx = (r + i) & PCM_RING_MASK;
        out[i * 2]     = g_pcm_ring[idx * 2];
        out[i * 2 + 1] = g_pcm_ring[idx * 2 + 1];
    }
    atomic_store_explicit(&g_ring_read_idx, r + to_read, memory_order_release);

    if (to_read < (uint32_t)frames_needed) {
        memset(&out[to_read * 2], 0, (size_t)((frames_needed - (int)to_read) * 2 * (int)sizeof(int16_t)));
    }

    if (to_read > 0) {
        uint64_t played = atomic_load_explicit(&g_played_out_frames, memory_order_relaxed) + to_read;
        atomic_store_explicit(&g_played_out_frames, played, memory_order_relaxed);
        int pos = (int)((played * 1000ULL) / AUDIO_RATE);
        int dur = atomic_load(&g_dec_dur_ms);
        if (dur > 0 && pos > dur) pos = dur;
        atomic_store(&g_dec_pos_ms, pos);
    }
}

/* Linear-interpolated stereo resampler -> 44.1 kHz S16LE ring buffer */
static double  g_resamp_phase  = 0.0;
static int16_t g_resamp_prev_l = 0;
static int16_t g_resamp_prev_r = 0;

static void reset_resampler(void) {
    g_resamp_phase  = 0.0;
    g_resamp_prev_l = 0;
    g_resamp_prev_r = 0;
}

static void push_decoded_pcm_to_ring(const int16_t *src, int src_frames, int src_rate, int src_ch) {
    if (src_frames <= 0 || src_rate <= 0 || src_ch <= 0) return;

    if (src_rate == AUDIO_RATE) {
        int i = 0;
        while (i < src_frames && g_dec_thread_running && !g_dec_cmd_open && !g_dec_cmd_seek) {
            uint32_t space = ring_available_to_write();
            if (space < 4) {
                usleep(2000);
                continue;
            }
            int chunk = src_frames - i;
            if ((uint32_t)chunk > space) chunk = (int)space;
            uint32_t w = atomic_load_explicit(&g_ring_write_idx, memory_order_relaxed);
            for (int k = 0; k < chunk; k++) {
                int16_t l, r;
                if (src_ch >= 2) {
                    l = src[(i + k) * src_ch];
                    r = src[(i + k) * src_ch + 1];
                } else {
                    l = r = src[i + k];
                }
                uint32_t slot = (w + (uint32_t)k) & PCM_RING_MASK;
                g_pcm_ring[slot * 2]     = l;
                g_pcm_ring[slot * 2 + 1] = r;
            }
            atomic_store_explicit(&g_ring_write_idx, w + (uint32_t)chunk, memory_order_release);
            i += chunk;
        }
        return;
    }

    /* Linear interpolation resampler across chunk boundaries */
    double step = (double)src_rate / (double)AUDIO_RATE;
    double pos  = g_resamp_phase;

    while (pos < (double)src_frames && g_dec_thread_running && !g_dec_cmd_open && !g_dec_cmd_seek) {
        if (ring_available_to_write() < 4) {
            usleep(2000);
            continue;
        }
        int idx0 = (int)floor(pos);
        double frac = pos - (double)idx0;
        int16_t l0, r0, l1, r1;

        if (idx0 < 0) {
            l0 = g_resamp_prev_l;
            r0 = g_resamp_prev_r;
        } else if (src_ch >= 2) {
            l0 = src[idx0 * src_ch];
            r0 = src[idx0 * src_ch + 1];
        } else {
            l0 = r0 = src[idx0];
        }

        int idx1 = idx0 + 1;
        if (idx1 >= src_frames) idx1 = src_frames - 1;
        if (src_ch >= 2) {
            l1 = src[idx1 * src_ch];
            r1 = src[idx1 * src_ch + 1];
        } else {
            l1 = r1 = src[idx1];
        }

        int16_t out_l = (int16_t)((double)l0 + ((double)l1 - (double)l0) * frac);
        int16_t out_r = (int16_t)((double)r0 + ((double)r1 - (double)r0) * frac);

        uint32_t w = atomic_load_explicit(&g_ring_write_idx, memory_order_relaxed);
        uint32_t slot = w & PCM_RING_MASK;
        g_pcm_ring[slot * 2]     = out_l;
        g_pcm_ring[slot * 2 + 1] = out_r;
        atomic_store_explicit(&g_ring_write_idx, w + 1, memory_order_release);

        pos += step;
    }

    if (src_ch >= 2) {
        g_resamp_prev_l = src[(src_frames - 1) * src_ch];
        g_resamp_prev_r = src[(src_frames - 1) * src_ch + 1];
    } else {
        g_resamp_prev_l = g_resamp_prev_r = src[src_frames - 1];
    }
    g_resamp_phase = pos - (double)src_frames;
}

/* Trim whitespace & non-ASCII safely into a metadata string */
static void store_meta_string(char *dst, size_t dst_sz, const char *src, size_t slen) {
    size_t o = 0;
    for (size_t i = 0; i < slen && o + 1 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\0') break;
        if (c >= 32 && c <= 126) {
            dst[o++] = (char)c;
        } else if (c == '\t') {
            dst[o++] = ' ';
        }
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
}

/* Lightweight pure-C ID3v2 (TIT2/TPE1) + ID3v1 tag reader for MP3 files */
static void read_mp3_id3_tags(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;

    char title[128]  = "";
    char artist[128] = "";

    /* 1. Try ID3v2 header at start of file */
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        uint8_t ver = hdr[3];
        uint32_t tag_sz = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                          ((uint32_t)(hdr[7] & 0x7F) << 14) |
                          ((uint32_t)(hdr[8] & 0x7F) << 7)  |
                          ((uint32_t)(hdr[9] & 0x7F));
        if (tag_sz > 0 && tag_sz < 512 * 1024 && (ver == 3 || ver == 4)) {
            uint8_t *buf = (uint8_t *)malloc(tag_sz);
            if (buf) {
                size_t got = fread(buf, 1, tag_sz, f);
                size_t pos = 0;
                while (pos + 10 <= got) {
                    if (buf[pos] == 0) break;
                    uint32_t fsz;
                    if (ver == 4) {
                        fsz = ((uint32_t)(buf[pos + 4] & 0x7F) << 21) |
                              ((uint32_t)(buf[pos + 5] & 0x7F) << 14) |
                              ((uint32_t)(buf[pos + 6] & 0x7F) << 7)  |
                              ((uint32_t)(buf[pos + 7] & 0x7F));
                    } else {
                        fsz = ((uint32_t)buf[pos + 4] << 24) |
                              ((uint32_t)buf[pos + 5] << 16) |
                              ((uint32_t)buf[pos + 6] << 8)  |
                              ((uint32_t)buf[pos + 7]);
                    }
                    if (fsz == 0 || pos + 10 + fsz > got) break;
                    bool is_tit2 = (memcmp(buf + pos, "TIT2", 4) == 0);
                    bool is_tpe1 = (memcmp(buf + pos, "TPE1", 4) == 0);
                    if ((is_tit2 || is_tpe1) && fsz > 1) {
                        uint8_t enc = buf[pos + 10];
                        const uint8_t *txt = buf + pos + 11;
                        size_t tlen = fsz - 1;
                        char tmp[128] = "";
                        if (enc == 0 || enc == 3) {
                            store_meta_string(tmp, sizeof(tmp), (const char *)txt, tlen);
                        } else if ((enc == 1 || enc == 2) && tlen >= 2) {
                            /* Basic ASCII extraction from UTF-16 */
                            size_t ti = 0, o = 0;
                            bool le = true;
                            if (enc == 1 && txt[0] == 0xFE && txt[1] == 0xFF) { le = false; ti = 2; }
                            else if (enc == 1 && txt[0] == 0xFF && txt[1] == 0xFE) { le = true; ti = 2; }
                            else if (enc == 2) { le = false; }
                            while (ti + 1 < tlen && o + 1 < sizeof(tmp)) {
                                uint16_t u = le ? (uint16_t)(txt[ti] | (txt[ti + 1] << 8))
                                                : (uint16_t)((txt[ti] << 8) | txt[ti + 1]);
                                if (u == 0) break;
                                if (u >= 32 && u <= 126) tmp[o++] = (char)u;
                                ti += 2;
                            }
                            tmp[o] = '\0';
                        }
                        if (is_tit2 && title[0] == '\0')  snprintf(title, sizeof(title), "%s", tmp);
                        if (is_tpe1 && artist[0] == '\0') snprintf(artist, sizeof(artist), "%s", tmp);
                    }
                    pos += 10 + fsz;
                }
                free(buf);
            }
        }
    }

    /* 2. Fallback to ID3v1 at end of file */
    if (title[0] == '\0' || artist[0] == '\0') {
        if (fseek(f, -128, SEEK_END) == 0) {
            uint8_t v1[128];
            if (fread(v1, 1, 128, f) == 128 && memcmp(v1, "TAG", 3) == 0) {
                if (title[0] == '\0')  store_meta_string(title, sizeof(title), (const char *)(v1 + 3), 30);
                if (artist[0] == '\0') store_meta_string(artist, sizeof(artist), (const char *)(v1 + 33), 30);
            }
        }
    }

    fclose(f);

    pthread_mutex_lock(&g_meta_mutex);
    if (title[0] != '\0')  snprintf(g_meta_title, sizeof(g_meta_title), "%s", title);
    if (artist[0] != '\0') snprintf(g_meta_artist, sizeof(g_meta_artist), "%s", artist);
    pthread_mutex_unlock(&g_meta_mutex);
}

/* FLAC Vorbis comment metadata callback */
static void flac_meta_cb(void *pUserData, drflac_metadata *pMetadata) {
    (void)pUserData;
    if (pMetadata->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
        drflac_vorbis_comment_iterator iter;
        drflac_init_vorbis_comment_iterator(&iter,
                                            pMetadata->data.vorbis_comment.commentCount,
                                            pMetadata->data.vorbis_comment.pComments);
        drflac_uint32 clen = 0;
        const char *cstr = NULL;
        pthread_mutex_lock(&g_meta_mutex);
        while ((cstr = drflac_next_vorbis_comment(&iter, &clen)) != NULL) {
            if (clen > 6 && strncasecmp(cstr, "TITLE=", 6) == 0) {
                store_meta_string(g_meta_title, sizeof(g_meta_title), cstr + 6, clen - 6);
            } else if (clen > 7 && strncasecmp(cstr, "ARTIST=", 7) == 0) {
                store_meta_string(g_meta_artist, sizeof(g_meta_artist), cstr + 7, clen - 7);
            }
        }
        pthread_mutex_unlock(&g_meta_mutex);
    }
}

/* Simple built-in RIFF/WAVE PCM decoder */
typedef struct {
    FILE     *fp;
    int       sample_rate;
    int       channels;
    int       bits_per_sample;
    long      data_offset;
    uint32_t  data_bytes;
    uint64_t  total_frames;
    uint64_t  cur_frame;
} WavDecoder;

static bool wav_open(WavDecoder *w, const char *path) {
    memset(w, 0, sizeof(*w));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }
    bool have_fmt = false, have_data = false;
    uint8_t chdr[8];
    while (fread(chdr, 1, 8, f) == 8) {
        uint32_t csz = (uint32_t)chdr[4] | ((uint32_t)chdr[5] << 8) |
                       ((uint32_t)chdr[6] << 16) | ((uint32_t)chdr[7] << 24);
        if (memcmp(chdr, "fmt ", 4) == 0 && csz >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) break;
            uint16_t audio_fmt = (uint16_t)(fmt[0] | (fmt[1] << 8));
            w->channels        = (int)(fmt[2] | (fmt[3] << 8));
            w->sample_rate     = (int)((uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                                       ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24));
            w->bits_per_sample = (int)(fmt[14] | (fmt[15] << 8));
            if (csz > 16) fseek(f, (long)(csz - 16), SEEK_CUR);
            if (audio_fmt == 1 && (w->bits_per_sample == 8 || w->bits_per_sample == 16) &&
                (w->channels == 1 || w->channels == 2) && w->sample_rate > 0) {
                have_fmt = true;
            }
        } else if (memcmp(chdr, "data", 4) == 0) {
            w->data_offset = ftell(f);
            w->data_bytes  = csz;
            have_data      = true;
            break;
        } else {
            fseek(f, (long)(csz + (csz & 1u)), SEEK_CUR);
        }
    }
    if (!have_fmt || !have_data) {
        fclose(f);
        return false;
    }
    uint32_t frame_bytes = (uint32_t)(w->channels * (w->bits_per_sample / 8));
    w->total_frames = (frame_bytes > 0) ? (w->data_bytes / frame_bytes) : 0;
    w->fp = f;
    return true;
}

static void wav_seek(WavDecoder *w, uint64_t frame) {
    if (!w->fp) return;
    if (frame > w->total_frames) frame = w->total_frames;
    uint32_t frame_bytes = (uint32_t)(w->channels * (w->bits_per_sample / 8));
    fseek(w->fp, w->data_offset + (long)(frame * frame_bytes), SEEK_SET);
    w->cur_frame = frame;
}

static int wav_read_s16(WavDecoder *w, int16_t *out, int max_frames) {
    if (!w->fp || w->cur_frame >= w->total_frames) return 0;
    uint64_t rem = w->total_frames - w->cur_frame;
    int to_read = (rem < (uint64_t)max_frames) ? (int)rem : max_frames;
    if (w->bits_per_sample == 16) {
        size_t got = fread(out, (size_t)(w->channels * 2), (size_t)to_read, w->fp);
        w->cur_frame += got;
        return (int)got;
    } else {
        uint8_t tmp[2048 * 2];
        if (to_read > 2048) to_read = 2048;
        size_t got = fread(tmp, (size_t)w->channels, (size_t)to_read, w->fp);
        for (size_t i = 0; i < got * (size_t)w->channels; i++) {
            out[i] = (int16_t)(((int)tmp[i] - 128) << 8);
        }
        w->cur_frame += got;
        return (int)got;
    }
}

static void wav_close(WavDecoder *w) {
    if (w->fp) {
        fclose(w->fp);
        w->fp = NULL;
    }
}

/* Dedicated Background Audio Decoder Thread */
static void *decoder_thread_fn(void *arg) {
    (void)arg;
    drmp3       mp3;
    bool        mp3_open = false;
    drflac     *flac     = NULL;
    stb_vorbis *ogg      = NULL;
    WavDecoder  wav;
    bool        wav_is_open = false;

    AudioFormat active_fmt = FMT_NONE;
    bool        local_eof  = false;
    int16_t     decode_buf[2048 * 2];

    while (g_dec_thread_running) {
        bool do_open = false;
        char open_path[PATH_MAX];
        int64_t start_ms = 0;

        bool do_seek = false;
        int64_t seek_delta_ms = 0;

        pthread_mutex_lock(&g_dec_mutex);
        if (g_dec_cmd_open) {
            do_open = true;
            snprintf(open_path, sizeof(open_path), "%s", g_dec_req_path);
            start_ms = g_dec_req_start_ms;
            g_dec_cmd_open = false;
            g_dec_cmd_seek = false;
        } else if (g_dec_cmd_seek) {
            do_seek = true;
            seek_delta_ms = g_dec_req_seek_delta;
            g_dec_cmd_seek = false;
        }
        pthread_mutex_unlock(&g_dec_mutex);

        /* 1. Open New Track */
        if (do_open) {
            ring_clear();
            reset_resampler();
            local_eof = false;
            atomic_store(&g_track_eof, false);

            if (mp3_open)    { drmp3_uninit(&mp3); mp3_open = false; }
            if (flac)        { drflac_close(flac); flac = NULL; }
            if (ogg)         { stb_vorbis_close(ogg); ogg = NULL; }
            if (wav_is_open) { wav_close(&wav); wav_is_open = false; }

            pthread_mutex_lock(&g_meta_mutex);
            g_meta_title[0]  = '\0';
            g_meta_artist[0] = '\0';
            pthread_mutex_unlock(&g_meta_mutex);

            struct stat fst;
            uint64_t file_bytes = (stat(open_path, &fst) == 0) ? (uint64_t)fst.st_size : 0;

            active_fmt = detect_audio_format(open_path);
            atomic_store(&g_dec_fmt, active_fmt);
            bool opened_ok = false;
            int dur_ms = 0;
            int rate   = 44100;
            int ch     = 2;

            if (active_fmt == FMT_MP3) {
                if (drmp3_init_file(&mp3, open_path, NULL)) {
                    mp3_open = true;
                    rate = (int)mp3.sampleRate;
                    ch   = (int)mp3.channels;
                    drmp3_uint64 total_frames = drmp3_get_pcm_frame_count(&mp3);
                    if (rate > 0 && total_frames > 0) {
                        dur_ms = (int)((total_frames * 1000ULL) / (uint64_t)rate);
                    }
                    read_mp3_id3_tags(open_path);
                    if (start_ms > 0 && rate > 0) {
                        drmp3_uint64 target = ((uint64_t)start_ms * (uint64_t)rate) / 1000ULL;
                        if (total_frames > 0 && target >= total_frames) { target = 0; start_ms = 0; }
                        drmp3_seek_to_pcm_frame(&mp3, target);
                    }
                    opened_ok = true;
                }
            } else if (active_fmt == FMT_FLAC) {
                flac = drflac_open_file_with_metadata(open_path, flac_meta_cb, NULL, NULL);
                if (flac) {
                    rate = (int)flac->sampleRate;
                    ch   = (int)flac->channels;
                    if (rate > 0 && flac->totalPCMFrameCount > 0) {
                        dur_ms = (int)((flac->totalPCMFrameCount * 1000ULL) / (uint64_t)rate);
                    }
                    if (start_ms > 0 && rate > 0) {
                        drflac_uint64 target = ((uint64_t)start_ms * (uint64_t)rate) / 1000ULL;
                        if (flac->totalPCMFrameCount > 0 && target >= flac->totalPCMFrameCount) {
                            target = 0;
                            start_ms = 0;
                        }
                        drflac_seek_to_pcm_frame(flac, target);
                    }
                    opened_ok = true;
                }
            } else if (active_fmt == FMT_OGG) {
                int verr = 0;
                ogg = stb_vorbis_open_filename(open_path, &verr, NULL);
                if (ogg) {
                    stb_vorbis_info vi = stb_vorbis_get_info(ogg);
                    rate = (int)vi.sample_rate;
                    ch   = vi.channels;
                    stb_vorbis_int64 total_frames = stb_vorbis_stream_length_in_samples64(ogg);
                    if (rate > 0 && total_frames > 0) {
                        dur_ms = (int)(((uint64_t)total_frames * 1000ULL) / (uint64_t)rate);
                    }
                    stb_vorbis_comment vc = stb_vorbis_get_comment(ogg);
                    pthread_mutex_lock(&g_meta_mutex);
                    for (int i = 0; i < vc.comment_list_length; i++) {
                        const char *c = vc.comment_list[i];
                        if (!c) continue;
                        if (strncasecmp(c, "TITLE=", 6) == 0) {
                            store_meta_string(g_meta_title, sizeof(g_meta_title), c + 6, strlen(c + 6));
                        } else if (strncasecmp(c, "ARTIST=", 7) == 0) {
                            store_meta_string(g_meta_artist, sizeof(g_meta_artist), c + 7, strlen(c + 7));
                        }
                    }
                    pthread_mutex_unlock(&g_meta_mutex);

                    if (start_ms > 0 && rate > 0) {
                        uint64_t target = ((uint64_t)start_ms * (uint64_t)rate) / 1000ULL;
                        if (total_frames > 0 && target >= (uint64_t)total_frames) {
                            target = 0;
                            start_ms = 0;
                        }
                        stb_vorbis_seek(ogg, (unsigned int)target);
                    }
                    opened_ok = true;
                }
            } else if (active_fmt == FMT_WAV) {
                if (wav_open(&wav, open_path)) {
                    wav_is_open = true;
                    rate = wav.sample_rate;
                    ch   = wav.channels;
                    if (rate > 0 && wav.total_frames > 0) {
                        dur_ms = (int)((wav.total_frames * 1000ULL) / (uint64_t)rate);
                    }
                    if (start_ms > 0 && rate > 0) {
                        uint64_t target = ((uint64_t)start_ms * (uint64_t)rate) / 1000ULL;
                        if (target >= wav.total_frames) { target = 0; start_ms = 0; }
                        wav_seek(&wav, target);
                    }
                    opened_ok = true;
                }
            }

            int kbps = (dur_ms > 0 && file_bytes > 0) ? (int)((file_bytes * 8ULL) / (uint64_t)dur_ms) : 0;
            atomic_store(&g_dec_src_rate, rate);
            atomic_store(&g_dec_src_channels, ch);
            atomic_store(&g_dec_dur_ms, dur_ms);
            atomic_store(&g_dec_bitrate_kbps, kbps);

            atomic_store(&g_played_out_frames, ((uint64_t)start_ms * AUDIO_RATE) / 1000ULL);
            atomic_store(&g_dec_pos_ms, (int)start_ms);
            atomic_store(&g_track_loaded, opened_ok);
            continue;
        }

        /* 2. Handle Seek Request (L1 / R1 Tap or Hold) */
        if (do_seek && atomic_load(&g_track_loaded)) {
            int cur_ms = atomic_load(&g_dec_pos_ms);
            int dur_ms = atomic_load(&g_dec_dur_ms);
            int64_t target_ms = (int64_t)cur_ms + seek_delta_ms;
            if (target_ms < 0) target_ms = 0;
            if (dur_ms > 500 && target_ms > dur_ms - 500) target_ms = dur_ms - 500;

            ring_clear();
            reset_resampler();
            local_eof = false;
            atomic_store(&g_track_eof, false);

            int rate = atomic_load(&g_dec_src_rate);
            if (rate <= 0) rate = 44100;
            uint64_t target_frame = ((uint64_t)target_ms * (uint64_t)rate) / 1000ULL;

            if (active_fmt == FMT_MP3 && mp3_open) {
                drmp3_seek_to_pcm_frame(&mp3, target_frame);
            } else if (active_fmt == FMT_FLAC && flac) {
                drflac_seek_to_pcm_frame(flac, target_frame);
            } else if (active_fmt == FMT_OGG && ogg) {
                stb_vorbis_seek(ogg, (unsigned int)target_frame);
            } else if (active_fmt == FMT_WAV && wav_is_open) {
                wav_seek(&wav, target_frame);
            }

            atomic_store(&g_played_out_frames, ((uint64_t)target_ms * AUDIO_RATE) / 1000ULL);
            atomic_store(&g_dec_pos_ms, (int)target_ms);
            continue;
        }

        /* 3. Decode Next PCM Chunk into Ring Buffer */
        if (!atomic_load(&g_track_loaded) || local_eof) {
            if (local_eof && ring_available_to_read() == 0) {
                atomic_store(&g_track_eof, true);
            }
            usleep(10000);
            continue;
        }

        if (ring_available_to_write() < 4096) {
            usleep(4000);
            continue;
        }

        int src_rate = atomic_load(&g_dec_src_rate);
        int src_ch   = atomic_load(&g_dec_src_channels);
        if (src_ch < 1) src_ch = 2;
        if (src_ch > 2) src_ch = 2;

        if (active_fmt == FMT_MP3 && mp3_open) {
            drmp3_uint64 frames = drmp3_read_pcm_frames_s16(&mp3, 1024, decode_buf);
            if (frames > 0) {
                push_decoded_pcm_to_ring(decode_buf, (int)frames, (int)mp3.sampleRate, (int)mp3.channels);
            } else {
                local_eof = true;
            }
        } else if (active_fmt == FMT_FLAC && flac) {
            drflac_uint64 frames = drflac_read_pcm_frames_s16(flac, 1024, decode_buf);
            if (frames > 0) {
                push_decoded_pcm_to_ring(decode_buf, (int)frames, (int)flac->sampleRate, (int)flac->channels);
            } else {
                local_eof = true;
            }
        } else if (active_fmt == FMT_OGG && ogg) {
            int frames = stb_vorbis_get_samples_short_interleaved(ogg, 2, decode_buf, 1024 * 2);
            if (frames > 0) {
                push_decoded_pcm_to_ring(decode_buf, frames, src_rate, 2);
            } else {
                local_eof = true;
            }
        } else if (active_fmt == FMT_WAV && wav_is_open) {
            int frames = wav_read_s16(&wav, decode_buf, 1024);
            if (frames > 0) {
                push_decoded_pcm_to_ring(decode_buf, frames, wav.sample_rate, wav.channels);
            } else {
                local_eof = true;
            }
        } else {
            usleep(10000);
        }
    }

    if (mp3_open)    drmp3_uninit(&mp3);
    if (flac)        drflac_close(flac);
    if (ogg)         stb_vorbis_close(ogg);
    if (wav_is_open) wav_close(&wav);
    return NULL;
}

static void request_decoder_open(const char *path, int64_t start_ms, bool auto_play) {
    pthread_mutex_lock(&g_dec_mutex);
    snprintf(g_dec_req_path, sizeof(g_dec_req_path), "%s", path);
    g_dec_req_start_ms = start_ms;
    g_dec_cmd_open     = true;
    g_dec_cmd_seek     = false;
    ring_clear();
    atomic_store(&g_track_loaded, false);
    atomic_store(&g_track_eof, false);
    atomic_store(&g_dec_pos_ms, (int)start_ms);
    atomic_store(&g_playing, auto_play);
    pthread_mutex_unlock(&g_dec_mutex);
}

static void request_decoder_seek(int64_t delta_ms) {
    if (!atomic_load(&g_track_loaded)) return;
    pthread_mutex_lock(&g_dec_mutex);
    if (g_dec_cmd_seek) {
        g_dec_req_seek_delta += delta_ms;
    } else {
        g_dec_req_seek_delta = delta_ms;
        g_dec_cmd_seek = true;
    }
    pthread_mutex_unlock(&g_dec_mutex);
}

/* ============================================================================
 * 4. DIRECTORY BROWSER, PLAYLIST & PERSISTENT STATE (.musicplayer_rg35xxsp.cfg)
 * ============================================================================ */

#define MAX_PLAYLIST_FILES 256
#define MAX_BROWSER_ITEMS  256
#define MAX_FILENAME_LEN   260

typedef enum {
    VIEW_BROWSER = 0,
    VIEW_PLAYER  = 1
} AppView;

typedef enum {
    BITEM_RESUME   = 0,
    BITEM_PLAY_DIR = 1,
    BITEM_PARENT   = 2,
    BITEM_SUBDIR   = 3,
    BITEM_AUDIO    = 4
} BrowserItemType;

typedef struct {
    BrowserItemType type;
    char            name[MAX_FILENAME_LEN];
    char            full_path[PATH_MAX];
    int             track_count;
    AudioFormat     fmt;
} BrowserItem;

static char        g_playlist_dir[PATH_MAX] = "";
static int         g_playlist_count         = 0;
static char        g_playlist_files[MAX_PLAYLIST_FILES][MAX_FILENAME_LEN];
static int         g_playing_idx            = 0;
static int         g_cursor_idx             = 0;
static int         g_playlist_scroll        = 0;

static bool        g_has_saved_session      = false;
static char        g_saved_dir[PATH_MAX]    = "";
static int         g_saved_playing_idx      = 0;
static int         g_saved_cursor_idx       = 0;
static int         g_saved_pos_ms           = 0;
static int         g_saved_count            = 0;
static char        g_saved_files[MAX_PLAYLIST_FILES][MAX_FILENAME_LEN];

static char        g_browser_root[PATH_MAX] = "/userdata/music";
static char        g_browser_dir[PATH_MAX]  = "/userdata/music";
static BrowserItem g_browser_items[MAX_BROWSER_ITEMS];
static int         g_browser_count          = 0;
static int         g_browser_cursor         = 0;
static int         g_browser_scroll         = 0;

static AppView     g_view                   = VIEW_BROWSER;
static bool        g_show_help              = false;
static char        g_config_path[PATH_MAX]  = "";

static int cmp_filename_asc(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

static int cmp_browser_item(const void *a, const void *b) {
    const BrowserItem *ia = (const BrowserItem *)a;
    const BrowserItem *ib = (const BrowserItem *)b;
    if (ia->type != ib->type) return (int)ia->type - (int)ib->type;
    return strcasecmp(ia->name, ib->name);
}

static int count_audio_files_in_dir(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (detect_audio_format(de->d_name) != FMT_NONE) count++;
    }
    closedir(d);
    return count;
}

static void dismiss_resume_notice(void) {
    g_has_saved_session = false;
    if (g_browser_count > 0 && g_browser_items[0].type == BITEM_RESUME) {
        if (g_browser_count > 1) {
            memmove(&g_browser_items[0], &g_browser_items[1],
                    (size_t)(g_browser_count - 1) * sizeof(BrowserItem));
        }
        g_browser_count--;
        if (g_browser_cursor > 0) g_browser_cursor--;
        if (g_browser_cursor >= g_browser_count) {
            g_browser_cursor = (g_browser_count > 0) ? (g_browser_count - 1) : 0;
        }
    }
}

static void scan_browser_directory(const char *dir_path) {
    char resolved[PATH_MAX];
    if (realpath(dir_path, resolved) != NULL) {
        snprintf(g_browser_dir, sizeof(g_browser_dir), "%s", resolved);
    } else {
        snprintf(g_browser_dir, sizeof(g_browser_dir), "%s", dir_path);
    }
    size_t dlen = strlen(g_browser_dir);
    while (dlen > 1 && g_browser_dir[dlen - 1] == '/') g_browser_dir[--dlen] = '\0';

    g_browser_count = 0;

    if (g_has_saved_session && g_saved_count > 0 &&
        g_saved_playing_idx >= 0 && g_saved_playing_idx < g_saved_count) {
        BrowserItem *it = &g_browser_items[g_browser_count++];
        memset(it, 0, sizeof(*it));
        it->type = BITEM_RESUME;
        const char *slash = strrchr(g_saved_dir, '/');
        const char *folder_short = (slash && slash[1]) ? (slash + 1) : g_saved_dir;
        snprintf(it->name, sizeof(it->name), "%.96s / %.150s",
                 folder_short, g_saved_files[g_saved_playing_idx]);
        snprintf(it->full_path, sizeof(it->full_path), "%s", g_saved_dir);
        it->track_count = g_saved_pos_ms / 1000;
    }

    int direct_audio_count = count_audio_files_in_dir(g_browser_dir);
    if (direct_audio_count > 0 && g_browser_count < MAX_BROWSER_ITEMS) {
        BrowserItem *it = &g_browser_items[g_browser_count++];
        memset(it, 0, sizeof(*it));
        it->type = BITEM_PLAY_DIR;
        snprintf(it->name, sizeof(it->name), "Play Entire Directory (%d tracks)", direct_audio_count);
        snprintf(it->full_path, sizeof(it->full_path), "%s", g_browser_dir);
        it->track_count = direct_audio_count;
    }

    if (strcmp(g_browser_dir, "/") != 0 && g_browser_count < MAX_BROWSER_ITEMS) {
        BrowserItem *it = &g_browser_items[g_browser_count++];
        memset(it, 0, sizeof(*it));
        it->type = BITEM_PARENT;
        snprintf(it->name, sizeof(it->name), ".. (Parent Directory)");
        snprintf(it->full_path, sizeof(it->full_path), "%s", g_browser_dir);
        char *last_slash = strrchr(it->full_path, '/');
        if (last_slash && last_slash != it->full_path) {
            *last_slash = '\0';
        } else {
            snprintf(it->full_path, sizeof(it->full_path), "/");
        }
    }

    int sortable_start = g_browser_count;
    DIR *d = opendir(g_browser_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && g_browser_count < MAX_BROWSER_ITEMS) {
            if (de->d_name[0] == '.') continue;
            char full[PATH_MAX];
            if (strcmp(g_browser_dir, "/") == 0) {
                snprintf(full, sizeof(full), "/%.3500s", de->d_name);
            } else {
                snprintf(full, sizeof(full), "%.3500s/%.256s", g_browser_dir, de->d_name);
            }
            struct stat st;
            if (stat(full, &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                BrowserItem *it = &g_browser_items[g_browser_count++];
                memset(it, 0, sizeof(*it));
                it->type = BITEM_SUBDIR;
                snprintf(it->name, sizeof(it->name), "%.255s", de->d_name);
                snprintf(it->full_path, sizeof(it->full_path), "%s", full);
                it->track_count = count_audio_files_in_dir(full);
            } else if (S_ISREG(st.st_mode)) {
                AudioFormat fmt = detect_audio_format(de->d_name);
                if (fmt != FMT_NONE) {
                    BrowserItem *it = &g_browser_items[g_browser_count++];
                    memset(it, 0, sizeof(*it));
                    it->type = BITEM_AUDIO;
                    snprintf(it->name, sizeof(it->name), "%.255s", de->d_name);
                    snprintf(it->full_path, sizeof(it->full_path), "%s", full);
                    it->fmt = fmt;
                }
            }
        }
        closedir(d);
    }

    if (g_browser_count > sortable_start) {
        qsort(&g_browser_items[sortable_start],
              (size_t)(g_browser_count - sortable_start),
              sizeof(BrowserItem),
              cmp_browser_item);
    }

    g_browser_cursor = 0;
    g_browser_scroll = 0;
}

static void start_playlist_track(int track_idx, int64_t start_ms, bool auto_play) {
    if (g_playlist_count <= 0) return;
    if (track_idx < 0) track_idx = 0;
    if (track_idx >= g_playlist_count) track_idx = 0;

    g_playing_idx = track_idx;
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%.3500s/%.256s",
             g_playlist_dir, g_playlist_files[g_playing_idx]);
    request_decoder_open(full_path, start_ms, auto_play);
}

static bool load_directory_playlist(const char *dir_path, int start_track_idx, int64_t start_ms, bool auto_play) {
    DIR *d = opendir(dir_path);
    if (!d) return false;

    char temp_files[MAX_PLAYLIST_FILES][MAX_FILENAME_LEN];
    int count = 0;
    struct dirent *de;

    while ((de = readdir(d)) != NULL && count < MAX_PLAYLIST_FILES) {
        if (de->d_name[0] == '.') continue;
        if (detect_audio_format(de->d_name) != FMT_NONE) {
            snprintf(temp_files[count], MAX_FILENAME_LEN, "%.255s", de->d_name);
            count++;
        }
    }
    closedir(d);

    if (count == 0) return false;

    qsort(temp_files, (size_t)count, MAX_FILENAME_LEN, cmp_filename_asc);

    snprintf(g_playlist_dir, sizeof(g_playlist_dir), "%s", dir_path);
    g_playlist_count = count;
    memcpy(g_playlist_files, temp_files, sizeof(temp_files));

    if (start_track_idx < 0 || start_track_idx >= g_playlist_count) start_track_idx = 0;
    g_playing_idx     = start_track_idx;
    g_cursor_idx      = start_track_idx;
    g_playlist_scroll = 0;

    dismiss_resume_notice();
    start_playlist_track(g_playing_idx, start_ms, auto_play);
    return true;
}

static bool resume_saved_session(bool auto_play) {
    if (!g_has_saved_session || g_saved_count <= 0) return false;
    snprintf(g_playlist_dir, sizeof(g_playlist_dir), "%s", g_saved_dir);
    g_playlist_count = g_saved_count;
    memcpy(g_playlist_files, g_saved_files, sizeof(g_saved_files));

    g_playing_idx = (g_saved_playing_idx >= 0 && g_saved_playing_idx < g_playlist_count)
                    ? g_saved_playing_idx : 0;
    g_cursor_idx  = (g_saved_cursor_idx >= 0 && g_saved_cursor_idx < g_playlist_count)
                    ? g_saved_cursor_idx : g_playing_idx;

    dismiss_resume_notice();
    start_playlist_track(g_playing_idx, g_saved_pos_ms, auto_play);
    return true;
}

static void resolve_config_path(void) {
    if (g_config_path[0] != '\0') return;
    const char *env_cfg = getenv("MUSICPLAYER_CFG");
    if (env_cfg && env_cfg[0] != '\0') {
        snprintf(g_config_path, sizeof(g_config_path), "%s", env_cfg);
        return;
    }
    const char *dirs[] = {
        "/userdata/roms/ports/musicplayer",
        "/userdata/system/configs",
        "/userdata",
        NULL
    };
    struct stat st;
    for (int i = 0; dirs[i]; i++) {
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && access(dirs[i], W_OK) == 0) {
            snprintf(g_config_path, sizeof(g_config_path), "%s/.musicplayer_rg35xxsp.cfg", dirs[i]);
            return;
        }
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "%s/.musicplayer_rg35xxsp.cfg", home);
    } else {
        snprintf(g_config_path, sizeof(g_config_path), "./.musicplayer_rg35xxsp.cfg");
    }
}

static void save_state(void) {
    resolve_config_path();
    if (g_playlist_count <= 0) return;

    FILE *f = fopen(g_config_path, "w");
    if (!f) return;

    int pos_ms = atomic_load(&g_dec_pos_ms);
    fprintf(f, "active_dir=%s\n", g_playlist_dir);
    fprintf(f, "playing_idx=%d\n", g_playing_idx);
    fprintf(f, "cursor_idx=%d\n", g_cursor_idx);
    fprintf(f, "position_ms=%d\n", pos_ms);
    fprintf(f, "playlist_count=%d\n", g_playlist_count);
    for (int i = 0; i < g_playlist_count; i++) {
        fprintf(f, "file_%d=%s\n", i, g_playlist_files[i]);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void load_state(void) {
    resolve_config_path();
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;

    g_has_saved_session = false;
    g_saved_count = 0;
    char line[PATH_MAX + 64];

    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';

        if (strncmp(line, "active_dir=", 11) == 0) {
            snprintf(g_saved_dir, sizeof(g_saved_dir), "%.*s", (int)(sizeof(g_saved_dir) - 1), line + 11);
        } else if (strncmp(line, "playing_idx=", 12) == 0) {
            g_saved_playing_idx = atoi(line + 12);
        } else if (strncmp(line, "cursor_idx=", 11) == 0) {
            g_saved_cursor_idx = atoi(line + 11);
        } else if (strncmp(line, "position_ms=", 12) == 0) {
            g_saved_pos_ms = atoi(line + 12);
        } else if (strncmp(line, "playlist_count=", 15) == 0) {
            int c = atoi(line + 15);
            if (c > MAX_PLAYLIST_FILES) c = MAX_PLAYLIST_FILES;
            if (c < 0) c = 0;
            g_saved_count = c;
        } else if (strncmp(line, "file_", 5) == 0) {
            const char *eq = strchr(line + 5, '=');
            if (eq) {
                int idx = atoi(line + 5);
                if (idx >= 0 && idx < MAX_PLAYLIST_FILES) {
                    snprintf(g_saved_files[idx], MAX_FILENAME_LEN, "%.*s",
                             MAX_FILENAME_LEN - 1, eq + 1);
                }
            }
        }
    }
    fclose(f);

    if (g_saved_dir[0] != '\0' && g_saved_count > 0 &&
        g_saved_playing_idx >= 0 && g_saved_playing_idx < g_saved_count) {
        char check_path[PATH_MAX + MAX_FILENAME_LEN + 4];
        snprintf(check_path, sizeof(check_path), "%.3800s/%.256s",
                 g_saved_dir, g_saved_files[g_saved_playing_idx]);
        if (access(check_path, R_OK) == 0) {
            g_has_saved_session = true;
        }
    }
}

/* ============================================================================
 * 5. NATIVE 640x480 UI RENDERING (CLEAN, DISTRACTION-FREE, NO SPECTRUM ANALYZER)
 * ============================================================================ */

static void format_mm_ss(int ms, char *out, size_t out_sz) {
    if (ms < 0) ms = 0;
    int total_sec = ms / 1000;
    int mins = total_sec / 60;
    int secs = total_sec % 60;
    snprintf(out, out_sz, "%02d:%02d", mins, secs);
}

static void render_help_overlay(void) {
    fill_rect_alpha(0, 0, NATIVE_W, NATIVE_H, COL_BG, 224);

    int mx = 24, my = 14, mw = NATIVE_W - 48, mh = NATIVE_H - 28;
    fill_rect(mx, my, mw, mh, COL_PANEL);
    draw_border(mx, my, mw, mh, 2, COL_GOLD);
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, COL_BORDER);

    fill_rect(mx + 4, my + 4, mw - 8, 40, COL_HEADER);
    fill_rect(mx + 4, my + 44, mw - 8, 2, COL_BORDER);
    draw_pill(mx + 14, my + 10, 68, 26, RGB(37, 99, 235), RGB(147, 197, 253));
    draw_text_centered(FONT_UI_14, mx + 14, my + 10, 68, 26, "HELP", COL_WHITE);
    draw_text(FONT_UI_20, mx + 92, my + 11, "MUSIC PLAYER - QUICK GUIDE", COL_GOLD);

    fill_rect(mx + 4, my + 46, mw - 8, 24, COL_PANEL_ALT);
    fill_rect(mx + 4, my + 70, mw - 8, 1, COL_BORDER);
    draw_text_centered(FONT_UI_12, mx + 8, my + 46, mw - 16, 24,
                       "Folder-based MP3, FLAC, OGG & WAV player with auto-resume & filename order.",
                       COL_CYAN);

    static const struct {
        const char *btn;
        const char *desc;
        uint32_t    col;
    } rows[8] = {
        { "A Button",   "Play / Pause Song (or Play Highlighted Dir)", COL_GREEN  },
        { "B Button",   "Player: Play Highlighted | Browser: Up Dir",  COL_CYAN   },
        { "UP / DOWN",  "Move Highlight Cursor (Hold to Scroll Fast)", COL_WHITE  },
        { "L1 / R1",    "Seek -5s / +5s (Hold: Rewind / Fast-Forward)",COL_GOLD   },
        { "SELECT / X", "Switch Directory Browser <-> Player View",    COL_ORANGE },
        { "LEFT/RIGHT", "Browser: Up/Enter Dir | Player: Page +/-5",   COL_PURPLE },
        { "Y Button",   "Toggle This Help & Controls Guide",           COL_CYAN   },
        { "MENU",       "Save Playlist + Exact Position & Quit",       COL_ACCENT }
    };

    for (int i = 0; i < 8; i++) {
        int ry = my + 78 + i * 37;
        draw_pill(mx + 16, ry, 118, 28, COL_PANEL_ALT, rows[i].col);
        draw_text_centered(FONT_UI_14, mx + 16, ry, 118, 28, rows[i].btn, rows[i].col);
        draw_text_clipped(FONT_UI_16, mx + 146, ry + 4, mw - 164, rows[i].desc, COL_WHITE);
    }

    fill_rect(mx + 16, my + mh - 66, mw - 32, 24, COL_PANEL_ALT);
    draw_border(mx + 16, my + mh - 66, mw - 32, 24, 1, COL_BORDER);
    draw_text_centered(FONT_UI_12, mx + 16, my + mh - 66, mw - 32, 24,
                       "Tip: Select [RESUME] at top of Browser to continue your last track.",
                       COL_GOLD);

    fill_rect(mx + 4, my + mh - 34, mw - 8, 30, COL_HEADER);
    fill_rect(mx + 4, my + mh - 34, mw - 8, 1, COL_BORDER);
    draw_text_centered(FONT_UI_14, mx, my + mh - 33, mw, 28,
                       "Formats: MP3, FLAC Lossless, OGG Vorbis, WAV   |   [Y] or [B] Close",
                       COL_GOLD);
}

static void render_browser_view(void) {
    /* 1. Top Header Bar (y: 0..46) */
    fill_rect(0, 0, NATIVE_W, 46, COL_HEADER);
    fill_rect(0, 46, NATIVE_W, 2, COL_BORDER);

    draw_pill(10, 9, 152, 28, COL_CYAN, RGB(186, 230, 253));
    draw_text_centered(FONT_UI_14, 10, 9, 152, 28, "MUSIC BROWSER", COL_BG);

    draw_path_tail(FONT_UI_16, 174, 13, NATIVE_W - 274, g_browser_dir, COL_GOLD);

    draw_pill(NATIVE_W - 92, 9, 82, 28, COL_PANEL_ALT, COL_BORDER);
    draw_text_centered(FONT_UI_14, NATIVE_W - 92, 9, 82, 28, "Y: Help", COL_GOLD);

    /* 2. Directory List Card */
    bool track_active = atomic_load(&g_track_loaded) && (g_playlist_count > 0);
    int card_h       = track_active ? 314 : 382;
    int visible_rows = track_active ? 8 : 10;

    fill_rect(10, 54, NATIVE_W - 20, card_h, COL_PANEL);
    draw_border(10, 54, NATIVE_W - 20, card_h, 2, COL_BORDER);

    fill_rect(12, 56, NATIVE_W - 24, 28, COL_HEADER);
    draw_text(FONT_UI_12, 22, 62, "SELECT DIRECTORY TO PLAY (SORTED BY FILENAME)", COL_MUTED);
    char cnt_hdr[32];
    snprintf(cnt_hdr, sizeof(cnt_hdr), "%d items", g_browser_count);
    draw_text_right(FONT_UI_12, NATIVE_W - 22, 62, cnt_hdr, COL_CYAN);

    if (g_browser_cursor < g_browser_scroll) {
        g_browser_scroll = g_browser_cursor;
    } else if (g_browser_cursor >= g_browser_scroll + visible_rows) {
        g_browser_scroll = g_browser_cursor - visible_rows + 1;
    }

    int row_h = 34;
    for (int row = 0; row < visible_rows; row++) {
        int idx = g_browser_scroll + row;
        if (idx >= g_browser_count) break;
        const BrowserItem *it = &g_browser_items[idx];
        bool is_sel = (idx == g_browser_cursor);
        int ry = 88 + row * row_h;

        if (is_sel) {
            fill_rect(16, ry, NATIVE_W - 32, row_h - 3, COL_ROW_SEL);
            draw_border(16, ry, NATIVE_W - 32, row_h - 3, 2,
                        (it->type == BITEM_RESUME) ? COL_GOLD : COL_CYAN);
        } else if (row % 2 == 1) {
            fill_rect(16, ry, NATIVE_W - 32, row_h - 3, COL_PANEL_ALT);
        }

        if (it->type == BITEM_RESUME) {
            draw_pill(22, ry + 4, 86, 23, COL_GOLD, RGB(254, 240, 138));
            draw_text_centered(FONT_UI_12, 22, ry + 4, 86, 23, "RESUME", COL_BG);
            char pos_lbl[24];
            format_mm_ss(it->track_count * 1000, pos_lbl, sizeof(pos_lbl));
            draw_text_right(FONT_MONO_16, NATIVE_W - 26, ry + 6, pos_lbl, COL_WHITE);
            draw_text_clipped(FONT_UI_16, 118, ry + 6, NATIVE_W - 220, it->name, COL_GOLD);
        } else if (it->type == BITEM_PLAY_DIR) {
            draw_pill(22, ry + 4, 86, 23, COL_GREEN, RGB(134, 239, 172));
            draw_text_centered(FONT_UI_12, 22, ry + 4, 86, 23, "PLAY ALL", COL_BG);
            draw_text_clipped(FONT_UI_16, 118, ry + 6, NATIVE_W - 144, it->name, COL_GREEN);
        } else if (it->type == BITEM_PARENT) {
            draw_pill(22, ry + 4, 68, 23, COL_PANEL_ALT, COL_MUTED);
            draw_text_centered(FONT_UI_12, 22, ry + 4, 68, 23, "UP ..", COL_MUTED);
            draw_text_clipped(FONT_UI_16, 102, ry + 6, NATIVE_W - 130, it->name, COL_MUTED);
        } else if (it->type == BITEM_SUBDIR) {
            draw_pill(22, ry + 4, 68, 23, COL_PANEL_ALT, COL_CYAN);
            draw_text_centered(FONT_UI_12, 22, ry + 4, 68, 23, "DIR", COL_CYAN);
            char cnt_str[24];
            snprintf(cnt_str, sizeof(cnt_str), "%d trk", it->track_count);
            draw_text_right(FONT_UI_14, NATIVE_W - 26, ry + 7, cnt_str, COL_MUTED);
            draw_text_clipped(FONT_UI_16, 102, ry + 6, NATIVE_W - 200, it->name,
                              is_sel ? COL_WHITE : COL_CYAN);
        } else if (it->type == BITEM_AUDIO) {
            uint32_t fcol = format_badge_col(it->fmt);
            draw_pill(22, ry + 4, 68, 23, COL_PANEL_ALT, fcol);
            draw_text_centered(FONT_UI_12, 22, ry + 4, 68, 23, format_badge_str(it->fmt), fcol);
            draw_text_clipped(FONT_UI_16, 102, ry + 6, NATIVE_W - 130, it->name,
                              is_sel ? COL_WHITE : COL_MUTED);
        }
    }

    /* 3. Mini Now-Playing + Seek Bar if a Track is Active (y: 374..436) */
    if (track_active) {
        fill_rect(10, 374, NATIVE_W - 20, 62, COL_PANEL_ALT);
        draw_border(10, 374, NATIVE_W - 20, 62, 2, COL_GOLD);

        bool is_playing = atomic_load(&g_playing);
        draw_pill(18, 381, 108, 22, is_playing ? COL_GREEN : COL_ORANGE,
                  is_playing ? RGB(134, 239, 172) : RGB(253, 186, 116));
        draw_text_centered(FONT_UI_12, 18, 381, 108, 22,
                           is_playing ? "NOW PLAYING" : "PAUSED", COL_BG);

        int pos_ms = atomic_load(&g_dec_pos_ms);
        int dur_ms = atomic_load(&g_dec_dur_ms);
        char t_pos[16], t_dur[16], t_both[40];
        format_mm_ss(pos_ms, t_pos, sizeof(t_pos));
        format_mm_ss(dur_ms, t_dur, sizeof(t_dur));
        snprintf(t_both, sizeof(t_both), "%s / %s", t_pos, t_dur);
        draw_text_right(FONT_MONO_16, NATIVE_W - 22, 382, t_both, COL_GOLD);

        draw_text_clipped(FONT_UI_16, 136, 382, NATIVE_W - 296,
                          g_playlist_files[g_playing_idx], COL_WHITE);

        /* Seek bar inside Mini Now-Playing card */
        int bx = 18, by = 412, bw = NATIVE_W - 36, bh = 14;
        fill_rect(bx, by, bw, bh, COL_BG);
        draw_border(bx, by, bw, bh, 1, COL_BORDER);
        if (dur_ms > 0) {
            int fill_w = (int)(((int64_t)pos_ms * (bw - 4)) / dur_ms);
            if (fill_w < 0) fill_w = 0;
            if (fill_w > bw - 4) fill_w = bw - 4;
            if (fill_w > 0) fill_rect(bx + 2, by + 2, fill_w, bh - 4, COL_GOLD);
        }
    }

    /* 4. Bottom Controls Footer (y: 444..480) */
    fill_rect(0, 444, NATIVE_W, 36, COL_HEADER);
    fill_rect(0, 444, NATIVE_W, 2, COL_BORDER);
    draw_text_centered(FONT_UI_12, 0, 446, NATIVE_W, 34,
                       "[A] Play Dir   [B] Up Dir   [RGT] Open   [L1/R1] Seek   [SEL] Player   [MENU] Quit",
                       COL_WHITE);
}

static void render_player_view(void) {
    bool is_playing = atomic_load(&g_playing);
    AudioFormat fmt = (AudioFormat)atomic_load(&g_dec_fmt);
    int rate        = atomic_load(&g_dec_src_rate);
    int kbps        = atomic_load(&g_dec_bitrate_kbps);
    int pos_ms      = atomic_load(&g_dec_pos_ms);
    int dur_ms      = atomic_load(&g_dec_dur_ms);

    /* ================= 1. TOP HEADER BAR (y: 0..46) ================= */
    fill_rect(0, 0, NATIVE_W, 46, COL_HEADER);
    fill_rect(0, 46, NATIVE_W, 2, COL_BORDER);

    draw_pill(10, 9, 98, 28, is_playing ? COL_GREEN : COL_ORANGE,
              is_playing ? RGB(134, 239, 172) : RGB(253, 186, 116));
    draw_text_centered(FONT_UI_14, 10, 9, 98, 28,
                       is_playing ? "PLAYING" : "PAUSED", COL_BG);

    uint32_t fcol = format_badge_col(fmt);
    draw_pill(116, 9, 68, 28, COL_PANEL_ALT, fcol);
    draw_text_centered(FONT_UI_14, 116, 9, 68, 28, format_badge_str(fmt), fcol);

    char hdr_info[96];
    if (kbps > 0) {
        snprintf(hdr_info, sizeof(hdr_info), "%.1f kHz  |  %d kbps  |  Track %02d / %02d",
                 (double)rate / 1000.0, kbps,
                 (g_playlist_count > 0) ? (g_playing_idx + 1) : 0,
                 g_playlist_count);
    } else {
        snprintf(hdr_info, sizeof(hdr_info), "%.1f kHz  |  Track %02d / %02d",
                 (double)rate / 1000.0,
                 (g_playlist_count > 0) ? (g_playing_idx + 1) : 0,
                 g_playlist_count);
    }
    draw_text_clipped(FONT_UI_14, 196, 14, NATIVE_W - 296, hdr_info, COL_WHITE);

    draw_pill(NATIVE_W - 92, 9, 82, 28, COL_PANEL_ALT, COL_BORDER);
    draw_text_centered(FONT_UI_14, NATIVE_W - 92, 9, 82, 28, "Y: Help", COL_GOLD);

    /* ================= 2. NOW PLAYING + PROGRESS CARD (y: 54..202) ================= */
    fill_rect(10, 54, NATIVE_W - 20, 148, COL_PANEL);
    draw_border(10, 54, NATIVE_W - 20, 148, 2, COL_BORDER);

    const char *cur_filename = (g_playlist_count > 0)
                               ? g_playlist_files[g_playing_idx]
                               : "No Track Loaded";
    draw_text_clipped(FONT_TITLE_24, 24, 66, NATIVE_W - 48, cur_filename, COL_GOLD);

    char sub_line[512];
    pthread_mutex_lock(&g_meta_mutex);
    if (g_meta_title[0] != '\0' && g_meta_artist[0] != '\0') {
        snprintf(sub_line, sizeof(sub_line), "%.120s  -  %.120s", g_meta_artist, g_meta_title);
    } else if (g_meta_title[0] != '\0') {
        snprintf(sub_line, sizeof(sub_line), "%.240s", g_meta_title);
    } else {
        snprintf(sub_line, sizeof(sub_line), "Folder: %.240s", g_playlist_dir);
    }
    pthread_mutex_unlock(&g_meta_mutex);
    draw_text_clipped(FONT_UI_16, 24, 102, NATIVE_W - 48, sub_line, COL_CYAN);

    /* Seek Progress Bar (y: 136..156) */
    int bar_x = 24, bar_y = 136, bar_w = NATIVE_W - 48, bar_h = 18;
    fill_rect(bar_x, bar_y, bar_w, bar_h, COL_BG);
    draw_border(bar_x, bar_y, bar_w, bar_h, 2, COL_BORDER);

    int fill_w = 0;
    if (dur_ms > 0) {
        fill_w = (int)(((int64_t)pos_ms * (bar_w - 6)) / dur_ms);
        if (fill_w < 0) fill_w = 0;
        if (fill_w > bar_w - 6) fill_w = bar_w - 6;
    }
    if (fill_w > 0) {
        fill_rect(bar_x + 3, bar_y + 3, fill_w, bar_h - 6,
                  is_playing ? COL_GREEN : COL_GOLD);
    }
    int thumb_x = bar_x + 3 + fill_w;
    if (thumb_x > bar_x + bar_w - 6) thumb_x = bar_x + bar_w - 6;
    fill_rect(thumb_x - 2, bar_y + 1, 5, bar_h - 2, COL_WHITE);

    char pos_str[16], dur_str[16], time_label[48];
    format_mm_ss(pos_ms, pos_str, sizeof(pos_str));
    format_mm_ss(dur_ms, dur_str, sizeof(dur_str));
    snprintf(time_label, sizeof(time_label), "%s / %s", pos_str, dur_str);
    draw_text(FONT_MONO_16, 24, 166, time_label, COL_WHITE);
    draw_text_right(FONT_UI_14, NATIVE_W - 24, 167,
                    "[L1/R1] Seek -/+5s  (Hold: Rew / FFW)", COL_MUTED);

    /* ================= 3. INTERACTIVE PLAYLIST CARD (y: 210..436) ================= */
    fill_rect(10, 210, NATIVE_W - 20, 226, COL_PANEL);
    draw_border(10, 210, NATIVE_W - 20, 226, 2, COL_BORDER);

    fill_rect(12, 212, NATIVE_W - 24, 28, COL_HEADER);
    draw_text(FONT_UI_12, 22, 218, "DIR:", COL_GOLD);
    draw_path_tail(FONT_UI_12, 58, 218, 340, g_playlist_dir, COL_GOLD);
    draw_text_right(FONT_UI_12, NATIVE_W - 22, 218, "[UP/DN] Select   [B] Play Highlighted", COL_CYAN);

    int visible_rows = 6;
    if (g_cursor_idx < g_playlist_scroll) {
        g_playlist_scroll = g_cursor_idx;
    } else if (g_cursor_idx >= g_playlist_scroll + visible_rows) {
        g_playlist_scroll = g_cursor_idx - visible_rows + 1;
    }

    int row_h = 32;
    for (int row = 0; row < visible_rows; row++) {
        int idx = g_playlist_scroll + row;
        if (idx >= g_playlist_count) break;
        bool is_cursor  = (idx == g_cursor_idx);
        bool is_current = (idx == g_playing_idx);
        int ry = 243 + row * row_h;

        if (is_cursor) {
            fill_rect(16, ry, NATIVE_W - 32, row_h - 3, COL_ROW_SEL);
            draw_border(16, ry, NATIVE_W - 32, row_h - 3, 2, is_current ? COL_GREEN : COL_GOLD);
        } else if (row % 2 == 1) {
            fill_rect(16, ry, NATIVE_W - 32, row_h - 3, COL_PANEL_ALT);
        }

        if (is_current) {
            draw_pill(22, ry + 3, 56, 23, is_playing ? COL_GREEN : COL_ORANGE,
                      is_playing ? RGB(134, 239, 172) : RGB(253, 186, 116));
            draw_text_centered(FONT_UI_12, 22, ry + 3, 56, 23,
                               is_playing ? "PLAY" : "PAUS", COL_BG);
        } else if (is_cursor) {
            draw_pill(22, ry + 3, 56, 23, COL_GOLD, RGB(254, 240, 138));
            draw_text_centered(FONT_UI_12, 22, ry + 3, 56, 23, "[B]", COL_BG);
        } else {
            char num_str[16];
            snprintf(num_str, sizeof(num_str), "%02d.", idx + 1);
            draw_text(FONT_UI_14, 34, ry + 6, num_str, COL_MUTED);
        }

        AudioFormat tfmt = detect_audio_format(g_playlist_files[idx]);
        uint32_t tfcol = format_badge_col(tfmt);
        draw_pill(NATIVE_W - 76, ry + 4, 52, 21, COL_PANEL_ALT, tfcol);
        draw_text_centered(FONT_UI_12, NATIVE_W - 76, ry + 4, 52, 21, format_badge_str(tfmt), tfcol);

        uint32_t name_col = is_current ? COL_GREEN : (is_cursor ? COL_GOLD : COL_WHITE);
        draw_text_clipped(FONT_UI_16, 88, ry + 5, NATIVE_W - 178,
                          g_playlist_files[idx], name_col);
    }

    /* ================= 4. BOTTOM CONTROLS FOOTER (y: 444..480) ================= */
    fill_rect(0, 444, NATIVE_W, 36, COL_HEADER);
    fill_rect(0, 444, NATIVE_W, 2, COL_BORDER);

    draw_pill(10, 450, 96, 24, is_playing ? COL_ORANGE : COL_GREEN,
              is_playing ? RGB(253, 186, 116) : RGB(134, 239, 172));
    draw_text_centered(FONT_UI_12, 10, 450, 96, 24,
                       is_playing ? "[A] PAUSE" : "[A] PLAY", COL_BG);

    draw_text(FONT_UI_12, 116, 455,
              "[B] Play Sel   [UP/DN] List   [L1/R1] Seek   [SEL] Dirs   [MENU] Save & Quit",
              COL_WHITE);
}

static void render_ui(void) {
    fill_rect(0, 0, NATIVE_W, NATIVE_H, COL_BG);
    if (g_view == VIEW_BROWSER) {
        render_browser_view();
    } else {
        render_player_view();
    }
    if (g_show_help) {
        render_help_overlay();
    }
}

/* ============================================================================
 * 6. AUTOMATED VERIFICATION (--verify) & HEADLESS SCREENSHOTS (--screenshot)
 * ============================================================================ */

static int run_verification_suite(const char *test_dir) {
    printf("Verifying static dr_mp3, dr_flac, and stb_vorbis decoders in %s...\n", test_dir);

    if (!load_directory_playlist(test_dir, 0, 0, false)) {
        fprintf(stderr, "FAIL: Could not load audio files from %s\n", test_dir);
        return 1;
    }

    for (int i = 1; i < g_playlist_count; i++) {
        if (strcasecmp(g_playlist_files[i - 1], g_playlist_files[i]) > 0) {
            fprintf(stderr, "FAIL: Playlist not sorted in filename order: %s vs %s\n",
                    g_playlist_files[i - 1], g_playlist_files[i]);
            return 1;
        }
    }
    printf("[OK] Playlist sorted in filename order (%d tracks)\n", g_playlist_count);

    g_dec_thread_running = true;
    pthread_create(&g_dec_thread, NULL, decoder_thread_fn, NULL);

    int mp3_ok = 0, flac_ok = 0, ogg_ok = 0;

    for (int i = 0; i < g_playlist_count; i++) {
        start_playlist_track(i, 0, true);
        int waited_ms = 0;
        while (waited_ms < 2500 && (!atomic_load(&g_track_loaded) || ring_available_to_read() < 2048)) {
            usleep(10000);
            waited_ms += 10;
        }
        uint32_t decoded_frames = ring_available_to_read();
        if (!atomic_load(&g_track_loaded) || decoded_frames == 0) {
            fprintf(stderr, "FAIL: Track %d (%s) failed to decode PCM frames\n",
                    i, g_playlist_files[i]);
            g_dec_thread_running = false;
            pthread_join(g_dec_thread, NULL);
            return 1;
        }

        /* Verify seeking forward +1000 ms */
        request_decoder_seek(1000);
        int swait = 0;
        while (swait < 1000 && (g_dec_cmd_seek || ring_available_to_read() < 1024)) {
            usleep(10000);
            swait += 10;
        }
        int seek_pos = atomic_load(&g_dec_pos_ms);
        AudioFormat fmt = (AudioFormat)atomic_load(&g_dec_fmt);
        if (fmt == FMT_MP3)  mp3_ok++;
        if (fmt == FMT_FLAC) flac_ok++;
        if (fmt == FMT_OGG)  ogg_ok++;

        printf("[OK] Verified %s (%s, %d Hz, %d kbps, dur=%d ms, decoded=%u frames, seek_pos=%d ms)\n",
               g_playlist_files[i],
               format_badge_str(fmt),
               atomic_load(&g_dec_src_rate),
               atomic_load(&g_dec_bitrate_kbps),
               atomic_load(&g_dec_dur_ms),
               decoded_frames,
               seek_pos);
    }

    /* Verify state persistence round-trip */
    snprintf(g_config_path, sizeof(g_config_path), "/tmp/.musicplayer_verify_test.cfg");
    g_playing_idx = (g_playlist_count > 1) ? 1 : 0;
    g_cursor_idx  = (g_playlist_count > 2) ? 2 : 0;
    atomic_store(&g_dec_pos_ms, 12345);
    save_state();

    g_has_saved_session = false;
    g_saved_count = 0;
    load_state();
    unlink(g_config_path);

    if (!g_has_saved_session || g_saved_count != g_playlist_count ||
        g_saved_playing_idx != g_playing_idx || g_saved_pos_ms != 12345) {
        fprintf(stderr, "FAIL: State persistence round-trip failed (saved=%d, cnt=%d, idx=%d, pos=%d)\n",
                g_has_saved_session, g_saved_count, g_saved_playing_idx, g_saved_pos_ms);
        g_dec_thread_running = false;
        pthread_join(g_dec_thread, NULL);
        return 1;
    }
    printf("[OK] State persistence verified (dir=%s, tracks=%d, idx=%d, pos_ms=%d)\n",
           g_saved_dir, g_saved_count, g_saved_playing_idx, g_saved_pos_ms);
    printf("[OK] Format coverage: MP3=%d, FLAC=%d, OGG=%d\n", mp3_ok, flac_ok, ogg_ok);

    g_dec_thread_running = false;
    pthread_join(g_dec_thread, NULL);
    return (mp3_ok > 0 && flac_ok > 0 && ogg_ok > 0) ? 0 : 1;
}

static void wait_for_track_ready(void) {
    int waited = 0;
    while (waited < 1500 && !atomic_load(&g_track_loaded)) {
        usleep(10000);
        waited += 10;
    }
}

static int run_screenshots(const char *out_dir, const char *media_root) {
    char path[512];
    char blues_dir[PATH_MAX];
    snprintf(blues_dir, sizeof(blues_dir), "%.3800s/01_Blues_Guitar_Sessions", media_root);

    g_dec_thread_running = true;
    pthread_create(&g_dec_thread, NULL, decoder_thread_fn, NULL);

    /* 1. Browser View with [RESUME] highlighted at top */
    if (load_directory_playlist(blues_dir, 1, 14000, false)) {
        wait_for_track_ready();
        snprintf(g_saved_dir, sizeof(g_saved_dir), "%s", g_playlist_dir);
        g_saved_count = g_playlist_count;
        memcpy(g_saved_files, g_playlist_files, sizeof(g_saved_files));
        g_saved_playing_idx = 1;
        g_saved_cursor_idx  = 1;
        g_saved_pos_ms      = 14000;
        g_has_saved_session = true;
    }
    atomic_store(&g_track_loaded, false);
    g_playlist_count = 0;
    scan_browser_directory(media_root);
    g_view      = VIEW_BROWSER;
    g_show_help = false;
    render_ui();
    snprintf(path, sizeof(path), "%s/musicplayer_browser.bmp", out_dir);
    save_bmp(path);

    /* 2. Now Playing (FLAC) & Playlist with cursor highlighted on track 3 */
    if (load_directory_playlist(blues_dir, 1, 1200, true)) {
        wait_for_track_ready();
        int dur = atomic_load(&g_dec_dur_ms);
        if (dur > 0) atomic_store(&g_dec_pos_ms, (dur * 42) / 100);
        g_cursor_idx = (g_playlist_count > 3) ? 3 : 0;
        g_view       = VIEW_PLAYER;
        g_show_help  = false;
        render_ui();
        snprintf(path, sizeof(path), "%s/musicplayer_playing.bmp", out_dir);
        save_bmp(path);
    }

    /* 3. OGG Vorbis Playback View */
    if (g_playlist_count > 2) {
        start_playlist_track(2, 1000, true);
        wait_for_track_ready();
        int dur = atomic_load(&g_dec_dur_ms);
        if (dur > 0) atomic_store(&g_dec_pos_ms, (dur * 64) / 100);
        g_cursor_idx = 2;
        g_view       = VIEW_PLAYER;
        g_show_help  = false;
        render_ui();
        snprintf(path, sizeof(path), "%s/musicplayer_ogg.bmp", out_dir);
        save_bmp(path);
    }

    /* 4. English Help Overlay */
    g_show_help = true;
    render_ui();
    snprintf(path, sizeof(path), "%s/musicplayer_help.bmp", out_dir);
    save_bmp(path);

    g_dec_thread_running = false;
    pthread_join(g_dec_thread, NULL);
    printf("[OK] Saved musicplayer_browser.bmp, musicplayer_playing.bmp, musicplayer_ogg.bmp, musicplayer_help.bmp to %s\n", out_dir);
    return 0;
}

/* ============================================================================
 * 7. MAIN ENTRY POINT & EVENT LOOP
 * ============================================================================ */

static void browser_go_parent(void) {
    for (int i = 0; i < g_browser_count; i++) {
        if (g_browser_items[i].type == BITEM_PARENT) {
            char parent_path[PATH_MAX];
            snprintf(parent_path, sizeof(parent_path), "%s", g_browser_items[i].full_path);
            scan_browser_directory(parent_path);
            return;
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    const char *cli_root_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            return run_verification_suite(argv[i + 1]);
        } else if ((strcmp(argv[i], "--screenshot") == 0 ||
                    strcmp(argv[i], "--screenshot-states") == 0) && i + 1 < argc) {
            const char *out_dir   = argv[i + 1];
            const char *media_dir = (i + 2 < argc && argv[i + 2][0] != '-') ? argv[i + 2] : "./sample_media";
            return run_screenshots(out_dir, media_dir);
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            snprintf(g_config_path, sizeof(g_config_path), "%s", argv[++i]);
        } else if (argv[i][0] != '-') {
            cli_root_dir = argv[i];
        }
    }

    load_state();

    struct stat st;
    if (cli_root_dir && stat(cli_root_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(g_browser_root, sizeof(g_browser_root), "%s", cli_root_dir);
    } else if (stat("/userdata/music", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(g_browser_root, sizeof(g_browser_root), "/userdata/music");
    } else if (stat("/userdata/media", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(g_browser_root, sizeof(g_browser_root), "/userdata/media");
    } else if (stat("./sample_media", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(g_browser_root, sizeof(g_browser_root), "./sample_media");
    } else if (stat("/userdata", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(g_browser_root, sizeof(g_browser_root), "/userdata");
    } else {
        snprintf(g_browser_root, sizeof(g_browser_root), ".");
    }

    scan_browser_directory(g_browser_root);

    g_dec_thread_running = true;
    pthread_create(&g_dec_thread, NULL, decoder_thread_fn, NULL);

    if (!platform_init("Anbernic RG35XXSP Music Player (MP3 / FLAC / OGG)")) {
        fprintf(stderr, "Failed to initialize platform backend\n");
        g_dec_thread_running = false;
        pthread_join(g_dec_thread, NULL);
        return 1;
    }

    uint32_t seek_hold_start_ms = 0, last_seek_repeat_ms = 0;
    uint32_t nav_hold_start_ms  = 0, last_nav_repeat_ms  = 0;

    input_state_t input;
    while (!g_sig_quit && platform_poll_input(&input)) {
        uint32_t now_ms = get_ticks_ms();
        uint32_t p = input.pressed;
        uint32_t h = input.held;

        /* Auto-advance to next song in filename order when current track ends */
        if (atomic_load(&g_playing) && atomic_load(&g_track_eof) && g_playlist_count > 0) {
            bool follow_cursor = (g_cursor_idx == g_playing_idx);
            int next_idx = (g_playing_idx + 1) % g_playlist_count;
            start_playlist_track(next_idx, 0, true);
            if (follow_cursor) g_cursor_idx = g_playing_idx;
        }

        /* MENU (Esc on desktop): SOLE quit key (saves playlist + exact position_ms) */
        if (p & BTN_MENU) {
            break;
        }

        /* Y: Toggle English Help Overlay */
        if (p & BTN_Y) {
            g_show_help = !g_show_help;
        } else if (g_show_help) {
            if (p & (BTN_B | BTN_A | BTN_SELECT)) {
                g_show_help = false;
            }
        } else {
            /* Global L1 / R1 Seek (works in BOTH Browser and Player views) */
            if (p & BTN_L1) {
                request_decoder_seek(-5000);
                seek_hold_start_ms  = now_ms;
                last_seek_repeat_ms = now_ms;
            } else if (p & BTN_R1) {
                request_decoder_seek(5000);
                seek_hold_start_ms  = now_ms;
                last_seek_repeat_ms = now_ms;
            }

            /* SELECT or X: Switch Directory Browser <-> Now Playing / Playlist View */
            if (p & (BTN_SELECT | BTN_X)) {
                if (g_view == VIEW_PLAYER) {
                    g_view = VIEW_BROWSER;
                } else if (g_playlist_count > 0) {
                    g_view = VIEW_PLAYER;
                }
            } else if (g_view == VIEW_BROWSER) {
                if (p & BTN_UP) {
                    if (g_browser_count > 0) {
                        g_browser_cursor = (g_browser_cursor + g_browser_count - 1) % g_browser_count;
                    }
                    nav_hold_start_ms  = now_ms;
                    last_nav_repeat_ms = now_ms;
                } else if (p & BTN_DOWN) {
                    if (g_browser_count > 0) {
                        g_browser_cursor = (g_browser_cursor + 1) % g_browser_count;
                    }
                    nav_hold_start_ms  = now_ms;
                    last_nav_repeat_ms = now_ms;
                } else if (p & (BTN_B | BTN_LEFT)) {
                    browser_go_parent();
                } else if (p & BTN_RIGHT) {
                    if (g_browser_count > 0) {
                        const BrowserItem *it = &g_browser_items[g_browser_cursor];
                        if (it->type == BITEM_SUBDIR) {
                            char sub_path[PATH_MAX];
                            snprintf(sub_path, sizeof(sub_path), "%s", it->full_path);
                            scan_browser_directory(sub_path);
                        }
                    }
                } else if (p & BTN_A) {
                    if (g_browser_count > 0) {
                        const BrowserItem *it = &g_browser_items[g_browser_cursor];
                        if (it->type == BITEM_RESUME) {
                            if (resume_saved_session(true)) g_view = VIEW_PLAYER;
                        } else if (it->type == BITEM_PLAY_DIR || it->type == BITEM_SUBDIR) {
                            if (load_directory_playlist(it->full_path, 0, 0, true)) {
                                g_view = VIEW_PLAYER;
                            } else if (it->type == BITEM_SUBDIR) {
                                char sub_path[PATH_MAX];
                                snprintf(sub_path, sizeof(sub_path), "%s", it->full_path);
                                scan_browser_directory(sub_path);
                            }
                        } else if (it->type == BITEM_PARENT) {
                            browser_go_parent();
                        } else if (it->type == BITEM_AUDIO) {
                            if (load_directory_playlist(g_browser_dir, 0, 0, true)) {
                                for (int i = 0; i < g_playlist_count; i++) {
                                    if (strcmp(g_playlist_files[i], it->name) == 0) {
                                        g_cursor_idx = i;
                                        start_playlist_track(i, 0, true);
                                        break;
                                    }
                                }
                                g_view = VIEW_PLAYER;
                            }
                        }
                    }
                }
            } else if (g_view == VIEW_PLAYER) {
                if (p & BTN_A) {
                    bool cur = atomic_load(&g_playing);
                    atomic_store(&g_playing, !cur);
                } else if (p & BTN_B) {
                    if (g_playlist_count > 0) {
                        start_playlist_track(g_cursor_idx, 0, true);
                    }
                } else if (p & BTN_UP) {
                    if (g_playlist_count > 0) {
                        g_cursor_idx = (g_cursor_idx + g_playlist_count - 1) % g_playlist_count;
                    }
                    nav_hold_start_ms  = now_ms;
                    last_nav_repeat_ms = now_ms;
                } else if (p & BTN_DOWN) {
                    if (g_playlist_count > 0) {
                        g_cursor_idx = (g_cursor_idx + 1) % g_playlist_count;
                    }
                    nav_hold_start_ms  = now_ms;
                    last_nav_repeat_ms = now_ms;
                } else if (p & BTN_LEFT) {
                    if (g_playlist_count > 0) {
                        g_cursor_idx -= 5;
                        if (g_cursor_idx < 0) g_cursor_idx = 0;
                    }
                } else if (p & BTN_RIGHT) {
                    if (g_playlist_count > 0) {
                        g_cursor_idx += 5;
                        if (g_cursor_idx >= g_playlist_count) g_cursor_idx = g_playlist_count - 1;
                    }
                }
            }

            /* Hold UP / DOWN for smooth rapid scrolling */
            bool up_held   = (h & BTN_UP) != 0;
            bool down_held = (h & BTN_DOWN) != 0;
            if ((up_held ^ down_held) && (now_ms - nav_hold_start_ms > 260)) {
                uint32_t rep = (now_ms - nav_hold_start_ms > 1100) ? 40 : 75;
                if (now_ms - last_nav_repeat_ms >= rep) {
                    if (g_view == VIEW_BROWSER && g_browser_count > 0) {
                        g_browser_cursor = up_held
                            ? (g_browser_cursor + g_browser_count - 1) % g_browser_count
                            : (g_browser_cursor + 1) % g_browser_count;
                    } else if (g_view == VIEW_PLAYER && g_playlist_count > 0) {
                        g_cursor_idx = up_held
                            ? (g_cursor_idx + g_playlist_count - 1) % g_playlist_count
                            : (g_cursor_idx + 1) % g_playlist_count;
                    }
                    last_nav_repeat_ms = now_ms;
                }
            }

            /* Hold L1 / R1 for accelerating Rewind / Fast-Forward in BOTH views */
            bool l1_held = (h & BTN_L1) != 0;
            bool r1_held = (h & BTN_R1) != 0;
            if ((l1_held ^ r1_held) && (now_ms - seek_hold_start_ms > 280)) {
                if (now_ms - last_seek_repeat_ms >= 90) {
                    int64_t step_ms = (now_ms - seek_hold_start_ms > 1500) ? 12000 : 4000;
                    request_decoder_seek(l1_held ? -step_ms : step_ms);
                    last_seek_repeat_ms = now_ms;
                }
            }
        }

        render_ui();
        platform_present();
        delay_ms(16);
    }

    save_state();
    platform_shutdown();
    g_dec_thread_running = false;
    pthread_join(g_dec_thread, NULL);
    return 0;
}
