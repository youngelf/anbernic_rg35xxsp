/*
 * metronome - Precision Metronome for Anbernic RG35XX SP (Knulli / Batocera) & Linux Host
 *
 * Target Hardware:
 *   Anbernic RG35XX SP (Allwinner H700, ARM Cortex-A53 aarch64, 640x480 IPS display)
 *   running Knulli (Scarab) / Batocera Linux, plus x86_64 Linux desktop X11 host.
 *
 * Controls on Anbernic RG35XX SP & Linux Host:
 *   A Button (or B / Space / 'A')   : Play / Pause metronome
 *   D-Pad UP / DOWN                 : Increase / Decrease Tempo (BPM)
 *   D-Pad LEFT / RIGHT              : Cycle Time Signature (4/4, 3/4, 2/4, 5/4, 7/4)
 *   L1 / R1 Shoulder Buttons        : Jump Tempo -5 / +5 BPM
 *   X / Y Buttons ('R' on Linux)    : Reset Tempo to 120 BPM
 *   MENU (M) / SELECT / START+SELECT: Save settings & Exit to Knulli
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

/* Virtual canvas is 320x240, scaled 2x to native 640x480 on RG35XX SP & Linux */
#define CANVAS_W 320
#define CANVAS_H 240
#define SCREEN_SCALE 2
#define NATIVE_W (CANVAS_W * SCREEN_SCALE) /* 640 */
#define NATIVE_H (CANVAS_H * SCREEN_SCALE) /* 480 */

#define SAMPLE_RATE 44100
#define AUDIO_CHANNELS 2

#define MIN_BPM 30
#define MAX_BPM 260
#define DEFAULT_BPM 120

/* Time Signatures: 4/4, 3/4, 2/4, 5/4, 7/4 */
#define NUM_TIME_SIGS 5
static const int k_time_sig_beats[NUM_TIME_SIGS] = { 4, 3, 2, 5, 7 };
static const char *k_time_sig_names[NUM_TIME_SIGS] = { "4/4", "3/4", "2/4", "5/4", "7/4" };

/* Logical input actions shared across RG35XX SP (SDL2 + evdev) and Linux X11 */
typedef enum {
    ACT_NONE = 0,
    ACT_PLAY_PAUSE,
    ACT_BPM_UP,
    ACT_BPM_DOWN,
    ACT_BPM_UP_FAST,
    ACT_BPM_DOWN_FAST,
    ACT_SIG_PREV,
    ACT_SIG_NEXT,
    ACT_RESET_BPM,
    ACT_QUIT
} InputAction;

static uint32_t get_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

static void delay_ms(uint32_t ms) {
    usleep(ms * 1000);
}

static inline uint32_t make_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (0xFF000000U) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* =========================================================================
 * Maximum-Loudness Click Synthesizer (Full-Scale +/-32767 Saturated Harmonic)
 * ========================================================================= */
static volatile int g_bpm = DEFAULT_BPM;
static volatile int g_time_sig_idx = 0;
static volatile bool g_playing = false;

static volatile int g_click_samples_left = 0;
static volatile bool g_click_is_accent = false;
static uint32_t g_click_sample_pos = 0;

#define ACCENT_CLICK_MS 75
#define NORMAL_CLICK_MS 55

static void trigger_click(bool accent) {
    g_click_is_accent = accent;
    g_click_sample_pos = 0;
    int duration_ms = accent ? ACCENT_CLICK_MS : NORMAL_CLICK_MS;
    g_click_samples_left = (SAMPLE_RATE * duration_ms) / 1000;
}

static void audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    int16_t *samples = (int16_t *)stream;
    int frames = len / (2 * AUDIO_CHANNELS);

    for (int i = 0; i < frames; i++) {
        int rem = g_click_samples_left;
        if (rem > 0) {
            bool accent = g_click_is_accent;
            uint32_t pos = g_click_sample_pos++;
            g_click_samples_left = rem - 1;

            int total_click_len = (SAMPLE_RATE * (accent ? ACCENT_CLICK_MS : NORMAL_CLICK_MS)) / 1000;
            double progress = (double)pos / (double)total_click_len;

            /* Sustained full-power envelope: hold at 100% full scale for the first
             * 70% (accent) or 65% (normal) of the click, then decay crisply */
            double sustain_frac = accent ? 0.70 : 0.65;
            double env = 1.0;
            if (progress > sustain_frac) {
                env = (1.0 - progress) / (1.0 - sustain_frac);
            }

            /* Frequencies tuned to handheld speaker resonant peak (1-3 kHz):
             * Beat 1 Accent: 1760 Hz (A6) + 880 Hz body + 2640 Hz harmonic
             * Normal Beat  : 1000 Hz + 2000 Hz octave + 3000 Hz edge */
            double freq = accent ? 1760.0 : 1000.0;
            if (pos < (SAMPLE_RATE * 5) / 1000) {
                freq *= 1.35;
            }

            double phase = (2.0 * 3.14159265358979323846 * freq * (double)pos) / (double)SAMPLE_RATE;

            /* High-RMS saturated square-harmonic wave (drives rail-to-rail +/-32767) */
            double raw = sin(phase) + 0.65 * sin(phase * 2.0) + 0.45 * sin(phase * 3.0);
            if (accent) {
                raw += 0.50 * sin(phase * 0.5);
            }

            /* Hard overdrive saturation (3.5x gain before clamp) for maximum RMS loudness */
            double driven = raw * 3.5;
            if (driven > 1.0)  driven = 1.0;
            if (driven < -1.0) driven = -1.0;

            int32_t val = (int32_t)(driven * 32767.0 * env);
            if (val > 32767)  val = 32767;
            if (val < -32767) val = -32767;

            int16_t s = (int16_t)val;
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                samples[i * AUDIO_CHANNELS + c] = s;
            }
        } else {
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                samples[i * AUDIO_CHANNELS + c] = 0;
            }
        }
    }
}

/* =========================================================================
 * Platform Backend:
 *   1. HOST_X11 : Linux Desktop (X11 640x480 window + PulseAudio dlopen)
 *   2. RG35XXSP : Knulli / Batocera aarch64 (SDL2 dlopen + /dev/fb0 fallback
 *                 + Linux evdev /dev/input/event* + ALSA fallback)
 * ========================================================================= */
#if defined(HOST_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

static Display *g_x11_dpy = NULL;
static Window g_x11_win = 0;
static GC g_x11_gc = 0;
static XImage *g_x11_img = NULL;
static Atom g_wm_delete_window = 0;
static char *g_scaled_buf = NULL;

static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;

typedef struct pa_simple pa_simple;
typedef struct {
    int format;
    uint32_t rate;
    uint8_t channels;
} pa_sample_spec;

#define PA_STREAM_PLAYBACK 1
#define PA_SAMPLE_S16LE    3

typedef pa_simple* (*pa_simple_new_fn)(const char*, const char*, int, const char*,
                                       const char*, const pa_sample_spec*,
                                       const void*, const void*, int*);
typedef int (*pa_simple_write_fn)(pa_simple*, const void*, size_t, int*);
typedef void (*pa_simple_free_fn)(pa_simple*);

static void *host_audio_worker(void *arg) {
    (void)arg;
    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    pa_simple *pa = NULL;
    pa_simple_new_fn fn_new = NULL;
    pa_simple_write_fn fn_write = NULL;
    pa_simple_free_fn fn_free = NULL;

    if (pa_lib) {
        fn_new = (pa_simple_new_fn)dlsym(pa_lib, "pa_simple_new");
        fn_write = (pa_simple_write_fn)dlsym(pa_lib, "pa_simple_write");
        fn_free = (pa_simple_free_fn)dlsym(pa_lib, "pa_simple_free");
        if (fn_new && fn_write && fn_free) {
            pa_sample_spec ss = { PA_SAMPLE_S16LE, SAMPLE_RATE, AUDIO_CHANNELS };
            int err = 0;
            pa = fn_new(NULL, "RG35XXSP Metronome", PA_STREAM_PLAYBACK, NULL, "Metronome Click", &ss, NULL, NULL, &err);
        }
    }

    int chunk_frames = 512;
    int chunk_bytes = chunk_frames * AUDIO_CHANNELS * 2;
    int16_t buf[512 * AUDIO_CHANNELS];

    while (g_audio_running) {
        audio_callback(NULL, (uint8_t *)buf, chunk_bytes);
        if (pa && fn_write) {
            int err = 0;
            fn_write(pa, buf, chunk_bytes, &err);
        } else {
            usleep((chunk_frames * 1000000ULL) / SAMPLE_RATE);
        }
    }

    if (pa && fn_free) fn_free(pa);
    if (pa_lib) dlclose(pa_lib);
    return NULL;
}

static bool platform_init(void) {
    g_x11_dpy = XOpenDisplay(NULL);
    if (!g_x11_dpy) {
        fprintf(stderr, "Cannot open X11 DISPLAY\n");
        return false;
    }
    int scr = DefaultScreen(g_x11_dpy);
    g_x11_win = XCreateSimpleWindow(
        g_x11_dpy, RootWindow(g_x11_dpy, scr),
        120, 120, NATIVE_W, NATIVE_H, 1,
        BlackPixel(g_x11_dpy, scr), BlackPixel(g_x11_dpy, scr)
    );

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = NATIVE_W;
    hints.min_height = hints.max_height = NATIVE_H;
    XSetWMNormalHints(g_x11_dpy, g_x11_win, &hints);

    XSelectInput(g_x11_dpy, g_x11_win, KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask);
    g_wm_delete_window = XInternAtom(g_x11_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_x11_dpy, g_x11_win, &g_wm_delete_window, 1);
    XStoreName(g_x11_dpy, g_x11_win, "RG35XX SP Metronome (640x480)");
    XMapWindow(g_x11_dpy, g_x11_win);
    g_x11_gc = DefaultGC(g_x11_dpy, scr);

    g_scaled_buf = (char *)calloc(NATIVE_W * NATIVE_H, 4);
    g_x11_img = XCreateImage(
        g_x11_dpy, DefaultVisual(g_x11_dpy, scr),
        DefaultDepth(g_x11_dpy, scr), ZPixmap, 0,
        g_scaled_buf, NATIVE_W, NATIVE_H, 32, 0
    );

    g_audio_running = true;
    pthread_create(&g_audio_thread, NULL, host_audio_worker, NULL);
    return true;
}

static void platform_present(const uint32_t *canvas) {
    uint32_t *dst = (uint32_t *)g_scaled_buf;
    for (int y = 0; y < CANVAS_H; y++) {
        for (int x = 0; x < CANVAS_W; x++) {
            uint32_t c = canvas[y * CANVAS_W + x];
            for (int sy = 0; sy < SCREEN_SCALE; sy++) {
                uint32_t *row = dst + (y * SCREEN_SCALE + sy) * NATIVE_W + (x * SCREEN_SCALE);
                for (int sx = 0; sx < SCREEN_SCALE; sx++) {
                    row[sx] = c;
                }
            }
        }
    }
    XPutImage(g_x11_dpy, g_x11_win, g_x11_gc, g_x11_img, 0, 0, 0, 0, NATIVE_W, NATIVE_H);
    XFlush(g_x11_dpy);
}

static InputAction platform_poll_input(void) {
    while (XPending(g_x11_dpy) > 0) {
        XEvent xev;
        XNextEvent(g_x11_dpy, &xev);
        if (xev.type == ClientMessage) {
            if ((Atom)xev.xclient.data.l[0] == g_wm_delete_window) {
                return ACT_QUIT;
            }
        } else if (xev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&xev.xkey, 0);
            if (ks >= XK_A && ks <= XK_Z) ks = ks - XK_A + XK_a;
            switch (ks) {
                case XK_a:
                case XK_b:
                case XK_space:
                case XK_Return:
                case XK_Alt_L:
                    return ACT_PLAY_PAUSE;
                case XK_Up:
                    return ACT_BPM_UP;
                case XK_Down:
                    return ACT_BPM_DOWN;
                case XK_Left:
                    return ACT_SIG_PREV;
                case XK_Right:
                    return ACT_SIG_NEXT;
                case XK_Page_Up:
                case XK_BackSpace:
                case XK_bracketright:
                    return ACT_BPM_UP_FAST;
                case XK_Page_Down:
                case XK_Tab:
                case XK_bracketleft:
                    return ACT_BPM_DOWN_FAST;
                case XK_r:
                case XK_x:
                case XK_y:
                case XK_Shift_L:
                    return ACT_RESET_BPM;
                case XK_q:
                case XK_m:
                case XK_Escape:
                    return ACT_QUIT;
                default:
                    break;
            }
        }
    }
    return ACT_NONE;
}

static void platform_shutdown(void) {
    if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
    if (g_x11_img) {
        XDestroyImage(g_x11_img);
        g_x11_img = NULL;
    }
    if (g_x11_dpy) {
        if (g_x11_win) XDestroyWindow(g_x11_dpy, g_x11_win);
        XCloseDisplay(g_x11_dpy);
        g_x11_dpy = NULL;
    }
}

#else /* RG35XX SP Target (Knulli / Batocera aarch64) */

#include <linux/fb.h>
#include <linux/input.h>
#include <sys/mman.h>

/* --- Dynamic SDL2 Definitions (loaded via dlopen from Knulli's libSDL2-2.0.so.0) --- */
#define SDL2_INIT_AUDIO          0x00000010U
#define SDL2_INIT_VIDEO          0x00000020U
#define SDL2_INIT_JOYSTICK       0x00000200U
#define SDL2_INIT_GAMECONTROLLER 0x00002000U

#define SDL2_WINDOWPOS_UNDEFINED 0x1FFF0000U
#define SDL2_WINDOW_SHOWN        0x00000004U
#define SDL2_WINDOW_FULLSCREEN_DESKTOP 0x00001001U
#define SDL2_RENDERER_ACCELERATED 0x00000002U
#define SDL2_RENDERER_PRESENTVSYNC 0x00000004U
#define SDL2_PIXELFORMAT_ARGB8888 0x16362004U
#define SDL2_TEXTUREACCESS_STREAMING 1
#define SDL2_AUDIO_S16LSB        0x8010

typedef void (*SDL2_AudioCallback)(void *userdata, uint8_t *stream, int len);

typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    SDL2_AudioCallback callback;
    void *userdata;
} SDL2_AudioSpec;

typedef union {
    uint32_t type;
    uint8_t padding[64];
} SDL2_Event;

#define SDL2_QUIT 0x100

static void *g_sdl2_lib = NULL;
static void *g_sdl2_win = NULL;
static void *g_sdl2_ren = NULL;
static void *g_sdl2_tex = NULL;
static bool  g_use_sdl2_video = false;
static bool  g_use_sdl2_audio = false;

static int  (*fn_SDL_Init)(uint32_t) = NULL;
static void (*fn_SDL_Quit)(void) = NULL;
static void*(*fn_SDL_CreateWindow)(const char*, int, int, int, int, uint32_t) = NULL;
static void (*fn_SDL_DestroyWindow)(void*) = NULL;
static void*(*fn_SDL_CreateRenderer)(void*, int, uint32_t) = NULL;
static void (*fn_SDL_DestroyRenderer)(void*) = NULL;
static void*(*fn_SDL_CreateTexture)(void*, uint32_t, int, int, int) = NULL;
static void (*fn_SDL_DestroyTexture)(void*) = NULL;
static int  (*fn_SDL_UpdateTexture)(void*, const void*, const void*, int) = NULL;
static int  (*fn_SDL_RenderClear)(void*) = NULL;
static int  (*fn_SDL_RenderCopy)(void*, void*, const void*, const void*) = NULL;
static void (*fn_SDL_RenderPresent)(void*) = NULL;
static int  (*fn_SDL_PollEvent)(SDL2_Event*) = NULL;
static int  (*fn_SDL_OpenAudio)(SDL2_AudioSpec*, SDL2_AudioSpec*) = NULL;
static void (*fn_SDL_PauseAudio)(int) = NULL;
static void (*fn_SDL_CloseAudio)(void) = NULL;
static int  (*fn_SDL_ShowCursor)(int) = NULL;

/* --- Fallback /dev/fb0 State --- */
static int g_fb_fd = -1;
static uint8_t *g_fb_mem = NULL;
static size_t g_fb_map_size = 0;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;
static uint32_t g_scaled_frame[NATIVE_W * NATIVE_H];

/* --- Direct Linux evdev (/dev/input/event*) Polling State --- */
#define MAX_EVDEV_FDS 32
static int g_evdev_fds[MAX_EVDEV_FDS];
static int g_num_evdev_fds = 0;
static bool g_btn_select_held = false;
static bool g_btn_start_held = false;
static int g_hat_x = 0;
static int g_hat_y = 0;

/* --- Fallback ALSA Thread (if SDL2 audio is unavailable) --- */
static pthread_t g_alsa_thread;
static volatile bool g_alsa_running = false;

typedef int (*snd_pcm_open_fn)(void**, const char*, int, int);
typedef int (*snd_pcm_set_params_fn)(void*, int, int, unsigned int, unsigned int, int, unsigned int);
typedef long (*snd_pcm_writei_fn)(void*, const void*, unsigned long);
typedef int (*snd_pcm_recover_fn)(void*, int, int);
typedef int (*snd_pcm_close_fn)(void*);

static void *alsa_audio_worker(void *arg) {
    (void)arg;
    void *asound = dlopen("libasound.so.2", RTLD_LAZY);
    if (!asound) return NULL;

    snd_pcm_open_fn fn_open = (snd_pcm_open_fn)dlsym(asound, "snd_pcm_open");
    snd_pcm_set_params_fn fn_params = (snd_pcm_set_params_fn)dlsym(asound, "snd_pcm_set_params");
    snd_pcm_writei_fn fn_writei = (snd_pcm_writei_fn)dlsym(asound, "snd_pcm_writei");
    snd_pcm_recover_fn fn_recover = (snd_pcm_recover_fn)dlsym(asound, "snd_pcm_recover");
    snd_pcm_close_fn fn_close = (snd_pcm_close_fn)dlsym(asound, "snd_pcm_close");

    void *pcm = NULL;
    if (!fn_open || !fn_params || !fn_writei || !fn_close) {
        dlclose(asound);
        return NULL;
    }

    /* SND_PCM_STREAM_PLAYBACK = 0, SND_PCM_FORMAT_S16_LE = 2, SND_PCM_ACCESS_RW_INTERLEAVED = 3 */
    if (fn_open(&pcm, "default", 0, 0) < 0) {
        if (fn_open(&pcm, "plughw:0,0", 0, 0) < 0) {
            dlclose(asound);
            return NULL;
        }
    }
    fn_params(pcm, 2, 3, AUDIO_CHANNELS, SAMPLE_RATE, 1, 25000);

    int chunk_frames = 512;
    int chunk_bytes = chunk_frames * AUDIO_CHANNELS * 2;
    int16_t buf[512 * AUDIO_CHANNELS];

    while (g_alsa_running) {
        audio_callback(NULL, (uint8_t *)buf, chunk_bytes);
        long written = fn_writei(pcm, buf, chunk_frames);
        if (written < 0 && fn_recover) {
            fn_recover(pcm, (int)written, 1);
        }
    }

    fn_close(pcm);
    dlclose(asound);
    return NULL;
}

static void open_evdev_devices(void) {
    g_num_evdev_fds = 0;
    for (int i = 0; i < MAX_EVDEV_FDS && g_num_evdev_fds < MAX_EVDEV_FDS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            g_evdev_fds[g_num_evdev_fds++] = fd;
        }
    }
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
    g_fb_map_size = g_finfo.smem_len ? g_finfo.smem_len : (g_vinfo.yres_virtual * g_finfo.line_length);
    g_fb_mem = (uint8_t *)mmap(NULL, g_fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        g_fb_mem = NULL;
        close(g_fb_fd);
        g_fb_fd = -1;
        return false;
    }
    return true;
}

static bool platform_init(void) {
    open_evdev_devices();

    g_sdl2_lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
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
                    "RG35XX SP Metronome",
                    SDL2_WINDOWPOS_UNDEFINED, SDL2_WINDOWPOS_UNDEFINED,
                    NATIVE_W, NATIVE_H,
                    SDL2_WINDOW_FULLSCREEN_DESKTOP | SDL2_WINDOW_SHOWN
                );
                if (g_sdl2_win) {
                    g_sdl2_ren = fn_SDL_CreateRenderer(
                        g_sdl2_win, -1,
                        SDL2_RENDERER_ACCELERATED | SDL2_RENDERER_PRESENTVSYNC
                    );
                    if (!g_sdl2_ren) {
                        g_sdl2_ren = fn_SDL_CreateRenderer(g_sdl2_win, -1, 0);
                    }
                    if (g_sdl2_ren) {
                        g_sdl2_tex = fn_SDL_CreateTexture(
                            g_sdl2_ren, SDL2_PIXELFORMAT_ARGB8888,
                            SDL2_TEXTUREACCESS_STREAMING,
                            NATIVE_W, NATIVE_H
                        );
                        if (g_sdl2_tex) {
                            g_use_sdl2_video = true;
                        }
                    }
                }

                if (fn_SDL_OpenAudio && fn_SDL_PauseAudio) {
                    SDL2_AudioSpec desired;
                    memset(&desired, 0, sizeof(desired));
                    desired.freq = SAMPLE_RATE;
                    desired.format = SDL2_AUDIO_S16LSB;
                    desired.channels = AUDIO_CHANNELS;
                    desired.samples = 512;
                    desired.callback = audio_callback;
                    if (fn_SDL_OpenAudio(&desired, NULL) == 0) {
                        fn_SDL_PauseAudio(0);
                        g_use_sdl2_audio = true;
                    }
                }
            }
        }
    }

    if (!g_use_sdl2_video) {
        if (!init_fbdev_fallback()) {
            fprintf(stderr, "Failed to initialize both SDL2 video and /dev/fb0\n");
            return false;
        }
    }

    if (!g_use_sdl2_audio) {
        g_alsa_running = true;
        pthread_create(&g_alsa_thread, NULL, alsa_audio_worker, NULL);
    }

    return true;
}

static void platform_present(const uint32_t *canvas) {
    /* Scale 320x240 canvas 2x to 640x480 */
    for (int y = 0; y < CANVAS_H; y++) {
        for (int x = 0; x < CANVAS_W; x++) {
            uint32_t c = canvas[y * CANVAS_W + x];
            for (int sy = 0; sy < SCREEN_SCALE; sy++) {
                uint32_t *row = g_scaled_frame + (y * SCREEN_SCALE + sy) * NATIVE_W + (x * SCREEN_SCALE);
                for (int sx = 0; sx < SCREEN_SCALE; sx++) {
                    row[sx] = c;
                }
            }
        }
    }

    if (g_use_sdl2_video) {
        fn_SDL_UpdateTexture(g_sdl2_tex, NULL, g_scaled_frame, NATIVE_W * 4);
        fn_SDL_RenderClear(g_sdl2_ren);
        fn_SDL_RenderCopy(g_sdl2_ren, g_sdl2_tex, NULL, NULL);
        fn_SDL_RenderPresent(g_sdl2_ren);
    } else if (g_fb_mem) {
        int fb_w = (int)g_vinfo.xres;
        int fb_h = (int)g_vinfo.yres;
        int bpp  = (int)g_vinfo.bits_per_pixel;
        int stride = (int)g_finfo.line_length;
        uint8_t *base = g_fb_mem + g_vinfo.yoffset * stride;

        int copy_w = (fb_w < NATIVE_W) ? fb_w : NATIVE_W;
        int copy_h = (fb_h < NATIVE_H) ? fb_h : NATIVE_H;

        for (int y = 0; y < copy_h; y++) {
            const uint32_t *src = g_scaled_frame + y * NATIVE_W;
            if (bpp == 32) {
                uint32_t *dst = (uint32_t *)(base + y * stride);
                memcpy(dst, src, copy_w * 4);
            } else if (bpp == 16) {
                uint16_t *dst = (uint16_t *)(base + y * stride);
                for (int x = 0; x < copy_w; x++) {
                    uint32_t c = src[x];
                    uint16_t r = (uint16_t)((c >> 19) & 0x1F);
                    uint16_t g = (uint16_t)((c >> 10) & 0x3F);
                    uint16_t b = (uint16_t)((c >> 3)  & 0x1F);
                    dst[x] = (uint16_t)((r << 11) | (g << 5) | b);
                }
            }
        }
        ioctl(g_fb_fd, FBIOPAN_DISPLAY, &g_vinfo);
    }
}

static InputAction platform_poll_input(void) {
    /* 1. Pump SDL2 event queue so window manager stays responsive */
    if (g_use_sdl2_video && fn_SDL_PollEvent) {
        SDL2_Event ev;
        while (fn_SDL_PollEvent(&ev)) {
            if (ev.type == SDL2_QUIT) {
                return ACT_QUIT;
            }
        }
    }

    /* 2. Poll Linux evdev (/dev/input/event*) directly for RG35XX SP controls */
    for (int i = 0; i < g_num_evdev_fds; i++) {
        struct input_event ie;
        while (read(g_evdev_fds[i], &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
            if (ie.type == EV_KEY) {
                bool pressed = (ie.value == 1); /* 1 = key press, 2 = autorepeat, 0 = release */
                if (ie.code == BTN_SELECT || ie.code == KEY_ESC) {
                    g_btn_select_held = (ie.value != 0);
                    if (pressed && g_btn_start_held) return ACT_QUIT;
                }
                if (ie.code == BTN_START || ie.code == KEY_ENTER) {
                    g_btn_start_held = (ie.value != 0);
                    if (pressed && g_btn_select_held) return ACT_QUIT;
                }

                if (!pressed) continue;

                switch (ie.code) {
                    /* A / B face buttons on RG35XX SP -> Play / Pause */
                    case 304:        /* BTN_SOUTH (Physical A) */
                    case 305:        /* BTN_EAST  (Physical B) */
                    case KEY_SPACE:
                    case KEY_LEFTALT:
                        return ACT_PLAY_PAUSE;

                    /* X / Y face buttons -> Reset BPM to 120 */
                    case 306:        /* BTN_C     (Physical X) */
                    case 307:        /* BTN_NORTH (Physical Y) */
                        return ACT_RESET_BPM;

                    /* D-Pad UP / DOWN -> Tempo +/- 1 BPM */
                    case BTN_DPAD_UP:
                    case KEY_UP:
                        return ACT_BPM_UP;
                    case BTN_DPAD_DOWN:
                    case KEY_DOWN:
                        return ACT_BPM_DOWN;

                    /* D-Pad LEFT / RIGHT -> Cycle Time Signature */
                    case BTN_DPAD_LEFT:
                    case KEY_LEFT:
                        return ACT_SIG_PREV;
                    case BTN_DPAD_RIGHT:
                    case KEY_RIGHT:
                        return ACT_SIG_NEXT;

                    /* Shoulder Buttons L1 / R1 -> Tempo -5 / +5 BPM */
                    case 308:        /* BTN_WEST (Physical L1 on H700) */
                    case KEY_TAB:
                        return ACT_BPM_DOWN_FAST;
                    case 309:        /* BTN_Z    (Physical R1 on H700) */
                    case 311:        /* BTN_TR   (R1 on standard gamepad) */
                    case KEY_BACKSPACE:
                        return ACT_BPM_UP_FAST;

                    /* Menu (M) button or SELECT -> Save & Quit */
                    case 310:        /* BTN_TL     (Physical SELECT on H700 ANBERNIC-keys) */
                    case 312:        /* BTN_TL2    (Physical MENU on H700 ANBERNIC-keys) */
                    case 313:        /* BTN_TR2 */
                    case 314:        /* BTN_SELECT (Select on Knulli/Batocera) */
                    case 316:        /* BTN_MODE   (Menu M button on Knulli/Batocera) */
                    case 294:
                    case 295:
                    case 296:
                    case 297:
                    case 298:
                    case 299:
                    case 704:        /* BTN_TRIGGER_HAPPY1 (Menu/Select on Batocera/Knulli) */
                    case 705:
                    case 706:
                    case 707:
                    case 708:
                    case 709:
                    case KEY_ESC:
                    case KEY_Q:
                    case KEY_M:
                    case KEY_F1:
                    case KEY_F4:
                    case KEY_MENU:
                    case KEY_HOMEPAGE:
                    case KEY_PROG1:
                    case KEY_PROG2:
                    case KEY_SELECT:
                    case KEY_BACK:
                        return ACT_QUIT;

                    default:
                        break;
                }
            } else if (ie.type == EV_ABS) {
                /* D-Pad reported via Hat axes (ABS_HAT0X / ABS_HAT0Y) or analog axes */
                if (ie.code == ABS_HAT0X) {
                    int prev = g_hat_x;
                    g_hat_x = ie.value;
                    if (g_hat_x < 0 && prev >= 0) return ACT_SIG_PREV;
                    if (g_hat_x > 0 && prev <= 0) return ACT_SIG_NEXT;
                } else if (ie.code == ABS_HAT0Y) {
                    int prev = g_hat_y;
                    g_hat_y = ie.value;
                    if (g_hat_y < 0 && prev >= 0) return ACT_BPM_UP;
                    if (g_hat_y > 0 && prev <= 0) return ACT_BPM_DOWN;
                }
            }
        }
    }
    return ACT_NONE;
}

static void platform_shutdown(void) {
    if (g_use_sdl2_audio && fn_SDL_CloseAudio) {
        fn_SDL_CloseAudio();
    }
    if (g_alsa_running) {
        g_alsa_running = false;
        pthread_join(g_alsa_thread, NULL);
    }
    if (g_use_sdl2_video) {
        if (g_sdl2_tex && fn_SDL_DestroyTexture) fn_SDL_DestroyTexture(g_sdl2_tex);
        if (g_sdl2_ren && fn_SDL_DestroyRenderer) fn_SDL_DestroyRenderer(g_sdl2_ren);
        if (g_sdl2_win && fn_SDL_DestroyWindow) fn_SDL_DestroyWindow(g_sdl2_win);
        if (fn_SDL_Quit) fn_SDL_Quit();
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
    for (int i = 0; i < g_num_evdev_fds; i++) {
        close(g_evdev_fds[i]);
    }
    g_num_evdev_fds = 0;
}

#endif /* HOST_X11 vs RG35XX SP */

/* =========================================================================
 * Built-in 5x7 ASCII Bitmap Font & Software Canvas Renderer (320x240 -> 640x480)
 * ========================================================================= */
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

static char g_config_path[512];

static const char *get_tempo_marking(int bpm) {
    if (bpm < 45)  return "GRAVE";
    if (bpm < 60)  return "LARGO";
    if (bpm < 66)  return "LARGHETTO";
    if (bpm < 76)  return "ADAGIO";
    if (bpm < 108) return "ANDANTE";
    if (bpm < 120) return "MODERATO";
    if (bpm < 156) return "ALLEGRO";
    if (bpm < 176) return "VIVACE";
    if (bpm < 200) return "PRESTO";
    return "PRESTISSIMO";
}

static void resolve_config_path(void) {
    struct stat st;
    if (stat("/userdata/system", &st) == 0 && S_ISDIR(st.st_mode) && access("/userdata/system", W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "/userdata/system/.metronome.cfg");
    } else {
        const char *home = getenv("HOME");
        if (home && access(home, W_OK) == 0) {
            snprintf(g_config_path, sizeof(g_config_path), "%s/.metronome.cfg", home);
        } else {
            snprintf(g_config_path, sizeof(g_config_path), "./.metronome.cfg");
        }
    }
}

static int parse_int(const char *s) {
    int val = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

static void load_config(void) {
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "bpm=", 4) == 0) {
            int val = parse_int(line + 4);
            if (val >= MIN_BPM && val <= MAX_BPM) g_bpm = val;
        } else if (strncmp(line, "time_sig_idx=", 13) == 0) {
            int val = parse_int(line + 13);
            if (val >= 0 && val < NUM_TIME_SIGS) g_time_sig_idx = val;
        }
    }
    fclose(f);
}

static void save_config(void) {
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "bpm=%d\n", g_bpm);
    fprintf(f, "time_sig_idx=%d\n", g_time_sig_idx);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* Canvas Drawing Helpers */
static void draw_rect(uint32_t *canvas, int x, int y, int w, int h, uint32_t color) {
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > CANVAS_W) ? CANVAS_W : (x + w);
    int y1 = (y + h > CANVAS_H) ? CANVAS_H : (y + h);
    for (int ry = y0; ry < y1; ry++) {
        uint32_t *row = canvas + ry * CANVAS_W;
        for (int rx = x0; rx < x1; rx++) {
            row[rx] = color;
        }
    }
}

static void draw_border(uint32_t *canvas, int x, int y, int w, int h, int thickness, uint32_t color) {
    draw_rect(canvas, x, y, w, thickness, color);
    draw_rect(canvas, x, y + h - thickness, w, thickness, color);
    draw_rect(canvas, x, y, thickness, h, color);
    draw_rect(canvas, x + w - thickness, y, thickness, h, color);
}

static void draw_char(uint32_t *canvas, int x, int y, char c, int scale, uint32_t color) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                draw_rect(canvas, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(uint32_t *canvas, int x, int y, const char *str, int scale, uint32_t color) {
    int cur_x = x;
    while (*str) {
        draw_char(canvas, cur_x, y, *str, scale, color);
        cur_x += 6 * scale;
        str++;
    }
}

static void draw_text_centered(uint32_t *canvas, int cx, int y, const char *str, int scale, uint32_t color) {
    int len = (int)strlen(str);
    if (len == 0) return;
    int width = len * 6 * scale - scale;
    draw_text(canvas, cx - width / 2, y, str, scale, color);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    resolve_config_path();
    load_config();

    if (!platform_init()) {
        return 1;
    }

    uint32_t *canvas = (uint32_t *)calloc(CANVAS_W * CANVAS_H, sizeof(uint32_t));

    /* Color Palette */
    uint32_t col_bg         = make_rgb(14, 18, 28);
    uint32_t col_panel      = make_rgb(26, 33, 50);
    uint32_t col_panel_dark = make_rgb(20, 25, 38);
    uint32_t col_border     = make_rgb(65, 82, 120);
    uint32_t col_white      = make_rgb(245, 247, 252);
    uint32_t col_muted      = make_rgb(145, 160, 190);
    uint32_t col_accent     = make_rgb(255, 70, 70);   /* Beat 1 Accent Red */
    uint32_t col_beat       = make_rgb(46, 213, 115);  /* Normal Beat Green */
    uint32_t col_gold       = make_rgb(255, 200, 60);  /* Time Signature Gold */
    uint32_t col_cyan       = make_rgb(70, 205, 255);  /* Tempo / UI Highlights */
    uint32_t col_box_idle   = make_rgb(36, 44, 66);

    bool running = true;
    int current_beat = 0; /* 0 .. beats_per_bar - 1 */
    double next_beat_time_ms = 0.0;
    uint32_t last_beat_trigger_ms = 0;

    while (running) {
        uint32_t now_ms = get_ticks_ms();

        InputAction act;
        while ((act = platform_poll_input()) != ACT_NONE) {
            switch (act) {
                case ACT_PLAY_PAUSE:
                    g_playing = !g_playing;
                    if (g_playing) {
                        current_beat = 0;
                        trigger_click(true);
                        last_beat_trigger_ms = now_ms;
                        next_beat_time_ms = (double)now_ms + (60000.0 / (double)g_bpm);
                    } else {
                        g_click_samples_left = 0;
                    }
                    break;

                case ACT_BPM_UP:
                    if (g_bpm < MAX_BPM) {
                        g_bpm++;
                        save_config();
                    }
                    break;

                case ACT_BPM_DOWN:
                    if (g_bpm > MIN_BPM) {
                        g_bpm--;
                        save_config();
                    }
                    break;

                case ACT_BPM_UP_FAST:
                    g_bpm += 5;
                    if (g_bpm > MAX_BPM) g_bpm = MAX_BPM;
                    save_config();
                    break;

                case ACT_BPM_DOWN_FAST:
                    g_bpm -= 5;
                    if (g_bpm < MIN_BPM) g_bpm = MIN_BPM;
                    save_config();
                    break;

                case ACT_SIG_PREV:
                    g_time_sig_idx = (g_time_sig_idx + NUM_TIME_SIGS - 1) % NUM_TIME_SIGS;
                    current_beat = 0;
                    if (g_playing) {
                        trigger_click(true);
                        last_beat_trigger_ms = now_ms;
                        next_beat_time_ms = (double)now_ms + (60000.0 / (double)g_bpm);
                    }
                    save_config();
                    break;

                case ACT_SIG_NEXT:
                    g_time_sig_idx = (g_time_sig_idx + 1) % NUM_TIME_SIGS;
                    current_beat = 0;
                    if (g_playing) {
                        trigger_click(true);
                        last_beat_trigger_ms = now_ms;
                        next_beat_time_ms = (double)now_ms + (60000.0 / (double)g_bpm);
                    }
                    save_config();
                    break;

                case ACT_RESET_BPM:
                    g_bpm = DEFAULT_BPM;
                    save_config();
                    break;

                case ACT_QUIT:
                    running = false;
                    break;

                default:
                    break;
            }
        }

        /* Metronome Beat Scheduler */
        now_ms = get_ticks_ms();
        int beats_per_bar = k_time_sig_beats[g_time_sig_idx];
        double interval_ms = 60000.0 / (double)g_bpm;

        if (g_playing) {
            if ((double)now_ms >= next_beat_time_ms) {
                current_beat = (current_beat + 1) % beats_per_bar;
                bool is_accent = (current_beat == 0);
                trigger_click(is_accent);
                last_beat_trigger_ms = now_ms;
                next_beat_time_ms += interval_ms;
                if ((double)now_ms - next_beat_time_ms > interval_ms) {
                    next_beat_time_ms = (double)now_ms + interval_ms;
                }
            }
        }

        /* =================================================================
         * Render UI (320x240 logical canvas -> upscaled 2x to 640x480)
         * ================================================================= */
        draw_rect(canvas, 0, 0, CANVAS_W, CANVAS_H, col_bg);

        /* 1. Top Status Bar (y: 0..26) */
        uint32_t top_bar_col = g_playing
            ? ((current_beat == 0 && (now_ms - last_beat_trigger_ms) < 120) ? col_accent : col_panel)
            : col_panel_dark;
        draw_rect(canvas, 0, 0, CANVAS_W, 26, top_bar_col);
        draw_rect(canvas, 0, 26, CANVAS_W, 2, col_border);

        if (g_playing) {
            draw_text_centered(canvas, CANVAS_W / 2, 6, "METRONOME PLAYING", 2, col_white);
        } else {
            draw_text_centered(canvas, CANVAS_W / 2, 6, "METRONOME PAUSED", 2, col_gold);
        }

        /* 2. Main Info Row: BPM Card (Left) & Time Signature Card (Right) */
        draw_rect(canvas, 12, 36, 184, 92, col_panel);
        draw_border(canvas, 12, 36, 184, 92, 2, col_border);

        draw_text_centered(canvas, 104, 43, "TEMPO (UP/DOWN)", 1, col_muted);

        char bpm_str[16];
        snprintf(bpm_str, sizeof(bpm_str), "%d", g_bpm);
        draw_text_centered(canvas, 88, 58, bpm_str, 5, col_white);
        draw_text(canvas, 150, 79, "BPM", 2, col_cyan);

        draw_text_centered(canvas, 104, 108, get_tempo_marking(g_bpm), 2, col_cyan);

        /* Time Signature Card (x: 204..308, y: 36..128) */
        draw_rect(canvas, 204, 36, 104, 92, col_panel);
        draw_border(canvas, 204, 36, 104, 92, 2, col_border);

        draw_text_centered(canvas, 256, 43, "TIME SIG", 1, col_muted);
        draw_text_centered(canvas, 256, 62, k_time_sig_names[g_time_sig_idx], 4, col_gold);
        draw_text_centered(canvas, 256, 108, "< LEFT/RIGHT >", 1, col_muted);

        /* 3. Beat Visualizer Panel (x: 12..308, y: 136..208) */
        draw_rect(canvas, 12, 136, 296, 72, col_panel);
        draw_border(canvas, 12, 136, 296, 72, 2, col_border);

        int margin_x = 22;
        int avail_w = 296 - 2 * margin_x;
        int gap = (beats_per_bar <= 4) ? 10 : 6;
        int box_w = (avail_w - gap * (beats_per_bar - 1)) / beats_per_bar;
        if (box_w > 48) box_w = 48;
        int total_boxes_w = box_w * beats_per_bar + gap * (beats_per_bar - 1);
        int start_x = 12 + (296 - total_boxes_w) / 2;
        int box_y = 146;
        int box_h = 42;

        bool beat_flash = g_playing && ((now_ms - last_beat_trigger_ms) < (uint32_t)(interval_ms * 0.55));

        for (int b = 0; b < beats_per_bar; b++) {
            int bx = start_x + b * (box_w + gap);
            bool is_active = g_playing && (b == current_beat);

            uint32_t fill_col = col_box_idle;
            uint32_t bdr_col  = (b == 0) ? col_accent : col_border;

            if (is_active) {
                fill_col = (b == 0) ? col_accent : col_beat;
                bdr_col  = col_white;
            }

            draw_rect(canvas, bx, box_y, box_w, box_h, fill_col);
            draw_border(canvas, bx, box_y, box_w, box_h, (b == 0) ? 2 : 1, bdr_col);

            char num_str[16];
            snprintf(num_str, sizeof(num_str), "%d", b + 1);
            uint32_t num_col = is_active ? col_bg : ((b == 0) ? col_accent : col_white);
            draw_text_centered(canvas, bx + box_w / 2, box_y + 10, num_str, 2, num_col);

            if (b == 0) {
                draw_text_centered(canvas, bx + box_w / 2, box_y + 29, "ACC", 1,
                                   is_active ? col_bg : col_accent);
            }
        }

        /* Pendulum / Phase Progress Bar inside Beat Panel (y: 194..202) */
        int pbar_x = 24, pbar_y = 194, pbar_w = 272, pbar_h = 6;
        draw_rect(canvas, pbar_x, pbar_y, pbar_w, pbar_h, col_bg);
        if (g_playing && interval_ms > 0.0) {
            double elapsed = (double)(now_ms - last_beat_trigger_ms);
            double phase = elapsed / interval_ms;
            if (phase < 0.0) phase = 0.0;
            if (phase > 1.0) phase = 1.0;
            double pos_norm = (current_beat % 2 == 0) ? phase : (1.0 - phase);
            int knob_w = 20;
            int knob_x = pbar_x + (int)(pos_norm * (double)(pbar_w - knob_w));
            uint32_t knob_col = (current_beat == 0 && beat_flash) ? col_accent : col_cyan;
            draw_rect(canvas, knob_x, pbar_y, knob_w, pbar_h, knob_col);
        }

        /* 4. Bottom Controls Bar (y: 214..240) */
        draw_rect(canvas, 0, 214, CANVAS_W, 26, col_panel_dark);
        draw_rect(canvas, 0, 214, CANVAS_W, 1, col_border);
        draw_text_centered(canvas, CANVAS_W / 2, 223,
                           "A:PLAY/PAUSE  UP/DN:BPM  L/R:TIME SIG  MENU:EXIT", 1, col_muted);

        platform_present(canvas);
        delay_ms(8); /* ~120 Hz poll & render loop */
    }

    save_config();
    free(canvas);
    platform_shutdown();
    return 0;
}
