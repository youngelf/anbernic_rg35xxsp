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
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(TARGET_RG35XXSP)
#include <fcntl.h>
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

static void draw_text_fit(int font_id, int bx, int by, int bw, int bh, const char *s, uint32_t color) {
    int fid = font_id;
    while (fid > FONT_UI_11 && text_width(fid, s) > bw - 6) {
        fid--;
    }
    draw_text_centered(fid, bx, by, bw, bh, s, color);
}

static void draw_text_clipped(int font_id, int x, int y, int max_w, const char *s, uint32_t color) {
    char buf[160];
    size_t len = strlen(s);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    if (text_width(font_id, buf) <= max_w) {
        draw_text(font_id, x, y, buf, color);
        return;
    }
    while (len > 3) {
        len--;
        buf[len] = '\0';
        char ell[164];
        snprintf(ell, sizeof(ell), "%s...", buf);
        if (text_width(font_id, ell) <= max_w) {
            draw_text(font_id, x, y, ell, color);
            return;
        }
    }
    draw_text(font_id, x, y, buf, color);
}

static void draw_text_wrapped_centered(int font_id, int bx, int by, int bw, int line_h,
                                       int max_lines, const char *s, uint32_t color) {
    char work[256];
    snprintf(work, sizeof(work), "%s", s ? s : "");
    char *p = work;
    int line_idx = 0;
    while (*p && line_idx < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        char line[192] = "";
        char *cursor = p;
        char *last_fit_end = NULL;
        while (*cursor) {
            while (*cursor == ' ') cursor++;
            if (!*cursor) break;
            char *word_start = cursor;
            while (*cursor && *cursor != ' ') cursor++;
            size_t cand_len = (size_t)(cursor - p);
            if (cand_len >= sizeof(line)) cand_len = sizeof(line) - 1;
            char cand[192];
            memcpy(cand, p, cand_len);
            cand[cand_len] = '\0';
            if (text_width(font_id, cand) <= bw - 8 || !last_fit_end) {
                snprintf(line, sizeof(line), "%s", cand);
                last_fit_end = cursor;
                if (text_width(font_id, cand) > bw - 8) break;
            } else {
                cursor = word_start;
                break;
            }
        }
        if (line_idx == max_lines - 1 && *cursor) {
            size_t ll = strlen(line);
            while (ll > 3) {
                char ell[200];
                snprintf(ell, sizeof(ell), "%.*s...", (int)ll, line);
                if (text_width(font_id, ell) <= bw - 8) {
                    snprintf(line, sizeof(line), "%.191s", ell);
                    break;
                }
                ll--;
            }
        }
        draw_text_centered(font_id, bx, by + line_idx * line_h, bw, line_h, line, color);
        if (!last_fit_end) break;
        p = last_fit_end;
        line_idx++;
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
 * 3. DISK-LOADED SONG STRUCTURES (*.song) & ROMAN NUMERAL PARSER
 * ============================================================================ */

#define MAX_SONGS        64
#define MAX_SONG_BARS    32
#define MAX_SECTION_ROWS 8
#define MAX_STEPS        8
#define NUM_TRACKS       5

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
    CHQ_MAJ    = 0,
    CHQ_MIN    = 1,
    CHQ_DOM7   = 2,
    CHQ_MAJ7   = 3,
    CHQ_MIN7   = 4,
    CHQ_MIN7B5 = 5,
    CHQ_DIM7   = 6,
    CHQ_SUS4   = 7
} BarChordQuality;

typedef enum {
    KIT_BLUES_SHUFFLE = 0,
    KIT_SLOW_BLUES    = 1,
    KIT_CLASSIC_ROCK  = 2,
    KIT_ROCK_BALLAD   = 3,
    KIT_JAZZ_SWING    = 4,
    KIT_JAZZ_WALTZ    = 5,
    KIT_BOSSA_NOVA    = 6,
    KIT_POP_SOUL      = 7,
    KIT_ACOUSTIC_FOLK = 8
} GrooveKit;

typedef struct {
    int             root_offset; /* Semitones above active Key root (0..11) */
    BarChordQuality qual;
    char            roman[12];
} BarChord;

typedef struct {
    char        filename[64];
    char        name[48];
    char        short_genre[24];
    char        examples[128];
    int         beats_per_bar;   /* 4 for 4/4 (8 steps), 3 for 3/4 (6 steps) */
    int         steps_per_bar;   /* 8 for 4/4, 6 for 3/4 */
    int         default_bpm;
    int         swing_pct;       /* 50 = Straight 8ths, 60..66 = Triplet Shuffle */
    int         default_key_pc;  /* -1 if unspecified, else 0..11 */
    GrooveKit   kit;
    int         num_bars;        /* 1..MAX_SONG_BARS (typically 8, 12, or 16) */
    BarChord    bars[MAX_SONG_BARS];
    int         num_sections;
    char        section_labels[MAX_SECTION_ROWS][8];
    char        scale_name[64];
    uint8_t     scale_mask[12];  /* 1 = scale tone, 2 = characteristic blue/color note */
    uint8_t     default_steps[NUM_TRACKS][MAX_STEPS];
} StylePreset;

static StylePreset g_songs[MAX_SONGS];
static int         g_num_songs = 0;
static char        g_songs_dir[512] = "";

static const char *k_note_names[12] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
};

static const char *k_interval_labels[12] = {
    "R", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7"
};

/* Standard guitar string pitch classes from top row (high e) to bottom row (low E) */
static const char *k_gtr_str_lbl[6]  = { "e", "B", "G", "D", "A", "E" };
static const int   k_gtr_str_pc[6]   = { 4, 11, 7, 2, 9, 4 };

static bool pb_streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

static bool pb_ends_with_ci(const char *str, const char *suf) {
    size_t sl = strlen(str), fl = strlen(suf);
    if (sl < fl) return false;
    return pb_streq_ci(str + sl - fl, suf);
}

static char *pb_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static int parse_key_pc_token(const char *tok) {
    if (!tok || !tok[0]) return -1;
    static const struct { const char *n; int pc; } k_map[] = {
        { "C", 0 }, { "B#", 0 },
        { "C#", 1 }, { "Db", 1 },
        { "D", 2 },
        { "D#", 3 }, { "Eb", 3 },
        { "E", 4 }, { "Fb", 4 },
        { "F", 5 }, { "E#", 5 },
        { "F#", 6 }, { "Gb", 6 },
        { "G", 7 },
        { "G#", 8 }, { "Ab", 8 },
        { "A", 9 },
        { "A#", 10 }, { "Bb", 10 },
        { "B", 11 }, { "Cb", 11 }
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
        if (pb_streq_ci(tok, k_map[i].n)) return k_map[i].pc;
    }
    return -1;
}

static GrooveKit parse_kit_name(const char *val) {
    if (!val || !val[0]) return KIT_BLUES_SHUFFLE;
    if (pb_streq_ci(val, "blues_shuffle") || pb_streq_ci(val, "blues")) return KIT_BLUES_SHUFFLE;
    if (pb_streq_ci(val, "slow_blues")    || pb_streq_ci(val, "blues_waltz")) return KIT_SLOW_BLUES;
    if (pb_streq_ci(val, "classic_rock")  || pb_streq_ci(val, "rock")) return KIT_CLASSIC_ROCK;
    if (pb_streq_ci(val, "rock_ballad")   || pb_streq_ci(val, "ballad")) return KIT_ROCK_BALLAD;
    if (pb_streq_ci(val, "jazz_swing")    || pb_streq_ci(val, "jazz")) return KIT_JAZZ_SWING;
    if (pb_streq_ci(val, "jazz_waltz"))   return KIT_JAZZ_WALTZ;
    if (pb_streq_ci(val, "bossa_nova")    || pb_streq_ci(val, "bossa") || pb_streq_ci(val, "latin")) return KIT_BOSSA_NOVA;
    if (pb_streq_ci(val, "pop_soul")      || pb_streq_ci(val, "soul")  || pb_streq_ci(val, "pop")) return KIT_POP_SOUL;
    if (pb_streq_ci(val, "acoustic_folk") || pb_streq_ci(val, "folk")  || pb_streq_ci(val, "acoustic")) return KIT_ACOUSTIC_FOLK;
    return KIT_BLUES_SHUFFLE;
}

static bool parse_roman_chord(const char *tok, BarChord *out) {
    if (!tok || !tok[0] || !out) return false;
    const char *p = tok;
    int acc = 0;
    if (*p == 'b') { acc = -1; p++; }
    else if (*p == '#') { acc = 1; p++; }

    int base_off = -1;
    int deg_len = 0;
    bool is_upper = isupper((unsigned char)p[0]) != 0;

    static const struct { const char *rom; int off; int len; } k_romans[] = {
        { "VII", 11, 3 }, { "III", 4, 3 },
        { "VI",   9, 2 }, { "IV",  5, 2 }, { "II", 2, 2 },
        { "V",    7, 1 }, { "I",   0, 1 }
    };
    for (size_t i = 0; i < sizeof(k_romans) / sizeof(k_romans[0]); i++) {
        int l = k_romans[i].len;
        bool match = true;
        for (int k = 0; k < l; k++) {
            if (!p[k] || toupper((unsigned char)p[k]) != k_romans[i].rom[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            base_off = k_romans[i].off;
            deg_len = l;
            break;
        }
    }
    if (base_off < 0) return false;
    p += deg_len;

    BarChordQuality q;
    if (strcmp(p, "") == 0) {
        q = is_upper ? CHQ_MAJ : CHQ_MIN;
    } else if (pb_streq_ci(p, "m7b5") || pb_streq_ci(p, "7b5") ||
               pb_streq_ci(p, "hdim7") || pb_streq_ci(p, "-7b5")) {
        q = CHQ_MIN7B5;
    } else if (pb_streq_ci(p, "dim7") || pb_streq_ci(p, "dim") ||
               pb_streq_ci(p, "o7") || pb_streq_ci(p, "o")) {
        q = CHQ_DIM7;
    } else if (strcmp(p, "maj7") == 0 || strcmp(p, "MAJ7") == 0 || strcmp(p, "M7") == 0) {
        q = CHQ_MAJ7;
    } else if (strcmp(p, "m7") == 0 || pb_streq_ci(p, "min7") || strcmp(p, "-7") == 0) {
        q = CHQ_MIN7;
    } else if (strcmp(p, "7") == 0) {
        q = is_upper ? CHQ_DOM7 : CHQ_MIN7;
    } else if (pb_streq_ci(p, "sus4") || pb_streq_ci(p, "sus")) {
        q = CHQ_SUS4;
    } else if (strcmp(p, "m") == 0 || pb_streq_ci(p, "min") || strcmp(p, "-") == 0) {
        q = CHQ_MIN;
    } else {
        return false;
    }

    out->root_offset = (base_off + acc + 12) % 12;
    out->qual = q;
    snprintf(out->roman, sizeof(out->roman), "%.11s", tok);
    return true;
}

static void parse_scale_line(const char *val, uint8_t mask[12]) {
    memset(mask, 0, 12);
    char buf[192];
    snprintf(buf, sizeof(buf), "%s", val ? val : "");
    char *p = buf;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == '|')) p++;
        if (!*p) break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '|') p++;
        if (*p) *p++ = '\0';

        uint8_t mv = 1;
        if (*start == '*') { mv = 2; start++; }

        int semi = -1;
        if (pb_streq_ci(start, "1") || pb_streq_ci(start, "R")) semi = 0;
        else if (pb_streq_ci(start, "b2") || pb_streq_ci(start, "#1")) semi = 1;
        else if (pb_streq_ci(start, "2")) semi = 2;
        else if (pb_streq_ci(start, "b3") || pb_streq_ci(start, "#2")) semi = 3;
        else if (pb_streq_ci(start, "3")) semi = 4;
        else if (pb_streq_ci(start, "4")) semi = 5;
        else if (pb_streq_ci(start, "b5") || pb_streq_ci(start, "#4")) semi = 6;
        else if (pb_streq_ci(start, "5")) semi = 7;
        else if (pb_streq_ci(start, "b6") || pb_streq_ci(start, "#5")) semi = 8;
        else if (pb_streq_ci(start, "6")) semi = 9;
        else if (pb_streq_ci(start, "b7") || pb_streq_ci(start, "#6")) semi = 10;
        else if (pb_streq_ci(start, "7")) semi = 11;

        if (semi >= 0 && semi < 12) {
            if (mv > mask[semi]) mask[semi] = mv;
        }
    }
    mask[0] = 1; /* Always include root */
}

static void parse_drum_row(const char *val, uint8_t out_steps[MAX_STEPS]) {
    memset(out_steps, 0, MAX_STEPS);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", val ? val : "");
    char *p = buf;
    int idx = 0;
    while (*p && idx < MAX_STEPS) {
        while (*p && (isspace((unsigned char)*p) || *p == '|' || *p == ',')) p++;
        if (!*p) break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != ',') p++;
        if (*p) *p++ = '\0';

        if (strcmp(start, ".") == 0 || strcmp(start, "-") == 0 || strcmp(start, "0") == 0) {
            out_steps[idx++] = 0;
        } else if (strcmp(start, "2") == 0 || strcmp(start, "X") == 0) {
            out_steps[idx++] = 2;
        } else if (strcmp(start, "1") == 0 || strcmp(start, "x") == 0) {
            out_steps[idx++] = 1;
        } else {
            out_steps[idx++] = 0;
        }
    }
}

static void parse_bass_row(const char *val, uint8_t out_steps[MAX_STEPS]) {
    memset(out_steps, 0, MAX_STEPS);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", val ? val : "");
    char *p = buf;
    int idx = 0;
    while (*p && idx < MAX_STEPS) {
        while (*p && (isspace((unsigned char)*p) || *p == '|' || *p == ',')) p++;
        if (!*p) break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != ',') p++;
        if (*p) *p++ = '\0';

        uint8_t deg = 0;
        if (strcmp(start, ".") == 0 || strcmp(start, "-") == 0 || strcmp(start, "0") == 0) deg = 0;
        else if (pb_streq_ci(start, "R") || strcmp(start, "1") == 0) deg = 1;
        else if (pb_streq_ci(start, "3") || pb_streq_ci(start, "b3")) deg = 2;
        else if (pb_streq_ci(start, "5") || pb_streq_ci(start, "b5")) deg = 3;
        else if (pb_streq_ci(start, "6") || pb_streq_ci(start, "b6")) deg = 4;
        else if (pb_streq_ci(start, "b7") || pb_streq_ci(start, "7")) deg = 5;
        else if (strcmp(start, "8") == 0) deg = 6;
        out_steps[idx++] = deg;
    }
}

static void strip_inline_comment(const char *key, char *val) {
    bool is_chords = pb_streq_ci(key, "chords") || pb_streq_ci(key, "bars");
    bool is_scale  = pb_streq_ci(key, "scale");
    for (char *p = val; *p; p++) {
        if (*p != '#') continue;
        /* Preserve musical sharps: note names (F#, C#), marked scale degrees (*#4),
         * sharp Roman numerals on chords lines (#ivdim7, #IV), and sharp scale degrees (#4). */
        if (p > val && (isalpha((unsigned char)p[-1]) || p[-1] == '*')) continue;
        if (is_chords && (p[1] == 'I' || p[1] == 'i' || p[1] == 'V' || p[1] == 'v')) continue;
        if (is_scale && (p[1] >= '1' && p[1] <= '6')) continue;
        *p = '\0';
        break;
    }
}

static bool load_song_file(const char *full_path, const char *filename, StylePreset *st) {
    FILE *f = fopen(full_path, "r");
    if (!f) return false;

    memset(st, 0, sizeof(*st));
    snprintf(st->filename, sizeof(st->filename), "%.63s", filename);
    snprintf(st->short_genre, sizeof(st->short_genre), "CUSTOM");
    snprintf(st->examples, sizeof(st->examples), "Custom progression (%.63s)", filename);
    st->beats_per_bar  = 4;
    st->steps_per_bar  = 8;
    st->default_bpm    = 110;
    st->swing_pct      = 50;
    st->default_key_pc = -1;
    st->kit            = KIT_BLUES_SHUFFLE;

    bool has_scale = false;
    bool has_pattern = false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = pb_trim(line);
        if (!s[0] || s[0] == '#') continue; /* Full-line '#' comment or blank line */

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = pb_trim(s);
        char *val = eq + 1;
        strip_inline_comment(key, val);
        val = pb_trim(val);
        if (!key[0] || !val[0]) continue;

        if (pb_streq_ci(key, "name") || pb_streq_ci(key, "title")) {
            snprintf(st->name, sizeof(st->name), "%s", val);
        } else if (pb_streq_ci(key, "genre") || pb_streq_ci(key, "short_genre")) {
            snprintf(st->short_genre, sizeof(st->short_genre), "%s", val);
        } else if (pb_streq_ci(key, "examples") || pb_streq_ci(key, "songs") || pb_streq_ci(key, "inspired_by")) {
            snprintf(st->examples, sizeof(st->examples), "%s", val);
        } else if (pb_streq_ci(key, "meter") || pb_streq_ci(key, "time_signature")) {
            if (val[0] == '3') {
                st->beats_per_bar = 3;
                st->steps_per_bar = 6;
            } else {
                st->beats_per_bar = 4;
                st->steps_per_bar = 8;
            }
        } else if (pb_streq_ci(key, "bpm") || pb_streq_ci(key, "tempo")) {
            int b = atoi(val);
            if (b < 45) b = 45;
            if (b > 220) b = 220;
            st->default_bpm = b;
        } else if (pb_streq_ci(key, "swing")) {
            int sw = atoi(val);
            if (sw < 50) sw = 50;
            if (sw > 68) sw = 68;
            st->swing_pct = sw;
        } else if (pb_streq_ci(key, "default_key") || pb_streq_ci(key, "key")) {
            st->default_key_pc = parse_key_pc_token(val);
        } else if (pb_streq_ci(key, "kit") || pb_streq_ci(key, "groove")) {
            st->kit = parse_kit_name(val);
        } else if (pb_streq_ci(key, "scale_name")) {
            snprintf(st->scale_name, sizeof(st->scale_name), "%s", val);
        } else if (pb_streq_ci(key, "scale")) {
            parse_scale_line(val, st->scale_mask);
            has_scale = true;
        } else if (pb_streq_ci(key, "sections")) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", val);
            char *p = buf;
            while (*p && st->num_sections < MAX_SECTION_ROWS) {
                while (*p && (isspace((unsigned char)*p) || *p == '|' || *p == ',')) p++;
                if (!*p) break;
                char *start = p;
                while (*p && *p != '|' && *p != ',') p++;
                if (*p) *p++ = '\0';
                char *lbl = pb_trim(start);
                if (lbl[0]) {
                    snprintf(st->section_labels[st->num_sections++],
                             sizeof(st->section_labels[0]), "%s", lbl);
                }
            }
        } else if (pb_streq_ci(key, "chords") || pb_streq_ci(key, "bars")) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", val);
            char *p = buf;
            while (*p && st->num_bars < MAX_SONG_BARS) {
                while (*p && (isspace((unsigned char)*p) || *p == '|' || *p == ',')) p++;
                if (!*p) break;
                char *start = p;
                while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != ',') p++;
                if (*p) *p++ = '\0';
                BarChord bc;
                if (parse_roman_chord(start, &bc)) {
                    st->bars[st->num_bars++] = bc;
                } else {
                    fprintf(stderr, "pocketband: [%s] ignoring unrecognized chord token '%s'\n",
                            filename, start);
                }
            }
        } else if (pb_streq_ci(key, "kick")) {
            parse_drum_row(val, st->default_steps[TRK_KICK]);
            has_pattern = true;
        } else if (pb_streq_ci(key, "snare")) {
            parse_drum_row(val, st->default_steps[TRK_SNARE]);
            has_pattern = true;
        } else if (pb_streq_ci(key, "clhat")) {
            parse_drum_row(val, st->default_steps[TRK_CLHAT]);
            has_pattern = true;
        } else if (pb_streq_ci(key, "ophat") || pb_streq_ci(key, "ride")) {
            parse_drum_row(val, st->default_steps[TRK_OPHAT]);
            has_pattern = true;
        } else if (pb_streq_ci(key, "bass")) {
            parse_bass_row(val, st->default_steps[TRK_BASS]);
            has_pattern = true;
        }
    }
    fclose(f);

    if (st->num_bars <= 0) {
        fprintf(stderr, "pocketband: skipping '%s' (no valid chords found)\n", filename);
        return false;
    }

    if (st->name[0] == '\0') {
        snprintf(st->name, sizeof(st->name), "%.47s", filename);
        char *dot = strrchr(st->name, '.');
        if (dot) *dot = '\0';
    }

    /* Sensible default solo scale if omitted in a minimal user file */
    if (!has_scale) {
        bool minor_tonic = (st->bars[0].qual == CHQ_MIN || st->bars[0].qual == CHQ_MIN7 ||
                            st->bars[0].qual == CHQ_MIN7B5);
        if (minor_tonic) {
            parse_scale_line("1 2 b3 4 5 b6 b7", st->scale_mask);
            if (!st->scale_name[0]) snprintf(st->scale_name, sizeof(st->scale_name), "Natural Minor (1 2 b3 4 5 b6 b7)");
        } else {
            parse_scale_line("1 2 3 4 5 6 7", st->scale_mask);
            if (!st->scale_name[0]) snprintf(st->scale_name, sizeof(st->scale_name), "Major / Ionian (1 2 3 4 5 6 7)");
        }
    } else if (!st->scale_name[0]) {
        snprintf(st->scale_name, sizeof(st->scale_name), "Custom Solo Scale");
    }

    /* Sensible default drum/bass groove if omitted in a minimal user file */
    if (!has_pattern) {
        if (st->beats_per_bar == 3) {
            parse_drum_row("2 . . 1 . .", st->default_steps[TRK_KICK]);
            parse_drum_row(". . . . 2 .", st->default_steps[TRK_SNARE]);
            parse_drum_row("2 1 1 1 1 .", st->default_steps[TRK_CLHAT]);
            parse_drum_row(". . . . . 1", st->default_steps[TRK_OPHAT]);
            parse_bass_row("R . 3 . 5 .", st->default_steps[TRK_BASS]);
        } else {
            parse_drum_row("2 . . . 1 . . .", st->default_steps[TRK_KICK]);
            parse_drum_row(". . 2 . . . 2 .", st->default_steps[TRK_SNARE]);
            parse_drum_row("2 1 2 1 2 1 2 1", st->default_steps[TRK_CLHAT]);
            parse_drum_row(". . . . . . . .", st->default_steps[TRK_OPHAT]);
            parse_bass_row("R . 3 . 5 . 8 5", st->default_steps[TRK_BASS]);
        }
    }
    return true;
}

static int cmp_song_filename(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static int scan_songs_in_dir(const char *dir_path) {
    if (!dir_path || !dir_path[0]) return 0;
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    char names[MAX_SONGS][64];
    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < MAX_SONGS) {
        if (de->d_name[0] == '.') continue; /* Skip '.', '..', and macOS '._*' dotfiles */
        if (!pb_ends_with_ci(de->d_name, ".song")) continue;
        snprintf(names[count], sizeof(names[count]), "%s", de->d_name);
        count++;
    }
    closedir(dir);
    if (count == 0) return 0;

    qsort(names, (size_t)count, sizeof(names[0]), cmp_song_filename);

    g_num_songs = 0;
    for (int i = 0; i < count && g_num_songs < MAX_SONGS; i++) {
        char full[768];
        snprintf(full, sizeof(full), "%.511s/%.63s", dir_path, names[i]);
        if (load_song_file(full, names[i], &g_songs[g_num_songs])) {
            g_num_songs++;
        }
    }
    if (g_num_songs > 0) {
        snprintf(g_songs_dir, sizeof(g_songs_dir), "%s", dir_path);
    }
    return g_num_songs;
}

static int load_all_songs_from_disk(void) {
    g_num_songs = 0;
    const char *env_dir = getenv("POCKETBAND_SONGS_DIR");
    if (env_dir && scan_songs_in_dir(env_dir) > 0) return g_num_songs;
    if (scan_songs_in_dir("./songs") > 0) return g_num_songs;

    /* Executable-relative <exe_dir>/songs fallback */
    char exe_path[512];
    ssize_t rlen = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (rlen > 0) {
        exe_path[rlen] = '\0';
        char *slash = strrchr(exe_path, '/');
        if (slash) {
            *slash = '\0';
            char rel_dir[576];
            snprintf(rel_dir, sizeof(rel_dir), "%s/songs", exe_path);
            if (scan_songs_in_dir(rel_dir) > 0) return g_num_songs;
        }
    }

    if (scan_songs_in_dir("/userdata/roms/ports/pocketband/songs") > 0) return g_num_songs;
    return g_num_songs;
}

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

static ViewMode       g_view           = VIEW_JAM;
static BackingMixMode g_mix_mode       = MIX_FULL_BAND;
static bool           g_show_help      = false;
static bool           g_show_fretboard = false;
static bool           g_neck_notes     = false; /* false = Intervals (R,b3,4,5,b7), true = Note Names (A,C,D,E,G) */

static int            g_style_idx      = 0;     /* Active disk-loaded song index */
static int            g_key_pc         = 9;     /* Default Key: A (9) */
static int            g_bpm            = 110;
static int            g_swing_pct      = 65;
static bool           g_auto_fill      = true;
static bool           g_fill_queued    = false;

static uint8_t        g_pattern[MAX_SONGS][NUM_TRACKS][MAX_STEPS];

/* Sequencer Transport State (shared with background audio thread) */
static volatile bool  g_playing        = true;
static volatile int   g_cur_bar        = 0;
static volatile int   g_cur_step       = 0;

/* Step Editor Cursor */
static int            g_edit_track     = 0;
static int            g_edit_step      = 0;

static char           g_config_path[512] = "";

static void format_chord_label(int key_pc, const BarChord *bc, char *buf, size_t sz) {
    int pc = (key_pc + bc->root_offset) % 12;
    const char *suf = "";
    switch (bc->qual) {
        case CHQ_MAJ:    suf = "";     break;
        case CHQ_MIN:    suf = "m";    break;
        case CHQ_DOM7:   suf = "7";    break;
        case CHQ_MAJ7:   suf = "maj7"; break;
        case CHQ_MIN7:   suf = "m7";   break;
        case CHQ_MIN7B5: suf = "m7b5"; break;
        case CHQ_DIM7:   suf = "dim7"; break;
        case CHQ_SUS4:   suf = "sus4"; break;
    }
    snprintf(buf, sz, "%s%s", k_note_names[pc], suf);
}

static void format_chord_tones(int key_pc, const BarChord *bc, char *buf, size_t sz) {
    int root_pc  = (key_pc + bc->root_offset) % 12;
    int third_semi = (bc->qual == CHQ_SUS4) ? 5 :
                     (bc->qual == CHQ_MIN || bc->qual == CHQ_MIN7 ||
                      bc->qual == CHQ_MIN7B5 || bc->qual == CHQ_DIM7) ? 3 : 4;
    int fifth_semi = (bc->qual == CHQ_MIN7B5 || bc->qual == CHQ_DIM7) ? 6 : 7;
    int third_pc = (root_pc + third_semi) % 12;
    int fifth_pc = (root_pc + fifth_semi) % 12;

    if (bc->qual == CHQ_DOM7 || bc->qual == CHQ_MAJ7 || bc->qual == CHQ_MIN7 ||
        bc->qual == CHQ_MIN7B5 || bc->qual == CHQ_DIM7) {
        int sev_semi = (bc->qual == CHQ_MAJ7) ? 11 : (bc->qual == CHQ_DIM7) ? 9 : 10;
        int sev_pc = (root_pc + sev_semi) % 12;
        snprintf(buf, sz, "%s - %s - %s - %s",
                 k_note_names[root_pc], k_note_names[third_pc],
                 k_note_names[fifth_pc], k_note_names[sev_pc]);
    } else {
        snprintf(buf, sz, "%s - %s - %s",
                 k_note_names[root_pc], k_note_names[third_pc], k_note_names[fifth_pc]);
    }
}

static int bass_degree_semitone_offset(const BarChord *bc, uint8_t bass_deg) {
    int third_semi = (bc->qual == CHQ_SUS4) ? 5 :
                     (bc->qual == CHQ_MIN || bc->qual == CHQ_MIN7 ||
                      bc->qual == CHQ_MIN7B5 || bc->qual == CHQ_DIM7) ? 3 : 4;
    int fifth_semi = (bc->qual == CHQ_MIN7B5 || bc->qual == CHQ_DIM7) ? 6 : 7;
    int sixth_semi = (bc->qual == CHQ_MIN7B5 || bc->qual == CHQ_DIM7) ? 8 : 9;
    int sev_semi   = (bc->qual == CHQ_MAJ7) ? 11 : (bc->qual == CHQ_DIM7) ? 9 : 10;
    switch (bass_deg) {
        case 1: return 0;
        case 2: return third_semi;
        case 3: return fifth_semi;
        case 4: return sixth_semi;
        case 5: return sev_semi;
        case 6: return 12;
        default: return 0;
    }
}

static void format_bass_pitch_name(int key_pc, const BarChord *bc, uint8_t bass_deg, char *buf, size_t sz) {
    if (bass_deg == 0 || bass_deg > 6) {
        snprintf(buf, sz, "REST");
        return;
    }
    int chord_root_pc = (key_pc + bc->root_offset) % 12;
    int semitone_off = bass_degree_semitone_offset(bc, bass_deg);
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

static bool kit_chord_is_guitar(GrooveKit kit) {
    return (kit == KIT_SLOW_BLUES || kit == KIT_CLASSIC_ROCK ||
            kit == KIT_ROCK_BALLAD || kit == KIT_JAZZ_SWING ||
            kit == KIT_BOSSA_NOVA || kit == KIT_ACOUSTIC_FOLK);
}

static bool kit_is_jazz_ride(GrooveKit kit) {
    return (kit == KIT_JAZZ_SWING || kit == KIT_JAZZ_WALTZ || kit == KIT_BOSSA_NOVA);
}

static bool kit_has_organ_pad(GrooveKit kit) {
    return (kit == KIT_SLOW_BLUES || kit == KIT_CLASSIC_ROCK ||
            kit == KIT_ROCK_BALLAD || kit == KIT_POP_SOUL);
}

static void sf2_configure_style_instruments(int style_id) {
    if (!rg35_sf2_ready(&g_sf2) || g_num_songs <= 0) return;
    if (style_id < 0 || style_id >= g_num_songs) style_id = 0;
    GrooveKit kit = g_songs[style_id].kit;

    int drum_pref1 = 0, drum_pref2 = 0;
    int bass_prog  = 33;
    int chord_p1   = 4, chord_p2 = 27;
    int organ_prog = -1;
    float chord_vol = 0.72f;
    float bass_vol  = 0.92f;
    float drum_vol  = 0.96f;

    switch (kit) {
        case KIT_BLUES_SHUFFLE:
            drum_pref1 = 8; drum_pref2 = 0;
            bass_prog  = 33;
            chord_p1   = 4; chord_p2 = 27;
            organ_prog = -1;
            chord_vol  = 0.74f;
            break;
        case KIT_SLOW_BLUES:
            drum_pref1 = 8; drum_pref2 = 0;
            bass_prog  = 33;
            chord_p1   = 27; chord_p2 = 26;
            organ_prog = 16;
            chord_vol  = 0.72f;
            break;
        case KIT_CLASSIC_ROCK:
            drum_pref1 = 16; drum_pref2 = 0;
            bass_prog  = 34;
            chord_p1   = 29; chord_p2 = 27;
            organ_prog = 18;
            chord_vol  = 0.68f;
            break;
        case KIT_ROCK_BALLAD:
            drum_pref1 = 0; drum_pref2 = 8;
            bass_prog  = 34;
            chord_p1   = 27; chord_p2 = 25;
            organ_prog = 18;
            chord_vol  = 0.72f;
            break;
        case KIT_JAZZ_SWING:
            drum_pref1 = 32; drum_pref2 = 0;
            bass_prog  = 32;
            chord_p1   = 26; chord_p2 = 4;
            organ_prog = -1;
            chord_vol  = 0.76f;
            drum_vol   = 0.88f;
            break;
        case KIT_JAZZ_WALTZ:
            drum_pref1 = 40; drum_pref2 = 32;
            bass_prog  = 32;
            chord_p1   = 4; chord_p2 = 26;
            organ_prog = -1;
            chord_vol  = 0.76f;
            drum_vol   = 0.86f;
            break;
        case KIT_BOSSA_NOVA:
            drum_pref1 = 40; drum_pref2 = 32;
            bass_prog  = 32;
            chord_p1   = 24; chord_p2 = 27; /* Nylon Guitar -> Clean Guitar fallback */
            organ_prog = -1;
            chord_vol  = 0.78f;
            drum_vol   = 0.84f;
            break;
        case KIT_POP_SOUL:
            drum_pref1 = 0; drum_pref2 = 8;
            bass_prog  = 33;
            chord_p1   = 4; chord_p2 = 27;
            organ_prog = 16;
            chord_vol  = 0.74f;
            drum_vol   = 0.90f;
            break;
        case KIT_ACOUSTIC_FOLK:
        default:
            drum_pref1 = 40; drum_pref2 = 0;
            bass_prog  = 32;
            chord_p1   = 25; chord_p2 = 27; /* Steel Acoustic -> Clean Guitar fallback */
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
    int semitone_off = bass_degree_semitone_offset(bc, bass_deg);
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
    int third_semi = (bc->qual == CHQ_SUS4) ? 5 :
                     (bc->qual == CHQ_MIN || bc->qual == CHQ_MIN7 ||
                      bc->qual == CHQ_MIN7B5 || bc->qual == CHQ_DIM7) ? 3 : 4;
    int fifth_semi = (bc->qual == CHQ_MIN7B5 || bc->qual == CHQ_DIM7) ? 6 : 7;
    int third_pc = (root_pc + third_semi) % 12;
    int fifth_pc = (root_pc + fifth_semi) % 12;
    int sev_pc;
    if (bc->qual == CHQ_MAJ7) {
        sev_pc = (root_pc + 11) % 12;
    } else if (bc->qual == CHQ_DOM7 || bc->qual == CHQ_MIN7 || bc->qual == CHQ_MIN7B5) {
        sev_pc = (root_pc + 10) % 12;
    } else if (bc->qual == CHQ_DIM7) {
        sev_pc = (root_pc + 9) % 12;
    } else {
        sev_pc = (root_pc + 2) % 12; /* Add 9th for lush 4-voice triad/sus4 comping */
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

static float style_comping_velocity(GrooveKit kit, int beats_per_bar, int bar_idx,
                                    int step_idx, bool next_changes) {
    int spb = beats_per_bar * 2;
    /* Subtle Beat-4 '&' (or Beat-3 '&') harmonic anticipation before a chord change */
    if (step_idx == spb - 1 && (next_changes || (bar_idx % 2 == 1))) {
        return 0.64f;
    }
    /* Bossa Nova: syncopated bossa clave guitar comping */
    if (kit == KIT_BOSSA_NOVA && beats_per_bar == 4) {
        if (bar_idx % 2 == 0) {
            if (step_idx == 0) return 0.72f;
            if (step_idx == 3) return 0.64f;
            if (step_idx == 6) return 0.60f;
        } else {
            if (step_idx == 1) return 0.62f;
            if (step_idx == 4) return 0.66f;
        }
        return 0.0f;
    }
    /* Jazz Swing: Charleston comping + bar-2 syncopated phrasing */
    if (kit == KIT_JAZZ_SWING) {
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
    if (g_num_songs <= 0) return;
    if (style_id < 0 || style_id >= g_num_songs) style_id = 0;
    const StylePreset *st = &g_songs[style_id];
    const BarChord *cur_bc  = &st->bars[bar_idx % st->num_bars];
    const BarChord *next_bc = &st->bars[(bar_idx + 1) % st->num_bars];
    bool next_changes = (cur_bc->root_offset != next_bc->root_offset) || (cur_bc->qual != next_bc->qual);

    int spb = st->steps_per_bar;
    bool is_fill_bar = ((bar_idx + 1) % 4 == 0 && g_auto_fill) || g_fill_queued;
    bool is_turnaround = ((bar_idx + 1) == st->num_bars) && is_fill_bar;
    int fill_start = is_turnaround ? (spb >= 4 ? spb - 4 : 0) : (spb >= 2 ? spb - 2 : 0);
    GrooveKit kit = st->kit;
    bool is_jazz = kit_is_jazz_ride(kit);
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
                         (kit == KIT_BLUES_SHUFFLE && step_idx == 1 && (bar_idx % 4 == 2));
            if (ghost) {
                if (use_sf2) {
                    int gnote = (kit == KIT_JAZZ_WALTZ || kit == KIT_BOSSA_NOVA) ? GM_SIDE_STICK : GM_SNARE_1;
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
        (kit == KIT_BLUES_SHUFFLE || kit == KIT_CLASSIC_ROCK)) {
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
            } else if (kit == KIT_BOSSA_NOVA) {
                s_note = GM_SIDE_STICK;
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
        bool anticipate = (step_idx == spb - 1) && next_changes &&
                          (kit == KIT_BLUES_SHUFFLE || kit == KIT_CLASSIC_ROCK);
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
        float comp_vel = style_comping_velocity(kit, st->beats_per_bar, bar_idx, step_idx, next_changes);
        if (comp_vel > 0.0f) {
            bool is_antic = (step_idx == spb - 1) && next_changes;
            const BarChord *chord_bc = is_antic ? next_bc : cur_bc;
            int chord_midi[4];
            compute_voice_led_chord(g_key_pc, chord_bc, chord_midi);

            if (use_sf2) {
                bool is_gtr = kit_chord_is_guitar(kit);
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

                /* Subtle warm Rock/Tonewheel Organ pad layer on Beat 1 for Rock, Soul & Slow Blues */
                if (step_idx == 0 && kit_has_organ_pad(kit)) {
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
        int spb   = (g_num_songs > 0) ? g_songs[g_style_idx].steps_per_bar : 8;
        int nbars = (g_num_songs > 0) ? g_songs[g_style_idx].num_bars : 1;
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
                        int spb   = (g_num_songs > 0) ? g_songs[g_style_idx].steps_per_bar : 8;
                        int nbars = (g_num_songs > 0) ? g_songs[g_style_idx].num_bars : 1;
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
    for (int s = 0; s < g_num_songs; s++) {
        for (int t = 0; t < NUM_TRACKS; t++) {
            for (int st = 0; st < MAX_STEPS; st++) {
                g_pattern[s][t][st] = g_songs[s].default_steps[t][st];
            }
        }
    }
}

static int find_song_by_filename(const char *fname) {
    if (!fname || !fname[0]) return -1;
    for (int i = 0; i < g_num_songs; i++) {
        if (strcmp(g_songs[i].filename, fname) == 0) return i;
    }
    return -1;
}

static void apply_style(int style_idx) {
    if (g_num_songs <= 0) return;
    style_idx = ((style_idx % g_num_songs) + g_num_songs) % g_num_songs;
    pthread_mutex_lock(&g_audio_mutex);
    g_style_idx = style_idx;
    g_bpm       = g_songs[style_idx].default_bpm;
    g_swing_pct = g_songs[style_idx].swing_pct;
    g_cur_bar   = 0;
    g_cur_step  = 0;
    if (g_edit_step >= g_songs[style_idx].steps_per_bar) {
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
    if (g_config_path[0] == '\0' || g_num_songs <= 0) return;
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "song_file=%s\n", g_songs[g_style_idx].filename);
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
    if (g_num_songs > 0 && g_songs[0].default_key_pc >= 0) {
        g_key_pc = g_songs[0].default_key_pc;
    }
    if (g_config_path[0] == '\0') return;
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;

    bool matched_file = false;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        char fname[64];
        if (sscanf(line, "song_file=%63s", fname) == 1) {
            int idx = find_song_by_filename(fname);
            if (idx >= 0) {
                apply_style(idx);
                matched_file = true;
            }
        } else if (!matched_file && sscanf(line, "style_idx=%d", &v) == 1 && v >= 0 && v < g_num_songs) {
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
 * 8. NATIVE 640x480 UI RENDERING: TOP BAR, SONG CHART JAM VIEW, STEP EDITOR,
 *    L2 FRETBOARD OVERLAY & Y HELP MODAL (WITH FAMOUS SONG EXAMPLES)
 * ============================================================================ */

static void render_top_status_bar(void) {
    const StylePreset *st = &g_songs[g_style_idx];
    fill_rect(0, 0, NATIVE_W, 42, RGB(21, 30, 48));
    fill_rect(0, 40, NATIVE_W, 2, RGB(56, 78, 112));

    /* 1. Mode Badge (SELECT) */
    draw_pill(8, 6, 128, 28,
              (g_view == VIEW_JAM) ? RGB(37, 99, 235) : RGB(124, 58, 237),
              (g_view == VIEW_JAM) ? RGB(147, 197, 253) : RGB(196, 181, 253));
    draw_text_centered(FONT_UI_13, 8, 6, 128, 28,
                       (g_view == VIEW_JAM) ? "SEL: JAM VIEW" : "SEL: STEP EDIT",
                       RGB(255, 255, 255));

    /* 2. Active Song Pill (auto-fits longer custom song names) */
    char song_hdr[72];
    snprintf(song_hdr, sizeof(song_hdr), "%d/%d %s", g_style_idx + 1, g_num_songs, st->name);
    draw_pill(142, 6, 198, 28, RGB(30, 41, 59), RGB(96, 165, 250));
    draw_text_fit(FONT_UI_13, 142, 6, 198, 28, song_hdr, RGB(254, 240, 138));

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
    const StylePreset *st = &g_songs[g_style_idx];
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
        ? "A: Play | L/R: Song | U/D: Key | L1/R1: BPM | L2: Fretboard"
        : "A: Cycle Hit/Note | B: Clear | DPad: Cursor | L2: Fretboard";
    draw_text_centered(FONT_UI_11, 146, by + 7, NATIVE_W - 242, 28, ctrl_txt, RGB(203, 213, 225));

    draw_pill(NATIVE_W - 92, by + 7, 84, 28, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, NATIVE_W - 92, by + 7, 84, 28, "Y: Help", RGB(254, 240, 138));
}

static void render_jam_view(void) {
    const StylePreset *st = &g_songs[g_style_idx];
    int bar      = g_cur_bar % st->num_bars;
    int next_bar = (bar + 1) % st->num_bars;

    const BarChord *cur_bc  = &st->bars[bar];
    const BarChord *next_bc = &st->bars[next_bar];

    char cur_chord_str[24], next_chord_str[24], tones_str[48];
    format_chord_label(g_key_pc, cur_bc, cur_chord_str, sizeof(cur_chord_str));
    format_chord_label(g_key_pc, next_bc, next_chord_str, sizeof(next_chord_str));
    format_chord_tones(g_key_pc, cur_bc, tones_str, sizeof(tones_str));

    /* 1. Top Strip (y = 48..138, height = 90px): NOW PLAYING + NEXT / LIVE MIX VU */
    int cy = 48, ch = 90;

    /* 1a. Left Card: NOW PLAYING CHORD & CHORD TONES (x = 8..364, w = 356px) */
    fill_rect(8, cy, 356, ch, RGB(21, 30, 48));
    draw_border(8, cy, 356, ch, 2, RGB(56, 189, 248));

    char bar_hdr[48];
    snprintf(bar_hdr, sizeof(bar_hdr), "NOW PLAYING: BAR %d OF %d", bar + 1, st->num_bars);
    draw_text(FONT_UI_13, 18, cy + 7, bar_hdr, RGB(125, 211, 252));

    draw_pill(274, cy + 5, 82, 22, RGB(146, 92, 12), RGB(250, 204, 21));
    draw_text_fit(FONT_UI_13, 274, cy + 5, 82, 22, cur_bc->roman, RGB(255, 255, 255));

    draw_pill(16, cy + 32, 146, 50, RGB(15, 23, 38), RGB(250, 204, 21));
    draw_text_fit(FONT_TITLE_28, 16, cy + 32, 146, 50, cur_chord_str, RGB(250, 204, 21));

    fill_rect(170, cy + 32, 186, 50, RGB(15, 23, 38));
    draw_border(170, cy + 32, 186, 50, 1, RGB(51, 68, 98));
    draw_text_centered(FONT_UI_11, 170, cy + 36, 186, 15, "CHORD TONES", RGB(148, 163, 184));
    draw_text_fit(FONT_UI_13, 170, cy + 54, 186, 22, tones_str, RGB(248, 250, 252));

    /* 1b. Right Card: NEXT CHORD, ACTIVE BASS & 5-TRACK LIVE VU METERS (x = 372..632, w = 260px) */
    fill_rect(372, cy, 260, ch, RGB(21, 30, 48));
    draw_border(372, cy, 260, ch, 2, RGB(51, 68, 98));

    char next_hdr[64];
    snprintf(next_hdr, sizeof(next_hdr), "NEXT (B%d): %s (%s)",
             next_bar + 1, next_chord_str, next_bc->roman);
    draw_text_clipped(FONT_UI_13, 382, cy + 7, 240, next_hdr, RGB(250, 204, 21));

    uint8_t cur_bass_deg = g_pattern[g_style_idx][TRK_BASS][g_cur_step % st->steps_per_bar];
    char b_pitch[32], b_line[64];
    format_bass_pitch_name(g_key_pc, cur_bc, cur_bass_deg, b_pitch, sizeof(b_pitch));
    snprintf(b_line, sizeof(b_line), "Bass: %s  |  %s", b_pitch, st->short_genre);
    draw_text_clipped(FONT_UI_11, 382, cy + 28, 240, b_line, RGB(110, 231, 183));

    /* Compact 5-Track Live VU Meter Strip */
    static const char *k_vu_lbl[5] = { "KCK", "SNR", "HAT", "BAS", "CHD" };
    const float vu_vals[5] = {
        g_synth.vu_kick, g_synth.vu_snare, g_synth.vu_hat, g_synth.vu_bass, g_synth.vu_chord
    };
    const uint32_t vu_cols[5] = {
        RGB(239, 68, 68), RGB(245, 158, 11), RGB(56, 189, 248),
        RGB(16, 185, 129), RGB(168, 85, 247)
    };
    int vu_x0 = 382;
    int vu_w  = 44;
    int vu_gap = 5;
    for (int i = 0; i < 5; i++) {
        int vx = vu_x0 + i * (vu_w + vu_gap);
        fill_rect(vx, cy + 48, vu_w, 16, RGB(15, 22, 34));
        draw_border(vx, cy + 48, vu_w, 16, 1, RGB(45, 62, 92));
        float lv = vu_vals[i];
        if (lv < 0.0f) lv = 0.0f;
        if (lv > 1.0f) lv = 1.0f;
        int fw = (int)(lv * (float)(vu_w - 4));
        if (fw > 0) {
            fill_rect(vx + 2, cy + 50, fw, 12, vu_cols[i]);
        }
        draw_text_centered(FONT_UI_11, vx, cy + 66, vu_w, 16, k_vu_lbl[i], RGB(148, 163, 184));
    }

    /* 2. HERO FULL SONG STRUCTURE CHART (x = 8..632, y = 144..376, height = 232px) */
    int gy = 144, gh = 232;
    fill_rect(8, gy, NATIVE_W - 16, gh, RGB(21, 30, 48));
    draw_border(8, gy, NATIVE_W - 16, gh, 2, RGB(51, 68, 98));

    int page_bars  = 16;
    int total_pages = (st->num_bars + page_bars - 1) / page_bars;
    int cur_page   = bar / page_bars;
    int page_start = cur_page * page_bars;
    int bars_on_page = st->num_bars - page_start;
    if (bars_on_page > page_bars) bars_on_page = page_bars;

    char chart_hdr[80];
    if (total_pages > 1) {
        snprintf(chart_hdr, sizeof(chart_hdr), "SONG CHART (%d BARS - PAGE %d/%d)",
                 st->num_bars, cur_page + 1, total_pages);
    } else {
        snprintf(chart_hdr, sizeof(chart_hdr), "SONG CHART (%d BARS)", st->num_bars);
    }
    draw_text(FONT_UI_13, 18, gy + 7, chart_hdr, RGB(250, 204, 21));

    /* Solo Scale summary (omit parenthetical degrees for clean fit) + L2 Fretboard shortcut pill */
    char clean_sc[64];
    snprintf(clean_sc, sizeof(clean_sc), "%s", st->scale_name);
    char *paren = strchr(clean_sc, '(');
    if (paren) {
        *paren = '\0';
        pb_trim(clean_sc);
    }
    char sc_mini[96];
    snprintf(sc_mini, sizeof(sc_mini), "Scale: %s %s", k_note_names[g_key_pc], clean_sc);
    draw_text_clipped(FONT_UI_11, 212, gy + 9, 284, sc_mini, RGB(148, 163, 184));

    draw_pill(504, gy + 4, 120, 22, RGB(23, 52, 74), RGB(56, 189, 248));
    draw_text_centered(FONT_UI_11, 504, gy + 4, 120, 22, "[L2] Fretboard", RGB(224, 242, 254));

    /* Multi-row 4-bar-per-line Chart Grid (y = gy + 30 .. gy + gh - 8 = 194px height) */
    int num_rows = (bars_on_page + 3) / 4;
    if (num_rows < 1) num_rows = 1;
    if (num_rows > 4) num_rows = 4;

    int grid_y0 = gy + 32;
    int grid_h  = gh - 40; /* 192px */
    int row_gap = (num_rows <= 2) ? 10 : (num_rows == 3) ? 8 : 6;
    int row_h   = (grid_h - (num_rows - 1) * row_gap) / num_rows;

    int sec_x   = 16;
    int sec_w   = 48;
    int col_x0  = 70;
    int col_gap = 6;
    int cell_w  = (624 - col_x0 - 3 * col_gap) / 4; /* 134px per bar cell */

    int spb = st->steps_per_bar;
    float bar_prog = g_playing ? ((float)(g_cur_step + 1) / (float)(spb > 0 ? spb : 1)) : 0.0f;

    for (int r = 0; r < num_rows; r++) {
        int ry = grid_y0 + r * (row_h + row_gap);
        int row_first_bar = page_start + r * 4;
        int row_idx_global = row_first_bar / 4;
        bool row_active = (bar >= row_first_bar && bar < row_first_bar + 4);

        /* Section Gutter Pill (A, A', B, BRG, TURN, or B1-4) */
        char sec_lbl[32];
        if (row_idx_global < st->num_sections && st->section_labels[row_idx_global][0]) {
            snprintf(sec_lbl, sizeof(sec_lbl), "%s", st->section_labels[row_idx_global]);
        } else {
            snprintf(sec_lbl, sizeof(sec_lbl), "%d-%d",
                     row_first_bar + 1,
                     (row_first_bar + 4 <= st->num_bars) ? (row_first_bar + 4) : st->num_bars);
        }
        draw_pill(sec_x, ry, sec_w, row_h,
                  row_active ? RGB(30, 58, 95) : RGB(15, 22, 34),
                  row_active ? RGB(250, 204, 21) : RGB(45, 62, 92));
        draw_text_fit(FONT_UI_13, sec_x, ry, sec_w, row_h, sec_lbl,
                      row_active ? RGB(254, 240, 138) : RGB(148, 163, 184));

        /* 4 Measure Cells in this Row */
        for (int c = 0; c < 4; c++) {
            int bidx = row_first_bar + c;
            int cx = col_x0 + c * (cell_w + col_gap);
            if (bidx >= st->num_bars) {
                draw_pill(cx, ry, cell_w, row_h, RGB(14, 20, 32), RGB(32, 44, 66));
                continue;
            }
            const BarChord *bc = &st->bars[bidx];
            char cstr[24];
            format_chord_label(g_key_pc, bc, cstr, sizeof(cstr));

            bool is_cur  = (bidx == bar);
            bool is_next = (bidx == next_bar);
            bool is_fill = ((bidx + 1) % 4 == 0 && g_auto_fill);

            uint32_t fill = is_cur  ? RGB(37, 99, 235) :
                            is_next ? RGB(26, 46, 76)  : RGB(15, 22, 34);
            uint32_t bdr  = is_cur  ? RGB(250, 204, 21) :
                            is_next ? RGB(96, 165, 250) : RGB(45, 62, 92);

            draw_pill(cx, ry, cell_w, row_h, fill, bdr);
            if (is_cur) {
                draw_border(cx - 1, ry - 1, cell_w + 2, row_h + 2, 1, RGB(254, 240, 138));
            }

            /* Bar number in top-left corner */
            char bnum[16];
            snprintf(bnum, sizeof(bnum), "%d", bidx + 1);
            draw_text(FONT_UI_11, cx + 6, ry + 3, bnum,
                      is_cur ? RGB(254, 240, 138) : RGB(100, 116, 139));

            /* Subtle turnaround / fill indicator dot in top-right of every 4th bar */
            if (is_fill) {
                fill_rect(cx + cell_w - 10, ry + 6, 4, 4,
                          is_cur ? RGB(244, 114, 182) : RGB(148, 86, 130));
            }

            if (num_rows >= 4) {
                /* 16-bar compact layout (134x44px): Chord centered-top, Roman bottom-right */
                draw_text_fit(FONT_UI_16, cx + 16, ry + 3, cell_w - 22, 22, cstr,
                              is_cur ? RGB(254, 240, 138) : RGB(241, 245, 249));
                draw_text_fit(FONT_UI_11, cx + 6, ry + 24, cell_w - 12, 14, bc->roman,
                              is_cur ? RGB(186, 230, 253) : RGB(120, 136, 158));
            } else if (num_rows == 3) {
                /* 12-bar spacious layout (134x58px) */
                draw_text_fit(FONT_UI_20, cx + 10, ry + 6, cell_w - 20, 28, cstr,
                              is_cur ? RGB(254, 240, 138) : RGB(241, 245, 249));
                draw_text_fit(FONT_UI_11, cx + 6, ry + 36, cell_w - 12, 16, bc->roman,
                              is_cur ? RGB(186, 230, 253) : RGB(120, 136, 158));
            } else {
                /* 8-bar hero layout (134x86px) */
                draw_text_fit(FONT_TITLE_28, cx + 8, ry + 14, cell_w - 16, 36, cstr,
                              is_cur ? RGB(254, 240, 138) : RGB(241, 245, 249));
                draw_text_fit(FONT_UI_13, cx + 6, ry + 54, cell_w - 12, 20, bc->roman,
                              is_cur ? RGB(186, 230, 253) : RGB(148, 163, 184));
            }

            /* Active-bar beat progress bar along the bottom edge of the active cell */
            if (is_cur && bar_prog > 0.0f) {
                int pw = (int)(bar_prog * (float)(cell_w - 8));
                if (pw < 4) pw = 4;
                fill_rect(cx + 4, ry + row_h - 5, cell_w - 8, 3, RGB(23, 52, 110));
                fill_rect(cx + 4, ry + row_h - 5, pw, 3, RGB(250, 204, 21));
            }
        }
    }
}

/* Full-screen [L2] Fretboard Modal Overlay for the Current Key & Active Song Scale */
static void render_fretboard_modal(void) {
    const StylePreset *st = &g_songs[g_style_idx];
    int bar = g_cur_bar % st->num_bars;
    const BarChord *cur_bc = &st->bars[bar];

    char cur_chord_str[24], tones_str[48];
    format_chord_label(g_key_pc, cur_bc, cur_chord_str, sizeof(cur_chord_str));
    format_chord_tones(g_key_pc, cur_bc, tones_str, sizeof(tones_str));

    int mx = 10, my = 12, mw = NATIVE_W - 20, mh = NATIVE_H - 24;
    fill_rect(mx, my, mw, mh, RGB(15, 23, 38));
    draw_border(mx, my, mw, mh, 2, RGB(56, 189, 248));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    /* Top Header */
    fill_rect(mx + 4, my + 4, mw - 8, 40, RGB(26, 38, 60));
    fill_rect(mx + 4, my + 44, mw - 8, 2, RGB(56, 78, 112));

    draw_pill(mx + 12, my + 10, 124, 28, RGB(23, 52, 74), RGB(56, 189, 248));
    draw_text_centered(FONT_UI_13, mx + 12, my + 10, 124, 28, "L2: FRETBOARD", RGB(224, 242, 254));

    char hdr_title[80];
    snprintf(hdr_title, sizeof(hdr_title), "KEY OF %s  -  %s", k_note_names[g_key_pc], st->name);
    draw_text_clipped(FONT_UI_16, mx + 144, my + 14, 316, hdr_title, RGB(254, 240, 138));

    char now_pill[32];
    snprintf(now_pill, sizeof(now_pill), "NOW: %s", cur_chord_str);
    draw_pill(mx + mw - 144, my + 10, 132, 28, RGB(146, 92, 12), RGB(250, 204, 21));
    draw_text_fit(FONT_UI_13, mx + mw - 144, my + 10, 132, 28, now_pill, RGB(255, 255, 255));

    /* Scale Banner + Color Legend */
    fill_rect(mx + 4, my + 46, mw - 8, 34, RGB(19, 29, 48));
    fill_rect(mx + 4, my + 80, mw - 8, 1, RGB(48, 66, 98));

    char sc_hdr[112];
    snprintf(sc_hdr, sizeof(sc_hdr), "SCALE: %s %s", k_note_names[g_key_pc], st->scale_name);
    draw_text_clipped(FONT_UI_13, mx + 14, my + 54, 380, sc_hdr, RGB(250, 204, 21));

    draw_pill(mx + mw - 196, my + 50, 184, 26, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_11, mx + mw - 196, my + 50, 184, 26,
                       g_neck_notes ? "[A/X] Labels: NOTE NAMES" : "[A/X] Labels: INTERVALS",
                       RGB(224, 242, 254));

    /* Spacious 12-Fret Rosewood Guitar Neck */
    int nut_x  = mx + 64;
    int fret_w = 44; /* 12 frets * 44px = 528px */
    int neck_w = fret_w * 12;
    int str_y0 = my + 124;
    int str_dy = 36; /* 5 spaces * 36px = 180px */
    int neck_h = str_dy * 5;

    fill_rect(nut_x, str_y0 - 14, neck_w, neck_h + 28, RGB(38, 26, 20));
    draw_border(nut_x, str_y0 - 14, neck_w, neck_h + 28, 2, RGB(108, 76, 52));
    fill_rect(nut_x - 6, str_y0 - 14, 7, neck_h + 28, RGB(241, 245, 249));

    for (int f = 0; f <= 12; f++) {
        int fx = nut_x + f * fret_w;
        if (f > 0) {
            fill_rect(fx - 1, str_y0 - 13, 2, neck_h + 26, RGB(148, 163, 184));
        }
        int cx = (f == 0) ? (nut_x - 20) : (nut_x + f * fret_w - fret_w / 2);
        char fn[8];
        snprintf(fn, sizeof(fn), "%d", f);
        bool marker = (f == 0 || f == 3 || f == 5 || f == 7 || f == 9 || f == 12);
        draw_text_centered(FONT_UI_13, cx - 12, my + 88, 24, 18, fn,
                           marker ? RGB(250, 204, 21) : RGB(148, 163, 184));

        if (f == 3 || f == 5 || f == 7 || f == 9) {
            draw_circle_aa((float)cx, (float)(str_y0 + neck_h / 2), 4.8f, 1.0f,
                           RGB(82, 75, 68), RGB(130, 122, 115));
        } else if (f == 12) {
            draw_circle_aa((float)cx, (float)(str_y0 + str_dy * 1 + str_dy / 2), 4.8f, 1.0f,
                           RGB(82, 75, 68), RGB(130, 122, 115));
            draw_circle_aa((float)cx, (float)(str_y0 + str_dy * 3 + str_dy / 2), 4.8f, 1.0f,
                           RGB(82, 75, 68), RGB(130, 122, 115));
        }
    }

    int cur_chord_pc = (g_key_pc + cur_bc->root_offset) % 12;

    for (int r = 0; r < 6; r++) {
        int sy = str_y0 + r * str_dy;
        int thick = (r >= 3) ? 2 : 1;
        fill_rect(nut_x, sy - thick / 2, neck_w, thick,
                  (r >= 3) ? RGB(203, 213, 225) : RGB(148, 163, 184));
        draw_text(FONT_UI_16, mx + 14, sy - 10, k_gtr_str_lbl[r], RGB(186, 230, 253));

        int open_pc = k_gtr_str_pc[r];
        for (int f = 0; f <= 12; f++) {
            int pc  = (open_pc + f) % 12;
            int rel = (pc - g_key_pc + 12) % 12;
            uint8_t mask = st->scale_mask[rel];
            if (mask > 0) {
                int cx = (f == 0) ? (nut_x - 20) : (nut_x + f * fret_w - fret_w / 2);
                bool is_key_root   = (pc == g_key_pc);
                bool is_chord_root = (pc == cur_chord_pc);
                uint32_t dfill = is_key_root   ? RGB(245, 158, 11) :
                                 is_chord_root ? RGB(5, 150, 105)  :
                                 (mask == 2)   ? RGB(219, 39, 119) : RGB(2, 132, 199);
                uint32_t dbdr  = is_key_root   ? RGB(254, 240, 138) :
                                 is_chord_root ? RGB(167, 243, 208) : RGB(224, 242, 254);
                uint32_t tcol  = is_key_root   ? RGB(15, 23, 42)    : RGB(255, 255, 255);

                draw_circle_aa((float)cx, (float)sy, 13.0f, 2.0f, dfill, dbdr);
                const char *dlbl = g_neck_notes ? k_note_names[pc] : k_interval_labels[rel];
                draw_text_centered(FONT_UI_13, cx - 13, sy - 12, 26, 24, dlbl, tcol);
            }
        }
    }

    /* Legend + Active Bar Target Chord Tones Strip */
    int ly = my + 332;
    fill_rect(mx + 14, ly, mw - 28, 74, RGB(21, 31, 50));
    draw_border(mx + 14, ly, mw - 28, 74, 1, RGB(51, 68, 98));

    /* Legend row */
    draw_circle_aa((float)(mx + 34), (float)(ly + 20), 8.0f, 1.5f, RGB(245, 158, 11), RGB(254, 240, 138));
    draw_text(FONT_UI_13, mx + 48, ly + 12, "Key Root", RGB(254, 240, 138));

    draw_circle_aa((float)(mx + 160), (float)(ly + 20), 8.0f, 1.5f, RGB(5, 150, 105), RGB(167, 243, 208));
    draw_text(FONT_UI_13, mx + 174, ly + 12, "Active Chord Root", RGB(167, 243, 208));

    draw_circle_aa((float)(mx + 344), (float)(ly + 20), 8.0f, 1.5f, RGB(219, 39, 119), RGB(251, 207, 232));
    draw_text(FONT_UI_13, mx + 358, ly + 12, "Blue / Color", RGB(251, 207, 232));

    draw_circle_aa((float)(mx + 478), (float)(ly + 20), 8.0f, 1.5f, RGB(2, 132, 199), RGB(224, 242, 254));
    draw_text(FONT_UI_13, mx + 492, ly + 12, "Scale Tone", RGB(224, 242, 254));

    /* Live Target Chord Tones */
    char target_line[160];
    snprintf(target_line, sizeof(target_line), "Bar %d/%d: %s (%s)   |   Target Chord Tones: %s",
             bar + 1, st->num_bars, cur_chord_str, cur_bc->roman, tones_str);
    draw_text_centered(FONT_UI_13, mx + 18, ly + 42, mw - 36, 24, target_line, RGB(248, 250, 252));

    /* Modal Footer */
    fill_rect(mx + 4, my + mh - 34, mw - 8, 30, RGB(12, 18, 30));
    fill_rect(mx + 4, my + mh - 34, mw - 8, 1, RGB(51, 65, 85));
    draw_text_centered(FONT_UI_11, mx + 8, my + mh - 32, mw - 16, 26,
                       "[A/X] Intervals/Notes  |  [U/D] Transpose Key  |  [L/R] Song  |  [L2/B] Close",
                       RGB(186, 230, 253));
}

static void render_step_editor_view(void) {
    const StylePreset *st = &g_songs[g_style_idx];
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
                    int off = bass_degree_semitone_offset(cur_bc, val);
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
    const StylePreset *st = &g_songs[g_style_idx];
    int mx = 16, my = 10, mw = NATIVE_W - 32, mh = NATIVE_H - 20;
    fill_rect(mx, my, mw, mh, RGB(15, 23, 38));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    fill_rect(mx + 4, my + 4, mw - 8, 36, RGB(28, 39, 60));
    fill_rect(mx + 4, my + 40, mw - 8, 2, RGB(64, 82, 115));
    draw_pill(mx + 12, my + 8, 72, 26, RGB(37, 99, 235), RGB(147, 197, 253));
    draw_text_centered(FONT_UI_13, mx + 12, my + 8, 72, 26, "HELP", RGB(255, 255, 255));
    draw_text(FONT_UI_16, mx + 94, my + 12, "POCKETBAND - SONG STRUCTURE & CONTROLS", RGB(248, 250, 252));

    /* Current Song Structure, Famous Examples & Groove Settings Card */
    int gy = my + 48;
    int gh = 114;
    fill_rect(mx + 14, gy, mw - 28, gh, RGB(23, 33, 52));
    draw_border(mx + 14, gy, mw - 28, gh, 2, RGB(52, 211, 153));

    char g1[112];
    snprintf(g1, sizeof(g1), "Song [%d/%d]: %s (%d Bars, %d/4)  |  Key %s  |  %d BPM",
             g_style_idx + 1, g_num_songs, st->name, st->num_bars,
             st->beats_per_bar, k_note_names[g_key_pc], g_bpm);
    draw_text_fit(FONT_UI_13, mx + 18, gy + 6, mw - 36, 20, g1, RGB(250, 204, 21));

    char ex_line[160];
    snprintf(ex_line, sizeof(ex_line), "Follows: %s", st->examples);
    draw_text_wrapped_centered(FONT_UI_13, mx + 20, gy + 28, mw - 40, 18, 2, ex_line, RGB(248, 250, 252));

    char file_line[112];
    snprintf(file_line, sizeof(file_line), "File: songs/%s   |   Scale: %s",
             st->filename, st->scale_name);
    draw_text_fit(FONT_UI_11, mx + 20, gy + 68, mw - 40, 16, file_line, RGB(148, 163, 184));

    char g2[96];
    snprintf(g2, sizeof(g2), "[LEFT / RIGHT] Swing: %d%%   |   [A] 4-Bar Auto-Fill: %s",
             g_swing_pct, g_auto_fill ? "ON" : "OFF");
    draw_text_centered(FONT_UI_13, mx + 18, gy + 88, mw - 36, 20, g2, RGB(167, 243, 208));

    /* Controls Table */
    int ky = gy + gh + 10;
    static const struct {
        const char *btn;
        const char *desc;
    } k_rows[7] = {
        { "SELECT",       "Switch Song Chart Jam View <-> 5-Track Step Editor" },
        { "A Button",     "Jam: Play/Stop  |  Step Edit: Cycle Hit / Bass Note" },
        { "B Button",     "Jam: Queue Drum Fill  |  Step Edit: Clear Step" },
        { "X Button",     "Cycle Mix: Full Band -> Drums+Bass -> Drums Only" },
        { "LEFT / RIGHT", "Jam: Cycle Disk Songs (*.song)  |  Edit: Select Step" },
        { "UP / DOWN",    "Jam: Transpose Key (C .. B)  |  Edit: Select Track" },
        { "L1/R1 / L2/R2","L1/R1: Tempo -/+5 BPM | L2: Fretboard | R2: Swing" }
    };

    for (int i = 0; i < 7; i++) {
        int ry = ky + i * 35;
        draw_pill(mx + 16, ry, 134, 27, RGB(30, 58, 95), RGB(71, 98, 145));
        draw_text_centered(FONT_UI_13, mx + 16, ry, 134, 27, k_rows[i].btn, RGB(254, 240, 138));
        draw_text(FONT_UI_13, mx + 160, ry + 5, k_rows[i].desc, RGB(226, 232, 240));
    }

    fill_rect(mx + 4, my + mh - 32, mw - 8, 28, RGB(12, 18, 30));
    fill_rect(mx + 4, my + mh - 32, mw - 8, 1, RGB(51, 65, 85));
    draw_text_centered(FONT_UI_13, mx, my + mh - 30, mw, 24,
                       "Press [Y] or [B] to close Help   |   [MENU] Save & Exit",
                       RGB(186, 230, 253));
}

static void render_ui(void) {
    fill_rect(0, 0, NATIVE_W, NATIVE_H, RGB(13, 18, 28));
    if (g_num_songs <= 0) {
        draw_text_centered(FONT_UI_20, 20, 200, NATIVE_W - 40, 32,
                           "No .song files found in songs/ directory", RGB(248, 113, 113));
        return;
    }
    render_top_status_bar();
    if (g_view == VIEW_JAM) {
        render_jam_view();
    } else {
        render_step_editor_view();
    }
    render_step_strip_and_footer();
    if (g_show_fretboard) {
        render_fretboard_modal();
    } else if (g_show_help) {
        render_help_modal();
    }
}

/* ============================================================================
 * 9. MAIN ENTRY POINT & 60 FPS EVENT LOOP
 * ============================================================================ */

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    if (load_all_songs_from_disk() <= 0) {
        fprintf(stderr, "pocketband: error: no valid *.song files found in ./songs or /userdata/roms/ports/pocketband/songs\n");
        return 1;
    }

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
        printf("pocketband audio-test: songs=%d sf2=%s path=%s peak=%d rms=%.1f\n",
               g_num_songs,
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

        /* 1. Jam View: 16-bar Blue Bossa (or first 16-bar song) showing full 4x4 chart */
        int bossa_idx = find_song_by_filename("11_blue_bossa.song");
        apply_style(bossa_idx >= 0 ? bossa_idx : 0);
        g_key_pc         = 0; /* Key of C */
        g_view           = VIEW_JAM;
        g_mix_mode       = MIX_FULL_BAND;
        g_neck_notes     = false;
        g_playing        = true;
        g_cur_bar        = 4;
        g_cur_step       = 0;
        g_show_help      = false;
        g_show_fretboard = false;
        g_synth.step_sample_pos = 0;
        for (int i = 0; i < 6; i++) audio_render_frames(dummy_pcm, AUDIO_FRAMES);
        g_cur_bar  = 4;
        g_cur_step = 3;
        render_ui();
        snprintf(path, sizeof(path), "%s/pocketband_jam.bmp", dir);
        save_bmp(path);

        /* 2. Step Sequencer View (4/4 Classic Rock Drum & Bass Grid) */
        int rock_idx = find_song_by_filename("04_classic_rock.song");
        apply_style(rock_idx >= 0 ? rock_idx : 0);
        g_key_pc         = 4; /* Key of E */
        g_view           = VIEW_STEP_EDITOR;
        g_edit_track     = TRK_BASS;
        g_edit_step      = 3;
        g_cur_step       = 4;
        g_show_help      = false;
        g_show_fretboard = false;
        render_ui();
        snprintf(path, sizeof(path), "%s/pocketband_steps.bmp", dir);
        save_bmp(path);

        /* 3. 12-Bar Blues Shuffle Jam View in Key of A */
        int blues_idx = find_song_by_filename("01_blues_shuffle.song");
        apply_style(blues_idx >= 0 ? blues_idx : 0);
        g_key_pc         = 9; /* Key of A */
        g_view           = VIEW_JAM;
        g_mix_mode       = MIX_FULL_BAND;
        g_cur_bar        = 4;
        g_cur_step       = 0;
        g_show_help      = false;
        g_show_fretboard = false;
        g_synth.step_sample_pos = 0;
        for (int i = 0; i < 6; i++) audio_render_frames(dummy_pcm, AUDIO_FRAMES);
        g_cur_bar  = 4;
        g_cur_step = 4;
        render_ui();
        snprintf(path, sizeof(path), "%s/pocketband_waltz.bmp", dir);
        save_bmp(path);

        /* 4. Full-Screen [L2] Fretboard Modal Overlay (Key of A, Intervals) */
        g_show_fretboard = true;
        g_neck_notes     = false;
        render_ui();
        snprintf(path, sizeof(path), "%s/pocketband_fretboard.bmp", dir);
        save_bmp(path);

        /* 5. Help & Song Structure Info Modal (showing famous song examples!) */
        int desc_idx = find_song_by_filename("06_descending_rock.song");
        apply_style(desc_idx >= 0 ? desc_idx : 0);
        g_key_pc         = 11; /* Key of B */
        g_show_fretboard = false;
        g_show_help      = true;
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
            if (g_show_help) g_show_fretboard = false;
        } else if (pressed & BTN_L2) {
            g_show_fretboard = !g_show_fretboard;
            if (g_show_fretboard) g_show_help = false;
        } else if (pressed & BTN_L1) {
            g_bpm -= 5;
            if (g_bpm < 45) g_bpm = 45;
            save_config();
        } else if (pressed & BTN_R1) {
            g_bpm += 5;
            if (g_bpm > 220) g_bpm = 220;
            save_config();
        } else if (pressed & BTN_R2) {
            g_swing_pct += 3;
            if (g_swing_pct > 68) g_swing_pct = 50;
            save_config();
        } else if (g_show_fretboard) {
            if (pressed & (BTN_A | BTN_X)) {
                g_neck_notes = !g_neck_notes;
                save_config();
            } else if (pressed & BTN_UP) {
                g_key_pc = (g_key_pc + 1) % 12;
                save_config();
            } else if (pressed & BTN_DOWN) {
                g_key_pc = (g_key_pc + 11) % 12;
                save_config();
            } else if (pressed & BTN_RIGHT) {
                apply_style(g_style_idx + 1);
                save_config();
            } else if (pressed & BTN_LEFT) {
                apply_style(g_style_idx - 1);
                save_config();
            } else if (pressed & (BTN_B | BTN_SELECT)) {
                g_show_fretboard = false;
            }
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
            } else if (pressed & BTN_X) {
                g_mix_mode = (BackingMixMode)(((int)g_mix_mode + 1) % 3);
                save_config();
            } else if (pressed & (BTN_B | BTN_SELECT)) {
                g_show_help = false;
            }
        } else if (pressed & BTN_X) {
            g_mix_mode = (BackingMixMode)(((int)g_mix_mode + 1) % 3);
            save_config();
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
            int spb = g_songs[g_style_idx].steps_per_bar;
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
