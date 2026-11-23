/*
 * pocketband - Pocket Drum Machine, Bassline & Backing Track Jam Buddy
 *              for Anbernic RG 35xxSP & Linux Desktop (x86_64 X11)
 *
 * Upgraded for Anbernic RG 35xxSP (Allwinner H700 Quad-Core ARM Cortex-A53):
 *   1. High-Fidelity 44.1 kHz Stereo Floating-Point Multi-Track Audio Engine:
 *        - Analog-Modeled Kick Drum (exponential pitch sweep 175 Hz -> 42 Hz +
 *          beater transient click + soft-clip punch saturation).
 *        - Acoustic / TR-909 Snare Drum (dual resonant shell modes at 185 Hz &
 *          330 Hz + biquad bandpass snare-wire noise + crisp rim snap).
 *        - 6-Oscillator Inharmonic Metallic Cymbal / Hi-Hat / Jazz Ride Bank
 *          with second-order high-pass filter, closed-hat choke, and stereo pan.
 *        - Physical-Modeled Electric Bass Synthesizer (plucked sawtooth +
 *          sub-octave fundamental through a resonant 2-pole SVF low-pass filter
 *          with dynamic pluck-envelope cutoff modulation).
 *        - 4-Voice Stereo Rhythm Guitar / Electric Piano Comping Engine with
 *          staggered up/down strum delays, stereo chorus, and room ambience.
 *   2. Dedicated Background Audio Thread:
 *        - Sequencer clock, voice triggering, synthesis, and stereo mixing run
 *          100% asynchronously in a real-time background thread (`audio_thread`)
 *          while the 640x480 UI thread stays locked at 60 FPS with zero stutter.
 *   3. Accurate Anbernic RG 35xxSP Hardware Input + Desktop X11 Keyboard Parity:
 *        - RG 35xxSP evdev codes: A=304, B=305, X=306, Y=307, L1=308, R1=309,
 *          SELECT=310/314, START=311/315, L2=312, R2=313, MENU=316, D-Pad Hat/Keys.
 *        - Desktop X11 keys: A, B, X, Y, L/R (or [/]), 1/2, Tab, Enter, Arrows.
 *   4. Native 640x480 IPS Display Layout:
 *        - Live Now Playing / Next Chord cards with chord tones and 4-bar
 *          progression preview.
 *        - Full 12-Fret Rosewood Guitar Soloing Scale Map with anti-aliased
 *          Interval (`R, b3, 4, b5, 5, b7`) or Note Name (`A, C, D, Eb, E, G`)
 *          dots (toggled with `L2`).
 *        - Interactive 5-Track Drum & Bassline Step Sequencer (`SELECT`) with
 *          live note pitch preview on the `BASS` row (`R, 3, 5, 6, b7, 8`).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(TARGET_RG35XXSP)
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#elif defined(HOST_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#endif

#include "pocketband_font.h"

#define NATIVE_W        640
#define NATIVE_H        480
#define SAMPLE_RATE     44100
#define AUDIO_CHANNELS  2
#define AUDIO_FRAMES    256

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static volatile sig_atomic_t g_running = 1;
static void handle_sig(int sig) {
    (void)sig;
    g_running = 0;
}

/* ============================================================================
 * 1. UNIFIED INPUT BITMASKS (RG 35xxSP EVDEV + DESKTOP X11) & SOUNDFONT ENGINE
 * ============================================================================ */

#include "../rg35xxsp_input.h"
#include "../rg35xxsp_keepawake.h"
#undef BTN_QUIT
#define BTN_QUIT    (1u << 20)

#define RG35XXSP_SF2_IMPLEMENTATION
#include "../rg35xxsp_sf2.h"

/* ============================================================================
 * 2. 640x480 SOFTWARE FRAMEBUFFER & ANTI-ALIASED DRAWING PRIMITIVES
 * ============================================================================ */

static uint32_t g_fb[NATIVE_W * NATIVE_H];

#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

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
    uint32_t ia = 255 - a;
    uint32_t r  = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * ia) / 255;
    uint32_t g  = (((fg >> 8)  & 0xFF) * a + ((bg >> 8)  & 0xFF) * ia) / 255;
    uint32_t b  = ((fg & 0xFF)         * a + (bg & 0xFF)         * ia) / 255;
    g_fb[y * NATIVE_W + x] = RGB(r, g, b);
}

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > NATIVE_W) ? NATIVE_W : (x + w);
    int y1 = (y + h > NATIVE_H) ? NATIVE_H : (y + h);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g_fb[yy * NATIVE_W];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = c;
        }
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

static void draw_circle_aa(float cx, float cy, float radius, float border_w, uint32_t fill_col, uint32_t border_col) {
    int x0 = (int)(cx - radius - 2.0f);
    int x1 = (int)(cx + radius + 2.0f);
    int y0 = (int)(cy - radius - 2.0f);
    int y1 = (int)(cy + radius + 2.0f);
    float inner_r = radius - border_w;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float d  = sqrtf(dx * dx + dy * dy);
            if (d <= inner_r - 0.7f) {
                put_pixel(x, y, fill_col);
            } else if (d <= inner_r + 0.7f) {
                float t = (inner_r + 0.7f - d) / 1.4f;
                put_pixel(x, y, border_col);
                blend_pixel(x, y, fill_col, (uint8_t)(t * 255.0f));
            } else if (d <= radius - 0.5f) {
                put_pixel(x, y, border_col);
            } else if (d <= radius + 0.8f) {
                float a = (radius + 0.8f - d) / 1.3f;
                blend_pixel(x, y, border_col, (uint8_t)(a * 255.0f));
            }
        }
    }
}

static int char_advance(int font_id, char c) {
    const CwFont *f = &g_cw_fonts[font_id];
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
    const CwFont *f = &g_cw_fonts[font_id];
    int cur_x = x;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 32 || c > 126) c = '?';
        const CwGlyph *g = &f->glyphs[c - 32];
        const uint8_t *bmp = &g_cw_font_bitmaps[g->offset];
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
    const CwFont *f = &g_cw_fonts[font_id];
    int tw = text_width(font_id, s);
    int tx = bx + (bw - tw) / 2;
    int ty = by + (bh - f->line_height) / 2;
    draw_text(font_id, tx, ty, s, color);
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
                (uint8_t)(c & 0xFF),
                (uint8_t)((c >> 8) & 0xFF),
                (uint8_t)((c >> 16) & 0xFF)
            };
            fwrite(bgr, 1, 3, f);
        }
    }
    fclose(f);
}

/* ============================================================================
 * 3. BUILT-IN ROCK, BLUES & JAZZ STYLES (4/4 & 3/4) + PATTERNS
 * ============================================================================ */

#define NUM_STYLES 6
#define MAX_STEPS  8
#define NUM_TRACKS 5

typedef enum {
    TRK_KICK  = 0,
    TRK_SNARE = 1,
    TRK_CLHAT = 2,
    TRK_OPHAT = 3,
    TRK_BASS  = 4
} TrackId;

static const char *k_track_short[NUM_TRACKS] = {
    "KICK", "SNARE", "CL HAT", "OP HAT", "BASS"
};

static const char *k_bass_deg_labels[7] = {
    ".", "R", "3", "5", "6", "b7", "8"
};

typedef enum {
    CHQ_MAJ  = 0,
    CHQ_MIN  = 1,
    CHQ_DOM7 = 2,
    CHQ_MAJ7 = 3,
    CHQ_MIN7 = 4
} BarChordQuality;

typedef struct {
    int             root_offset; /* Semitones above active Key root (0..11) */
    BarChordQuality qual;
    const char     *roman;
} BarChord;

typedef struct {
    const char *name;
    const char *short_genre;
    int         beats_per_bar;   /* 4 for 4/4 (8 steps), 3 for 3/4 (6 steps) */
    int         steps_per_bar;   /* 8 for 4/4, 6 for 3/4 */
    int         default_bpm;
    int         swing_pct;       /* 50 = Straight 8ths, 60..66 = Triplet Shuffle */
    int         num_bars;        /* 4 or 12 bars in progression */
    BarChord    bars[12];
    const char *scale_name;
    uint8_t     scale_mask[12];  /* 1 = scale tone, 2 = characteristic blue/color note */
    uint8_t     default_steps[NUM_TRACKS][MAX_STEPS];
} StylePreset;

static const StylePreset k_styles[NUM_STYLES] = {
    /* 0: Blues Shuffle 4/4 (12-Bar Blues, I7 - IV7 - V7, 65% Triplet Shuffle) */
    {
        "Blues Shuffle 4/4", "BLUES 4/4", 4, 8, 110, 65, 12,
        {
            { 0, CHQ_DOM7, "I7"  }, { 0, CHQ_DOM7, "I7"  }, { 0, CHQ_DOM7, "I7"  }, { 0, CHQ_DOM7, "I7"  },
            { 5, CHQ_DOM7, "IV7" }, { 5, CHQ_DOM7, "IV7" }, { 0, CHQ_DOM7, "I7"  }, { 0, CHQ_DOM7, "I7"  },
            { 7, CHQ_DOM7, "V7"  }, { 5, CHQ_DOM7, "IV7" }, { 0, CHQ_DOM7, "I7"  }, { 7, CHQ_DOM7, "V7"  }
        },
        "Minor Pentatonic + Blues (1 b3 4 b5 5 b7)",
        { 1, 0, 0, 1, 0, 1, 2, 1, 0, 0, 1, 0 },
        {
            /* steps:   1  &  2  &  3  &  4  &  */
            /* KICK */ { 2, 0, 0, 0, 1, 0, 0, 1 },
            /* SNARE*/ { 0, 0, 2, 0, 0, 0, 2, 0 },
            /* CLHAT*/ { 2, 1, 2, 1, 2, 1, 2, 1 },
            /* OPHAT*/ { 0, 0, 0, 0, 0, 0, 0, 0 },
            /* BASS */ { 1, 3, 4, 3, 1, 3, 4, 5 }  /* Classic Blues Box: R-5-6-5 R-5-6-b7 */
        }
    },

    /* 1: Slow Blues 3/4 (12-Bar Waltz Blues in 3/4, 1 & 2 & 3 &) */
    {
        "Slow Blues 3/4", "BLUES 3/4", 3, 6, 86, 62, 12,
        {
            { 0, CHQ_DOM7, "I7"  }, { 0, CHQ_DOM7, "I7"  }, { 0, CHQ_DOM7, "I7"  }, { 0, CHQ_DOM7, "I7"  },
            { 5, CHQ_DOM7, "IV7" }, { 5, CHQ_DOM7, "IV7" }, { 0, CHQ_DOM7, "I7"  }, { 0, CHQ_DOM7, "I7"  },
            { 7, CHQ_DOM7, "V7"  }, { 5, CHQ_DOM7, "IV7" }, { 0, CHQ_DOM7, "I7"  }, { 7, CHQ_DOM7, "V7"  }
        },
        "Minor Pentatonic + Blues (1 b3 4 b5 5 b7)",
        { 1, 0, 0, 1, 0, 1, 2, 1, 0, 0, 1, 0 },
        {
            /* steps:   1  &  2  &  3  &       */
            /* KICK */ { 2, 0, 0, 1, 0, 0, 0, 0 },
            /* SNARE*/ { 0, 0, 0, 0, 2, 0, 0, 0 },
            /* CLHAT*/ { 2, 1, 1, 1, 1, 0, 0, 0 },
            /* OPHAT*/ { 0, 0, 0, 0, 0, 1, 0, 0 },
            /* BASS */ { 1, 2, 3, 4, 3, 2, 0, 0 }  /* 3/4 Blues Arpeggio: R-3-5-6-5-3 */
        }
    },

    /* 2: Classic Rock 4/4 (Straight 8ths, I - bVII - IV - I) */
    {
        "Classic Rock 4/4", "ROCK 4/4", 4, 8, 120, 50, 4,
        {
            {  0, CHQ_MAJ, "I"    },
            { 10, CHQ_MAJ, "bVII" },
            {  5, CHQ_MAJ, "IV"   },
            {  0, CHQ_MAJ, "I"    }
        },
        "Mixolydian / Rock Pentatonic (1 2 3 4 5 6 b7)",
        { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 2, 0 },
        {
            /* steps:   1  &  2  &  3  &  4  &  */
            /* KICK */ { 2, 0, 0, 1, 2, 0, 0, 0 },
            /* SNARE*/ { 0, 0, 2, 0, 0, 0, 2, 0 },
            /* CLHAT*/ { 2, 1, 2, 1, 2, 1, 2, 0 },
            /* OPHAT*/ { 0, 0, 0, 0, 0, 0, 0, 1 },
            /* BASS */ { 1, 1, 1, 3, 1, 1, 6, 3 }  /* Driving 8th Rock Bass: R-R-R-5 R-R-8-5 */
        }
    },

    /* 3: Rock Ballad 3/4 (3/4 Time, i - bVI - bVII - V7) */
    {
        "Rock Ballad 3/4", "ROCK 3/4", 3, 6, 98, 50, 4,
        {
            {  0, CHQ_MIN,  "i"    },
            {  8, CHQ_MAJ,  "bVI"  },
            { 10, CHQ_MAJ,  "bVII" },
            {  7, CHQ_DOM7, "V7"   }
        },
        "Natural Minor / Aeolian (1 2 b3 4 5 b6 b7)",
        { 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0 },
        {
            /* steps:   1  &  2  &  3  &       */
            /* KICK */ { 2, 0, 0, 1, 0, 0, 0, 0 },
            /* SNARE*/ { 0, 0, 0, 0, 2, 0, 0, 0 },
            /* CLHAT*/ { 2, 1, 1, 1, 2, 0, 0, 0 },
            /* OPHAT*/ { 0, 0, 0, 0, 0, 1, 0, 0 },
            /* BASS */ { 1, 1, 3, 3, 6, 3, 0, 0 }  /* 3/4 Rock Bass: R-R-5-5-8-5 */
        }
    },

    /* 4: Jazz Swing 4/4 (ii7 - V7 - Imaj7 - VI7, Walking Quarter-Note Bass) */
    {
        "Jazz Swing 4/4", "JAZZ 4/4", 4, 8, 126, 63, 4,
        {
            { 2, CHQ_MIN7, "ii7"   },
            { 7, CHQ_DOM7, "V7"    },
            { 0, CHQ_MAJ7, "Imaj7" },
            { 9, CHQ_DOM7, "VI7"   }
        },
        "Major / Jazz Bebop Modes (1 2 3 4 5 6 7)",
        { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 },
        {
            /* steps:   1  &  2  &  3  &  4  &  */
            /* KICK */ { 1, 0, 0, 0, 1, 0, 0, 0 },
            /* SNARE*/ { 0, 0, 0, 1, 0, 0, 0, 1 },
            /* CLHAT*/ { 0, 0, 2, 0, 0, 0, 2, 0 }, /* Hi-hat pedal chick on Beats 2 and 4 */
            /* OPHAT*/ { 2, 0, 1, 1, 2, 0, 1, 1 }, /* Jazz Ride spang-a-lang: 1, 2 &, 3, 4 & */
            /* BASS */ { 1, 0, 2, 0, 3, 0, 5, 0 }  /* Quarter-note Walking Jazz Bass: R-3-5-b7 */
        }
    },

    /* 5: Jazz Waltz 3/4 (Imaj7 - vi7 - ii7 - V7 in 3/4 Swing) */
    {
        "Jazz Waltz 3/4", "JAZZ 3/4", 3, 6, 132, 60, 4,
        {
            { 0, CHQ_MAJ7, "Imaj7" },
            { 9, CHQ_MIN7, "vi7"   },
            { 2, CHQ_MIN7, "ii7"   },
            { 7, CHQ_DOM7, "V7"    }
        },
        "Major / Lydian Waltz (1 2 3 4 5 6 7)",
        { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 },
        {
            /* steps:   1  &  2  &  3  &       */
            /* KICK */ { 1, 0, 0, 0, 0, 0, 0, 0 },
            /* SNARE*/ { 0, 0, 0, 1, 0, 0, 0, 0 },
            /* CLHAT*/ { 0, 0, 2, 0, 0, 0, 0, 0 }, /* Hi-hat chick on Beat 2 */
            /* OPHAT*/ { 2, 0, 1, 1, 1, 0, 0, 0 }, /* Jazz Waltz Ride: 1, 2 &, 3 */
            /* BASS */ { 1, 0, 2, 0, 3, 4, 0, 0 }  /* 3/4 Walking Waltz Bass: R . 3 . 5 6 */
        }
    }
};

static const char *k_note_names[12] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
};

static const char *k_interval_labels[12] = {
    "R", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7"
};

/* Standard guitar string pitch classes from top row (high e) to bottom row (low E) */
static const char *k_gtr_str_lbl[6]  = { "e", "B", "G", "D", "A", "E" };
static const int   k_gtr_str_pc[6]   = { 4, 11, 7, 2, 9, 4 };

/* ============================================================================
 * 4. APPLICATION STATE & EDITABLE PATTERN GRID
 * ============================================================================ */

typedef enum {
    VIEW_JAM         = 0,
    VIEW_STEP_EDITOR = 1
} ViewMode;

typedef enum {
    MIX_FULL_BAND  = 0, /* Drums + Bass + Stereo Chord Comping */
    MIX_DRUM_BASS  = 1, /* Drums + Bassline */
    MIX_DRUMS_ONLY = 2  /* Drums Only */
} BackingMixMode;

static ViewMode       g_view         = VIEW_JAM;
static BackingMixMode g_mix_mode     = MIX_FULL_BAND;
static bool           g_show_help    = false;
static bool           g_neck_notes   = false; /* false = Intervals (R,b3,4,5,b7), true = Note Names (A,C,D,E,G) */

static int            g_style_idx    = 0;     /* Default: Blues Shuffle 4/4 */
static int            g_key_pc       = 9;     /* Default Key: A (9) */
static int            g_bpm          = 110;
static int            g_swing_pct    = 65;
static bool           g_auto_fill    = true;
static bool           g_fill_queued  = false;

static uint8_t        g_pattern[NUM_STYLES][NUM_TRACKS][MAX_STEPS];

/* Sequencer Transport State (shared with background audio thread) */
static volatile bool  g_playing      = true;
static volatile int   g_cur_bar      = 0;
static volatile int   g_cur_step     = 0;

/* Step Editor Cursor */
static int            g_edit_track   = 0;
static int            g_edit_step    = 0;

static char           g_config_path[512] = "";

static void format_chord_label(int key_pc, const BarChord *bc, char *buf, size_t sz) {
    int pc = (key_pc + bc->root_offset) % 12;
    const char *suf = "";
    switch (bc->qual) {
        case CHQ_MAJ:  suf = "";     break;
        case CHQ_MIN:  suf = "m";    break;
        case CHQ_DOM7: suf = "7";    break;
        case CHQ_MAJ7: suf = "maj7"; break;
        case CHQ_MIN7: suf = "m7";   break;
    }
    snprintf(buf, sz, "%s%s", k_note_names[pc], suf);
}

static void format_bass_pitch_name(int key_pc, const BarChord *bc, uint8_t bass_deg, char *buf, size_t sz) {
    if (bass_deg == 0 || bass_deg > 6) {
        snprintf(buf, sz, "REST");
        return;
    }
    int chord_root_pc = (key_pc + bc->root_offset) % 12;
    int third_semi = (bc->qual == CHQ_MIN || bc->qual == CHQ_MIN7) ? 3 : 4;
    int semitone_off = 0;
    switch (bass_deg) {
        case 1: semitone_off = 0;          break;
        case 2: semitone_off = third_semi; break;
        case 3: semitone_off = 7;          break;
        case 4: semitone_off = 9;          break;
        case 5: semitone_off = 10;         break;
        case 6: semitone_off = 12;         break;
    }
    int note_pc = (chord_root_pc + semitone_off) % 12;
    snprintf(buf, sz, "%s (%s)", k_bass_deg_labels[bass_deg], k_note_names[note_pc]);
}

/* ============================================================================
 * 5. STUDIO 44.1 kHz STEREO SOUNDFONT (SF2) + PHYSICAL-MODELING FALLBACK SYNTH
 *    - Runs 100% on the dedicated background audio thread (zero UI blocking)
 *    - Style-matched GM SoundFont instruments (GeneralUser-GS.sf2 / pocketband.sf2)
 *    - Smooth 4-voice chord voice leading, staggered natural strums, Beat-4 '&'
 *      anticipation, chromatic walking approach bass, 16th-note ghost snares &
 *      turnaround tom fills, velocity/micro-timing humanization, hi-hat choke,
 *      and stereo Schroeder/Freeverb room reverb (rg35xxsp_sf2.h)
 * ============================================================================ */

/* Dedicated MIDI channel layout for wide studio stereo staging */
#define PB_CH_BASS      1
#define PB_CH_CHORD_0   2
#define PB_CH_CHORD_1   3
#define PB_CH_CHORD_2   4
#define PB_CH_CHORD_3   5
#define PB_CH_ORGAN     6
#define PB_CH_DRUM_KIT  9   /* Kick, Snare, Cross-stick, Toms (Center) */
#define PB_CH_DRUM_HAT  10  /* Closed / Pedal / Open Hi-Hat (Left, with choke) */
#define PB_CH_DRUM_CYM  11  /* Jazz Ride, Ride Bell, Crash (Right) */

/* General MIDI Percussion Note Numbers (Bank 128) */
#define GM_KICK_1       35
#define GM_KICK_2       36
#define GM_SIDE_STICK   37
#define GM_SNARE_1      38
#define GM_SNARE_2      40
#define GM_LOW_TOM      45
#define GM_MID_TOM      47
#define GM_HIGH_TOM     50
#define GM_CLOSED_HAT   42
#define GM_PEDAL_HAT    44
#define GM_OPEN_HAT     46
#define GM_CRASH_CYM    49
#define GM_RIDE_CYM     51
#define GM_RIDE_BELL    53

#define PB_MAX_PENDING_EV 64
#define PB_MAX_ACTIVE_GATES 64

typedef struct {
    bool  used;
    int   delay_samples;
    int   channel;
    int   key;
    float vel;
    int   gate_samples;
    bool  choke_channel;
} PendingMidiEvent;

typedef struct {
    bool active;
    int  channel;
    int  key;
    int  rem_samples;
} ActiveMidiGate;

typedef struct {
    /* Sequencer sample clock (8th-note steps + midpoint 16th-note sub-steps) */
    uint32_t step_sample_pos;
    uint32_t step_sample_len;
    bool     substep_16th_fired;
    bool     was_playing;
    int      sf2_configured_style;

    /* Smooth 4-voice chord voice-leading memory */
    bool     prev_voicing_valid;
    int      prev_voicing[4];

    /* Zero-malloc micro-timing / strum delay queue & note-off gate tracker */
    PendingMidiEvent pending_ev[PB_MAX_PENDING_EV];
    ActiveMidiGate   active_gates[PB_MAX_ACTIVE_GATES];

    /* --- Fallback Physical-Modeling Synth State (used if no .sf2 found) --- */
    /* 1. Analog Kick Drum (Exponential pitch sweep + beater click) */
    int      kick_rem;
    int      kick_total;
    float    kick_phase;
    float    kick_amp;

    /* 2. Snare Drum (Dual resonant shell modes + bandpass wire noise + rim click) */
    int      snare_rem;
    int      snare_total;
    float    snare_phase1;
    float    snare_phase2;
    float    snare_bp_z1;
    float    snare_bp_z2;
    float    snare_amp;

    /* 3. Closed & Open Hi-Hat / Jazz Ride Cymbal (6-oscillator metallic bank + 2-pole HPF) */
    int      hat_rem;
    int      hat_total;
    float    hat_amp;
    bool     hat_is_open;
    float    hat_phases[6];
    float    hat_hp_x1;
    float    hat_hp_y1;

    /* 4. Electric Bass Synthesizer (Saw + Sub + 2-Pole Resonant Low-Pass SVF) */
    int      bass_rem;
    int      bass_total;
    float    bass_phase;
    float    bass_sub_phase;
    float    bass_freq;
    float    bass_svf_lp;
    float    bass_svf_bp;
    float    bass_amp;

    /* 5. 4-Voice Stereo Rhythm Guitar / Rhodes Chord Comping Engine */
    int      chord_rem;
    int      chord_total;
    int      chord_strum_delay[4];
    float    chord_phases[4];
    float    chord_freqs[4];
    float    chord_lp[4];
    float    chord_amp;
    float    chorus_lfo;

    /* Per-track visual VU meters (0.0 .. 1.0) */
    volatile float vu_kick;
    volatile float vu_snare;
    volatile float vu_hat;
    volatile float vu_bass;
    volatile float vu_chord;

    uint32_t lfsr;
} SynthState;

static SynthState      g_synth;
static Rg35Sf2Engine   g_sf2;
static volatile bool   g_sf2_loaded = false;
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline float midi_to_freq(int midi_note) {
    return 440.0f * powf(2.0f, (float)(midi_note - 69) / 12.0f);
}

static inline uint32_t next_rng_u32(void) {
    g_synth.lfsr ^= g_synth.lfsr << 13;
    g_synth.lfsr ^= g_synth.lfsr >> 17;
    g_synth.lfsr ^= g_synth.lfsr << 5;
    return g_synth.lfsr;
}

static inline float next_white_noise(void) {
    uint32_t r = next_rng_u32();
    return (float)((int32_t)(r >> 9) - 4194304) * (1.0f / 4194304.0f);
}

static inline float humanize_vel(float base_vel, float max_jitter) {
    int32_t r = (int32_t)(next_rng_u32() >> 16) & 0x7FFF;
    float delta = (((float)r / 16383.5f) - 1.0f) * max_jitter;
    float v = base_vel + delta;
    if (v < 0.08f) v = 0.08f;
    if (v > 1.0f)  v = 1.0f;
    return v;
}

static inline int humanize_micro_delay(int max_ms) {
    if (max_ms <= 0) return 0;
    int max_samp = (SAMPLE_RATE * max_ms) / 1000;
    if (max_samp <= 0) return 0;
    return (int)(next_rng_u32() % (uint32_t)(max_samp + 1));
}

static uint32_t compute_step_samples(int step_idx) {
    int bpm = g_bpm;
    if (bpm < 40)  bpm = 40;
    if (bpm > 240) bpm = 240;
    double beat_samples = ((double)SAMPLE_RATE * 60.0) / (double)bpm;
    double ratio = (step_idx % 2 == 0) ? ((double)g_swing_pct / 50.0)
                                       : ((double)(100 - g_swing_pct) / 50.0);
    uint32_t len = (uint32_t)(beat_samples * 0.5 * ratio);
    return (len < 1000) ? 1000 : len;
}

/* Select preferred GM program with smart fallback when running on compact pocketband.sf2 */
static void sf2_set_prog_fallback(int channel, int bank, int pref1, int pref2, int pref3) {
    if (!rg35_sf2_ready(&g_sf2)) return;
    int idx = tsf_get_presetindex(g_sf2.synth, bank, pref1);
    if (idx < 0 && pref2 >= 0) idx = tsf_get_presetindex(g_sf2.synth, bank, pref2);
    if (idx < 0 && pref3 >= 0) idx = tsf_get_presetindex(g_sf2.synth, bank, pref3);
    if (idx < 0) idx = (bank == 128) ? tsf_get_presetindex(g_sf2.synth, 128, 0) : 0;
    if (idx >= 0) tsf_channel_set_presetindex(g_sf2.synth, channel, idx);
}

static int style_chord_is_guitar(int style_id) {
    return (style_id == 1 || style_id == 2 || style_id == 3 || style_id == 4);
}

static void sf2_configure_style_instruments(int style_id) {
    if (!rg35_sf2_ready(&g_sf2)) return;
    int drum_pref1 = 0, drum_pref2 = 0;
    int bass_prog  = 33;
    int chord_p1   = 4, chord_p2 = 27;
    int organ_prog = -1;
    float chord_vol = 0.72f;
    float bass_vol  = 0.92f;
    float drum_vol  = 0.96f;

    switch (style_id) {
        case 0: /* Blues Shuffle 4/4: Room/Standard Kit, Finger Bass, Tine E.Piano */
            drum_pref1 = 8; drum_pref2 = 0;
            bass_prog  = 33;
            chord_p1   = 4; chord_p2 = 27;
            organ_prog = -1;
            chord_vol  = 0.74f;
            break;
        case 1: /* Slow Blues 3/4: Room/Standard Kit, Finger Bass, Clean Guitar + Tonewheel Organ */
            drum_pref1 = 8; drum_pref2 = 0;
            bass_prog  = 33;
            chord_p1   = 27; chord_p2 = 26;
            organ_prog = 16;
            chord_vol  = 0.72f;
            break;
        case 2: /* Classic Rock 4/4: Power/Standard Kit, Picked Bass, Overdrive/Clean Guitar + Rock Organ */
            drum_pref1 = 16; drum_pref2 = 0;
            bass_prog  = 34;
            chord_p1   = 29; chord_p2 = 27;
            organ_prog = 18;
            chord_vol  = 0.68f;
            break;
        case 3: /* Rock Ballad 3/4: Standard Kit, Picked Bass, Clean Guitar + Rock Organ */
            drum_pref1 = 0; drum_pref2 = 8;
            bass_prog  = 34;
            chord_p1   = 27; chord_p2 = 25;
            organ_prog = 18;
            chord_vol  = 0.72f;
            break;
        case 4: /* Jazz Swing 4/4: Jazz Kit, Acoustic Upright Bass, Jazz Hollowbody Guitar */
            drum_pref1 = 32; drum_pref2 = 0;
            bass_prog  = 32;
            chord_p1   = 26; chord_p2 = 4;
            organ_prog = -1;
            chord_vol  = 0.76f;
            drum_vol   = 0.88f;
            break;
        case 5: /* Jazz Waltz 3/4: Brush/Jazz Kit, Acoustic Upright Bass, Tine E.Piano */
        default:
            drum_pref1 = 40; drum_pref2 = 32;
            bass_prog  = 32;
            chord_p1   = 4; chord_p2 = 26;
            organ_prog = -1;
            chord_vol  = 0.76f;
            drum_vol   = 0.86f;
            break;
    }

    sf2_set_prog_fallback(PB_CH_DRUM_KIT, 128, drum_pref1, drum_pref2, 0);
    sf2_set_prog_fallback(PB_CH_DRUM_HAT, 128, drum_pref1, drum_pref2, 0);
    sf2_set_prog_fallback(PB_CH_DRUM_CYM, 128, drum_pref1, drum_pref2, 0);
    rg35_sf2_set_channel_pan_vol(&g_sf2, PB_CH_DRUM_KIT, 0.50f, drum_vol);
    rg35_sf2_set_channel_pan_vol(&g_sf2, PB_CH_DRUM_HAT, 0.34f, drum_vol * 0.88f);
    rg35_sf2_set_channel_pan_vol(&g_sf2, PB_CH_DRUM_CYM, 0.68f, drum_vol * 0.86f);

    sf2_set_prog_fallback(PB_CH_BASS, 0, bass_prog, 33, 32);
    rg35_sf2_set_channel_pan_vol(&g_sf2, PB_CH_BASS, 0.46f, bass_vol);

    static const float k_chord_pans[4] = { 0.36f, 0.47f, 0.57f, 0.68f };
    for (int v = 0; v < 4; v++) {
        sf2_set_prog_fallback(PB_CH_CHORD_0 + v, 0, chord_p1, chord_p2, 4);
        rg35_sf2_set_channel_pan_vol(&g_sf2, PB_CH_CHORD_0 + v, k_chord_pans[v], chord_vol);
    }

    if (organ_prog >= 0) {
        sf2_set_prog_fallback(PB_CH_ORGAN, 0, organ_prog, 16, 4);
        rg35_sf2_set_channel_pan_vol(&g_sf2, PB_CH_ORGAN, 0.30f, 0.42f);
    }
    g_synth.sf2_configured_style = style_id;
}

static void sf2_clear_queues(void) {
    memset(g_synth.pending_ev, 0, sizeof(g_synth.pending_ev));
    memset(g_synth.active_gates, 0, sizeof(g_synth.active_gates));
    if (rg35_sf2_ready(&g_sf2)) {
        rg35_sf2_all_notes_off(&g_sf2);
    }
}

static void sf2_fire_note_now(int channel, int key, float vel, int gate_samples, bool choke_channel) {
    if (!rg35_sf2_ready(&g_sf2)) return;
    if (choke_channel) {
        tsf_channel_sounds_off_all(g_sf2.synth, channel);
        for (int i = 0; i < PB_MAX_ACTIVE_GATES; i++) {
            if (g_synth.active_gates[i].active && g_synth.active_gates[i].channel == channel) {
                g_synth.active_gates[i].active = false;
            }
        }
    } else if (gate_samples > 0) {
        /* Smoothly release any previous still-gated note on the same channel+key
         * before retriggering so every note_on has an exact 1:1 note_off pairing. */
        for (int i = 0; i < PB_MAX_ACTIVE_GATES; i++) {
            if (g_synth.active_gates[i].active &&
                g_synth.active_gates[i].channel == channel &&
                g_synth.active_gates[i].key == key) {
                rg35_sf2_note_off(&g_sf2, channel, key);
                g_synth.active_gates[i].active = false;
                break;
            }
        }
    }
    rg35_sf2_note_on(&g_sf2, channel, key, vel);
    if (gate_samples > 0) {
        /* Claim free slot or release shortest-remaining gate if full */
        int slot = -1;
        int min_rem = 0x7FFFFFFF;
        int min_idx = 0;
        for (int i = 0; i < PB_MAX_ACTIVE_GATES; i++) {
            if (!g_synth.active_gates[i].active) {
                slot = i;
                break;
            }
            if (g_synth.active_gates[i].rem_samples < min_rem) {
                min_rem = g_synth.active_gates[i].rem_samples;
                min_idx = i;
            }
        }
        if (slot < 0) {
            rg35_sf2_note_off(&g_sf2, g_synth.active_gates[min_idx].channel, g_synth.active_gates[min_idx].key);
            slot = min_idx;
        }
        g_synth.active_gates[slot].active = true;
        g_synth.active_gates[slot].channel = channel;
        g_synth.active_gates[slot].key = key;
        g_synth.active_gates[slot].rem_samples = gate_samples;
    }
}

static void sf2_schedule_note(int delay_samples, int channel, int key, float vel,
                              int gate_samples, bool choke_channel) {
    if (!rg35_sf2_ready(&g_sf2)) return;
    if (delay_samples <= 0) {
        sf2_fire_note_now(channel, key, vel, gate_samples, choke_channel);
        return;
    }
    for (int i = 0; i < PB_MAX_PENDING_EV; i++) {
        if (!g_synth.pending_ev[i].used) {
            g_synth.pending_ev[i].used = true;
            g_synth.pending_ev[i].delay_samples = delay_samples;
            g_synth.pending_ev[i].channel = channel;
            g_synth.pending_ev[i].key = key;
            g_synth.pending_ev[i].vel = vel;
            g_synth.pending_ev[i].gate_samples = gate_samples;
            g_synth.pending_ev[i].choke_channel = choke_channel;
            return;
        }
    }
    /* Queue full: fire immediately rather than dropping */
    sf2_fire_note_now(channel, key, vel, gate_samples, choke_channel);
}

static void sf2_release_channel(int channel) {
    if (!rg35_sf2_ready(&g_sf2)) return;
    rg35_sf2_channel_notes_off(&g_sf2, channel);
    for (int i = 0; i < PB_MAX_ACTIVE_GATES; i++) {
        if (g_synth.active_gates[i].active && g_synth.active_gates[i].channel == channel) {
            g_synth.active_gates[i].active = false;
        }
    }
    for (int i = 0; i < PB_MAX_PENDING_EV; i++) {
        if (g_synth.pending_ev[i].used && g_synth.pending_ev[i].channel == channel) {
            g_synth.pending_ev[i].used = false;
        }
    }
}

/* Compute exact bass MIDI note in E1 (28) .. Eb2 (39) + scale degree offset */
static int compute_bass_midi(int key_pc, const BarChord *bc, uint8_t bass_deg) {
    if (bass_deg == 0 || bass_deg > 6 || !bc) return -1;
    int chord_root_pc = (key_pc + bc->root_offset) % 12;
    int third_semi = (bc->qual == CHQ_MIN || bc->qual == CHQ_MIN7) ? 3 : 4;
    int semitone_off = 0;
    switch (bass_deg) {
        case 1: semitone_off = 0;          break; /* R  */
        case 2: semitone_off = third_semi; break; /* 3  */
        case 3: semitone_off = 7;          break; /* 5  */
        case 4: semitone_off = 9;          break; /* 6  */
        case 5: semitone_off = 10;         break; /* b7 */
        case 6: semitone_off = 12;         break; /* 8  */
    }
    int base_midi = 28 + ((chord_root_pc - 4 + 12) % 12);
    return base_midi + semitone_off;
}

/* Chromatic walking approach note on Beat 4 leading smoothly into next bar's root */
static int compute_walking_approach_bass(int key_pc, const BarChord *cur_bc,
                                         const BarChord *next_bc, int cur_midi, int bar_idx) {
    if (!next_bc) return cur_midi;
    int next_root = compute_bass_midi(key_pc, next_bc, 1);
    if (next_root <= 0) return cur_midi;
    bool changing = (!cur_bc) || (cur_bc->root_offset != next_bc->root_offset)
                    || (cur_bc->qual != next_bc->qual);
    if (changing || (bar_idx % 2 == 1)) {
        int approach = (next_root > cur_midi) ? (next_root - 1) : (next_root + 1);
        if (approach == next_root || abs(approach - next_root) > 2) {
            approach = next_root - 1;
        }
        if (approach < 28) approach = 28;
        if (approach > 52) approach = 52;
        return approach;
    }
    return cur_midi;
}

static void sort3_int(int a[3]) {
    if (a[0] > a[1]) { int t = a[0]; a[0] = a[1]; a[1] = t; }
    if (a[1] > a[2]) { int t = a[1]; a[1] = a[2]; a[2] = t; }
    if (a[0] > a[1]) { int t = a[0]; a[0] = a[1]; a[1] = t; }
}

/* Smooth 4-voice chord voice leading minimizing semitone movement across bar changes */
static void compute_voice_led_chord(int key_pc, const BarChord *bc, int out_midi[4]) {
    int root_pc  = (key_pc + bc->root_offset) % 12;
    int third_pc = (root_pc + ((bc->qual == CHQ_MIN || bc->qual == CHQ_MIN7) ? 3 : 4)) % 12;
    int fifth_pc = (root_pc + 7) % 12;
    int sev_pc;
    if (bc->qual == CHQ_MAJ7) {
        sev_pc = (root_pc + 11) % 12;
    } else if (bc->qual == CHQ_DOM7 || bc->qual == CHQ_MIN7) {
        sev_pc = (root_pc + 10) % 12;
    } else {
        sev_pc = (root_pc + 2) % 12; /* Add 9th for lush 4-voice triad comping */
    }

    int target_center = 61;
    if (g_synth.prev_voicing_valid) {
        target_center = (g_synth.prev_voicing[1] + g_synth.prev_voicing[2] + g_synth.prev_voicing[3]) / 3;
        if (target_center < 57) target_center = 57;
        if (target_center > 65) target_center = 65;
    }

    int best[4] = { 48 + root_pc, 52 + third_pc, 55 + fifth_pc, 58 + sev_pc };
    int best_cost = 0x7FFFFFFF;
    int upper_pcs[3] = { third_pc, fifth_pc, sev_pc };

    for (int base_midi = 52; base_midi <= 64; base_midi++) {
        int cand[4];
        cand[0] = 48 + root_pc;
        if (cand[0] < 45) cand[0] += 12;

        int placed[3];
        for (int i = 0; i < 3; i++) {
            placed[i] = base_midi + ((upper_pcs[i] - (base_midi % 12) + 12) % 12);
        }
        sort3_int(placed);
        if (placed[0] <= cand[0] + 2) {
            placed[0] += 12;
            sort3_int(placed);
        }
        cand[1] = placed[0];
        cand[2] = placed[1];
        cand[3] = placed[2];

        int center = (cand[1] + cand[2] + cand[3]) / 3;
        int cost = abs(center - target_center) * 2;
        if (g_synth.prev_voicing_valid) {
            for (int v = 1; v < 4; v++) {
                cost += abs(cand[v] - g_synth.prev_voicing[v]) * 3;
            }
        }
        if (cost < best_cost) {
            best_cost = cost;
            for (int v = 0; v < 4; v++) best[v] = cand[v];
        }
    }

    for (int v = 0; v < 4; v++) {
        out_midi[v] = best[v];
        g_synth.prev_voicing[v] = best[v];
    }
    g_synth.prev_voicing_valid = true;
}

static float style_comping_velocity(int style_id, int beats_per_bar, int bar_idx,
                                    int step_idx, bool next_changes) {
    int spb = beats_per_bar * 2;
    /* Subtle Beat-4 '&' (or Beat-3 '&') harmonic anticipation before a chord change */
    if (step_idx == spb - 1 && (next_changes || (bar_idx % 2 == 1))) {
        return 0.64f;
    }
    /* Jazz Swing (style 4): Charleston comping + bar-2 syncopated phrasing */
    if (style_id == 4) {
        if (bar_idx % 2 == 0) {
            if (step_idx == 0) return 0.72f;
            if (step_idx == 3) return 0.66f;
            if (step_idx == 6) return 0.56f;
        } else {
            if (step_idx == 1) return 0.64f;
            if (step_idx == 4) return 0.60f;
        }
        return 0.0f;
    }
    if (beats_per_bar == 4) {
        if (step_idx == 0) return 0.76f;
        if (step_idx == 3) return 0.66f;
        if (step_idx == 4 && (bar_idx % 2 == 0)) return 0.58f;
        if (step_idx == 5 && (bar_idx % 2 == 1)) return 0.62f;
        return 0.0f;
    }
    /* 3/4 styles (Slow Blues 3/4, Rock Ballad 3/4, Jazz Waltz 3/4) */
    if (step_idx == 0) return 0.74f;
    if (step_idx == 2) return 0.60f;
    if (step_idx == 4) return 0.64f;
    return 0.0f;
}

static void trigger_step_voices(int style_id, int bar_idx, int step_idx, int sub_step) {
    const StylePreset *st = &k_styles[style_id];
    const BarChord *cur_bc  = &st->bars[bar_idx % st->num_bars];
    const BarChord *next_bc = &st->bars[(bar_idx + 1) % st->num_bars];
    bool next_changes = (cur_bc->root_offset != next_bc->root_offset) || (cur_bc->qual != next_bc->qual);

    int spb = st->steps_per_bar;
    bool is_fill_bar = ((bar_idx + 1) % 4 == 0 && g_auto_fill) || g_fill_queued;
    bool is_turnaround = ((bar_idx + 1) == st->num_bars) && is_fill_bar;
    int fill_start = is_turnaround ? (spb >= 4 ? spb - 4 : 0) : (spb >= 2 ? spb - 2 : 0);
    bool is_jazz = (style_id == 4 || style_id == 5);
    bool use_sf2 = rg35_sf2_ready(&g_sf2);

    if (use_sf2 && g_synth.sf2_configured_style != style_id) {
        sf2_configure_style_instruments(style_id);
    }

    /* =====================================================================
     * Mid-Step 16th-Note Subdivision (sub_step == 1):
     *   - Expressive 16th-note descending tom/snare turnaround fills
     *   - Subtle 16th-note ghost snare notes for live studio groove
     *   - Chromatic 16th bass pickup into turnaround bar 1
     * ===================================================================== */
    if (sub_step == 1) {
        if (is_fill_bar && step_idx >= fill_start) {
            int fp = step_idx - fill_start;
            int fill_note = (fp == 0) ? GM_SNARE_1 :
                            (fp == 1) ? GM_HIGH_TOM :
                            (fp == 2) ? GM_MID_TOM : GM_LOW_TOM;
            if (use_sf2) {
                int fill_gate = (fill_note == GM_SNARE_1) ? ((SAMPLE_RATE * 320) / 1000)
                                                          : ((SAMPLE_RATE * 650) / 1000);
                sf2_schedule_note(humanize_micro_delay(2), PB_CH_DRUM_KIT, fill_note,
                                  humanize_vel(0.68f, 0.06f), fill_gate, false);
            } else {
                g_synth.snare_total  = (SAMPLE_RATE * 110) / 1000;
                g_synth.snare_rem    = g_synth.snare_total;
                g_synth.snare_phase1 = 0.0f;
                g_synth.snare_phase2 = 0.0f;
                g_synth.snare_amp    = 0.42f;
            }
            g_synth.vu_snare = 0.72f;
        } else {
            /* Subtle 16th-note ghost snare on odd bars (Blues / Rock / Jazz pocket) */
            bool ghost = ((bar_idx % 2 == 1) && (step_idx == 3 || step_idx == spb - 1)) ||
                         (style_id == 0 && step_idx == 1 && (bar_idx % 4 == 2));
            if (ghost) {
                if (use_sf2) {
                    int gnote = (style_id == 5) ? GM_SIDE_STICK : GM_SNARE_1;
                    sf2_schedule_note(humanize_micro_delay(3), PB_CH_DRUM_KIT, gnote,
                                      humanize_vel(0.28f, 0.04f), (SAMPLE_RATE * 240) / 1000, false);
                } else {
                    g_synth.snare_total = (SAMPLE_RATE * 75) / 1000;
                    g_synth.snare_rem   = g_synth.snare_total;
                    g_synth.snare_amp   = 0.22f;
                }
            }
        }

        /* Chromatic 16th bass pickup on final 16th of a fill bar */
        if (g_mix_mode != MIX_DRUMS_ONLY && is_fill_bar && step_idx == spb - 1) {
            int next_root = compute_bass_midi(g_key_pc, next_bc, 1);
            if (next_root > 0) {
                int pickup = (next_root > 28) ? (next_root - 1) : 28;
                if (use_sf2) {
                    sf2_release_channel(PB_CH_BASS);
                    sf2_schedule_note(0, PB_CH_BASS, pickup, humanize_vel(0.62f, 0.04f),
                                      (int)(g_synth.step_sample_len / 2), false);
                } else {
                    g_synth.bass_total = (SAMPLE_RATE * 140) / 1000;
                    g_synth.bass_rem   = g_synth.bass_total;
                    g_synth.bass_freq  = midi_to_freq(pickup);
                    g_synth.bass_amp   = 0.48f;
                }
            }
        }
        return;
    }

    /* =====================================================================
     * Primary Eighth-Note Step (sub_step == 0)
     * ===================================================================== */
    uint8_t kick_hit  = g_pattern[style_id][TRK_KICK][step_idx];
    uint8_t snare_hit = g_pattern[style_id][TRK_SNARE][step_idx];
    uint8_t clhat_hit = g_pattern[style_id][TRK_CLHAT][step_idx];
    uint8_t ophat_hit = g_pattern[style_id][TRK_OPHAT][step_idx];
    uint8_t bass_deg  = g_pattern[style_id][TRK_BASS][step_idx];

    /* Subtle bar-2 syncopated kick pickup on Blues / Classic Rock */
    if (!is_fill_bar && (bar_idx % 2 == 1) && step_idx == spb - 3 && kick_hit == 0 &&
        (style_id == 0 || style_id == 2)) {
        kick_hit = 1;
    }

    /* Turnaround drum fill on final beat(s) of a fill bar */
    if (is_fill_bar && step_idx >= fill_start) {
        snare_hit = (step_idx == spb - 1) ? 2 : 1;
        if (step_idx == fill_start) kick_hit = 2;
    }
    if (step_idx == spb - 1 && g_fill_queued) {
        g_fill_queued = false;
    }

    /* 1. Trigger Drums */
    if (use_sf2) {
        /* Tasteful crash cymbal on Beat 1 after a turnaround / fill bar */
        if (step_idx == 0 && bar_idx == 0 && g_auto_fill && (next_rng_u32() % 100u) < 40u) {
            sf2_schedule_note(0, PB_CH_DRUM_CYM, GM_CRASH_CYM, humanize_vel(0.62f, 0.05f),
                              (SAMPLE_RATE * 2000) / 1000, false);
        }

        if (kick_hit > 0) {
            int k_note = is_jazz ? GM_KICK_1 : GM_KICK_2;
            float k_vel = (kick_hit == 2) ? 0.92f : 0.74f;
            sf2_schedule_note(0, PB_CH_DRUM_KIT, k_note, humanize_vel(k_vel, 0.04f),
                              (SAMPLE_RATE * 350) / 1000, false);
            g_synth.vu_kick = (kick_hit == 2) ? 1.0f : 0.78f;
        }

        if (snare_hit > 0) {
            int s_note;
            int s_gate = (SAMPLE_RATE * 350) / 1000;
            if (is_fill_bar && step_idx >= fill_start) {
                s_note = (step_idx == spb - 1) ? GM_LOW_TOM :
                         (step_idx == spb - 2) ? GM_MID_TOM : GM_SNARE_1;
                if (s_note != GM_SNARE_1) s_gate = (SAMPLE_RATE * 650) / 1000;
            } else {
                s_note = (snare_hit == 2) ? GM_SNARE_2 : GM_SNARE_1;
            }
            float s_vel = (snare_hit == 2) ? 0.88f : 0.64f;
            sf2_schedule_note(humanize_micro_delay(2), PB_CH_DRUM_KIT, s_note,
                              humanize_vel(s_vel, 0.05f), s_gate, false);
            g_synth.vu_snare = (snare_hit == 2) ? 1.0f : 0.72f;
        }

        /* Hi-Hat / Ride Cymbal with authentic closed-hat choke */
        if (ophat_hit > 0) {
            if (is_jazz) {
                int r_note = (ophat_hit == 2) ? GM_RIDE_BELL : GM_RIDE_CYM;
                float r_vel = (ophat_hit == 2) ? 0.78f : 0.60f;
                sf2_schedule_note(humanize_micro_delay(2), PB_CH_DRUM_CYM, r_note,
                                  humanize_vel(r_vel, 0.05f), (SAMPLE_RATE * 1100) / 1000, false);
            } else {
                float oh_vel = (ophat_hit == 2) ? 0.78f : 0.62f;
                sf2_schedule_note(humanize_micro_delay(2), PB_CH_DRUM_HAT, GM_OPEN_HAT,
                                  humanize_vel(oh_vel, 0.04f), (SAMPLE_RATE * 900) / 1000, false);
            }
            g_synth.vu_hat = (ophat_hit == 2) ? 0.95f : 0.70f;
        } else if (clhat_hit > 0) {
            int ch_note = is_jazz ? GM_PEDAL_HAT : GM_CLOSED_HAT;
            float ch_vel = (clhat_hit == 2) ? 0.74f : 0.52f;
            /* choke_channel=true cleanly chokes any ringing Open Hi-Hat */
            sf2_schedule_note(humanize_micro_delay(2), PB_CH_DRUM_HAT, ch_note,
                              humanize_vel(ch_vel, 0.05f), (SAMPLE_RATE * 180) / 1000, true);
            g_synth.vu_hat = (clhat_hit == 2) ? 0.75f : 0.52f;
        }
    } else {
        /* Fallback physical-modeling drums */
        if (kick_hit > 0) {
            g_synth.kick_total = (SAMPLE_RATE * 155) / 1000;
            g_synth.kick_rem   = g_synth.kick_total;
            g_synth.kick_phase = 0.0f;
            g_synth.kick_amp   = (kick_hit == 2) ? 0.88f : 0.68f;
            g_synth.vu_kick    = (kick_hit == 2) ? 1.0f  : 0.78f;
        }
        if (snare_hit > 0) {
            g_synth.snare_total  = (SAMPLE_RATE * 165) / 1000;
            g_synth.snare_rem    = g_synth.snare_total;
            g_synth.snare_phase1 = 0.0f;
            g_synth.snare_phase2 = 0.0f;
            g_synth.snare_amp    = (snare_hit == 2) ? 0.76f : 0.54f;
            g_synth.vu_snare     = (snare_hit == 2) ? 1.0f  : 0.72f;
        }
        if (ophat_hit > 0) {
            g_synth.hat_total   = (SAMPLE_RATE * 260) / 1000;
            g_synth.hat_rem     = g_synth.hat_total;
            g_synth.hat_amp     = (ophat_hit == 2) ? 0.42f : 0.30f;
            g_synth.hat_is_open = true;
            g_synth.vu_hat      = (ophat_hit == 2) ? 0.95f : 0.70f;
        } else if (clhat_hit > 0) {
            g_synth.hat_total   = (SAMPLE_RATE * 54) / 1000;
            g_synth.hat_rem     = g_synth.hat_total;
            g_synth.hat_amp     = (clhat_hit == 2) ? 0.38f : 0.25f;
            g_synth.hat_is_open = false;
            g_synth.vu_hat      = (clhat_hit == 2) ? 0.75f : 0.52f;
        }
    }

    /* 2. Trigger Electric / Upright Bass (with Beat-4 '&' anticipation & chromatic walking approach) */
    if (g_mix_mode != MIX_DRUMS_ONLY) {
        bool anticipate = (step_idx == spb - 1) && next_changes && (style_id == 0 || style_id == 2);
        const BarChord *bass_bc = anticipate ? next_bc : cur_bc;
        uint8_t eff_deg = anticipate ? 1 : bass_deg;

        if (eff_deg > 0) {
            int midi_note = compute_bass_midi(g_key_pc, bass_bc, eff_deg);
            if (!anticipate && step_idx >= spb - 2) {
                bool is_last_bass = (step_idx == spb - 1) ||
                                    (step_idx == spb - 2 && g_pattern[style_id][TRK_BASS][spb - 1] == 0);
                if (is_last_bass) {
                    midi_note = compute_walking_approach_bass(g_key_pc, cur_bc, next_bc, midi_note, bar_idx);
                }
            }
            if (midi_note > 0) {
                int sustain_steps = 1;
                for (int look = step_idx + 1; look < spb; look++) {
                    if (g_pattern[style_id][TRK_BASS][look] > 0) break;
                    sustain_steps++;
                }
                if (use_sf2) {
                    float b_vel = (step_idx == 0 || anticipate) ? 0.88f : ((step_idx % 2 == 0) ? 0.78f : 0.68f);
                    int gate_samp = (int)((double)g_synth.step_sample_len * ((double)sustain_steps * 0.90));
                    sf2_release_channel(PB_CH_BASS);
                    sf2_schedule_note(0, PB_CH_BASS, midi_note, humanize_vel(b_vel, 0.04f), gate_samp, false);
                } else {
                    g_synth.bass_total = (SAMPLE_RATE * 350) / 1000;
                    g_synth.bass_rem   = g_synth.bass_total;
                    g_synth.bass_freq  = midi_to_freq(midi_note);
                    g_synth.bass_amp   = (step_idx == 0) ? 0.68f : 0.56f;
                }
                g_synth.vu_bass = (step_idx == 0) ? 0.96f : 0.80f;
            }
        }
    }

    /* 3. Trigger 4-Voice Smooth Voice-Led Chords (with staggered strum & Beat-4 '&' anticipation) */
    if (g_mix_mode == MIX_FULL_BAND) {
        float comp_vel = style_comping_velocity(style_id, st->beats_per_bar, bar_idx, step_idx, next_changes);
        if (comp_vel > 0.0f) {
            bool is_antic = (step_idx == spb - 1) && next_changes;
            const BarChord *chord_bc = is_antic ? next_bc : cur_bc;
            int chord_midi[4];
            compute_voice_led_chord(g_key_pc, chord_bc, chord_midi);

            if (use_sf2) {
                int is_gtr = style_chord_is_guitar(style_id);
                int strum_step = is_gtr ? ((SAMPLE_RATE * 11) / 1000) : ((SAMPLE_RATE * 3) / 1000);
                bool upstroke = is_gtr && ((step_idx % 2) == 1);
                int gate_mult = (step_idx == 0) ? 2 : 1;
                int gate_samp = (int)((double)g_synth.step_sample_len * (double)gate_mult * 0.92);
                int jitter = humanize_micro_delay(2);

                for (int v = 0; v < 4; v++) {
                    sf2_release_channel(PB_CH_CHORD_0 + v);
                }
                for (int v = 0; v < 4; v++) {
                    int voice_idx = upstroke ? (3 - v) : v;
                    int dly = jitter + v * strum_step;
                    float v_vel = humanize_vel(comp_vel - (float)v * 0.03f, 0.04f);
                    sf2_schedule_note(dly, PB_CH_CHORD_0 + voice_idx, chord_midi[voice_idx],
                                      v_vel, gate_samp, false);
                }

                /* Subtle warm Rock/Tonewheel Organ pad layer on Beat 1 for Rock & Slow Blues */
                if (step_idx == 0 && (style_id == 1 || style_id == 2 || style_id == 3)) {
                    sf2_release_channel(PB_CH_ORGAN);
                    int pad_gate = (int)((double)g_synth.step_sample_len * (double)(spb - 1) * 0.88);
                    sf2_schedule_note(0, PB_CH_ORGAN, chord_midi[0], 0.42f, pad_gate, false);
                    sf2_schedule_note(0, PB_CH_ORGAN, chord_midi[1], 0.36f, pad_gate, false);
                    sf2_schedule_note(0, PB_CH_ORGAN, chord_midi[2], 0.36f, pad_gate, false);
                }
            } else {
                for (int v = 0; v < 4; v++) {
                    g_synth.chord_freqs[v] = midi_to_freq(chord_midi[v]);
                    g_synth.chord_strum_delay[v] = v * ((SAMPLE_RATE * 10) / 1000);
                }
                g_synth.chord_total = (SAMPLE_RATE * ((step_idx == 0) ? 340 : 190)) / 1000;
                g_synth.chord_rem   = g_synth.chord_total;
                g_synth.chord_amp   = (step_idx == 0) ? 0.32f : 0.24f;
            }
            g_synth.vu_chord = (step_idx == 0) ? 0.90f : 0.68f;
        }
    }
}

static void init_synth(void) {
    memset(&g_synth, 0, sizeof(g_synth));
    g_synth.lfsr = 0x13579BDFu;
    g_synth.sf2_configured_style = -1;
    g_synth.step_sample_len = compute_step_samples(0);
    g_synth.step_sample_pos = 0;
}

static void init_sf2_engine(void) {
    if (g_sf2_loaded) return;
    /* reverb_wet=0.20 gives lush studio room depth; master_gain=1.35f feeds soft limiter */
    bool ok = rg35_sf2_init(&g_sf2, SAMPLE_RATE, 96, 0.20f, 1.35f);
    g_sf2_loaded = ok;
    if (ok) {
        g_synth.sf2_configured_style = -1;
        sf2_configure_style_instruments(g_style_idx);
    }
}

/* Advance sequencer clock, pending strum/micro-timing events, and note-off gates by 1 sample */
static void sequencer_tick_one_sample(bool playing) {
    /* 1. Fire any delayed micro-timing / staggered strum notes */
    for (int i = 0; i < PB_MAX_PENDING_EV; i++) {
        if (g_synth.pending_ev[i].used) {
            if (--g_synth.pending_ev[i].delay_samples <= 0) {
                PendingMidiEvent ev = g_synth.pending_ev[i];
                g_synth.pending_ev[i].used = false;
                sf2_fire_note_now(ev.channel, ev.key, ev.vel, ev.gate_samples, ev.choke_channel);
            }
        }
    }

    /* 2. Release expired MIDI note gates */
    for (int i = 0; i < PB_MAX_ACTIVE_GATES; i++) {
        if (g_synth.active_gates[i].active) {
            if (--g_synth.active_gates[i].rem_samples <= 0) {
                g_synth.active_gates[i].active = false;
                rg35_sf2_note_off(&g_sf2, g_synth.active_gates[i].channel, g_synth.active_gates[i].key);
            }
        }
    }

    if (!playing) return;

    /* 3. Trigger 8th-note step (sub_step=0) or 16th-note midpoint (sub_step=1) */
    if (g_synth.step_sample_pos == 0) {
        g_synth.substep_16th_fired = false;
        trigger_step_voices(g_style_idx, g_cur_bar, g_cur_step, 0);
    } else if (!g_synth.substep_16th_fired && g_synth.step_sample_pos >= (g_synth.step_sample_len >> 1)) {
        g_synth.substep_16th_fired = true;
        trigger_step_voices(g_style_idx, g_cur_bar, g_cur_step, 1);
    }

    g_synth.step_sample_pos++;
    if (g_synth.step_sample_pos >= g_synth.step_sample_len) {
        g_synth.step_sample_pos = 0;
        int spb   = k_styles[g_style_idx].steps_per_bar;
        int nbars = k_styles[g_style_idx].num_bars;
        int next_step = g_cur_step + 1;
        if (next_step >= spb) {
            next_step = 0;
            g_cur_bar = (g_cur_bar + 1) % nbars;
        }
        g_cur_step = next_step;
        g_synth.step_sample_len = compute_step_samples(g_cur_step);
    }
}

/* Compute how many samples until the next scheduled sequencer/MIDI event (for sub-block SF2 rendering) */
static int samples_until_next_event(bool playing, int max_rem) {
    int n = max_rem;
    for (int i = 0; i < PB_MAX_PENDING_EV; i++) {
        if (g_synth.pending_ev[i].used && g_synth.pending_ev[i].delay_samples < n) {
            n = g_synth.pending_ev[i].delay_samples;
        }
    }
    for (int i = 0; i < PB_MAX_ACTIVE_GATES; i++) {
        if (g_synth.active_gates[i].active && g_synth.active_gates[i].rem_samples < n) {
            n = g_synth.active_gates[i].rem_samples;
        }
    }
    if (playing) {
        if (g_synth.step_sample_pos == 0) return 0;
        uint32_t half = g_synth.step_sample_len >> 1;
        if (!g_synth.substep_16th_fired && g_synth.step_sample_pos <= half) {
            int d = (int)(half - g_synth.step_sample_pos);
            if (d < n) n = d;
        }
        int to_end = (int)(g_synth.step_sample_len - g_synth.step_sample_pos);
        if (to_end < n) n = to_end;
    }
    return (n < 0) ? 0 : n;
}

static void audio_render_frames(int16_t *out, int frames) {
    pthread_mutex_lock(&g_audio_mutex);

    bool playing = g_playing;
    if (!playing && g_synth.was_playing) {
        sf2_clear_queues();
    }
    g_synth.was_playing = playing;

    if (rg35_sf2_ready(&g_sf2)) {
        /* Sample-accurate sub-block SoundFont 2.01 + Stereo Room Reverb rendering */
        int pos = 0;
        while (pos < frames) {
            int rem = frames - pos;
            int until = samples_until_next_event(playing, rem);
            if (until == 0) {
                sequencer_tick_one_sample(playing);
                rg35_sf2_render_i16(&g_sf2, out + pos * 2, 1);
                pos += 1;
            } else {
                int chunk = (until < rem) ? until : rem;
                /* Advance timers across chunk without per-sample branching */
                for (int i = 0; i < PB_MAX_PENDING_EV; i++) {
                    if (g_synth.pending_ev[i].used) g_synth.pending_ev[i].delay_samples -= chunk;
                }
                for (int i = 0; i < PB_MAX_ACTIVE_GATES; i++) {
                    if (g_synth.active_gates[i].active) g_synth.active_gates[i].rem_samples -= chunk;
                }
                if (playing) {
                    g_synth.step_sample_pos += (uint32_t)chunk;
                    if (g_synth.step_sample_pos >= g_synth.step_sample_len) {
                        g_synth.step_sample_pos = 0;
                        int spb   = k_styles[g_style_idx].steps_per_bar;
                        int nbars = k_styles[g_style_idx].num_bars;
                        int next_step = g_cur_step + 1;
                        if (next_step >= spb) {
                            next_step = 0;
                            g_cur_bar = (g_cur_bar + 1) % nbars;
                        }
                        g_cur_step = next_step;
                        g_synth.step_sample_len = compute_step_samples(g_cur_step);
                    }
                }
                rg35_sf2_render_i16(&g_sf2, out + pos * 2, chunk);
                pos += chunk;
            }
        }

        if (!playing) {
            g_synth.vu_kick  *= 0.85f;
            g_synth.vu_snare *= 0.85f;
            g_synth.vu_hat   *= 0.85f;
            g_synth.vu_bass  *= 0.85f;
            g_synth.vu_chord *= 0.85f;
        } else {
            g_synth.vu_kick  *= 0.92f;
            g_synth.vu_snare *= 0.92f;
            g_synth.vu_hat   *= 0.90f;
            g_synth.vu_bass  *= 0.93f;
            g_synth.vu_chord *= 0.92f;
        }
        pthread_mutex_unlock(&g_audio_mutex);
        return;
    }

    /* Fallback: Physical-Modeling DSP Synthesizer + Stereo Room Reverb */
    if (!playing) {
        memset(out, 0, (size_t)(frames * AUDIO_CHANNELS * sizeof(int16_t)));
        g_synth.vu_kick  *= 0.85f;
        g_synth.vu_snare *= 0.85f;
        g_synth.vu_hat   *= 0.85f;
        g_synth.vu_bass  *= 0.85f;
        g_synth.vu_chord *= 0.85f;
        pthread_mutex_unlock(&g_audio_mutex);
        return;
    }

    static const float k_hat_freqs[6] = {
        205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f
    };

    for (int f = 0; f < frames; f++) {
        sequencer_tick_one_sample(true);

        float mix_l = 0.0f;
        float mix_r = 0.0f;

        /* 1. Analog Kick Drum (Center Pan) */
        if (g_synth.kick_rem > 0) {
            float norm = (float)g_synth.kick_rem / (float)g_synth.kick_total;
            float pitch_env = norm * norm * norm;
            float freq = 44.0f + 130.0f * pitch_env;
            int elapsed = g_synth.kick_total - g_synth.kick_rem;
            if (elapsed < (SAMPLE_RATE * 10) / 1000) {
                freq += 95.0f;
            }
            g_synth.kick_phase += freq * (1.0f / (float)SAMPLE_RATE);
            if (g_synth.kick_phase >= 1.0f) g_synth.kick_phase -= 1.0f;

            float sig = sinf(2.0f * (float)M_PI * g_synth.kick_phase);
            if (elapsed < (SAMPLE_RATE * 4) / 1000) {
                sig += 0.35f * next_white_noise();
            }
            float env = norm * norm * g_synth.kick_amp;
            float k_out = tanhf(sig * 1.4f) * env;
            mix_l += k_out;
            mix_r += k_out;
            g_synth.kick_rem--;
        }

        /* 2. Acoustic / 909 Snare Drum (Slightly Left of Center) */
        if (g_synth.snare_rem > 0) {
            float norm = (float)g_synth.snare_rem / (float)g_synth.snare_total;
            g_synth.snare_phase1 += 186.0f * (1.0f / (float)SAMPLE_RATE);
            g_synth.snare_phase2 += 332.0f * (1.0f / (float)SAMPLE_RATE);
            if (g_synth.snare_phase1 >= 1.0f) g_synth.snare_phase1 -= 1.0f;
            if (g_synth.snare_phase2 >= 1.0f) g_synth.snare_phase2 -= 1.0f;

            float body = 0.55f * sinf(2.0f * (float)M_PI * g_synth.snare_phase1) +
                         0.45f * sinf(2.0f * (float)M_PI * g_synth.snare_phase2);
            float body_env = powf(norm, 2.6f);

            float wn = next_white_noise();
            g_synth.snare_bp_z1 += 0.42f * (wn - g_synth.snare_bp_z1 - 0.35f * g_synth.snare_bp_z2);
            g_synth.snare_bp_z2 += 0.42f * g_synth.snare_bp_z1;
            float wires = g_synth.snare_bp_z1 * (norm * norm);

            float s_out = (0.48f * body * body_env + 0.72f * wires) * g_synth.snare_amp;
            mix_l += s_out * 1.04f;
            mix_r += s_out * 0.94f;
            g_synth.snare_rem--;
        }

        /* 3. Closed / Open Hi-Hat & Jazz Ride (Slightly Right of Center) */
        if (g_synth.hat_rem > 0) {
            float norm = (float)g_synth.hat_rem / (float)g_synth.hat_total;
            float sq_sum = 0.0f;
            for (int o = 0; o < 6; o++) {
                g_synth.hat_phases[o] += k_hat_freqs[o] * (1.0f / (float)SAMPLE_RATE);
                if (g_synth.hat_phases[o] >= 1.0f) g_synth.hat_phases[o] -= 1.0f;
                sq_sum += (g_synth.hat_phases[o] < 0.5f) ? 0.16f : -0.16f;
            }
            float raw_in = sq_sum + 0.35f * next_white_noise();
            float hpf = raw_in - g_synth.hat_hp_x1 + 0.18f * g_synth.hat_hp_y1;
            g_synth.hat_hp_x1 = raw_in;
            g_synth.hat_hp_y1 = hpf;

            float env = g_synth.hat_is_open ? (norm * norm) : (norm * norm * norm);
            float h_out = hpf * env * g_synth.hat_amp;
            mix_l += h_out * 0.85f;
            mix_r += h_out * 1.15f;
            g_synth.hat_rem--;
        }

        /* 4. Electric Bass (Center Pan with Dynamic Pluck SVF Low-Pass Filter) */
        if (g_synth.bass_rem > 0) {
            float norm = (float)g_synth.bass_rem / (float)g_synth.bass_total;
            g_synth.bass_phase += g_synth.bass_freq * (1.0f / (float)SAMPLE_RATE);
            if (g_synth.bass_phase >= 1.0f) g_synth.bass_phase -= 1.0f;
            g_synth.bass_sub_phase += (0.5f * g_synth.bass_freq) * (1.0f / (float)SAMPLE_RATE);
            if (g_synth.bass_sub_phase >= 1.0f) g_synth.bass_sub_phase -= 1.0f;

            float saw = 2.0f * g_synth.bass_phase - 1.0f;
            float sub = sinf(2.0f * (float)M_PI * g_synth.bass_sub_phase);

            float cutoff_f = 0.045f + 0.14f * (norm * norm * norm);
            float hp = saw - g_synth.bass_svf_lp - 0.65f * g_synth.bass_svf_bp;
            g_synth.bass_svf_bp += cutoff_f * hp;
            g_synth.bass_svf_lp += cutoff_f * g_synth.bass_svf_bp;

            float b_sig = (0.68f * g_synth.bass_svf_lp + 0.42f * sub) * sqrtf(norm) * g_synth.bass_amp;
            mix_l += b_sig;
            mix_r += b_sig;
            g_synth.bass_rem--;
        }

        /* 5. 4-Voice Stereo Rhythm Guitar / Electric Piano Comping */
        if (g_synth.chord_rem > 0) {
            float norm = (float)g_synth.chord_rem / (float)g_synth.chord_total;
            g_synth.chorus_lfo += 2.2f * (1.0f / (float)SAMPLE_RATE);
            if (g_synth.chorus_lfo >= 1.0f) g_synth.chorus_lfo -= 1.0f;
            float lfo = 0.0018f * sinf(2.0f * (float)M_PI * g_synth.chorus_lfo);

            float ch_l = 0.0f;
            float ch_r = 0.0f;
            for (int v = 0; v < 4; v++) {
                if (g_synth.chord_strum_delay[v] > 0) {
                    g_synth.chord_strum_delay[v]--;
                    continue;
                }
                float detune = (v & 1) ? (1.0f + lfo) : (1.0f - lfo);
                g_synth.chord_phases[v] += (g_synth.chord_freqs[v] * detune) * (1.0f / (float)SAMPLE_RATE);
                if (g_synth.chord_phases[v] >= 1.0f) g_synth.chord_phases[v] -= 1.0f;

                float ph = g_synth.chord_phases[v];
                float raw = 0.60f * sinf(2.0f * (float)M_PI * ph) +
                            0.28f * sinf(4.0f * (float)M_PI * ph) +
                            0.12f * (2.0f * ph - 1.0f);
                float alpha = 0.12f + 0.25f * (norm * norm);
                g_synth.chord_lp[v] += alpha * (raw - g_synth.chord_lp[v]);
                float v_out = g_synth.chord_lp[v] * 0.25f;

                float pan = 0.25f + 0.25f * (float)v;
                ch_l += v_out * (1.0f - pan * 0.55f);
                ch_r += v_out * (0.45f + pan * 0.55f);
            }
            float env = norm * norm * g_synth.chord_amp;
            mix_l += ch_l * env;
            mix_r += ch_r * env;
            g_synth.chord_rem--;
        }

        /* Stereo Freeverb room reverb + 20 Hz DC blocker + soft-clip master limiter */
        float out_l, out_r;
        rg35_sf2_fx_sample(&g_sf2, mix_l, mix_r, &out_l, &out_r);

        int32_t il = (int32_t)(out_l * 31000.0f);
        int32_t ir = (int32_t)(out_r * 31000.0f);
        if (il >  32767) il =  32767;
        if (il < -32767) il = -32767;
        if (ir >  32767) ir =  32767;
        if (ir < -32767) ir = -32767;

        out[f * 2 + 0] = (int16_t)il;
        out[f * 2 + 1] = (int16_t)ir;
    }

    g_synth.vu_kick  *= 0.92f;
    g_synth.vu_snare *= 0.92f;
    g_synth.vu_hat   *= 0.90f;
    g_synth.vu_bass  *= 0.93f;
    g_synth.vu_chord *= 0.92f;
    pthread_mutex_unlock(&g_audio_mutex);
}

/* ============================================================================
 * 6. PLATFORM BACKENDS (RG35XXSP FBDEV+EVDEV+ALSA vs LINUX X11+PULSE/ALSA)
 * ============================================================================ */

static pthread_t     g_audio_thread;
static volatile bool g_audio_running = false;

/* Dynamic ALSA (libasound.so.2) types for background thread */
typedef struct _snd_pcm snd_pcm_t;
typedef int (*snd_pcm_open_fn)(snd_pcm_t **, const char *, int, int);
typedef int (*snd_pcm_set_params_fn)(snd_pcm_t *, int, int, unsigned int, unsigned int, int, unsigned int);
typedef long (*snd_pcm_writei_fn)(snd_pcm_t *, const void *, unsigned long);
typedef int (*snd_pcm_recover_fn)(snd_pcm_t *, int, int);
typedef int (*snd_pcm_close_fn)(snd_pcm_t *);

/* Dynamic PulseAudio Simple (libpulse-simple.so.0) fallback for desktop */
typedef struct pa_simple pa_simple;
typedef struct {
    int      format;
    uint32_t rate;
    uint8_t  channels;
} pa_sample_spec;
typedef pa_simple *(*pa_simple_new_fn)(const char *, const char *, int, const char *,
                                       const char *, const pa_sample_spec *,
                                       const void *, const void *, int *);
typedef int (*pa_simple_write_fn)(pa_simple *, const void *, size_t, int *);
typedef void (*pa_simple_free_fn)(pa_simple *);

static void *audio_thread_worker(void *arg) {
    (void)arg;
    int16_t period_buf[AUDIO_FRAMES * AUDIO_CHANNELS];

    /* Load studio SoundFont (GeneralUser-GS.sf2 -> pocketband.sf2) on background thread */
    pthread_mutex_lock(&g_audio_mutex);
    init_sf2_engine();
    pthread_mutex_unlock(&g_audio_mutex);

    /* 1. Try PulseAudio Simple first on Desktop X11 */
    void *pa_lib = NULL;
    pa_simple *pa = NULL;
    pa_simple_write_fn fn_pa_write = NULL;
    pa_simple_free_fn  fn_pa_free  = NULL;

#if defined(HOST_X11)
    pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    if (pa_lib) {
        pa_simple_new_fn fn_pa_new = (pa_simple_new_fn)dlsym(pa_lib, "pa_simple_new");
        fn_pa_write = (pa_simple_write_fn)dlsym(pa_lib, "pa_simple_write");
        fn_pa_free  = (pa_simple_free_fn)dlsym(pa_lib, "pa_simple_free");
        if (fn_pa_new && fn_pa_write && fn_pa_free) {
            pa_sample_spec ss = { 3 /* PA_SAMPLE_S16LE */, SAMPLE_RATE, AUDIO_CHANNELS };
            int err = 0;
            pa = fn_pa_new(NULL, "RG35XXSP PocketBand", 1 /* PLAYBACK */, NULL,
                           "PocketBand Backing Synth", &ss, NULL, NULL, &err);
        }
    }
#endif

    /* 2. Try ALSA (libasound.so.2) on RG 35xxSP or if PulseAudio is unavailable */
    void *alsa_lib = NULL;
    snd_pcm_t *pcm = NULL;
    snd_pcm_writei_fn  fn_pcm_writei  = NULL;
    snd_pcm_recover_fn fn_pcm_recover = NULL;
    snd_pcm_close_fn   fn_pcm_close   = NULL;

    if (!pa) {
        alsa_lib = dlopen("libasound.so.2", RTLD_LAZY);
        if (alsa_lib) {
            snd_pcm_open_fn       fn_open   = (snd_pcm_open_fn)dlsym(alsa_lib, "snd_pcm_open");
            snd_pcm_set_params_fn fn_params = (snd_pcm_set_params_fn)dlsym(alsa_lib, "snd_pcm_set_params");
            fn_pcm_writei  = (snd_pcm_writei_fn)dlsym(alsa_lib, "snd_pcm_writei");
            fn_pcm_recover = (snd_pcm_recover_fn)dlsym(alsa_lib, "snd_pcm_recover");
            fn_pcm_close   = (snd_pcm_close_fn)dlsym(alsa_lib, "snd_pcm_close");
            if (fn_open && fn_params && fn_pcm_writei) {
                if (fn_open(&pcm, "default", 0 /* PLAYBACK */, 0) < 0) {
                    fn_open(&pcm, "plughw:0,0", 0, 0);
                }
                if (pcm) {
                    /* SND_PCM_FORMAT_S16_LE = 2, SND_PCM_ACCESS_RW_INTERLEAVED = 3 */
                    fn_params(pcm, 2, 3, AUDIO_CHANNELS, SAMPLE_RATE, 1, 25000);
                }
            }
        }
    }

    while (g_audio_running) {
        audio_render_frames(period_buf, AUDIO_FRAMES);
        if (pa && fn_pa_write) {
            int err = 0;
            fn_pa_write(pa, period_buf, sizeof(period_buf), &err);
        } else if (pcm && fn_pcm_writei) {
            long r = fn_pcm_writei(pcm, period_buf, AUDIO_FRAMES);
            if (r < 0 && fn_pcm_recover) {
                fn_pcm_recover(pcm, (int)r, 1);
            }
        } else {
            usleep((AUDIO_FRAMES * 1000000ULL) / SAMPLE_RATE);
        }
    }

    if (pa && fn_pa_free)   fn_pa_free(pa);
    if (pa_lib)             dlclose(pa_lib);
    if (pcm && fn_pcm_close) fn_pcm_close(pcm);
    if (alsa_lib)           dlclose(alsa_lib);

    pthread_mutex_lock(&g_audio_mutex);
    rg35_sf2_free(&g_sf2);
    g_sf2_loaded = false;
    pthread_mutex_unlock(&g_audio_mutex);
    return NULL;
}

#if defined(TARGET_RG35XXSP)

static int                     g_fb_fd   = -1;
static uint8_t                *g_fb_mem  = NULL;
static size_t                  g_fb_size = 0;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;

#define MAX_EVDEV_FDS 16
static int      g_ev_fds[MAX_EVDEV_FDS];
static int      g_num_ev_fds = 0;
static uint32_t g_held_btns  = 0;

static void init_evdev(void) {
    g_num_ev_fds = 0;
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && g_num_ev_fds < MAX_EVDEV_FDS) {
        if (strncmp(de->d_name, "event", 5) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                g_ev_fds[g_num_ev_fds++] = fd;
            }
        }
    }
    closedir(dir);
}

static int platform_init(void) {
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd >= 0) {
        if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) == 0 &&
            ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) == 0) {
            g_fb_size = g_finfo.smem_len;
            g_fb_mem = (uint8_t *)mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
            if (g_fb_mem == MAP_FAILED) g_fb_mem = NULL;
        }
    }
    init_evdev();
    rg35xxsp_keepawake_init("pocketband", NULL);
    g_audio_running = true;
    pthread_create(&g_audio_thread, NULL, audio_thread_worker, NULL);
    return 0;
}

static void platform_present(void) {
    rg35xxsp_keepawake_tick_now();
    if (!g_fb_mem) return;
    int dst_w   = (int)g_vinfo.xres;
    int dst_h   = (int)g_vinfo.yres;
    int bpp     = (int)g_vinfo.bits_per_pixel;
    int stride  = (int)g_finfo.line_length;
    uint8_t *base = g_fb_mem + g_vinfo.yoffset * stride + g_vinfo.xoffset * (bpp / 8);

    int copy_w = (dst_w < NATIVE_W) ? dst_w : NATIVE_W;
    int copy_h = (dst_h < NATIVE_H) ? dst_h : NATIVE_H;

    if (bpp == 32) {
        for (int y = 0; y < copy_h; y++) {
            memcpy(base + y * stride, &g_fb[y * NATIVE_W], (size_t)copy_w * 4);
        }
    } else if (bpp == 16) {
        for (int y = 0; y < copy_h; y++) {
            uint16_t *drow = (uint16_t *)(base + y * stride);
            const uint32_t *srow = &g_fb[y * NATIVE_W];
            for (int x = 0; x < copy_w; x++) {
                uint32_t c = srow[x];
                uint16_t r = (uint16_t)((c >> 19) & 0x1F);
                uint16_t g = (uint16_t)((c >> 10) & 0x3F);
                uint16_t b = (uint16_t)((c >> 3)  & 0x1F);
                drow[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }
}

static uint32_t platform_poll_pressed(void) {
    uint32_t prev = g_held_btns;
    for (int i = 0; i < g_num_ev_fds; i++) {
        struct input_event ev;
        while (read(g_ev_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                uint32_t mask = rg35xxsp_map_evkey(ev.code);
                if (ev.value == 1) {
                    if (mask & BTN_LID_CLOSE) { rg35xxsp_keepawake_set_lid_closed(true);  continue; }
                    if (mask & BTN_LID_OPEN)  { rg35xxsp_keepawake_set_lid_closed(false); continue; }
                }
                if (mask) {
                    if (ev.value > 0) g_held_btns |= mask;
                    else              g_held_btns &= ~mask;
                }
            } else if (ev.type == EV_ABS) {
                g_held_btns = rg35xxsp_update_evabs(g_held_btns, ev.code, ev.value, 0, 0);
            }
        }
    }
    return g_held_btns & ~prev;
}

static void platform_shutdown(void) {
    rg35xxsp_keepawake_shutdown();
    if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
    for (int i = 0; i < g_num_ev_fds; i++) close(g_ev_fds[i]);
    if (g_fb_mem) munmap(g_fb_mem, g_fb_size);
    if (g_fb_fd >= 0) close(g_fb_fd);
}

#elif defined(HOST_X11)

static Display *g_dpy = NULL;
static Window   g_win = 0;
static GC       g_gc  = 0;
static XImage  *g_img = NULL;
static Atom     g_wm_delete = 0;
static char    *g_x11_pix = NULL;

static int platform_init(void) {
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "Failed to open X11 display\n");
        return -1;
    }
    int scr = DefaultScreen(g_dpy);
    g_win = XCreateSimpleWindow(
        g_dpy, RootWindow(g_dpy, scr),
        100, 100, NATIVE_W, NATIVE_H, 1,
        BlackPixel(g_dpy, scr), BlackPixel(g_dpy, scr)
    );

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width  = hints.max_width  = NATIVE_W;
    hints.min_height = hints.max_height = NATIVE_H;
    XSetWMNormalHints(g_dpy, g_win, &hints);

    XStoreName(g_dpy, g_win, "Anbernic RG35XXSP - PocketBand (640x480)");
    XSelectInput(g_dpy, g_win, KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask);
    g_wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wm_delete, 1);
    XMapRaised(g_dpy, g_win);
    g_gc = DefaultGC(g_dpy, scr);

    g_x11_pix = (char *)calloc((size_t)(NATIVE_W * NATIVE_H), 4);
    g_img = XCreateImage(
        g_dpy, DefaultVisual(g_dpy, scr),
        DefaultDepth(g_dpy, scr), ZPixmap, 0,
        g_x11_pix, NATIVE_W, NATIVE_H, 32, 0
    );

    g_audio_running = true;
    pthread_create(&g_audio_thread, NULL, audio_thread_worker, NULL);
    return 0;
}

static void platform_present(void) {
    if (!g_dpy || !g_img) return;
    memcpy(g_x11_pix, g_fb, sizeof(g_fb));
    XPutImage(g_dpy, g_win, g_gc, g_img, 0, 0, 0, 0, NATIVE_W, NATIVE_H);
    XFlush(g_dpy);
}

static uint32_t platform_poll_pressed(void) {
    uint32_t pressed = 0;
    while (XPending(g_dpy) > 0) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == g_wm_delete) {
            pressed |= BTN_QUIT;
        } else if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            pressed |= rg35xxsp_map_x11_keysym(ks);
        }
    }
    return pressed;
}

static void platform_shutdown(void) {
    if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
    if (g_img) {
        XDestroyImage(g_img);
        g_img = NULL;
    }
    if (g_dpy) {
        if (g_win) XDestroyWindow(g_dpy, g_win);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }
}

#endif

/* ============================================================================
 * 7. PERSISTENT CONFIGURATION (/mnt/mod/ports/pocketband/.pocketband.cfg)
 * ============================================================================ */

static void init_default_patterns(void) {
    for (int s = 0; s < NUM_STYLES; s++) {
        for (int t = 0; t < NUM_TRACKS; t++) {
            for (int st = 0; st < MAX_STEPS; st++) {
                g_pattern[s][t][st] = k_styles[s].default_steps[t][st];
            }
        }
    }
}

static void apply_style(int style_idx) {
    style_idx = ((style_idx % NUM_STYLES) + NUM_STYLES) % NUM_STYLES;
    pthread_mutex_lock(&g_audio_mutex);
    g_style_idx = style_idx;
    g_bpm       = k_styles[style_idx].default_bpm;
    g_swing_pct = k_styles[style_idx].swing_pct;
    g_cur_bar   = 0;
    g_cur_step  = 0;
    if (g_edit_step >= k_styles[style_idx].steps_per_bar) {
        g_edit_step = 0;
    }
    g_synth.step_sample_pos = 0;
    g_synth.step_sample_len = compute_step_samples(0);
    g_synth.substep_16th_fired = false;
    g_synth.prev_voicing_valid = false;
    sf2_clear_queues();
    if (rg35_sf2_ready(&g_sf2)) {
        sf2_configure_style_instruments(style_idx);
    }
    pthread_mutex_unlock(&g_audio_mutex);
}

static void resolve_config_path(void) {
    const char *env_cfg = getenv("POCKETBAND_CFG");
    if (env_cfg && env_cfg[0] != '\0') {
        snprintf(g_config_path, sizeof(g_config_path), "%s", env_cfg);
        return;
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "%s/.pocketband_rg35xxsp.cfg", home);
    } else {
        snprintf(g_config_path, sizeof(g_config_path), "./.pocketband_rg35xxsp.cfg");
    }
}

static void save_config(void) {
    if (g_config_path[0] == '\0') return;
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "style_idx=%d\n", g_style_idx);
    fprintf(f, "key_pc=%d\n", g_key_pc);
    fprintf(f, "bpm=%d\n", g_bpm);
    fprintf(f, "swing_pct=%d\n", g_swing_pct);
    fprintf(f, "mix_mode=%d\n", (int)g_mix_mode);
    fprintf(f, "auto_fill=%d\n", g_auto_fill ? 1 : 0);
    fprintf(f, "neck_notes=%d\n", g_neck_notes ? 1 : 0);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void load_config(void) {
    init_default_patterns();
    apply_style(0);
    if (g_config_path[0] == '\0') return;
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        if (sscanf(line, "style_idx=%d", &v) == 1 && v >= 0 && v < NUM_STYLES) {
            apply_style(v);
        } else if (sscanf(line, "key_pc=%d", &v) == 1 && v >= 0 && v < 12) {
            g_key_pc = v;
        } else if (sscanf(line, "bpm=%d", &v) == 1 && v >= 45 && v <= 220) {
            g_bpm = v;
        } else if (sscanf(line, "swing_pct=%d", &v) == 1 && v >= 50 && v <= 68) {
            g_swing_pct = v;
        } else if (sscanf(line, "mix_mode=%d", &v) == 1 && v >= 0 && v <= 2) {
            g_mix_mode = (BackingMixMode)v;
        } else if (sscanf(line, "auto_fill=%d", &v) == 1) {
            g_auto_fill = (v != 0);
        } else if (sscanf(line, "neck_notes=%d", &v) == 1) {
            g_neck_notes = (v != 0);
        }
    }
    fclose(f);
}

/* ============================================================================
 * 8. NATIVE 640x480 UI RENDERING: TOP BAR, JAM VIEW, STEP EDITOR & HELP MODAL
 * ============================================================================ */

static void render_top_status_bar(void) {
    const StylePreset *st = &k_styles[g_style_idx];
    fill_rect(0, 0, NATIVE_W, 42, RGB(21, 30, 48));
    fill_rect(0, 40, NATIVE_W, 2, RGB(56, 78, 112));

    /* 1. Mode Badge (SELECT) */
    draw_pill(8, 6, 128, 28,
              (g_view == VIEW_JAM) ? RGB(37, 99, 235) : RGB(124, 58, 237),
              (g_view == VIEW_JAM) ? RGB(147, 197, 253) : RGB(196, 181, 253));
    draw_text_centered(FONT_UI_13, 8, 6, 128, 28,
                       (g_view == VIEW_JAM) ? "SEL: JAM VIEW" : "SEL: STEP EDIT",
                       RGB(255, 255, 255));

    /* 2. Style / Genre Pill */
    draw_pill(142, 6, 198, 28, RGB(30, 41, 59), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, 142, 6, 198, 28, st->name, RGB(254, 240, 138));

    /* 3. Key Badge */
    char key_str[24];
    snprintf(key_str, sizeof(key_str), "KEY %s", k_note_names[g_key_pc]);
    draw_pill(346, 6, 86, 28, RGB(124, 45, 18), RGB(251, 146, 60));
    draw_text_centered(FONT_UI_13, 346, 6, 86, 28, key_str, RGB(255, 255, 255));

    /* 4. Tempo BPM Badge */
    char bpm_str[24];
    snprintf(bpm_str, sizeof(bpm_str), "%d BPM", g_bpm);
    draw_pill(438, 6, 102, 28, RGB(23, 37, 58), RGB(71, 98, 145));
    draw_text_centered(FONT_UI_13, 438, 6, 102, 28, bpm_str, RGB(186, 230, 253));

    /* 5. Play / Stop Transport Badge */
    draw_pill(546, 6, 86, 28,
              g_playing ? RGB(16, 185, 129) : RGB(180, 83, 9),
              g_playing ? RGB(110, 231, 183) : RGB(251, 191, 36));
    draw_text_centered(FONT_UI_13, 546, 6, 86, 28,
                       g_playing ? "PLAYING" : "STOPPED", RGB(255, 255, 255));
}

static void render_step_strip_and_footer(void) {
    const StylePreset *st = &k_styles[g_style_idx];
    int spb = st->steps_per_bar;

    /* Beat & '&' Step Strip: y = 382..434 (52px) */
    int sy = 382, sh = 52;
    fill_rect(8, sy, NATIVE_W - 16, sh, RGB(19, 27, 43));
    draw_border(8, sy, NATIVE_W - 16, sh, 2, RGB(45, 62, 92));

    char meter_lbl[24];
    snprintf(meter_lbl, sizeof(meter_lbl), "%d/4 METER", st->beats_per_bar);
    draw_text(FONT_UI_13, 18, sy + 9, meter_lbl, RGB(250, 204, 21));
    char swg_lbl[24];
    snprintf(swg_lbl, sizeof(swg_lbl), "Swing %d%%", g_swing_pct);
    draw_text(FONT_UI_11, 18, sy + 29, swg_lbl, RGB(148, 163, 184));

    int strip_x = 118;
    int avail_w = NATIVE_W - strip_x - 110;
    int box_w   = (avail_w - (spb - 1) * 6) / spb;

    for (int s = 0; s < spb; s++) {
        int bx = strip_x + s * (box_w + 6);
        bool active  = g_playing && (g_cur_step == s);
        bool is_beat = (s % 2 == 0);

        uint32_t fill = active ? (is_beat ? RGB(245, 158, 11) : RGB(16, 185, 129))
                               : (is_beat ? RGB(30, 43, 68)   : RGB(22, 31, 49));
        uint32_t bdr  = active ? RGB(255, 255, 255)
                               : (is_beat ? RGB(96, 165, 250) : RGB(51, 68, 98));
        draw_pill(bx, sy + 7, box_w, sh - 14, fill, bdr);

        char slbl[16];
        if (is_beat) snprintf(slbl, sizeof(slbl), "%d", (s / 2) + 1);
        else         snprintf(slbl, sizeof(slbl), "&");
        draw_text_centered(FONT_UI_16, bx, sy + 7, box_w, sh - 14, slbl,
                           active ? RGB(15, 23, 42) : (is_beat ? RGB(248, 250, 252) : RGB(148, 163, 184)));
    }

    /* Drum Fill Status Pill on Right of Step Strip */
    bool fill_now = (((g_cur_bar + 1) % 4 == 0 && g_auto_fill) || g_fill_queued);
    draw_pill(NATIVE_W - 96, sy + 7, 80, sh - 14,
              fill_now ? RGB(190, 24, 93) : RGB(28, 38, 58),
              fill_now ? RGB(244, 114, 182) : RGB(64, 82, 115));
    draw_text_centered(FONT_UI_13, NATIVE_W - 96, sy + 7, 80, sh - 14,
                       fill_now ? "FILL!" : "B: FILL", RGB(255, 255, 255));

    /* Bottom Control Bar: y = 438..480 (42px) */
    int by = 438, bh = 42;
    fill_rect(0, by, NATIVE_W, bh, RGB(15, 22, 35));
    fill_rect(0, by, NATIVE_W, 2, RGB(45, 62, 92));

    const char *mix_lbl = (g_mix_mode == MIX_FULL_BAND) ? "X: Full Band" :
                          (g_mix_mode == MIX_DRUM_BASS) ? "X: Drums+Bass" :
                                                          "X: Drums Only";
    draw_pill(8, by + 7, 134, 28, RGB(23, 52, 74), RGB(56, 189, 248));
    draw_text_centered(FONT_UI_13, 8, by + 7, 134, 28, mix_lbl, RGB(224, 242, 254));

    const char *ctrl_txt = (g_view == VIEW_JAM)
        ? "A: Play/Stop  |  L/R: Style  |  U/D: Key  |  L1/R1: BPM  |  L2: Neck"
        : "A: Cycle Hit/Note  |  B: Clear  |  DPad: Move Cursor  |  L1/R1: BPM";
    draw_text_centered(FONT_UI_11, 146, by + 7, NATIVE_W - 242, 28, ctrl_txt, RGB(203, 213, 225));

    draw_pill(NATIVE_W - 92, by + 7, 84, 28, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, NATIVE_W - 92, by + 7, 84, 28, "Y: Help", RGB(254, 240, 138));
}

static void render_jam_view(void) {
    const StylePreset *st = &k_styles[g_style_idx];
    int bar      = g_cur_bar % st->num_bars;
    int next_bar = (bar + 1) % st->num_bars;

    const BarChord *cur_bc  = &st->bars[bar];
    const BarChord *next_bc = &st->bars[next_bar];

    char cur_chord_str[24], next_chord_str[24];
    format_chord_label(g_key_pc, cur_bc, cur_chord_str, sizeof(cur_chord_str));
    format_chord_label(g_key_pc, next_bc, next_chord_str, sizeof(next_chord_str));

    /* 1. Left Card: NOW PLAYING CHORD, CHORD TONES & BASS WALK (x=8..316, y=48..178) */
    int cy = 48, ch = 130;
    fill_rect(8, cy, 308, ch, RGB(21, 30, 48));
    draw_border(8, cy, 308, ch, 2, RGB(56, 189, 248));

    char bar_hdr[40];
    snprintf(bar_hdr, sizeof(bar_hdr), "NOW PLAYING: BAR %d OF %d", bar + 1, st->num_bars);
    draw_text(FONT_UI_13, 18, cy + 8, bar_hdr, RGB(125, 211, 252));

    draw_pill(232, cy + 6, 74, 24, RGB(146, 92, 12), RGB(250, 204, 21));
    draw_text_centered(FONT_UI_13, 232, cy + 6, 74, 24, cur_bc->roman, RGB(255, 255, 255));

    /* Hero Chord Pill on Left + Chord Tones Card on Right of Left Card */
    draw_pill(18, cy + 36, 132, 56, RGB(15, 23, 38), RGB(250, 204, 21));
    draw_text_centered(FONT_TITLE_28, 18, cy + 36, 132, 56, cur_chord_str, RGB(250, 204, 21));

    int root_pc  = (g_key_pc + cur_bc->root_offset) % 12;
    int third_pc = (root_pc + ((cur_bc->qual == CHQ_MIN || cur_bc->qual == CHQ_MIN7) ? 3 : 4)) % 12;
    int fifth_pc = (root_pc + 7) % 12;
    char tones_str[48];
    if (cur_bc->qual == CHQ_DOM7 || cur_bc->qual == CHQ_MAJ7 || cur_bc->qual == CHQ_MIN7) {
        int sev_pc = (root_pc + ((cur_bc->qual == CHQ_MAJ7) ? 11 : 10)) % 12;
        snprintf(tones_str, sizeof(tones_str), "%s - %s - %s - %s",
                 k_note_names[root_pc], k_note_names[third_pc],
                 k_note_names[fifth_pc], k_note_names[sev_pc]);
    } else {
        snprintf(tones_str, sizeof(tones_str), "%s - %s - %s",
                 k_note_names[root_pc], k_note_names[third_pc], k_note_names[fifth_pc]);
    }
    fill_rect(158, cy + 36, 148, 56, RGB(15, 23, 38));
    draw_border(158, cy + 36, 148, 56, 1, RGB(51, 68, 98));
    draw_text_centered(FONT_UI_11, 158, cy + 42, 148, 16, "CHORD TONES", RGB(148, 163, 184));
    draw_text_centered(FONT_UI_13, 158, cy + 62, 148, 22, tones_str, RGB(248, 250, 252));

    /* Active Bassline Sequence Strip at bottom of Left Card */
    char bass_seq[64] = "Bass Walk:  ";
    for (int s = 0; s < st->steps_per_bar; s++) {
        uint8_t d = g_pattern[g_style_idx][TRK_BASS][s];
        strncat(bass_seq, k_bass_deg_labels[d], sizeof(bass_seq) - strlen(bass_seq) - 1);
        if (s + 1 < st->steps_per_bar) {
            strncat(bass_seq, "  ", sizeof(bass_seq) - strlen(bass_seq) - 1);
        }
    }
    draw_text(FONT_UI_13, 18, cy + 100, bass_seq, RGB(110, 231, 183));

    /* 2. Right Card: NEXT CHORD & 4-BAR PROGRESSION (x=324..632, y=48..178) */
    fill_rect(324, cy, 308, ch, RGB(21, 30, 48));
    draw_border(324, cy, 308, ch, 2, RGB(51, 68, 98));

    char next_hdr[64];
    snprintf(next_hdr, sizeof(next_hdr), "NEXT (BAR %d): %s (%s)",
             next_bar + 1, next_chord_str, next_bc->roman);
    draw_text(FONT_UI_13, 334, cy + 8, next_hdr, RGB(250, 204, 21));

    uint8_t cur_bass_deg = g_pattern[g_style_idx][TRK_BASS][g_cur_step % st->steps_per_bar];
    char b_pitch[32], b_line[64];
    format_bass_pitch_name(g_key_pc, cur_bc, cur_bass_deg, b_pitch, sizeof(b_pitch));
    snprintf(b_line, sizeof(b_line), "Step %d Bass: %s", (g_cur_step % st->steps_per_bar) + 1, b_pitch);
    draw_text(FONT_UI_13, 334, cy + 32, b_line, RGB(186, 230, 253));

    /* 4-Bar Progression Window Preview inside Right Card */
    int win_start = (bar / 4) * 4;
    int cell_w    = 68;
    for (int i = 0; i < 4; i++) {
        int bidx = (win_start + i) % st->num_bars;
        char cstr[16];
        format_chord_label(g_key_pc, &st->bars[bidx], cstr, sizeof(cstr));
        bool is_cur = (bidx == bar);
        int cx = 334 + i * (cell_w + 5);
        draw_pill(cx, cy + 60, cell_w, 60,
                  is_cur ? RGB(37, 99, 235) : RGB(15, 22, 34),
                  is_cur ? RGB(250, 204, 21) : RGB(51, 68, 98));
        draw_text_centered(FONT_UI_16, cx, cy + 64, cell_w, 26, cstr,
                           is_cur ? RGB(254, 240, 138) : RGB(226, 232, 240));
        char rstr[24];
        snprintf(rstr, sizeof(rstr), "B%d:%s", bidx + 1, st->bars[bidx].roman);
        draw_text_centered(FONT_UI_11, cx, cy + 92, cell_w, 20, rstr,
                           is_cur ? RGB(186, 230, 253) : RGB(120, 136, 158));
    }

    /* 3. Hero 12-Fret Guitar Soloing Scale Map (x=8..632, y=184..376, height=192px) */
    int fy = 184, fh = 192;
    fill_rect(8, fy, NATIVE_W - 16, fh, RGB(21, 30, 48));
    draw_border(8, fy, NATIVE_W - 16, fh, 2, RGB(51, 68, 98));

    char sc_hdr[112];
    snprintf(sc_hdr, sizeof(sc_hdr), "SOLO SCALE: %s %s", k_note_names[g_key_pc], st->scale_name);
    draw_text(FONT_UI_13, 18, fy + 6, sc_hdr, RGB(250, 204, 21));

    /* Color Legend on top-right of Scale Card */
    draw_circle_aa(468.0f, (float)(fy + 15), 6.0f, 1.5f, RGB(245, 158, 11), RGB(254, 240, 138));
    draw_text(FONT_UI_11, 478, fy + 8, "Key", RGB(254, 240, 138));
    draw_circle_aa(518.0f, (float)(fy + 15), 6.0f, 1.5f, RGB(5, 150, 105), RGB(167, 243, 208));
    draw_text(FONT_UI_11, 528, fy + 8, "Chord", RGB(167, 243, 208));
    draw_circle_aa(580.0f, (float)(fy + 15), 6.0f, 1.5f, RGB(219, 39, 119), RGB(251, 207, 232));
    draw_text(FONT_UI_11, 590, fy + 8, "Blue", RGB(251, 207, 232));

    int nut_x  = 66;
    int fret_w = 45; /* 12 frets * 45px = 540px (x = 66..606) */
    int neck_w = fret_w * 12;
    int str_y0 = fy + 48;
    int str_dy = 25; /* 5 spaces * 25px = 125px */
    int neck_h = str_dy * 5;

    /* Rosewood fingerboard + bone nut + fret wires */
    fill_rect(nut_x, str_y0 - 10, neck_w, neck_h + 20, RGB(38, 26, 20));
    draw_border(nut_x, str_y0 - 10, neck_w, neck_h + 20, 2, RGB(92, 64, 44));
    fill_rect(nut_x - 5, str_y0 - 10, 6, neck_h + 20, RGB(241, 245, 249));

    for (int f = 0; f <= 12; f++) {
        int fx = nut_x + f * fret_w;
        if (f > 0) {
            fill_rect(fx - 1, str_y0 - 9, 2, neck_h + 18, RGB(148, 163, 184));
        }
        int cx = (f == 0) ? 46 : (nut_x + f * fret_w - fret_w / 2);
        char fn[8];
        snprintf(fn, sizeof(fn), "%d", f);
        bool marker = (f == 0 || f == 3 || f == 5 || f == 7 || f == 9 || f == 12);
        draw_text_centered(FONT_UI_11, cx - 12, fy + 25, 24, 14, fn,
                           marker ? RGB(250, 204, 21) : RGB(148, 163, 184));

        if (f == 3 || f == 5 || f == 7 || f == 9) {
            draw_circle_aa((float)cx, (float)(str_y0 + neck_h / 2), 4.5f, 1.0f,
                           RGB(82, 75, 68), RGB(120, 113, 108));
        } else if (f == 12) {
            draw_circle_aa((float)cx, (float)(str_y0 + str_dy * 1 + 12), 4.5f, 1.0f,
                           RGB(82, 75, 68), RGB(120, 113, 108));
            draw_circle_aa((float)cx, (float)(str_y0 + str_dy * 3 + 12), 4.5f, 1.0f,
                           RGB(82, 75, 68), RGB(120, 113, 108));
        }
    }

    int cur_chord_pc = (g_key_pc + cur_bc->root_offset) % 12;

    /* 6 guitar strings + labeled anti-aliased scale/chord/root circles */
    for (int r = 0; r < 6; r++) {
        int sy = str_y0 + r * str_dy;
        int thick = (r >= 3) ? 2 : 1;
        fill_rect(nut_x, sy - thick / 2, neck_w, thick,
                  (r >= 3) ? RGB(203, 213, 225) : RGB(148, 163, 184));
        draw_text(FONT_UI_13, 18, sy - 9, k_gtr_str_lbl[r], RGB(186, 230, 253));

        int open_pc = k_gtr_str_pc[r];
        for (int f = 0; f <= 12; f++) {
            int pc  = (open_pc + f) % 12;
            int rel = (pc - g_key_pc + 12) % 12;
            uint8_t mask = st->scale_mask[rel];
            if (mask > 0) {
                int cx = (f == 0) ? 46 : (nut_x + f * fret_w - fret_w / 2);
                bool is_key_root   = (pc == g_key_pc);
                bool is_chord_root = (pc == cur_chord_pc);
                uint32_t dfill = is_key_root   ? RGB(245, 158, 11) :
                                 is_chord_root ? RGB(5, 150, 105)  :
                                 (mask == 2)   ? RGB(219, 39, 119) : RGB(2, 132, 199);
                uint32_t dbdr  = is_key_root   ? RGB(254, 240, 138) :
                                 is_chord_root ? RGB(167, 243, 208) : RGB(224, 242, 254);
                uint32_t tcol  = is_key_root   ? RGB(15, 23, 42)    : RGB(255, 255, 255);

                draw_circle_aa((float)cx, (float)sy, 10.5f, 1.8f, dfill, dbdr);
                const char *dlbl = g_neck_notes ? k_note_names[pc] : k_interval_labels[rel];
                draw_text_centered(FONT_UI_11, cx - 10, sy - 10, 20, 20, dlbl, tcol);
            }
        }
    }
}

static void render_step_editor_view(void) {
    const StylePreset *st = &k_styles[g_style_idx];
    int spb = st->steps_per_bar;
    int bar = g_cur_bar % st->num_bars;
    const BarChord *cur_bc = &st->bars[bar];

    int cy = 48, ch = 328;
    fill_rect(8, cy, NATIVE_W - 16, ch, RGB(21, 30, 48));
    draw_border(8, cy, NATIVE_W - 16, ch, 2, RGB(51, 68, 98));

    /* Column Header Row */
    draw_text(FONT_UI_13, 20, cy + 10, "TRACK", RGB(250, 204, 21));

    int grid_x  = 144;
    int avail_w = NATIVE_W - grid_x - 20;
    int cell_w  = (avail_w - (spb - 1) * 8) / spb;

    for (int s = 0; s < spb; s++) {
        int cx = grid_x + s * (cell_w + 8);
        bool is_beat = (s % 2 == 0);
        char hdr[16];
        if (is_beat) snprintf(hdr, sizeof(hdr), "BEAT %d", (s / 2) + 1);
        else         snprintf(hdr, sizeof(hdr), "&");
        draw_text_centered(FONT_UI_13, cx, cy + 10, cell_w, 20, hdr,
                           is_beat ? RGB(250, 204, 21) : RGB(148, 163, 184));
    }

    /* 5 Track Rows (KICK, SNARE, CL HAT, OP HAT, BASS) */
    int row_y0 = cy + 38;
    int row_h  = 56;

    for (int t = 0; t < NUM_TRACKS; t++) {
        int ry = row_y0 + t * row_h;
        bool trk_sel = (g_edit_track == t);

        draw_pill(16, ry, 118, row_h - 8,
                  trk_sel ? RGB(30, 58, 95) : RGB(15, 22, 34),
                  trk_sel ? RGB(250, 204, 21) : RGB(45, 62, 92));
        draw_text_centered(FONT_UI_13, 16, ry, 118, row_h - 8, k_track_short[t],
                           trk_sel ? RGB(254, 240, 138) : RGB(226, 232, 240));

        for (int s = 0; s < spb; s++) {
            int cx = grid_x + s * (cell_w + 8);
            uint8_t val   = g_pattern[g_style_idx][t][s];
            bool cursor   = (g_edit_track == t && g_edit_step == s);
            bool play_col = g_playing && (g_cur_step == s);

            uint32_t fill = RGB(15, 22, 34);
            uint32_t bdr  = play_col ? RGB(147, 197, 253) : RGB(45, 62, 92);
            char cell_top[16] = ".";
            char cell_sub[16] = "";
            uint32_t tcol = RGB(100, 116, 139);

            if (t == TRK_BASS) {
                if (val > 0 && val <= 6) {
                    fill = RGB(14, 116, 144);
                    bdr  = RGB(56, 189, 248);
                    snprintf(cell_top, sizeof(cell_top), "%s", k_bass_deg_labels[val]);
                    int chord_root_pc = (g_key_pc + cur_bc->root_offset) % 12;
                    int third_semi = (cur_bc->qual == CHQ_MIN || cur_bc->qual == CHQ_MIN7) ? 3 : 4;
                    int off = (val == 1) ? 0 : (val == 2) ? third_semi : (val == 3) ? 7 :
                              (val == 4) ? 9 : (val == 5) ? 10 : 12;
                    snprintf(cell_sub, sizeof(cell_sub), "%s", k_note_names[(chord_root_pc + off) % 12]);
                    tcol = RGB(255, 255, 255);
                }
            } else {
                if (val == 1) {
                    fill = RGB(16, 185, 129);
                    bdr  = RGB(110, 231, 183);
                    snprintf(cell_top, sizeof(cell_top), "ON");
                    tcol = RGB(15, 23, 42);
                } else if (val == 2) {
                    fill = RGB(245, 158, 11);
                    bdr  = RGB(254, 240, 138);
                    snprintf(cell_top, sizeof(cell_top), "ACC");
                    tcol = RGB(15, 23, 42);
                }
            }

            if (cursor) {
                bdr = RGB(250, 204, 21);
            }

            draw_pill(cx, ry, cell_w, row_h - 8, fill, bdr);
            if (cursor) {
                draw_border(cx - 2, ry - 2, cell_w + 4, row_h - 4, 2, RGB(250, 204, 21));
            }
            if (cell_sub[0] != '\0') {
                draw_text_centered(FONT_UI_13, cx, ry + 4, cell_w, 22, cell_top, tcol);
                draw_text_centered(FONT_UI_11, cx, ry + 25, cell_w, 18, cell_sub, RGB(186, 230, 253));
            } else {
                draw_text_centered(FONT_UI_16, cx, ry, cell_w, row_h - 8, cell_top, tcol);
            }
        }
    }
}

static void render_help_modal(void) {
    int mx = 20, my = 12, mw = NATIVE_W - 40, mh = NATIVE_H - 24;
    fill_rect(mx, my, mw, mh, RGB(15, 23, 38));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    fill_rect(mx + 4, my + 4, mw - 8, 38, RGB(28, 39, 60));
    fill_rect(mx + 4, my + 42, mw - 8, 2, RGB(64, 82, 115));
    draw_pill(mx + 12, my + 8, 76, 28, RGB(37, 99, 235), RGB(147, 197, 253));
    draw_text_centered(FONT_UI_13, mx + 12, my + 8, 76, 28, "HELP", RGB(255, 255, 255));
    draw_text(FONT_UI_20, mx + 98, my + 10, "POCKETBAND - QUICK GUIDE & GROOVE", RGB(248, 250, 252));

    fill_rect(mx + 4, my + 44, mw - 8, 22, RGB(19, 29, 48));
    fill_rect(mx + 4, my + 66, mw - 8, 1, RGB(48, 66, 98));
    draw_text_centered(FONT_UI_13, mx + 8, my + 44, mw - 16, 22,
                       "5-track drum, bass & chord backing band with 12-fret guitar soloing map.",
                       RGB(186, 230, 253));

    /* Groove & Feel Box */
    int gy = my + 72;
    fill_rect(mx + 14, gy, mw - 28, 68, RGB(23, 33, 52));
    draw_border(mx + 14, gy, mw - 28, 68, 2, RGB(52, 211, 153));

    char g1[96];
    snprintf(g1, sizeof(g1), "Style: %s (%d/4)   |   Key: %s   |   %d BPM",
             k_styles[g_style_idx].name, k_styles[g_style_idx].beats_per_bar,
             k_note_names[g_key_pc], g_bpm);
    draw_text_centered(FONT_UI_16, mx + 14, gy + 8, mw - 28, 24, g1, RGB(250, 204, 21));

    char g2[96];
    snprintf(g2, sizeof(g2), "[LEFT / RIGHT] Swing: %d%%   |   [A] 4-Bar Auto-Fill: %s",
             g_swing_pct, g_auto_fill ? "ON" : "OFF");
    draw_text_centered(FONT_UI_13, mx + 14, gy + 38, mw - 28, 20, g2, RGB(167, 243, 208));

    /* Controls Table */
    int ky = gy + 78;
    static const struct {
        const char *btn;
        const char *desc;
    } k_rows[7] = {
        { "SELECT",       "Switch Guitarist Jam View <-> 5-Track Step Editor" },
        { "A Button",     "Jam: Play/Stop  |  Step Edit: Cycle Hit / Bass Note" },
        { "B Button",     "Jam: Queue Drum Fill  |  Step Edit: Clear Step" },
        { "X Button",     "Cycle Mix: Full Band -> Drums+Bass -> Drums Only" },
        { "LEFT / RIGHT", "Jam: Cycle 6 Styles (Blues/Rock/Jazz) | Edit: Step" },
        { "UP / DOWN",    "Jam: Transpose Key (C .. B) | Edit: Select Track" },
        { "L1/R1 / L2/R2","L1/R1: Tempo -/+5 BPM | L2: Neck Dots | R2: Swing" }
    };

    for (int i = 0; i < 7; i++) {
        int ry = ky + i * 37;
        draw_pill(mx + 16, ry, 136, 28, RGB(30, 58, 95), RGB(71, 98, 145));
        draw_text_centered(FONT_UI_13, mx + 16, ry, 136, 28, k_rows[i].btn, RGB(254, 240, 138));
        draw_text(FONT_UI_13, mx + 164, ry + 6, k_rows[i].desc, RGB(226, 232, 240));
    }

    fill_rect(mx + 4, my + mh - 34, mw - 8, 30, RGB(12, 18, 30));
    fill_rect(mx + 4, my + mh - 34, mw - 8, 1, RGB(51, 65, 85));
    draw_text_centered(FONT_UI_13, mx, my + mh - 32, mw, 26,
                       "Press [Y] or [B] to close Help   |   [MENU] Save & Exit",
                       RGB(186, 230, 253));
}

static void render_ui(void) {
    fill_rect(0, 0, NATIVE_W, NATIVE_H, RGB(13, 18, 28));
    render_top_status_bar();
    if (g_view == VIEW_JAM) {
        render_jam_view();
    } else {
        render_step_editor_view();
    }
    render_step_strip_and_footer();
    if (g_show_help) {
        render_help_modal();
    }
}

/* ============================================================================
 * 9. MAIN ENTRY POINT & 60 FPS EVENT LOOP
 * ============================================================================ */

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    init_synth();
    resolve_config_path();
    load_config();

    /* Automated Audio Self-Test (--audio-test) */
    if (argc >= 2 && strcmp(argv[1], "--audio-test") == 0) {
        init_sf2_engine();
        g_playing = true;
        int16_t pcm[AUDIO_FRAMES * AUDIO_CHANNELS];
        double energy = 0.0;
        int peak = 0;
        int total_frames = SAMPLE_RATE; /* 1 second across beats */
        for (int done = 0; done < total_frames; done += AUDIO_FRAMES) {
            audio_render_frames(pcm, AUDIO_FRAMES);
            for (int i = 0; i < AUDIO_FRAMES * AUDIO_CHANNELS; i++) {
                int v = pcm[i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
                energy += (double)pcm[i] * (double)pcm[i];
            }
        }
        double rms = sqrt(energy / (double)(total_frames * AUDIO_CHANNELS));
        printf("pocketband audio-test: sf2=%s path=%s peak=%d rms=%.1f\n",
               g_sf2_loaded ? "YES" : "NO",
               g_sf2_loaded ? g_sf2.loaded_path : "(fallback-dsp)",
               peak, rms);
        rg35_sf2_free(&g_sf2);
        return (peak > 100) ? 0 : 1;
    }

    /* Automated Screenshot Mode (--screenshot-states <dir>) */
    if (argc >= 3 && strcmp(argv[1], "--screenshot-states") == 0) {
        const char *dir = argv[2];
        char path[512];
        int16_t dummy_pcm[AUDIO_FRAMES * AUDIO_CHANNELS];

        /* 1. Default Guitarist Jam View: Blues Shuffle 4/4 in Key of A */
        apply_style(0);
        g_key_pc     = 9; /* Key of A */
        g_view       = VIEW_JAM;
        g_mix_mode   = MIX_FULL_BAND;
        g_neck_notes = false;
        g_playing    = true;
        g_cur_bar    = 0;
        g_cur_step   = 0;
        g_show_help  = false;
        g_synth.step_sample_pos = 0;
        for (int i = 0; i < 6; i++) audio_render_frames(dummy_pcm, AUDIO_FRAMES);
        g_cur_step = 2;
        render_ui();
        snprintf(path, sizeof(path), "%s/pocketband_jam.bmp", dir);
        save_bmp(path);

        /* 2. Step Sequencer View (4/4 Classic Rock Drum & Bass Grid) */
        apply_style(2);
        g_key_pc     = 4; /* Key of E */
        g_view       = VIEW_STEP_EDITOR;
        g_edit_track = TRK_BASS;
        g_edit_step  = 3;
        g_cur_step   = 4;
        g_show_help  = false;
        render_ui();
        snprintf(path, sizeof(path), "%s/pocketband_steps.bmp", dir);
        save_bmp(path);

        /* 3. Jazz Waltz 3/4 Jam View in Key of C with Note Names on Fretboard */
        apply_style(5);
        g_key_pc     = 0; /* Key of C */
        g_view       = VIEW_JAM;
        g_mix_mode   = MIX_DRUM_BASS;
        g_neck_notes = true;
        g_cur_bar    = 2;
        g_cur_step   = 0;
        g_show_help  = false;
        g_synth.step_sample_pos = 0;
        for (int i = 0; i < 6; i++) audio_render_frames(dummy_pcm, AUDIO_FRAMES);
        g_cur_step = 4;
        render_ui();
        snprintf(path, sizeof(path), "%s/pocketband_waltz.bmp", dir);
        save_bmp(path);

        /* 4. Help & Groove Settings Modal */
        g_show_help = true;
        render_ui();
        snprintf(path, sizeof(path), "%s/pocketband_help.bmp", dir);
        save_bmp(path);
        return 0;
    }

    if (platform_init() != 0) return 1;

    while (g_running) {
        uint32_t pressed = platform_poll_pressed();
        if (pressed & (BTN_QUIT | BTN_MENU)) {
            g_running = 0;
            break;
        }

        if (pressed & BTN_Y) {
            g_show_help = !g_show_help;
        } else if (pressed & BTN_L1) {
            g_bpm -= 5;
            if (g_bpm < 45) g_bpm = 45;
            save_config();
        } else if (pressed & BTN_R1) {
            g_bpm += 5;
            if (g_bpm > 220) g_bpm = 220;
            save_config();
        } else if (pressed & BTN_L2) {
            g_neck_notes = !g_neck_notes;
            save_config();
        } else if (pressed & BTN_R2) {
            g_swing_pct += 3;
            if (g_swing_pct > 68) g_swing_pct = 50;
            save_config();
        } else if (pressed & BTN_X) {
            g_mix_mode = (BackingMixMode)(((int)g_mix_mode + 1) % 3);
            save_config();
        } else if (g_show_help) {
            if (pressed & BTN_LEFT) {
                if (g_swing_pct > 50) g_swing_pct -= 2;
                save_config();
            } else if (pressed & BTN_RIGHT) {
                if (g_swing_pct < 68) g_swing_pct += 2;
                save_config();
            } else if (pressed & BTN_A) {
                g_auto_fill = !g_auto_fill;
                save_config();
            } else if (pressed & (BTN_B | BTN_SELECT)) {
                g_show_help = false;
            }
        } else if (pressed & BTN_SELECT) {
            g_view = (g_view == VIEW_JAM) ? VIEW_STEP_EDITOR : VIEW_JAM;
        } else if (g_view == VIEW_JAM) {
            if (pressed & BTN_A) {
                pthread_mutex_lock(&g_audio_mutex);
                g_playing = !g_playing;
                if (g_playing) {
                    g_cur_bar  = 0;
                    g_cur_step = 0;
                    g_synth.step_sample_pos = 0;
                    g_synth.substep_16th_fired = false;
                    g_synth.prev_voicing_valid = false;
                }
                pthread_mutex_unlock(&g_audio_mutex);
            } else if (pressed & BTN_B) {
                g_fill_queued = !g_fill_queued;
            } else if (pressed & BTN_RIGHT) {
                apply_style(g_style_idx + 1);
                save_config();
            } else if (pressed & BTN_LEFT) {
                apply_style(g_style_idx - 1);
                save_config();
            } else if (pressed & BTN_UP) {
                g_key_pc = (g_key_pc + 1) % 12;
                save_config();
            } else if (pressed & BTN_DOWN) {
                g_key_pc = (g_key_pc + 11) % 12;
                save_config();
            }
        } else {
            int spb = k_styles[g_style_idx].steps_per_bar;
            if (pressed & BTN_UP) {
                g_edit_track = (g_edit_track + NUM_TRACKS - 1) % NUM_TRACKS;
            } else if (pressed & BTN_DOWN) {
                g_edit_track = (g_edit_track + 1) % NUM_TRACKS;
            } else if (pressed & BTN_LEFT) {
                g_edit_step = (g_edit_step + spb - 1) % spb;
            } else if (pressed & BTN_RIGHT) {
                g_edit_step = (g_edit_step + 1) % spb;
            } else if (pressed & BTN_A) {
                uint8_t cur = g_pattern[g_style_idx][g_edit_track][g_edit_step];
                int max_state = (g_edit_track == TRK_BASS) ? 6 : 2;
                g_pattern[g_style_idx][g_edit_track][g_edit_step] = (uint8_t)((cur + 1) % (max_state + 1));
            } else if (pressed & BTN_B) {
                g_pattern[g_style_idx][g_edit_track][g_edit_step] = 0;
            }
        }

        render_ui();
        platform_present();
        usleep(16000);
    }

    save_config();
    platform_shutdown();
    return 0;
}
