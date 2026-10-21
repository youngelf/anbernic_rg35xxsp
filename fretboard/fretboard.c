/*
 * Anbernic RG 35xxSP Interactive Guitar Fretboard, CAGED Scale, Chord & Arpeggio
 * Reference (fretboard.c)
 *
 * Native 640x480 single-screen edition engineered for the Anbernic RG 35xxSP
 * (Allwinner H700 quad-core 64-bit ARM Cortex-A53) and x86_64 Linux (X11).
 *
 * Key Upgrades for RG 35xxSP:
 *   1. High-Fidelity 44.1 kHz Stereo Floating-Point Extended Karplus-Strong (EKS)
 *      Audio Engine (upgraded from 22.05 kHz mono integer):
 *      - 6-voice polyphonic physical string modeling with first-order Thiran
 *        all-pass fractional delay interpolation, pick-position comb filtering,
 *        dual-biquad acoustic guitar body resonance (~102 Hz air + ~215 Hz top),
 *        and stereo modulated chorus.
 *      - 3 selectable guitar timbres (L2 / '1'): Acoustic Dreadnought, Chorus
 *        Electric, and Warm Overdrive Lead.
 *   2. Dedicated Background Audio Synthesis Thread:
 *      - All scale run scheduling, chord strumming, waveguide excitation, and
 *        body filtering run asynchronously in the background audio thread via a
 *        lock-free atomic command ring buffer so the 640x480 UI stays at 60 FPS.
 *   3. Hardware-Accurate RG 35xxSP Controls & Linux X11 Keyboard Support:
 *      - Maps evdev codes (304=A, 305=B, 306=X, 307=Y, 308=L1, 309=R1,
 *        310/314=SELECT, 311/315=START, 312=L2, 313=R2) plus desktop keys.
 *   4. Rich 640x480 Full-Neck Visualization:
 *      - Spacious 15-fret 6-string rosewood guitar neck with real-time vibrating
 *        string animation, glowing active plucked notes, CAGED box frames,
 *        6 alternate guitar tunings (R2 / '2'), and a live oscilloscope.
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
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#if defined(HOST_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#else
#include <dirent.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

#include "fretboard_font.h"

/* ============================================================================
 * 1. CONSTANTS & DISPLAY DIMENSIONS (640x480 NATIVE)
 * ============================================================================ */

#define NATIVE_W        640
#define NATIVE_H        480
#define SAMPLE_RATE     44100
#define AUDIO_CHANNELS  2
#define NUM_STRINGS     6
#define MAX_FRET        15
#define NUM_FRETS       16
#define SCOPE_SAMPLES   512

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static volatile sig_atomic_t g_running = 1;
static void handle_sig(int sig) { (void)sig; g_running = 0; }

static uint32_t g_fb[NATIVE_W * NATIVE_H];

/* ============================================================================
 * 2. INPUT BUTTON BITMASKS (RG 35xxSP + X11 DESKTOP)
 * ============================================================================ */

#undef BTN_UP
#undef BTN_DOWN
#undef BTN_LEFT
#undef BTN_RIGHT
#undef BTN_A
#undef BTN_B
#undef BTN_X
#undef BTN_Y
#undef BTN_L1
#undef BTN_R1
#undef BTN_L2
#undef BTN_R2
#undef BTN_SELECT
#undef BTN_START
#undef BTN_MENU
#undef BTN_QUIT

#define BTN_UP      (1u << 0)
#define BTN_DOWN    (1u << 1)
#define BTN_LEFT    (1u << 2)
#define BTN_RIGHT   (1u << 3)
#define BTN_A       (1u << 4)   /*RG35xxSP 304: Audition Scale / Strum / Pluck  */
#define BTN_B       (1u << 5)   /*RG35xxSP 305: Toggle Skeleton / Arp / Lock    */
#define BTN_X       (1u << 6)   /*RG35xxSP 306: Cycle Dot Labels (Int/Note/Deg) */
#define BTN_Y       (1u << 7)   /*RG35xxSP 307: Help & Guitar Tuning Modal      */
#define BTN_L1      (1u << 8)   /*RG35xxSP 308: Prev CAGED Box / Chord Voicing  */
#define BTN_R1      (1u << 9)   /*RG35xxSP 309: Next CAGED Box / Chord Voicing  */
#define BTN_L2      (1u << 10)  /*RG35xxSP 312: Cycle Timbre (Acoustic/El/Lead) */
#define BTN_R2      (1u << 11)  /*RG35xxSP 313: Cycle 6 Guitar Tunings          */
#define BTN_SELECT  (1u << 12)  /*RG35xxSP 310/314: Mode (SCALES/CHORDS/EXPLORE)*/
#define BTN_START   (1u << 13)  /*RG35xxSP 311/315: Save & Exit                 */
#define BTN_MENU    (1u << 14)  /*RG35xxSP 316: Save & Exit                     */
#define BTN_QUIT    (1u << 15)

/* ============================================================================
 * 3. MUSIC THEORY DATABASE: NOTES, TUNINGS, 14 SCALES & 12 CHORD QUALITIES
 * ============================================================================ */

static const char *k_note_names[12] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
};

static const char *k_interval_labels[12] = {
    "R", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7"
};

static const char *k_degree_labels[12] = {
    "1", "b2", "2", "b3", "3", "4", "#4", "5", "b6", "6", "b7", "7"
};

typedef struct {
    const char *name;
    const char *short_name;
    int         midi[6];    /* String 0 (lowest 6th string) .. String 5 (highest 1st string) */
    const char *row_lbl[6]; /* Row 0 (top/high string) .. Row 5 (bottom/low string) */
} GuitarTuning;

static const GuitarTuning k_tunings[6] = {
    { "Standard (E A D G B e)",          "Std E",  { 40, 45, 50, 55, 59, 64 }, { "e", "B", "G", "D", "A", "E" } },
    { "Drop D (D A D G B e)",            "Drop D", { 38, 45, 50, 55, 59, 64 }, { "e", "B", "G", "D", "A", "D" } },
    { "Eb Standard (Eb Ab Db Gb Bb eb)", "Eb Std", { 39, 44, 49, 54, 58, 63 }, { "eb","Bb","Gb","Db","Ab","Eb"} },
    { "D Standard (D G C F A d)",        "D Std",  { 38, 43, 48, 53, 57, 62 }, { "d", "A", "F", "C", "G", "D" } },
    { "DADGAD (D A D G A d)",            "DADGAD", { 38, 45, 50, 55, 57, 62 }, { "d", "A", "G", "D", "A", "D" } },
    { "Open G (D G D G B d)",            "Open G", { 38, 43, 50, 55, 59, 62 }, { "d", "B", "G", "D", "G", "D" } }
};
#define NUM_TUNINGS 6

typedef struct {
    const char *name;
    const char *short_name;
    const char *formula;
    const char *skeleton_chord;
    uint8_t     mask[12]; /* 4=Root, 2=Chord Skeleton, 3=Color/Blue Note, 1=Scale Tone */
} ScaleDef;

static const ScaleDef k_scales[14] = {
    { "Minor Pentatonic",      "Min Pent",   "1 - b3 - 4 - 5 - b7",           "m7 / 7",        { 4,0,0,2,0,1,0,2,0,0,2,0 } },
    { "Blues Scale",           "Blues",      "1 - b3 - 4 - b5 - 5 - b7",      "7 / m7 (b5)",   { 4,0,0,2,0,1,3,2,0,0,2,0 } },
    { "Major Pentatonic",      "Maj Pent",   "1 - 2 - 3 - 5 - 6",             "Maj / 6 / Maj7",{ 4,0,1,0,2,0,0,2,0,3,0,0 } },
    { "Natural Major (Ionian)","Major",      "1 - 2 - 3 - 4 - 5 - 6 - 7",     "Maj7 (I-IV-V)", { 4,0,1,0,2,1,0,2,0,1,0,2 } },
    { "Natural Minor (Aeolian)","Nat Minor", "1 - 2 - b3 - 4 - 5 - b6 - b7",  "m7 (i-iv-v)",   { 4,0,1,2,0,1,0,2,3,0,2,0 } },
    { "Dorian Mode",           "Dorian",     "1 - 2 - b3 - 4 - 5 - 6 - b7",   "m7 / m6 (6)",   { 4,0,1,2,0,1,0,2,0,3,2,0 } },
    { "Mixolydian Mode",       "Mixolydian", "1 - 2 - 3 - 4 - 5 - 6 - b7",    "Dom 7 (b7)",    { 4,0,1,0,2,1,0,2,0,1,3,0 } },
    { "Lydian Mode",           "Lydian",     "1 - 2 - 3 - #4 - 5 - 6 - 7",    "Maj7#11 (#4)",  { 4,0,1,0,2,0,3,2,0,1,0,2 } },
    { "Phrygian Mode",         "Phrygian",   "1 - b2 - b3 - 4 - 5 - b6 - b7", "m7 / susb9",    { 4,3,0,2,0,1,0,2,1,0,2,0 } },
    { "Harmonic Minor",        "Harm Minor", "1 - 2 - b3 - 4 - 5 - b6 - 7",   "m(maj7)",       { 4,0,1,2,0,1,0,2,1,0,0,3 } },
    { "Melodic Minor (Jazz)",  "Mel Minor",  "1 - 2 - b3 - 4 - 5 - 6 - 7",    "m(maj7) / m6",  { 4,0,1,2,0,1,0,2,0,3,0,3 } },
    { "Phrygian Dominant",     "Phryg Dom",  "1 - b2 - 3 - 4 - 5 - b6 - b7",  "7b9 (Flamenco)",{ 4,3,0,0,2,1,0,2,1,0,2,0 } },
    { "Locrian Mode",          "Locrian",    "1 - b2 - b3 - 4 - b5 - b6 - b7","m7b5",          { 4,1,0,2,0,1,3,0,1,0,2,0 } },
    { "Whole-Half Diminished", "Diminished", "1 - 2 - b3 - 4 - b5 - b6 - 6 - 7","dim7",        { 4,0,1,2,0,1,3,0,1,2,0,3 } }
};
#define NUM_SCALES 14

typedef struct {
    const char *name;
    const char *suffix;
    const char *formula;
    uint8_t     mask[12];
    int         pref_bass_str;
} ChordQualityDef;

static const ChordQualityDef k_chords[12] = {
    { "Major Triad",      "",     "1 - 3 - 5",        { 4,0,0,0,2,0,0,2,0,0,0,0 }, 0 },
    { "Minor Triad",      "m",    "1 - b3 - 5",       { 4,0,0,2,0,0,0,2,0,0,0,0 }, 0 },
    { "Dominant 7th",     "7",    "1 - 3 - 5 - b7",   { 4,0,0,0,2,0,0,2,0,0,3,0 }, 0 },
    { "Major 7th",        "maj7", "1 - 3 - 5 - 7",    { 4,0,0,0,2,0,0,2,0,0,0,3 }, 0 },
    { "Minor 7th",        "m7",   "1 - b3 - 5 - b7",  { 4,0,0,2,0,0,0,2,0,0,3,0 }, 0 },
    { "Half-Diminished",  "m7b5", "1 - b3 - b5 - b7", { 4,0,0,2,0,0,3,0,0,0,2,0 }, 1 },
    { "Diminished 7th",   "dim7", "1 - b3 - b5 - 6",  { 4,0,0,2,0,0,3,0,0,2,0,0 }, 1 },
    { "Suspended 2nd",    "sus2", "1 - 2 - 5",        { 4,0,3,0,0,0,0,2,0,0,0,0 }, 1 },
    { "Suspended 4th",    "sus4", "1 - 4 - 5",        { 4,0,0,0,0,3,0,2,0,0,0,0 }, 0 },
    { "Major Add 9",      "add9", "1 - 2 - 3 - 5",    { 4,0,3,0,2,0,0,2,0,0,0,0 }, 0 },
    { "Major 6th",        "6",    "1 - 3 - 5 - 6",    { 4,0,0,0,2,0,0,2,0,3,0,0 }, 0 },
    { "Minor 6th",        "m6",   "1 - b3 - 5 - 6",   { 4,0,0,2,0,0,0,2,0,3,0,0 }, 0 }
};
#define NUM_CHORDS 12

static const char *k_caged_pos_names[6] = {
    "ALL NECK", "POS 1 (E)", "POS 2 (D)", "POS 3 (C)", "POS 4 (A)", "POS 5 (G)"
};
static const int k_caged_rel_start[5] = { 0, 2, 4, 7, 9 };

static float midi_to_freq(int midi) {
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}

/* ============================================================================
 * 4. BACKGROUND FLOATING-POINT EKS GUITAR SYNTHESIZER (44.1 kHz STEREO)
 * ============================================================================ */

typedef enum {
    TIMBRE_ACOUSTIC  = 0, /* Warm Steel-String Acoustic Dreadnought */
    TIMBRE_CHORUS_EL = 1, /* Clean Jazz Electric + Stereo Chorus    */
    TIMBRE_OVERDRIVE = 2  /* Warm Tube Overdrive Lead               */
} GuitarTimbre;

#define MAX_DELAY_LEN   1024
#define MAX_SCHED_PLUCK 36
#define CMD_RING_SIZE   16

typedef struct {
    int str_idx;
    int fret;
    int midi_note;
    int delay_ms;
} PluckSpec;

typedef struct {
    int count;
    int timbre;
    PluckSpec notes[MAX_SCHED_PLUCK];
} PluckBatchCmd;

static PluckBatchCmd     g_cmd_ring[CMD_RING_SIZE];
static volatile uint32_t g_cmd_write_idx = 0;
static volatile uint32_t g_cmd_read_idx  = 0;

typedef struct {
    bool  active;
    float delay_line[MAX_DELAY_LEN];
    int   delay_len;
    int   pos;
    int   samples_left;
    float damping;
    float stretch;
    float ap_coeff;
    float ap_x1;
    float ap_y1;
    float lp_prev;
    float pan_l;
    float pan_r;
    bool  overdrive;
} EksStringVoice;

typedef struct {
    bool pending;
    int  delay_samples;
    int  str_idx;
    int  fret;
    int  midi_note;
    int  timbre;
} ScheduledEvent;

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} BiquadFilter;

static EksStringVoice g_voices[NUM_STRINGS];
static ScheduledEvent g_sched[MAX_SCHED_PLUCK];
static uint32_t       g_prng_state = 0x9E3779B9u;
static BiquadFilter   g_body_air;
static BiquadFilter   g_body_top;
static bool           g_dsp_inited = false;

#define CHORUS_BUF_LEN 2048
static float          g_chorus_buf[CHORUS_BUF_LEN];
static int            g_chorus_pos = 0;
static float          g_chorus_lfo = 0.0f;

/* Shared atomic visual telemetry for 60 FPS UI */
static volatile int      g_active_play_str  = -1;
static volatile int      g_active_play_fret = -1;
static volatile int      g_active_play_ttl  = 0;
static volatile float    g_string_env[NUM_STRINGS];
static volatile int16_t  g_scope_buf[SCOPE_SAMPLES];
static volatile uint32_t g_scope_write_idx = 0;

static inline float rand_bipolar(void) {
    g_prng_state = g_prng_state * 1664525u + 1013904223u;
    int32_t s = (int32_t)(g_prng_state >> 16) - 32768;
    return (float)s / 32768.0f;
}

static void biquad_set_peaking(BiquadFilter *f, float freq, float q, float gain_db) {
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * (float)M_PI * freq / (float)SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * q);
    float a0    = 1.0f + alpha / A;
    f->b0 = (1.0f + alpha * A) / a0;
    f->b1 = (-2.0f * cosw0) / a0;
    f->b2 = (1.0f - alpha * A) / a0;
    f->a1 = (-2.0f * cosw0) / a0;
    f->a2 = (1.0f - alpha / A) / a0;
    f->z1 = 0.0f;
    f->z2 = 0.0f;
}

static inline float biquad_process(BiquadFilter *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1   = f->b1 * x - f->a1 * y + f->z2;
    f->z2   = f->b2 * x - f->a2 * y;
    return y;
}

static void init_dsp_filters(void) {
    if (g_dsp_inited) return;
    biquad_set_peaking(&g_body_air, 102.0f, 2.2f, 4.5f);
    biquad_set_peaking(&g_body_top, 215.0f, 2.0f, 3.5f);
    g_dsp_inited = true;
}

static void excite_string_in_audio_thread(int str_idx, int fret, int midi_note, int timbre) {
    if (str_idx < 0 || str_idx >= NUM_STRINGS) return;
    float freq = midi_to_freq(midi_note);
    float exact_period = (float)SAMPLE_RATE / freq;
    int dlen = (int)floorf(exact_period - 0.5f);
    if (dlen < 12) dlen = 12;
    if (dlen >= MAX_DELAY_LEN - 4) dlen = MAX_DELAY_LEN - 4;

    float frac = exact_period - 0.5f - (float)dlen;
    if (frac < 0.05f) frac = 0.05f;
    if (frac > 0.95f) frac = 0.95f;

    EksStringVoice *v = &g_voices[str_idx];
    v->active       = false;
    v->delay_len    = dlen;
    v->pos          = 0;
    v->samples_left = (SAMPLE_RATE * 22) / 10;
    v->ap_coeff     = (1.0f - frac) / (1.0f + frac);
    v->ap_x1        = 0.0f;
    v->ap_y1        = 0.0f;
    v->lp_prev      = 0.0f;
    v->overdrive    = (timbre == TIMBRE_OVERDRIVE);

    float pan = ((float)str_idx / 5.0f) * 0.48f - 0.24f;
    v->pan_l  = 0.707f * (1.0f - pan);
    v->pan_r  = 0.707f * (1.0f + pan);

    if (timbre == TIMBRE_OVERDRIVE) {
        v->damping = (str_idx < 3) ? 0.9972f : 0.9962f;
        v->stretch = 0.62f;
    } else if (timbre == TIMBRE_CHORUS_EL) {
        v->damping = (str_idx < 3) ? 0.9965f : 0.9950f;
        v->stretch = 0.56f;
    } else {
        v->damping = (str_idx < 3) ? 0.9955f : 0.9935f;
        v->stretch = 0.48f;
    }

    int pick_offset = (dlen * 22) / 100;
    if (pick_offset < 2) pick_offset = 2;

    float raw[MAX_DELAY_LEN];
    float lp = 0.0f;
    float alpha = (timbre == TIMBRE_ACOUSTIC) ? 0.36f : 0.54f;

    for (int i = 0; i < dlen; i++) {
        float ph = (2.0f * (float)M_PI * (float)i) / (float)dlen;
        float body = 0.44f * sinf(ph)
                   + 0.22f * sinf(2.0f * ph)
                   + 0.11f * sinf(3.0f * ph);
        float n = rand_bipolar();
        lp = lp + alpha * (n - lp);
        raw[i] = body + lp * 0.55f;
    }

    float mean = 0.0f;
    for (int i = 0; i < dlen; i++) {
        int j = (i + dlen - pick_offset) % dlen;
        v->delay_line[i] = (raw[i] - 0.55f * raw[j]) * 0.75f;
        mean += v->delay_line[i];
    }
    mean /= (float)dlen;
    for (int i = 0; i < dlen; i++) {
        v->delay_line[i] -= mean;
    }

    g_string_env[str_idx] = 1.0f;
    g_active_play_str     = str_idx;
    g_active_play_fret    = fret;
    g_active_play_ttl     = SAMPLE_RATE / 4;
    v->active             = true;
}

static void post_pluck_batch(const PluckBatchCmd *cmd) {
    uint32_t w = g_cmd_write_idx;
    g_cmd_ring[w & (CMD_RING_SIZE - 1)] = *cmd;
    __sync_synchronize();
    g_cmd_write_idx = w + 1;
}

static void audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    init_dsp_filters();

    int16_t *out = (int16_t *)stream;
    int frames = len / (2 * AUDIO_CHANNELS);
    bool any_overdrive = false;

    while (g_cmd_read_idx != g_cmd_write_idx) {
        PluckBatchCmd cmd = g_cmd_ring[g_cmd_read_idx & (CMD_RING_SIZE - 1)];
        __sync_synchronize();
        g_cmd_read_idx++;

        for (int i = 0; i < MAX_SCHED_PLUCK; i++) g_sched[i].pending = false;
        for (int s = 0; s < NUM_STRINGS; s++)     g_voices[s].active = false;
        g_active_play_ttl = 0;

        for (int n = 0; n < cmd.count && n < MAX_SCHED_PLUCK; n++) {
            g_sched[n].pending       = true;
            g_sched[n].delay_samples = (SAMPLE_RATE * cmd.notes[n].delay_ms) / 1000;
            g_sched[n].str_idx       = cmd.notes[n].str_idx;
            g_sched[n].fret          = cmd.notes[n].fret;
            g_sched[n].midi_note     = cmd.notes[n].midi_note;
            g_sched[n].timbre        = cmd.timbre;
        }
    }

    for (int i = 0; i < frames; i++) {
        for (int s = 0; s < MAX_SCHED_PLUCK; s++) {
            if (g_sched[s].pending) {
                if (g_sched[s].delay_samples <= 0) {
                    g_sched[s].pending = false;
                    excite_string_in_audio_thread(g_sched[s].str_idx,
                                                  g_sched[s].fret,
                                                  g_sched[s].midi_note,
                                                  g_sched[s].timbre);
                } else {
                    g_sched[s].delay_samples--;
                }
            }
        }

        if (g_active_play_ttl > 0) {
            g_active_play_ttl--;
            if (g_active_play_ttl == 0) {
                g_active_play_str  = -1;
                g_active_play_fret = -1;
            }
        }

        float mix_l = 0.0f;
        float mix_r = 0.0f;
        float mono_sum = 0.0f;

        for (int s = 0; s < NUM_STRINGS; s++) {
            EksStringVoice *sv = &g_voices[s];
            if (!sv->active) continue;
            if (sv->samples_left <= 0) {
                sv->active = false;
                continue;
            }
            sv->samples_left--;
            if (sv->overdrive) any_overdrive = true;

            int p0 = sv->pos;
            int p1 = (p0 + 1 < sv->delay_len) ? (p0 + 1) : 0;
            float s0 = sv->delay_line[p0];
            float s1 = sv->delay_line[p1];

            float avg = (1.0f - sv->stretch) * s0 + sv->stretch * s1;
            float damped = (0.86f * avg + 0.14f * sv->lp_prev) * sv->damping;
            sv->lp_prev = damped;

            float ap_out = sv->ap_coeff * damped + sv->ap_x1 - sv->ap_coeff * sv->ap_y1;
            sv->ap_x1 = damped;
            sv->ap_y1 = ap_out;

            sv->delay_line[p0] = ap_out;
            sv->pos = p1;

            float sig = s0;
            if (sv->overdrive) {
                float x = sig * 2.4f;
                sig = x / (1.0f + 0.65f * fabsf(x));
            }

            mix_l    += sig * sv->pan_l;
            mix_r    += sig * sv->pan_r;
            mono_sum += sig;
        }

        if ((i & 63) == 0) {
            for (int s = 0; s < NUM_STRINGS; s++) {
                float e = g_string_env[s] * 0.968f;
                g_string_env[s] = (e < 0.005f) ? 0.0f : e;
            }
        }

        float body = biquad_process(&g_body_top, biquad_process(&g_body_air, mono_sum * 0.35f));
        mix_l += body * 0.42f;
        mix_r += body * 0.42f;

        g_chorus_buf[g_chorus_pos] = mono_sum * 0.45f;
        g_chorus_lfo += 2.0f * (float)M_PI * 1.15f / (float)SAMPLE_RATE;
        if (g_chorus_lfo > 2.0f * (float)M_PI) g_chorus_lfo -= 2.0f * (float)M_PI;

        float d_l = 240.0f + 50.0f * sinf(g_chorus_lfo);
        float d_r = 280.0f + 50.0f * cosf(g_chorus_lfo);
        int idx_l = (g_chorus_pos + CHORUS_BUF_LEN - (int)d_l) & (CHORUS_BUF_LEN - 1);
        int idx_r = (g_chorus_pos + CHORUS_BUF_LEN - (int)d_r) & (CHORUS_BUF_LEN - 1);
        float wet = any_overdrive ? 0.20f : 0.25f;
        mix_l += g_chorus_buf[idx_l] * wet;
        mix_r -= g_chorus_buf[idx_r] * wet;
        g_chorus_pos = (g_chorus_pos + 1) & (CHORUS_BUF_LEN - 1);

        float out_l = mix_l * 13500.0f;
        float out_r = mix_r * 13500.0f;
        if (out_l >  32700.0f) out_l =  32700.0f;
        if (out_l < -32700.0f) out_l = -32700.0f;
        if (out_r >  32700.0f) out_r =  32700.0f;
        if (out_r < -32700.0f) out_r = -32700.0f;

        int16_t sl = (int16_t)out_l;
        int16_t sr = (int16_t)out_r;
        out[i * 2 + 0] = sl;
        out[i * 2 + 1] = sr;

        if ((i & 3) == 0) {
            uint32_t sw = g_scope_write_idx;
            g_scope_buf[sw & (SCOPE_SAMPLES - 1)] = (int16_t)((sl + sr) / 2);
            g_scope_write_idx = sw + 1;
        }
    }
}

/* ============================================================================
 * 5. PLATFORM BACKEND: RG 35xxSP (SDL2 / FBDEV / ALSA / EVDEV) & LINUX X11
 * ============================================================================ */

#if defined(HOST_X11)

static Display   *g_x11_dpy = NULL;
static Window     g_x11_win = 0;
static GC         g_x11_gc  = 0;
static XImage    *g_x11_img = NULL;
static Atom       g_wm_delete = 0;
static uint32_t  *g_x11_pix = NULL;

static pthread_t     g_audio_thread;
static volatile bool g_audio_running = false;

typedef struct pa_simple pa_simple;
typedef struct { int format; uint32_t rate; uint8_t channels; } pa_sample_spec;
#define PA_STREAM_PLAYBACK 1
#define PA_SAMPLE_S16LE    3
typedef pa_simple* (*pa_new_fn)(const char*, const char*, int, const char*, const char*,
                                const pa_sample_spec*, const void*, const void*, int*);
typedef int  (*pa_write_fn)(pa_simple*, const void*, size_t, int*);
typedef void (*pa_free_fn)(pa_simple*);

static void *host_audio_worker(void *arg) {
    (void)arg;
    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    pa_simple *pa = NULL;
    pa_new_fn   fn_new   = NULL;
    pa_write_fn fn_write = NULL;
    pa_free_fn  fn_free  = NULL;

    if (pa_lib) {
        fn_new   = (pa_new_fn)dlsym(pa_lib, "pa_simple_new");
        fn_write = (pa_write_fn)dlsym(pa_lib, "pa_simple_write");
        fn_free  = (pa_free_fn)dlsym(pa_lib, "pa_simple_free");
        if (fn_new && fn_write && fn_free) {
            pa_sample_spec ss = { PA_SAMPLE_S16LE, SAMPLE_RATE, AUDIO_CHANNELS };
            int err = 0;
            pa = fn_new(NULL, "RG35xxSP Fretboard", PA_STREAM_PLAYBACK, NULL,
                        "EKS Guitar Synth", &ss, NULL, NULL, &err);
        }
    }

    int16_t buf[512 * AUDIO_CHANNELS];
    while (g_audio_running) {
        audio_callback(NULL, (uint8_t *)buf, sizeof(buf));
        if (pa && fn_write) {
            int err = 0;
            fn_write(pa, buf, sizeof(buf), &err);
        } else {
            usleep((512 * 1000000ULL) / SAMPLE_RATE);
        }
    }

    if (pa && fn_free) fn_free(pa);
    if (pa_lib) dlclose(pa_lib);
    return NULL;
}

static int platform_init(void) {
    g_x11_dpy = XOpenDisplay(NULL);
    if (!g_x11_dpy) {
        fprintf(stderr, "Failed to open X11 display.\n");
        return -1;
    }
    int scr = DefaultScreen(g_x11_dpy);
    g_x11_win = XCreateSimpleWindow(g_x11_dpy, RootWindow(g_x11_dpy, scr),
                                    80, 80, NATIVE_W, NATIVE_H, 1,
                                    BlackPixel(g_x11_dpy, scr),
                                    BlackPixel(g_x11_dpy, scr));
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width  = hints.max_width  = NATIVE_W;
    hints.min_height = hints.max_height = NATIVE_H;
    XSetWMNormalHints(g_x11_dpy, g_x11_win, &hints);
    XStoreName(g_x11_dpy, g_x11_win, "Anbernic RG 35xxSP Fretboard (640x480)");

    XSelectInput(g_x11_dpy, g_x11_win, KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask);
    g_wm_delete = XInternAtom(g_x11_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_x11_dpy, g_x11_win, &g_wm_delete, 1);
    XMapRaised(g_x11_dpy, g_x11_win);
    g_x11_gc = DefaultGC(g_x11_dpy, scr);

    g_x11_pix = (uint32_t *)calloc((size_t)(NATIVE_W * NATIVE_H), sizeof(uint32_t));
    g_x11_img = XCreateImage(g_x11_dpy, DefaultVisual(g_x11_dpy, scr),
                             DefaultDepth(g_x11_dpy, scr), ZPixmap, 0,
                             (char *)g_x11_pix, NATIVE_W, NATIVE_H, 32, 0);

    g_audio_running = true;
    pthread_create(&g_audio_thread, NULL, host_audio_worker, NULL);
    return 0;
}

static void platform_present(void) {
    if (!g_x11_dpy || !g_x11_img) return;
    memcpy(g_x11_pix, g_fb, sizeof(g_fb));
    XPutImage(g_x11_dpy, g_x11_win, g_x11_gc, g_x11_img, 0, 0, 0, 0, NATIVE_W, NATIVE_H);
    XFlush(g_x11_dpy);
}

static uint32_t map_x11_keysym(KeySym ks) {
    switch (ks) {
        case XK_Up:           return BTN_UP;
        case XK_Down:         return BTN_DOWN;
        case XK_Left:         return BTN_LEFT;
        case XK_Right:        return BTN_RIGHT;
        case XK_a: case XK_A:
        case XK_space:
        case XK_z: case XK_Z: return BTN_A;
        case XK_b: case XK_B:
        case XK_BackSpace:    return BTN_B;
        case XK_x: case XK_X: return BTN_X;
        case XK_y: case XK_Y:
        case XK_h: case XK_H: return BTN_Y;
        case XK_l: case XK_L:
        case XK_q: case XK_Q:
        case XK_bracketleft:  return BTN_L1;
        case XK_r: case XK_R:
        case XK_e: case XK_E:
        case XK_bracketright: return BTN_R1;
        case XK_1:            return BTN_L2;
        case XK_2:            return BTN_R2;
        case XK_Tab:
        case XK_c: case XK_C: return BTN_SELECT;
        case XK_Return:       return BTN_START;
        case XK_Escape:
        case XK_m: case XK_M: return BTN_MENU;
        default:              return 0;
    }
}

static uint32_t platform_poll_pressed(void) {
    uint32_t pressed = 0;
    while (g_x11_dpy && XPending(g_x11_dpy) > 0) {
        XEvent ev;
        XNextEvent(g_x11_dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == g_wm_delete) {
            pressed |= BTN_QUIT;
        } else if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            pressed |= map_x11_keysym(ks);
        }
    }
    return pressed;
}

static void platform_shutdown(void) {
    if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
    if (g_x11_img) {
        XDestroyImage(g_x11_img);
        g_x11_img = NULL;
        g_x11_pix = NULL;
    }
    if (g_x11_dpy) {
        if (g_x11_win) XDestroyWindow(g_x11_dpy, g_x11_win);
        XCloseDisplay(g_x11_dpy);
        g_x11_dpy = NULL;
    }
}

#else /* TARGET_RG35XXSP (Allwinner H700 ARM64) */

#define SDL2_INIT_VIDEO       0x00000020u
#define SDL2_INIT_AUDIO       0x00000010u
#define SDL2_WINDOWPOS_UNDEF  0x1FFF0000u
#define SDL2_WINDOW_SHOWN     0x00000004u
#define SDL2_WINDOW_FULLSCR   0x00000001u
#define SDL2_RENDERER_ACCEL   0x00000002u
#define SDL2_RENDERER_VSYNC   0x00000004u
#define SDL2_PIXFMT_ARGB8888  0x16362004u
#define SDL2_TEXTURE_STREAM   1
#define SDL2_AUDIO_S16SYS     0x8010u

typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
} SDL2_AudioSpec;

static void *g_sdl2_lib = NULL;
static void *g_sdl_win  = NULL;
static void *g_sdl_ren  = NULL;
static void *g_sdl_tex  = NULL;
static bool  g_use_sdl2 = false;

static int   (*fn_SDL_Init)(uint32_t) = NULL;
static void  (*fn_SDL_Quit)(void) = NULL;
static void* (*fn_SDL_CreateWindow)(const char*, int, int, int, int, uint32_t) = NULL;
static void* (*fn_SDL_CreateRenderer)(void*, int, uint32_t) = NULL;
static void* (*fn_SDL_CreateTexture)(void*, uint32_t, int, int, int) = NULL;
static int   (*fn_SDL_UpdateTexture)(void*, const void*, const void*, int) = NULL;
static int   (*fn_SDL_RenderClear)(void*) = NULL;
static int   (*fn_SDL_RenderCopy)(void*, void*, const void*, const void*) = NULL;
static void  (*fn_SDL_RenderPresent)(void*) = NULL;
static int   (*fn_SDL_ShowCursor)(int) = NULL;
static int   (*fn_SDL_OpenAudio)(SDL2_AudioSpec*, SDL2_AudioSpec*) = NULL;
static void  (*fn_SDL_PauseAudio)(int) = NULL;
static void  (*fn_SDL_CloseAudio)(void) = NULL;
static void  (*fn_SDL_DestroyTexture)(void*) = NULL;
static void  (*fn_SDL_DestroyRenderer)(void*) = NULL;
static void  (*fn_SDL_DestroyWindow)(void*) = NULL;

static int                      g_fb_fd  = -1;
static uint8_t                 *g_fb_mem = NULL;
static size_t                   g_fb_len = 0;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;

static pthread_t     g_alsa_thread;
static volatile bool g_alsa_running = false;

typedef struct _snd_pcm snd_pcm_t;
#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_FORMAT_S16_LE   2

static void *alsa_audio_worker(void *arg) {
    (void)arg;
    void *asound = dlopen("libasound.so.2", RTLD_LAZY);
    if (!asound) return NULL;

    int (*snd_open)(snd_pcm_t**, const char*, int, int) = dlsym(asound, "snd_pcm_open");
    int (*snd_set_params)(snd_pcm_t*, int, int, unsigned int, unsigned int, int, unsigned int) = dlsym(asound, "snd_pcm_set_params");
    long (*snd_writei)(snd_pcm_t*, const void*, unsigned long) = dlsym(asound, "snd_pcm_writei");
    int (*snd_recover)(snd_pcm_t*, int, int) = dlsym(asound, "snd_pcm_recover");
    int (*snd_close)(snd_pcm_t*) = dlsym(asound, "snd_pcm_close");

    if (!snd_open || !snd_set_params || !snd_writei || !snd_close) {
        dlclose(asound);
        return NULL;
    }

    snd_pcm_t *pcm = NULL;
    if (snd_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        if (snd_open(&pcm, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            dlclose(asound);
            return NULL;
        }
    }
    snd_set_params(pcm, SND_PCM_FORMAT_S16_LE, 3, AUDIO_CHANNELS, SAMPLE_RATE, 1, 40000);

    int16_t buf[512 * AUDIO_CHANNELS];
    while (g_alsa_running) {
        audio_callback(NULL, (uint8_t *)buf, sizeof(buf));
        long r = snd_writei(pcm, buf, 512);
        if (r < 0 && snd_recover) snd_recover(pcm, (int)r, 1);
    }

    snd_close(pcm);
    dlclose(asound);
    return NULL;
}

#define MAX_EVDEV_FDS 16
static int      g_ev_fds[MAX_EVDEV_FDS];
static int      g_num_ev_fds = 0;
static uint32_t g_held_mask  = 0;

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
            if (fd >= 0) g_ev_fds[g_num_ev_fds++] = fd;
        }
    }
    closedir(dir);
}

static int platform_init(void) {
    init_evdev();

    g_sdl2_lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
    if (g_sdl2_lib) {
        fn_SDL_Init            = dlsym(g_sdl2_lib, "SDL_Init");
        fn_SDL_Quit            = dlsym(g_sdl2_lib, "SDL_Quit");
        fn_SDL_CreateWindow    = dlsym(g_sdl2_lib, "SDL_CreateWindow");
        fn_SDL_CreateRenderer  = dlsym(g_sdl2_lib, "SDL_CreateRenderer");
        fn_SDL_CreateTexture   = dlsym(g_sdl2_lib, "SDL_CreateTexture");
        fn_SDL_UpdateTexture   = dlsym(g_sdl2_lib, "SDL_UpdateTexture");
        fn_SDL_RenderClear     = dlsym(g_sdl2_lib, "SDL_RenderClear");
        fn_SDL_RenderCopy      = dlsym(g_sdl2_lib, "SDL_RenderCopy");
        fn_SDL_RenderPresent   = dlsym(g_sdl2_lib, "SDL_RenderPresent");
        fn_SDL_ShowCursor      = dlsym(g_sdl2_lib, "SDL_ShowCursor");
        fn_SDL_OpenAudio       = dlsym(g_sdl2_lib, "SDL_OpenAudio");
        fn_SDL_PauseAudio      = dlsym(g_sdl2_lib, "SDL_PauseAudio");
        fn_SDL_CloseAudio      = dlsym(g_sdl2_lib, "SDL_CloseAudio");
        fn_SDL_DestroyTexture  = dlsym(g_sdl2_lib, "SDL_DestroyTexture");
        fn_SDL_DestroyRenderer = dlsym(g_sdl2_lib, "SDL_DestroyRenderer");
        fn_SDL_DestroyWindow   = dlsym(g_sdl2_lib, "SDL_DestroyWindow");

        if (fn_SDL_Init && fn_SDL_CreateWindow && fn_SDL_CreateRenderer &&
            fn_SDL_CreateTexture && fn_SDL_UpdateTexture && fn_SDL_RenderPresent) {
            if (fn_SDL_Init(SDL2_INIT_VIDEO | SDL2_INIT_AUDIO) == 0) {
                if (fn_SDL_ShowCursor) fn_SDL_ShowCursor(0);
                g_sdl_win = fn_SDL_CreateWindow("Fretboard",
                                                SDL2_WINDOWPOS_UNDEF, SDL2_WINDOWPOS_UNDEF,
                                                NATIVE_W, NATIVE_H,
                                                SDL2_WINDOW_SHOWN | SDL2_WINDOW_FULLSCR);
                if (g_sdl_win) {
                    g_sdl_ren = fn_SDL_CreateRenderer(g_sdl_win, -1,
                                                      SDL2_RENDERER_ACCEL | SDL2_RENDERER_VSYNC);
                    if (!g_sdl_ren) g_sdl_ren = fn_SDL_CreateRenderer(g_sdl_win, -1, 0);
                    if (g_sdl_ren) {
                        g_sdl_tex = fn_SDL_CreateTexture(g_sdl_ren, SDL2_PIXFMT_ARGB8888,
                                                         SDL2_TEXTURE_STREAM, NATIVE_W, NATIVE_H);
                        if (g_sdl_tex) g_use_sdl2 = true;
                    }
                }
                if (fn_SDL_OpenAudio && fn_SDL_PauseAudio) {
                    SDL2_AudioSpec desired;
                    memset(&desired, 0, sizeof(desired));
                    desired.freq     = SAMPLE_RATE;
                    desired.format   = SDL2_AUDIO_S16SYS;
                    desired.channels = AUDIO_CHANNELS;
                    desired.samples  = 512;
                    desired.callback = audio_callback;
                    if (fn_SDL_OpenAudio(&desired, NULL) == 0) {
                        fn_SDL_PauseAudio(0);
                    }
                }
            }
        }
    }

    if (!g_use_sdl2) {
        g_fb_fd = open("/dev/fb0", O_RDWR);
        if (g_fb_fd >= 0) {
            if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) == 0 &&
                ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) == 0) {
                g_fb_len = g_finfo.smem_len;
                g_fb_mem = (uint8_t *)mmap(NULL, g_fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
            }
        }
        g_alsa_running = true;
        pthread_create(&g_alsa_thread, NULL, alsa_audio_worker, NULL);
    }
    return 0;
}

static void platform_present(void) {
    if (g_use_sdl2) {
        fn_SDL_UpdateTexture(g_sdl_tex, NULL, g_fb, NATIVE_W * 4);
        if (fn_SDL_RenderClear) fn_SDL_RenderClear(g_sdl_ren);
        fn_SDL_RenderCopy(g_sdl_ren, g_sdl_tex, NULL, NULL);
        fn_SDL_RenderPresent(g_sdl_ren);
        return;
    }
    if (!g_fb_mem || g_fb_mem == MAP_FAILED) return;

    int bpp = g_vinfo.bits_per_pixel / 8;
    int stride = g_finfo.line_length;
    int max_y = ((int)g_vinfo.yres < NATIVE_H) ? (int)g_vinfo.yres : NATIVE_H;
    int max_x = ((int)g_vinfo.xres < NATIVE_W) ? (int)g_vinfo.xres : NATIVE_W;

    for (int y = 0; y < max_y; y++) {
        uint8_t *dst = g_fb_mem + (y + g_vinfo.yoffset) * stride + g_vinfo.xoffset * bpp;
        const uint32_t *src = &g_fb[y * NATIVE_W];
        if (bpp == 4) {
            memcpy(dst, src, (size_t)max_x * 4);
        } else if (bpp == 2) {
            uint16_t *d16 = (uint16_t *)dst;
            for (int x = 0; x < max_x; x++) {
                uint32_t c = src[x];
                uint16_t r = (uint16_t)((c >> 16) & 0xFF);
                uint16_t g = (uint16_t)((c >> 8)  & 0xFF);
                uint16_t b = (uint16_t)(c & 0xFF);
                d16[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            }
        }
    }
}

static uint32_t platform_poll_pressed(void) {
    uint32_t prev = g_held_mask;
    struct input_event ev;
    for (int i = 0; i < g_num_ev_fds; i++) {
        while (read(g_ev_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY) {
                uint32_t bit = 0;
                switch (ev.code) {
                    case KEY_UP:    case BTN_DPAD_UP:    bit = BTN_UP;     break;
                    case KEY_DOWN:  case BTN_DPAD_DOWN:  bit = BTN_DOWN;   break;
                    case KEY_LEFT:  case BTN_DPAD_LEFT:  bit = BTN_LEFT;   break;
                    case KEY_RIGHT: case BTN_DPAD_RIGHT: bit = BTN_RIGHT;  break;
                    case 304:       case KEY_SPACE:      bit = BTN_A;      break;
                    case 305:       case KEY_LEFTCTRL:   bit = BTN_B;      break;
                    case 306:       case KEY_LEFTSHIFT:  bit = BTN_X;      break;
                    case 307:       case KEY_LEFTALT:    bit = BTN_Y;      break;
                    case 308:       case KEY_TAB:        bit = BTN_L1;     break;
                    case 309:       case KEY_BACKSPACE:  bit = BTN_R1;     break;
                    case 310:       case 314:            bit = BTN_SELECT; break;
                    case 311:       case 315:            bit = BTN_START;  break;
                    case 312:                            bit = BTN_L2;     break;
                    case 313:                            bit = BTN_R2;     break;
                    case 316:       case KEY_ESC:        bit = BTN_MENU;   break;
                    default: break;
                }
                if (bit) {
                    if (ev.value > 0) g_held_mask |= bit;
                    else              g_held_mask &= ~bit;
                }
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_HAT0X || ev.code == ABS_X) {
                    g_held_mask &= ~(BTN_LEFT | BTN_RIGHT);
                    if (ev.value < 0)      g_held_mask |= BTN_LEFT;
                    else if (ev.value > 0) g_held_mask |= BTN_RIGHT;
                } else if (ev.code == ABS_HAT0Y || ev.code == ABS_Y) {
                    g_held_mask &= ~(BTN_UP | BTN_DOWN);
                    if (ev.value < 0)      g_held_mask |= BTN_UP;
                    else if (ev.value > 0) g_held_mask |= BTN_DOWN;
                }
            }
        }
    }
    return g_held_mask & ~prev;
}

static void platform_shutdown(void) {
    if (g_alsa_running) {
        g_alsa_running = false;
        pthread_join(g_alsa_thread, NULL);
    }
    if (g_use_sdl2) {
        if (fn_SDL_CloseAudio)      fn_SDL_CloseAudio();
        if (fn_SDL_DestroyTexture)  fn_SDL_DestroyTexture(g_sdl_tex);
        if (fn_SDL_DestroyRenderer) fn_SDL_DestroyRenderer(g_sdl_ren);
        if (fn_SDL_DestroyWindow)   fn_SDL_DestroyWindow(g_sdl_win);
        if (fn_SDL_Quit)            fn_SDL_Quit();
        dlclose(g_sdl2_lib);
    }
    if (g_fb_mem && g_fb_mem != MAP_FAILED) munmap(g_fb_mem, g_fb_len);
    if (g_fb_fd >= 0) close(g_fb_fd);
    for (int i = 0; i < g_num_ev_fds; i++) close(g_ev_fds[i]);
}
#endif

/* ============================================================================
 * 6. 2D ANTI-ALIASED DRAWING & TYPOGRAPHY PRIMITIVES
 * ============================================================================ */

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

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > NATIVE_W) ? NATIVE_W : (x + w);
    int y1 = (y + h > NATIVE_H) ? NATIVE_H : (y + h);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g_fb[yy * NATIVE_W];
        for (int xx = x0; xx < x1; xx++) row[xx] = color;
    }
}

static void draw_border(int x, int y, int w, int h, int thick, uint32_t color) {
    for (int t = 0; t < thick; t++) {
        fill_rect(x + t, y + t, w - 2 * t, 1, color);
        fill_rect(x + t, y + h - 1 - t, w - 2 * t, 1, color);
        fill_rect(x + t, y + t, 1, h - 2 * t, color);
        fill_rect(x + w - 1 - t, y + t, 1, h - 2 * t, color);
    }
}

static void draw_pill(int x, int y, int w, int h, uint32_t fill_col, uint32_t border_col) {
    fill_rect(x + 2, y + 2, w - 4, h - 4, fill_col);
    fill_rect(x + 2, y, w - 4, 2, border_col);
    fill_rect(x + 2, y + h - 2, w - 4, 2, border_col);
    fill_rect(x, y + 2, 2, h - 4, border_col);
    fill_rect(x + w - 2, y + 2, 2, h - 4, border_col);
    blend_pixel(x + 1, y + 1, border_col, 200);
    blend_pixel(x + w - 2, y + 1, border_col, 200);
    blend_pixel(x + 1, y + h - 2, border_col, 200);
    blend_pixel(x + w - 2, y + h - 2, border_col, 200);
}

static void draw_circle_aa(int cx, int cy, float r, float border_w, uint32_t fill_col, uint32_t border_col) {
    int ir = (int)ceilf(r + 1.0f);
    for (int dy = -ir; dy <= ir; dy++) {
        for (int dx = -ir; dx <= ir; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            if (d <= r + 0.7f) {
                float cov = r + 0.5f - d;
                if (cov > 1.0f) cov = 1.0f;
                if (cov < 0.0f) cov = 0.0f;
                uint32_t col = (d >= r - border_w) ? border_col : fill_col;
                blend_pixel(cx + dx, cy + dy, col, (uint8_t)(cov * 255.0f));
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
                if (a > 0) blend_pixel(cur_x + g->xoff + col, y + g->yoff + row, color, a);
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
    memcpy(&hdr[2], &file_size, 4);
    hdr[10] = 54; hdr[14] = 40;
    int32_t w = NATIVE_W, neg_h = -NATIVE_H;
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &neg_h, 4);
    hdr[26] = 1; hdr[28] = 24;
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
 * 7. APPLICATION STATE, CAGED VOICING ENGINE & REVERSE CHORD IDENTIFIER
 * ============================================================================ */

typedef enum {
    MODE_SCALES   = 0,
    MODE_CHORDS   = 1,
    MODE_EXPLORER = 2
} AppMode;

typedef enum {
    LABEL_INTERVALS = 0,
    LABEL_NOTES     = 1,
    LABEL_DEGREES   = 2
} DotLabelMode;

static AppMode      g_mode           = MODE_SCALES;
static DotLabelMode g_label_mode     = LABEL_INTERVALS;
static GuitarTimbre g_timbre         = TIMBRE_ACOUSTIC;
static bool         g_show_help      = false;
static int          g_anim_tick      = 0;

static int          g_root_pc        = 9;    /* Key of A */
static int          g_scale_idx      = 1;    /* Blues Scale */
static int          g_caged_pos      = 1;    /* POS 1 (E-Shape) */
static bool         g_chord_skeleton = true;

static int          g_chord_idx      = 3;    /* maj7 */
static int          g_voicing_pos    = 0;    /* 0..4 */
static bool         g_show_arp_bg    = true;
static int          g_tuning_idx     = 0;    /* Std E */

static int          g_exp_row        = 2;
static int          g_exp_fret       = 5;
static int          g_exp_locked[6]  = { 0, 2, 2, 0, 0, 0 };

static char         g_cfg_path[512];

static void resolve_config_path(void) {
    struct stat st;
    if (stat("/userdata/system", &st) == 0 && S_ISDIR(st.st_mode) && access("/userdata/system", W_OK) == 0) {
        snprintf(g_cfg_path, sizeof(g_cfg_path), "/userdata/system/.fretboard.cfg");
        return;
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_cfg_path, sizeof(g_cfg_path), "%s/.fretboard.cfg", home);
    } else {
        snprintf(g_cfg_path, sizeof(g_cfg_path), "./.fretboard.cfg");
    }
}

static void load_config(void) {
    FILE *fp = fopen(g_cfg_path, "r");
    if (!fp) return;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        int val = 0;
        if (sscanf(line, "mode=%d", &val) == 1 && val >= 0 && val <= 2) g_mode = (AppMode)val;
        else if (sscanf(line, "label_mode=%d", &val) == 1 && val >= 0 && val <= 2) g_label_mode = (DotLabelMode)val;
        else if (sscanf(line, "root_pc=%d", &val) == 1 && val >= 0 && val < 12) g_root_pc = val;
        else if (sscanf(line, "scale_idx=%d", &val) == 1 && val >= 0 && val < NUM_SCALES) g_scale_idx = val;
        else if (sscanf(line, "caged_pos=%d", &val) == 1 && val >= 0 && val <= 5) g_caged_pos = val;
        else if (sscanf(line, "chord_skeleton=%d", &val) == 1) g_chord_skeleton = (val != 0);
        else if (sscanf(line, "chord_idx=%d", &val) == 1 && val >= 0 && val < NUM_CHORDS) g_chord_idx = val;
        else if (sscanf(line, "voicing_pos=%d", &val) == 1 && val >= 0 && val < 5) g_voicing_pos = val;
        else if (sscanf(line, "show_arp_bg=%d", &val) == 1) g_show_arp_bg = (val != 0);
        else if (sscanf(line, "tuning_idx=%d", &val) == 1 && val >= 0 && val < NUM_TUNINGS) g_tuning_idx = val;
        else if (sscanf(line, "timbre=%d", &val) == 1 && val >= 0 && val <= 2) g_timbre = (GuitarTimbre)val;
    }
    fclose(fp);
}

static void save_config(void) {
    FILE *fp = fopen(g_cfg_path, "w");
    if (!fp) return;
    fprintf(fp, "mode=%d\n", (int)g_mode);
    fprintf(fp, "label_mode=%d\n", (int)g_label_mode);
    fprintf(fp, "root_pc=%d\n", g_root_pc);
    fprintf(fp, "scale_idx=%d\n", g_scale_idx);
    fprintf(fp, "caged_pos=%d\n", g_caged_pos);
    fprintf(fp, "chord_skeleton=%d\n", g_chord_skeleton ? 1 : 0);
    fprintf(fp, "chord_idx=%d\n", g_chord_idx);
    fprintf(fp, "voicing_pos=%d\n", g_voicing_pos);
    fprintf(fp, "show_arp_bg=%d\n", g_show_arp_bg ? 1 : 0);
    fprintf(fp, "tuning_idx=%d\n", g_tuning_idx);
    fprintf(fp, "timbre=%d\n", (int)g_timbre);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
}

static void compute_caged_box_range(int root_pc, int caged_pos, int *out_low, int *out_high) {
    if (caged_pos <= 0) {
        *out_low  = 0;
        *out_high = MAX_FRET;
        return;
    }
    int open_low_pc = k_tunings[g_tuning_idx].midi[0] % 12;
    int root_fret_6 = (root_pc - open_low_pc + 12) % 12;
    int start = root_fret_6 + k_caged_rel_start[caged_pos - 1];
    while (start > 11) start -= 12;
    if (start < 0) start = 0;
    int low  = (start > 0) ? (start - 1) : 0;
    int high = low + 4;
    if (high > MAX_FRET) high = MAX_FRET;
    *out_low  = low;
    *out_high = high;
}

typedef struct {
    int frets[6];
    int fingers[6];
    int barre_fret;
    int barre_low_str;
    int base_fret;
    int max_fret;
} ComputedVoicing;

static void compute_chord_voicing(int root_pc, int chord_idx, int voicing_idx, ComputedVoicing *out) {
    const ChordQualityDef *cd = &k_chords[chord_idx];
    const GuitarTuning    *tn = &k_tunings[g_tuning_idx];

    int open_s0_pc = tn->midi[0] % 12;
    int open_s1_pc = tn->midi[1] % 12;
    int root_f6    = (root_pc - open_s0_pc + 12) % 12;
    int root_f5    = (root_pc - open_s1_pc + 12) % 12;

    int anchors[5];
    int bass_str[5];
    anchors[0] = root_f6;                              bass_str[0] = 0;
    anchors[1] = (root_f6 + 2) % 12;                   bass_str[1] = 2;
    anchors[2] = root_f5;                              bass_str[2] = 1;
    anchors[3] = (root_f5 + 3) % 12;                   bass_str[3] = 0;
    anchors[4] = root_f6 + ((root_f6 <= 3) ? 12 : 7);  bass_str[4] = 0;

    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (anchors[j] < anchors[i]) {
                int ta = anchors[i]; anchors[i] = anchors[j]; anchors[j] = ta;
                int tb = bass_str[i]; bass_str[i] = bass_str[j]; bass_str[j] = tb;
            }
        }
    }
    for (int i = 1; i < 5; i++) {
        if (anchors[i] <= anchors[i - 1]) anchors[i] = anchors[i - 1] + 2;
        if (anchors[i] > 12) anchors[i] = 12;
    }

    int win_low = anchors[voicing_idx % 5];
    int min_str = bass_str[voicing_idx % 5];
    if (cd->pref_bass_str > min_str && win_low > 0) min_str = cd->pref_bass_str;
    int win_high = (win_low == 0) ? 3 : (win_low + 3);
    if (win_high > MAX_FRET) win_high = MAX_FRET;

    for (int s = 0; s < 6; s++) {
        out->frets[s]   = -1;
        out->fingers[s] = 0;
    }

    bool bass_placed = false;
    for (int s = min_str; s <= 2; s++) {
        for (int f = win_low; f <= win_high + 1 && f <= MAX_FRET; f++) {
            if ((tn->midi[s] + f) % 12 == root_pc) {
                out->frets[s] = f;
                min_str = s;
                bass_placed = true;
                break;
            }
        }
        if (bass_placed) break;
    }

    for (int s = min_str; s < 6; s++) {
        if (out->frets[s] >= 0) continue;
        int best_f = -1, best_score = -999;
        for (int f = win_low; f <= win_high + 1 && f <= MAX_FRET; f++) {
            int pc  = (tn->midi[s] + f) % 12;
            int rel = (pc - root_pc + 12) % 12;
            uint8_t m = cd->mask[rel];
            if (m > 0) {
                int score = (int)m * 10 - abs(f - (win_low + 1)) * 3;
                if (s >= 2 && (m == 2 || m == 3)) score += 12;
                if (score > best_score) {
                    best_score = score;
                    best_f     = f;
                }
            }
        }
        out->frets[s] = best_f;
    }

    int min_f = 99, max_f = 0, min_f_count = 0, min_f_low_str = 6;
    for (int s = 0; s < 6; s++) {
        int f = out->frets[s];
        if (f > 0) {
            if (f < min_f) {
                min_f = f;
                min_f_count = 1;
                min_f_low_str = s;
            } else if (f == min_f) {
                min_f_count++;
                if (s < min_f_low_str) min_f_low_str = s;
            }
            if (f > max_f) max_f = f;
        }
    }
    if (min_f == 99) { min_f = 0; max_f = 3; }

    out->base_fret     = (min_f > 0) ? min_f : 1;
    out->max_fret      = (max_f > out->base_fret) ? max_f : (out->base_fret + 3);
    out->barre_fret    = (min_f > 0 && min_f_count >= 2) ? min_f : 0;
    out->barre_low_str = min_f_low_str;

    int next_finger = (out->barre_fret > 0) ? 2 : 1;
    for (int f = min_f; f <= max_f && f > 0; f++) {
        for (int s = 0; s < 6; s++) {
            if (out->frets[s] == f) {
                if (f == out->barre_fret) {
                    out->fingers[s] = 1;
                } else {
                    out->fingers[s] = (next_finger <= 4) ? next_finger : 4;
                    if (next_finger < 4) next_finger++;
                }
            }
        }
    }
}

static void identify_locked_chord(char *out_name, size_t name_sz, char *out_notes, size_t notes_sz) {
    const GuitarTuning *tn = &k_tunings[g_tuning_idx];
    uint16_t pc_mask = 0;
    int bass_pc   = -1;
    int bass_midi = 999;
    int note_cnt  = 0;

    out_notes[0] = '\0';
    for (int s = 0; s < 6; s++) {
        int f = g_exp_locked[s];
        if (f >= 0) {
            int midi = tn->midi[s] + f;
            int pc   = midi % 12;
            pc_mask |= (uint16_t)(1u << pc);
            if (midi < bass_midi) {
                bass_midi = midi;
                bass_pc   = pc;
            }
            if (note_cnt > 0) strncat(out_notes, "-", notes_sz - strlen(out_notes) - 1);
            strncat(out_notes, k_note_names[pc], notes_sz - strlen(out_notes) - 1);
            note_cnt++;
        }
    }

    if (note_cnt == 0) {
        snprintf(out_name, name_sz, "None (Press B to Lock)");
        snprintf(out_notes, notes_sz, "DPad + A to pluck");
        return;
    }
    if (note_cnt == 1) {
        snprintf(out_name, name_sz, "%s (Single Note)", k_note_names[bass_pc]);
        return;
    }

    static const struct {
        const char *suffix;
        uint16_t    req_mask;
        uint16_t    allow_mask;
        int         bonus;
    } k_templates[] = {
        { "",       (1<<0)|(1<<4)|(1<<7),         (1<<0)|(1<<4)|(1<<7),                30 },
        { "m",      (1<<0)|(1<<3)|(1<<7),         (1<<0)|(1<<3)|(1<<7),                30 },
        { "5",      (1<<0)|(1<<7),                (1<<0)|(1<<7),                       24 },
        { "7",      (1<<0)|(1<<4)|(1<<10),        (1<<0)|(1<<4)|(1<<7)|(1<<10),        34 },
        { "maj7",   (1<<0)|(1<<4)|(1<<11),        (1<<0)|(1<<4)|(1<<7)|(1<<11),        34 },
        { "m7",     (1<<0)|(1<<3)|(1<<10),        (1<<0)|(1<<3)|(1<<7)|(1<<10),        34 },
        { "m(maj7)",(1<<0)|(1<<3)|(1<<11),        (1<<0)|(1<<3)|(1<<7)|(1<<11),        32 },
        { "sus2",   (1<<0)|(1<<2)|(1<<7),         (1<<0)|(1<<2)|(1<<7),                28 },
        { "sus4",   (1<<0)|(1<<5)|(1<<7),         (1<<0)|(1<<5)|(1<<7),                28 },
        { "7sus4",  (1<<0)|(1<<5)|(1<<10),        (1<<0)|(1<<5)|(1<<7)|(1<<10),        32 },
        { "add9",   (1<<0)|(1<<2)|(1<<4),         (1<<0)|(1<<2)|(1<<4)|(1<<7),         33 },
        { "m(add9)",(1<<0)|(1<<2)|(1<<3),         (1<<0)|(1<<2)|(1<<3)|(1<<7),         35 },
        { "6",      (1<<0)|(1<<4)|(1<<9),         (1<<0)|(1<<4)|(1<<7)|(1<<9),         31 },
        { "m6",     (1<<0)|(1<<3)|(1<<9),         (1<<0)|(1<<3)|(1<<7)|(1<<9),         31 },
        { "m7b5",   (1<<0)|(1<<3)|(1<<6)|(1<<10), (1<<0)|(1<<3)|(1<<6)|(1<<10),        36 },
        { "dim7",   (1<<0)|(1<<3)|(1<<6)|(1<<9),  (1<<0)|(1<<3)|(1<<6)|(1<<9),         36 },
        { "dim",    (1<<0)|(1<<3)|(1<<6),         (1<<0)|(1<<3)|(1<<6),                28 },
        { "aug",    (1<<0)|(1<<4)|(1<<8),         (1<<0)|(1<<4)|(1<<8),                28 },
        { "9",      (1<<0)|(1<<2)|(1<<4)|(1<<10), (1<<0)|(1<<2)|(1<<4)|(1<<7)|(1<<10), 36 },
        { "m9",     (1<<0)|(1<<2)|(1<<3)|(1<<10), (1<<0)|(1<<2)|(1<<3)|(1<<7)|(1<<10), 36 },
        { "maj9",   (1<<0)|(1<<2)|(1<<4)|(1<<11), (1<<0)|(1<<2)|(1<<4)|(1<<7)|(1<<11), 36 }
    };

    int best_score = -999;
    int best_root  = bass_pc;
    const char *best_suf = "";

    for (int r = 0; r < 12; r++) {
        if (!(pc_mask & (1u << r))) continue;
        uint16_t rel_mask = 0;
        for (int p = 0; p < 12; p++) {
            if (pc_mask & (1u << p)) rel_mask |= (uint16_t)(1u << ((p - r + 12) % 12));
        }
        for (size_t t = 0; t < sizeof(k_templates) / sizeof(k_templates[0]); t++) {
            if ((rel_mask & k_templates[t].req_mask) == k_templates[t].req_mask &&
                (rel_mask & ~k_templates[t].allow_mask) == 0) {
                int score = k_templates[t].bonus + ((r == bass_pc) ? 10 : 0);
                if (score > best_score) {
                    best_score = score;
                    best_root  = r;
                    best_suf   = k_templates[t].suffix;
                }
            }
        }
    }

    if (best_score < 0) {
        int rel_iv = 0;
        for (int p = 1; p < 12; p++) {
            if (pc_mask & (1u << ((bass_pc + p) % 12))) { rel_iv = p; break; }
        }
        snprintf(out_name, name_sz, "%s+%s Cluster", k_note_names[bass_pc], k_interval_labels[rel_iv]);
        return;
    }

    if (best_root != bass_pc) {
        snprintf(out_name, name_sz, "%s%s/%s", k_note_names[best_root], best_suf, k_note_names[bass_pc]);
    } else {
        snprintf(out_name, name_sz, "%s%s", k_note_names[best_root], best_suf);
    }
}

static void audition_current_mode(void) {
    PluckBatchCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.timbre = (int)g_timbre;
    const GuitarTuning *tn = &k_tunings[g_tuning_idx];

    if (g_mode == MODE_SCALES) {
        const ScaleDef *sc = &k_scales[g_scale_idx];
        int low_f, high_f;
        compute_caged_box_range(g_root_pc, (g_caged_pos == 0) ? 1 : g_caged_pos, &low_f, &high_f);

        int k = 0;
        int last_midi = -1;
        for (int s = 0; s < NUM_STRINGS; s++) {
            for (int f = low_f; f <= high_f && k < MAX_SCHED_PLUCK; f++) {
                int midi = tn->midi[s] + f;
                int pc   = midi % 12;
                int rel  = (pc - g_root_pc + 12) % 12;
                if (sc->mask[rel] > 0 && midi > last_midi) {
                    cmd.notes[k] = (PluckSpec){ s, f, midi, k * 135 };
                    last_midi = midi;
                    k++;
                }
            }
        }
        cmd.count = k;
    } else if (g_mode == MODE_CHORDS) {
        ComputedVoicing cv;
        compute_chord_voicing(g_root_pc, g_chord_idx, g_voicing_pos, &cv);
        int k = 0;
        for (int s = 0; s < NUM_STRINGS; s++) {
            if (cv.frets[s] >= 0) {
                cmd.notes[k] = (PluckSpec){ s, cv.frets[s], tn->midi[s] + cv.frets[s], k * 34 };
                k++;
            }
        }
        cmd.count = k;
    } else {
        int cur_str = 5 - g_exp_row;
        if (g_exp_locked[cur_str] == g_exp_fret) {
            int k = 0;
            for (int s = 0; s < NUM_STRINGS; s++) {
                if (g_exp_locked[s] >= 0) {
                    cmd.notes[k] = (PluckSpec){ s, g_exp_locked[s], tn->midi[s] + g_exp_locked[s], k * 34 };
                    k++;
                }
            }
            cmd.count = k;
        } else {
            cmd.count = 1;
            cmd.notes[0] = (PluckSpec){ cur_str, g_exp_fret, tn->midi[cur_str] + g_exp_fret, 0 };
        }
    }
    post_pluck_batch(&cmd);
}

/* ============================================================================
 * 8. 640x480 UI RENDERING: TOP BAR, THEORY + SCOPE CARD, 15-FRET HERO NECK
 * ============================================================================ */

static void render_top_status_bar(void) {
    fill_rect(0, 0, NATIVE_W, 40, RGB(19, 27, 43));
    fill_rect(0, 40, NATIVE_W, 2, RGB(45, 62, 92));

    /* 1. Mode Pill (SELECT) */
    const char *mode_txt = (g_mode == MODE_SCALES) ? "SEL: SCALES" :
                           (g_mode == MODE_CHORDS) ? "SEL: CHORDS" : "SEL: EXPLORER";
    uint32_t mode_bg  = (g_mode == MODE_SCALES) ? RGB(37, 99, 235)  :
                        (g_mode == MODE_CHORDS) ? RGB(124, 58, 237) : RGB(13, 148, 136);
    uint32_t mode_bdr = (g_mode == MODE_SCALES) ? RGB(147, 197, 253) :
                        (g_mode == MODE_CHORDS) ? RGB(216, 180, 254) : RGB(94, 234, 212);
    draw_pill(8, 6, 122, 28, mode_bg, mode_bdr);
    draw_text_centered(FONT_UI_13, 8, 6, 122, 28, mode_txt, RGB(255, 255, 255));

    /* 2. Key Badge */
    char key_lbl[24];
    snprintf(key_lbl, sizeof(key_lbl), "KEY %s", k_note_names[g_root_pc]);
    draw_pill(136, 6, 76, 28, RGB(146, 64, 14), RGB(251, 146, 60));
    draw_text_centered(FONT_UI_13, 136, 6, 76, 28, key_lbl, RGB(255, 247, 237));

    /* 3. Center Title Pill */
    char center_lbl[48];
    if (g_mode == MODE_SCALES) {
        snprintf(center_lbl, sizeof(center_lbl), "%s", k_scales[g_scale_idx].name);
    } else if (g_mode == MODE_CHORDS) {
        snprintf(center_lbl, sizeof(center_lbl), "%s%s (%s)",
                 k_note_names[g_root_pc], k_chords[g_chord_idx].suffix, k_chords[g_chord_idx].name);
    } else {
        snprintf(center_lbl, sizeof(center_lbl), "Reverse Chord Finder");
    }
    draw_pill(218, 6, 186, 28, RGB(28, 39, 62), RGB(96, 165, 250));
    int cfont = (text_width(FONT_UI_13, center_lbl) > 174) ? FONT_UI_11 : FONT_UI_13;
    draw_text_centered(cfont, 218, 6, 186, 28, center_lbl, RGB(254, 240, 138));

    /* 4. Position / Box Badge */
    char pos_lbl[32];
    if (g_mode == MODE_SCALES) {
        snprintf(pos_lbl, sizeof(pos_lbl), "%s", k_caged_pos_names[g_caged_pos]);
    } else if (g_mode == MODE_CHORDS) {
        snprintf(pos_lbl, sizeof(pos_lbl), "VOICING %d/5", g_voicing_pos + 1);
    } else {
        snprintf(pos_lbl, sizeof(pos_lbl), "STR %s : F%d",
                 k_tunings[g_tuning_idx].row_lbl[g_exp_row], g_exp_fret);
    }
    draw_pill(410, 6, 98, 28, RGB(24, 34, 54), RGB(71, 98, 145));
    draw_text_centered(FONT_UI_11, 410, 6, 98, 28, pos_lbl, RGB(224, 242, 254));

    /* 5. Timbre & Tuning Pill (L2 / R2) */
    char tt_lbl[32];
    snprintf(tt_lbl, sizeof(tt_lbl), "%s | %s",
             k_tunings[g_tuning_idx].short_name,
             (g_timbre == TIMBRE_ACOUSTIC) ? "ACOU" :
             (g_timbre == TIMBRE_CHORUS_EL) ? "CHOR" : "LEAD");
    draw_pill(514, 6, 118, 28, RGB(16, 74, 58), RGB(52, 211, 153));
    draw_text_centered(FONT_UI_11, 514, 6, 118, 28, tt_lbl, RGB(236, 253, 245));
}

static void render_theory_and_scope_card(const ComputedVoicing *cv) {
    int cy = 46, ch = 82;
    fill_rect(8, cy, NATIVE_W - 16, ch, RGB(21, 30, 48));
    draw_border(8, cy, NATIVE_W - 16, ch, 2, RGB(51, 68, 98));

    if (g_mode == MODE_SCALES) {
        const ScaleDef *sc = &k_scales[g_scale_idx];
        char hdr1[96];
        snprintf(hdr1, sizeof(hdr1), "%s %s", k_note_names[g_root_pc], sc->name);
        draw_text(FONT_UI_20, 18, cy + 6, hdr1, RGB(250, 204, 21));

        char f_str[64];
        snprintf(f_str, sizeof(f_str), "Formula: %s", sc->formula);
        draw_text(FONT_UI_13, 18, cy + 34, f_str, RGB(226, 232, 240));

        char notes_buf[96] = "Scale Notes: ";
        for (int i = 0; i < 12; i++) {
            if (sc->mask[i] > 0) {
                int pc = (g_root_pc + i) % 12;
                strncat(notes_buf, k_note_names[pc], sizeof(notes_buf) - strlen(notes_buf) - 1);
                strncat(notes_buf, "  ", sizeof(notes_buf) - strlen(notes_buf) - 1);
            }
        }
        draw_text(FONT_UI_13, 18, cy + 56, notes_buf, RGB(125, 211, 252));

        int low_f, high_f;
        compute_caged_box_range(g_root_pc, g_caged_pos, &low_f, &high_f);
        char box_buf[64];
        if (g_caged_pos == 0) {
            snprintf(box_buf, sizeof(box_buf), "Skeleton: %s%s", k_note_names[g_root_pc], sc->skeleton_chord);
        } else {
            snprintf(box_buf, sizeof(box_buf), "Box Frets %d-%d  |  %s%s",
                     low_f, high_f, k_note_names[g_root_pc], sc->skeleton_chord);
        }
        draw_text_right(FONT_UI_13, 468, cy + 34, box_buf, RGB(110, 231, 183));

    } else if (g_mode == MODE_CHORDS) {
        const ChordQualityDef *cd = &k_chords[g_chord_idx];
        char ch_name[32];
        snprintf(ch_name, sizeof(ch_name), "%s%s", k_note_names[g_root_pc], cd->suffix);
        draw_pill(16, cy + 10, 96, 62, RGB(30, 43, 68), RGB(250, 204, 21));
        draw_text_centered(FONT_UI_20, 16, cy + 10, 96, 62, ch_name, RGB(250, 204, 21));

        char line1[96];
        snprintf(line1, sizeof(line1), "%s  (%s)", cd->name, cd->formula);
        draw_text(FONT_UI_16, 124, cy + 8, line1, RGB(248, 250, 252));

        char line2[96];
        snprintf(line2, sizeof(line2), "CAGED Voicing %d/5  (Frets %d - %d)",
                 g_voicing_pos + 1, cv->base_fret, cv->max_fret);
        draw_text(FONT_UI_13, 124, cy + 34, line2, RGB(186, 230, 253));

        char f0[16], f1[16], f2[16], f3[16], f4[16], f5[16];
        snprintf(f0, sizeof(f0), cv->frets[0] < 0 ? "X" : "%d", cv->frets[0]);
        snprintf(f1, sizeof(f1), cv->frets[1] < 0 ? "X" : "%d", cv->frets[1]);
        snprintf(f2, sizeof(f2), cv->frets[2] < 0 ? "X" : "%d", cv->frets[2]);
        snprintf(f3, sizeof(f3), cv->frets[3] < 0 ? "X" : "%d", cv->frets[3]);
        snprintf(f4, sizeof(f4), cv->frets[4] < 0 ? "X" : "%d", cv->frets[4]);
        snprintf(f5, sizeof(f5), cv->frets[5] < 0 ? "X" : "%d", cv->frets[5]);
        char tab_str[160];
        snprintf(tab_str, sizeof(tab_str), "String Tab (E-A-D-G-B-e):  %s - %s - %s - %s - %s - %s",
                 f0, f1, f2, f3, f4, f5);
        draw_text(FONT_UI_13, 124, cy + 56, tab_str, RGB(110, 231, 183));

    } else {
        char ch_id[64], ch_notes[64];
        identify_locked_chord(ch_id, sizeof(ch_id), ch_notes, sizeof(ch_notes));

        char id_line[160];
        snprintf(id_line, sizeof(id_line), "CHORD FINDER: %s", ch_id);
        draw_text(FONT_UI_20, 18, cy + 6, id_line, RGB(250, 204, 21));

        char nt_line[96];
        snprintf(nt_line, sizeof(nt_line), "Locked Tones: %s", ch_notes);
        draw_text(FONT_UI_13, 18, cy + 34, nt_line, RGB(110, 231, 183));

        int cur_str  = 5 - g_exp_row;
        int cur_midi = k_tunings[g_tuning_idx].midi[cur_str] + g_exp_fret;
        int cur_pc   = cur_midi % 12;
        int cur_oct  = (cur_midi / 12) - 1;
        float cur_hz = midi_to_freq(cur_midi);
        int rel_iv   = (cur_pc - g_root_pc + 12) % 12;

        char cur_line[128];
        snprintf(cur_line, sizeof(cur_line),
                 "Cursor: Str %d (%s) Fret %d  ->  %s%d  (%.1f Hz, Interval %s vs %s)",
                 g_exp_row + 1, k_tunings[g_tuning_idx].row_lbl[g_exp_row], g_exp_fret,
                 k_note_names[cur_pc], cur_oct, cur_hz,
                 k_interval_labels[rel_iv], k_note_names[g_root_pc]);
        draw_text(FONT_UI_13, 18, cy + 56, cur_line, RGB(186, 230, 253));
    }

    /* Live Audio Waveform Oscilloscope on Right Side of Card (x = 480..622) */
    int ox = 480, oy = cy + 8, ow = 142, oh = ch - 16;
    fill_rect(ox, oy, ow, oh, RGB(12, 18, 30));
    draw_border(ox, oy, ow, oh, 1, RGB(45, 62, 92));
    int mid_y = oy + oh / 2;
    fill_rect(ox + 2, mid_y, ow - 4, 1, RGB(32, 46, 70));
    uint32_t r_idx = g_scope_write_idx;
    int prev_wy = mid_y;
    for (int x = 2; x < ow - 2; x++) {
        uint32_t s_idx = (r_idx - (uint32_t)((ow - 2 - x) * 2)) & (SCOPE_SAMPLES - 1);
        int16_t samp = g_scope_buf[s_idx];
        int wy = mid_y - (samp * (oh / 2 - 4)) / 18000;
        if (wy < oy + 2)      wy = oy + 2;
        if (wy > oy + oh - 3) wy = oy + oh - 3;
        int y0 = (wy < prev_wy) ? wy : prev_wy;
        int y1 = (wy > prev_wy) ? wy : prev_wy;
        fill_rect(ox + x, y0, 1, y1 - y0 + 2, RGB(52, 211, 153));
        prev_wy = wy;
    }
}

static void format_dot_label(int pc, int rel_iv, int finger_num, char *out, size_t out_sz) {
    if (g_label_mode == LABEL_NOTES) {
        snprintf(out, out_sz, "%s", k_note_names[pc]);
    } else if (g_label_mode == LABEL_DEGREES) {
        if (g_mode == MODE_CHORDS && finger_num > 0 && finger_num <= 4) {
            snprintf(out, out_sz, "F%d", finger_num & 7);
        } else {
            snprintf(out, out_sz, "%s", k_degree_labels[rel_iv]);
        }
    } else {
        snprintf(out, out_sz, "%s", k_interval_labels[rel_iv]);
    }
}

static void render_hero_fretboard(const ComputedVoicing *cv) {
    /* Hero 15-Fret Guitar Neck Card: y = 132..392 (height = 260px) */
    int fy = 132, fh = 260;
    fill_rect(8, fy, NATIVE_W - 16, fh, RGB(21, 30, 48));
    draw_border(8, fy, NATIVE_W - 16, fh, 2, RGB(51, 68, 98));

    const GuitarTuning *tn = &k_tunings[g_tuning_idx];

    /* Neck geometry on 640x480:
     *   Open string column (Fret 0): center x = 52
     *   Bone nut bar: x = 68..73 (6px)
     *   Frets 1..15: 15 frets * 36px = 540px (x = 72..612)
     *   6 strings: y = 178 + r * 36 (y = 178..358, span = 180px)
     */
    int nut_x  = 72;
    int fret_w = 36;
    int neck_w = fret_w * MAX_FRET; /* 540 px */
    int str_y0 = 178;
    int str_dy = 36;
    int neck_h = str_dy * 5;        /* 180 px */

    /* Rosewood fingerboard background */
    fill_rect(nut_x, str_y0 - 12, neck_w, neck_h + 24, RGB(38, 26, 20));
    draw_border(nut_x, str_y0 - 12, neck_w, neck_h + 24, 2, RGB(92, 64, 44));

    /* Highlight active CAGED Box or Chord Voicing span on the fretboard wood */
    int box_low = 0, box_high = MAX_FRET;
    if (g_mode == MODE_SCALES) {
        compute_caged_box_range(g_root_pc, g_caged_pos, &box_low, &box_high);
        if (g_caged_pos > 0) {
            int bx0 = (box_low <= 1) ? nut_x : (nut_x + (box_low - 1) * fret_w);
            int bx1 = nut_x + box_high * fret_w;
            fill_rect(bx0, str_y0 - 10, bx1 - bx0, neck_h + 20, RGB(58, 44, 30));
            draw_border(bx0, str_y0 - 12, bx1 - bx0, neck_h + 24, 2, RGB(245, 158, 11));
        }
    } else if (g_mode == MODE_CHORDS) {
        box_low  = cv->base_fret;
        box_high = cv->max_fret;
        int bx0  = (box_low <= 1) ? nut_x : (nut_x + (box_low - 1) * fret_w);
        int bx1  = nut_x + box_high * fret_w;
        fill_rect(bx0, str_y0 - 10, bx1 - bx0, neck_h + 20, RGB(48, 38, 62));
        draw_border(bx0, str_y0 - 12, bx1 - bx0, neck_h + 24, 2, RGB(167, 139, 250));
    }

    /* Fret numbers 0..15 along top of neck + pearloid fret inlays */
    for (int f = 0; f <= MAX_FRET; f++) {
        int f_cx = (f == 0) ? (nut_x - 18) : (nut_x + f * fret_w - fret_w / 2);
        if (f > 0) {
            int wire_x = nut_x + f * fret_w;
            fill_rect(wire_x, str_y0 - 10, 2, neck_h + 20, RGB(130, 142, 160));
        }
        bool is_marker = (f == 0 || f == 3 || f == 5 || f == 7 || f == 9 || f == 12 || f == 15);
        char fn[8];
        snprintf(fn, sizeof(fn), "%d", f);
        draw_text_centered(FONT_UI_13, f_cx - 12, fy + 8, 24, 18, fn,
                           is_marker ? RGB(250, 204, 21) : RGB(120, 135, 158));

        if (f == 3 || f == 5 || f == 7 || f == 9 || f == 15) {
            draw_circle_aa(f_cx, str_y0 + neck_h / 2, 5.5f, 1.0f, RGB(95, 86, 78), RGB(125, 115, 105));
        } else if (f == 12) {
            draw_circle_aa(f_cx, str_y0 + str_dy * 1 + str_dy / 2, 5.5f, 1.0f, RGB(95, 86, 78), RGB(125, 115, 105));
            draw_circle_aa(f_cx, str_y0 + str_dy * 3 + str_dy / 2, 5.5f, 1.0f, RGB(95, 86, 78), RGB(125, 115, 105));
        }
    }

    /* Bone Nut */
    fill_rect(nut_x - 4, str_y0 - 12, 6, neck_h + 24, RGB(241, 245, 249));

    /* 6 Horizontal Guitar Strings with real-time vibration displacement */
    for (int r = 0; r < NUM_STRINGS; r++) {
        int str_idx = 5 - r;
        int sy = str_y0 + r * str_dy;
        float env = g_string_env[str_idx];
        uint32_t scol = (env > 0.05f) ? RGB(250, 204, 21) :
                        ((r >= 3) ? RGB(205, 175, 128) : RGB(195, 206, 222));
        int thick = (r >= 4) ? 3 : ((r >= 2) ? 2 : 1);

        if (env > 0.04f) {
            for (int x = 0; x < neck_w; x++) {
                float u = (float)x / (float)neck_w;
                float disp = sinf(u * (float)M_PI) * sinf((float)g_anim_tick * 0.85f + (float)str_idx * 1.3f) * env * 3.5f;
                fill_rect(nut_x + x, sy + (int)roundf(disp), 1, thick, scol);
            }
        } else {
            fill_rect(nut_x, sy, neck_w, thick, scol);
        }
        draw_text(FONT_UI_16, 16, sy - 11, tn->row_lbl[r], RGB(186, 230, 253));
    }

    /* Barre Indicator Bar in Chords Mode */
    if (g_mode == MODE_CHORDS && cv->barre_fret > 0) {
        int bf_cx = nut_x + cv->barre_fret * fret_w - fret_w / 2;
        int top_r = 0;
        int bot_r = 5 - cv->barre_low_str;
        int by0   = str_y0 + top_r * str_dy - 8;
        int by1   = str_y0 + bot_r * str_dy + 8;
        draw_pill(bf_cx - 7, by0, 14, by1 - by0, RGB(30, 64, 175), RGB(147, 197, 253));
    }

    /* Render Scale / Chord / Explorer Note Dots across 6 strings x 16 frets */
    for (int r = 0; r < NUM_STRINGS; r++) {
        int str_idx = 5 - r;
        int sy      = str_y0 + r * str_dy;

        if (g_mode == MODE_CHORDS && cv->frets[str_idx] < 0) {
            draw_text_centered(FONT_UI_16, nut_x - 32, sy - 11, 24, 22, "X", RGB(248, 113, 113));
        }

        for (int f = 0; f <= MAX_FRET; f++) {
            int f_cx     = (f == 0) ? (nut_x - 18) : (nut_x + f * fret_w - fret_w / 2);
            int midi     = tn->midi[str_idx] + f;
            int pc       = midi % 12;
            int rel_iv   = (pc - g_root_pc + 12) % 12;
            bool ringing = (g_active_play_ttl > 0 && g_active_play_str == str_idx && g_active_play_fret == f);

            if (g_mode == MODE_SCALES) {
                uint8_t m = k_scales[g_scale_idx].mask[rel_iv];
                if (m == 0) continue;

                bool in_box = (g_caged_pos == 0) || (f >= box_low && f <= box_high);
                if (!in_box) {
                    uint32_t gfill = (m == 4) ? RGB(92, 64, 18) : RGB(32, 46, 68);
                    uint32_t gbdr  = (m == 4) ? RGB(161, 98, 7)  : RGB(58, 78, 110);
                    draw_circle_aa(f_cx, sy, 7.5f, 1.5f, gfill, gbdr);
                    continue;
                }

                uint32_t dfill, dbdr, tcol;
                if (ringing) {
                    dfill = RGB(255, 255, 255);
                    dbdr  = RGB(250, 204, 21);
                    tcol  = RGB(15, 23, 42);
                } else if (m == 4) {
                    dfill = RGB(245, 158, 11);
                    dbdr  = RGB(254, 240, 138);
                    tcol  = RGB(15, 23, 42);
                } else if (m == 3) {
                    dfill = RGB(219, 39, 119);
                    dbdr  = RGB(251, 207, 232);
                    tcol  = RGB(255, 255, 255);
                } else if (m == 2 && g_chord_skeleton) {
                    dfill = RGB(5, 150, 105);
                    dbdr  = RGB(167, 243, 208);
                    tcol  = RGB(255, 255, 255);
                } else {
                    dfill = RGB(2, 132, 199);
                    dbdr  = RGB(186, 230, 253);
                    tcol  = RGB(255, 255, 255);
                }

                draw_circle_aa(f_cx, sy, 13.0f, 2.0f, dfill, dbdr);
                char dlbl[8];
                format_dot_label(pc, rel_iv, 0, dlbl, sizeof(dlbl));
                draw_text_centered(FONT_UI_13, f_cx - 13, sy - 13, 26, 26, dlbl, tcol);

            } else if (g_mode == MODE_CHORDS) {
                uint8_t m = k_chords[g_chord_idx].mask[rel_iv];
                bool is_voicing_note = (cv->frets[str_idx] == f);

                if (!is_voicing_note) {
                    if (!g_show_arp_bg || m == 0) continue;
                    uint32_t afill = (m == 4) ? RGB(110, 74, 16) : RGB(28, 64, 68);
                    uint32_t abdr  = (m == 4) ? RGB(217, 119, 6) : RGB(45, 122, 118);
                    draw_circle_aa(f_cx, sy, 7.5f, 1.5f, afill, abdr);
                    continue;
                }

                uint32_t dfill, dbdr, tcol;
                if (ringing) {
                    dfill = RGB(255, 255, 255);
                    dbdr  = RGB(250, 204, 21);
                    tcol  = RGB(15, 23, 42);
                } else if (m == 4) {
                    dfill = RGB(245, 158, 11);
                    dbdr  = RGB(254, 240, 138);
                    tcol  = RGB(15, 23, 42);
                } else if (m == 3) {
                    dfill = RGB(219, 39, 119);
                    dbdr  = RGB(251, 207, 232);
                    tcol  = RGB(255, 255, 255);
                } else {
                    dfill = RGB(5, 150, 105);
                    dbdr  = RGB(167, 243, 208);
                    tcol  = RGB(255, 255, 255);
                }

                draw_circle_aa(f_cx, sy, 13.5f, 2.0f, dfill, dbdr);
                char dlbl[8];
                format_dot_label(pc, rel_iv, cv->fingers[str_idx], dlbl, sizeof(dlbl));
                draw_text_centered(FONT_UI_13, f_cx - 13, sy - 13, 26, 26, dlbl, tcol);

            } else {
                bool is_locked = (g_exp_locked[str_idx] == f);
                bool is_cursor = (g_exp_row == r && g_exp_fret == f);

                if (!is_locked && !is_cursor) {
                    if (pc == g_root_pc) {
                        draw_circle_aa(f_cx, sy, 5.5f, 1.5f, RGB(68, 52, 22), RGB(146, 92, 18));
                    }
                    continue;
                }

                uint32_t dfill = is_locked ? ((pc == g_root_pc) ? RGB(245, 158, 11) : RGB(5, 150, 105))
                                           : RGB(37, 99, 235);
                uint32_t dbdr  = is_cursor ? RGB(255, 255, 255) : RGB(167, 243, 208);
                uint32_t tcol  = (is_locked && pc == g_root_pc) ? RGB(15, 23, 42) : RGB(255, 255, 255);

                if (ringing) {
                    dfill = RGB(255, 255, 255);
                    tcol  = RGB(15, 23, 42);
                }

                if (is_cursor) {
                    draw_border(f_cx - 16, sy - 16, 32, 32, 2, RGB(250, 204, 21));
                }
                draw_circle_aa(f_cx, sy, 13.5f, 2.0f, dfill, dbdr);
                char dlbl[8];
                format_dot_label(pc, rel_iv, 0, dlbl, sizeof(dlbl));
                draw_text_centered(FONT_UI_13, f_cx - 13, sy - 13, 26, 26, dlbl, tcol);
            }
        }
    }
}

static void render_legend_and_footer(void) {
    /* Legend & CAGED Box Strip: y = 396..434 (38px) */
    int ly = 396, lh = 38;
    fill_rect(8, ly, NATIVE_W - 16, lh, RGB(21, 30, 48));
    draw_border(8, ly, NATIVE_W - 16, lh, 2, RGB(51, 68, 98));

    draw_circle_aa(28, ly + 19, 8.5f, 1.5f, RGB(245, 158, 11), RGB(254, 240, 138));
    draw_text(FONT_UI_13, 42, ly + 10, "Root (R)", RGB(254, 240, 138));

    draw_circle_aa(126, ly + 19, 8.5f, 1.5f, RGB(5, 150, 105), RGB(167, 243, 208));
    draw_text(FONT_UI_13, 140, ly + 10, "Chord (3/5/7)", RGB(167, 243, 208));

    draw_circle_aa(268, ly + 19, 8.5f, 1.5f, RGB(219, 39, 119), RGB(251, 207, 232));
    draw_text(FONT_UI_13, 282, ly + 10, "Color / Blue", RGB(251, 207, 232));

    draw_circle_aa(398, ly + 19, 8.5f, 1.5f, RGB(2, 132, 199), RGB(186, 230, 253));
    draw_text(FONT_UI_13, 412, ly + 10, "Scale Tone", RGB(186, 230, 253));

    draw_pill(504, ly + 6, 120, 26, RGB(30, 43, 68), RGB(96, 165, 250));
    const char *lr_txt = (g_mode == MODE_SCALES) ? "L1/R1: CAGED" :
                         (g_mode == MODE_CHORDS) ? "L1/R1: Voicing" : "B: Lock Fret";
    draw_text_centered(FONT_UI_13, 504, ly + 6, 120, 26, lr_txt, RGB(224, 242, 254));

    /* Bottom Control Bar: y = 438..480 (42px) */
    int by = 438, bh = 42;
    fill_rect(0, by, NATIVE_W, bh, RGB(15, 22, 35));
    fill_rect(0, by, NATIVE_W, 2, RGB(45, 62, 92));

    const char *lbl_txt = (g_label_mode == LABEL_INTERVALS) ? "X: Intervals" :
                          (g_label_mode == LABEL_NOTES)     ? "X: Note Names" :
                                                              "X: Degrees";
    draw_pill(8, by + 7, 118, 28, RGB(23, 52, 74), RGB(56, 189, 248));
    draw_text_centered(FONT_UI_13, 8, by + 7, 118, 28, lbl_txt, RGB(224, 242, 254));

    const char *ctrl_txt = (g_mode == MODE_SCALES)
        ? "A: Play Box  |  B: Skeleton  |  L/R: Scale  |  U/D: Key"
        : (g_mode == MODE_CHORDS)
        ? "A: Strum  |  B: Arpeggio  |  L/R: Chord  |  U/D: Key"
        : "A: Pluck/Strum  |  B: Lock Note  |  DPad: Move Cursor";
    draw_text_centered(FONT_UI_13, 130, by + 7, NATIVE_W - 224, 28, ctrl_txt, RGB(203, 213, 225));

    draw_pill(NATIVE_W - 92, by + 7, 84, 28, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, NATIVE_W - 92, by + 7, 84, 28, "Y: Help", RGB(254, 240, 138));
}

static void render_help_modal(void) {
    int mx = 20, my = 18, mw = NATIVE_W - 40, mh = NATIVE_H - 36;
    fill_rect(mx, my, mw, mh, RGB(15, 23, 38));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    fill_rect(mx + 4, my + 4, mw - 8, 40, RGB(28, 39, 60));
    fill_rect(mx + 4, my + 44, mw - 8, 2, RGB(64, 82, 115));
    draw_pill(mx + 12, my + 8, 84, 30, RGB(37, 99, 235), RGB(147, 197, 253));
    draw_text_centered(FONT_UI_13, mx + 12, my + 8, 84, 30, "HELP", RGB(255, 255, 255));
    draw_text(FONT_UI_20, mx + 108, my + 10, "RG35XXSP FRETBOARD & TUNINGS", RGB(248, 250, 252));

    int gy = my + 54;
    fill_rect(mx + 14, gy, mw - 28, 72, RGB(23, 33, 52));
    draw_border(mx + 14, gy, mw - 28, 72, 2, RGB(52, 211, 153));

    char t1[96];
    snprintf(t1, sizeof(t1), "[L/R or R2] Tuning (%d/%d): %s",
             g_tuning_idx + 1, NUM_TUNINGS, k_tunings[g_tuning_idx].name);
    draw_text_centered(FONT_UI_16, mx + 14, gy + 10, mw - 28, 24, t1, RGB(250, 204, 21));
    draw_text_centered(FONT_UI_13, mx + 14, gy + 40, mw - 28, 20,
                       "Tunings: Std E  |  Drop D  |  Eb Std  |  D Std  |  DADGAD  |  Open G",
                       RGB(167, 243, 208));

    int ky = gy + 84;
    static const struct {
        const char *btn;
        const char *desc;
    } k_rows[7] = {
        { "SELECT",       "Switch Mode: Scales & CAGED -> Chords -> Fret Explorer" },
        { "A Button",     "Audition Scale Box / Strum Chord Voicing / Pluck Note" },
        { "B Button",     "Scales: Chord Tones | Chords: Full Arp | Explorer: Lock" },
        { "X Button",     "Cycle Dot Labels: Intervals (R,b3) -> Notes -> Degrees" },
        { "L1 / R1",      "Cycle 5 CAGED Boxes (POS 1..5) or 5 Chord Voicings" },
        { "L2 / R2",      "L2: Timbre (Acoustic/Chorus/Lead) | R2: Cycle 6 Tunings" },
        { "DPAD / START", "U/D: Root Key | L/R: Scale/Chord | START: Save & Exit" }
    };

    for (int i = 0; i < 7; i++) {
        int ry = ky + i * 36;
        draw_pill(mx + 16, ry, 132, 28, RGB(30, 58, 95), RGB(71, 98, 145));
        draw_text_centered(FONT_UI_13, mx + 16, ry, 132, 28, k_rows[i].btn, RGB(254, 240, 138));
        draw_text(FONT_UI_13, mx + 160, ry + 6, k_rows[i].desc, RGB(226, 232, 240));
    }

    fill_rect(mx + 4, my + mh - 32, mw - 8, 28, RGB(12, 18, 30));
    draw_text_centered(FONT_UI_13, mx, my + mh - 30, mw, 24,
                       "Press [Y] or [B] to close Help and return to Fretboard",
                       RGB(186, 230, 253));
}

static void render_ui(void) {
    fill_rect(0, 0, NATIVE_W, NATIVE_H, RGB(13, 18, 28));

    ComputedVoicing cv;
    compute_chord_voicing(g_root_pc, g_chord_idx, g_voicing_pos, &cv);

    render_top_status_bar();
    render_theory_and_scope_card(&cv);
    render_hero_fretboard(&cv);
    render_legend_and_footer();

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

    resolve_config_path();
    load_config();

    /* Automated Screenshot State Generator (--screenshot-states <dir>) */
    if (argc >= 3 && strcmp(argv[1], "--screenshot-states") == 0) {
        const char *dir = argv[2];
        char path[512];
        uint8_t dummy_pcm[2048];

        /* 1. Scales & CAGED View: Key of A, Blues Scale, POS 1 (E-Shape) at Frets 4-8 */
        g_mode           = MODE_SCALES;
        g_root_pc        = 9; /* A */
        g_scale_idx      = 1; /* Blues Scale */
        g_caged_pos      = 1; /* POS 1 (E-Shape) */
        g_chord_skeleton = true;
        g_label_mode     = LABEL_INTERVALS;
        g_tuning_idx     = 0;
        g_timbre         = TIMBRE_ACOUSTIC;
        g_show_help      = false;
        audition_current_mode();
        for (int i = 0; i < 16; i++) audio_callback(NULL, dummy_pcm, sizeof(dummy_pcm));
        g_anim_tick = 5;
        render_ui();
        snprintf(path, sizeof(path), "%s/fretboard_scales.bmp", dir);
        save_bmp(path);

        /* 2. Chords & Arpeggios View: Key of C, Cmaj7, Voicing 2 (A-Shape Barre) + Full-Neck Arp */
        g_mode        = MODE_CHORDS;
        g_root_pc     = 0; /* C */
        g_chord_idx   = 3; /* maj7 */
        g_voicing_pos = 1; /* Voicing 2 (A-Shape Barre at Fret 3) */
        g_show_arp_bg = true;
        g_label_mode  = LABEL_NOTES;
        g_timbre      = TIMBRE_CHORUS_EL;
        g_show_help   = false;
        audition_current_mode();
        for (int i = 0; i < 18; i++) audio_callback(NULL, dummy_pcm, sizeof(dummy_pcm));
        g_anim_tick = 12;
        render_ui();
        snprintf(path, sizeof(path), "%s/fretboard_chords.bmp", dir);
        save_bmp(path);

        /* 3. Fret Explorer / Reverse Chord Finder: Em(add9) (0-2-4-0-0-0) */
        g_mode          = MODE_EXPLORER;
        g_root_pc       = 4; /* E */
        g_exp_locked[0] = 0;
        g_exp_locked[1] = 2;
        g_exp_locked[2] = 4;
        g_exp_locked[3] = 0;
        g_exp_locked[4] = 0;
        g_exp_locked[5] = 0;
        g_exp_row       = 3; /* D string */
        g_exp_fret      = 4; /* Fret 4 (F#) */
        g_label_mode    = LABEL_NOTES;
        g_timbre        = TIMBRE_OVERDRIVE;
        g_show_help     = false;
        audition_current_mode();
        for (int i = 0; i < 18; i++) audio_callback(NULL, dummy_pcm, sizeof(dummy_pcm));
        g_anim_tick = 19;
        render_ui();
        snprintf(path, sizeof(path), "%s/fretboard_explorer.bmp", dir);
        save_bmp(path);

        /* 4. Help & Guitar Tuning Modal (Y Button) */
        g_show_help = true;
        render_ui();
        snprintf(path, sizeof(path), "%s/fretboard_help.bmp", dir);
        save_bmp(path);
        return 0;
    }

    if (platform_init() != 0) return 1;

    audition_current_mode();

    while (g_running) {
        uint32_t pressed = platform_poll_pressed();
        if (pressed & (BTN_QUIT | BTN_START | BTN_MENU)) {
            g_running = 0;
            break;
        }

        if (pressed & BTN_Y) {
            g_show_help = !g_show_help;
        } else if (g_show_help) {
            if (pressed & BTN_LEFT) {
                g_tuning_idx = (g_tuning_idx + NUM_TUNINGS - 1) % NUM_TUNINGS;
            } else if (pressed & (BTN_RIGHT | BTN_R2)) {
                g_tuning_idx = (g_tuning_idx + 1) % NUM_TUNINGS;
            } else if (pressed & (BTN_A | BTN_B | BTN_SELECT)) {
                g_show_help = false;
            }
        } else {
            if (pressed & BTN_SELECT) {
                g_mode = (AppMode)(((int)g_mode + 1) % 3);
                audition_current_mode();
            } else if (pressed & BTN_X) {
                g_label_mode = (DotLabelMode)(((int)g_label_mode + 1) % 3);
            } else if (pressed & BTN_A) {
                audition_current_mode();
            } else if (pressed & BTN_B) {
                if (g_mode == MODE_SCALES) {
                    g_chord_skeleton = !g_chord_skeleton;
                } else if (g_mode == MODE_CHORDS) {
                    g_show_arp_bg = !g_show_arp_bg;
                } else {
                    int str_idx = 5 - g_exp_row;
                    if (g_exp_locked[str_idx] == g_exp_fret) {
                        g_exp_locked[str_idx] = -1;
                    } else {
                        g_exp_locked[str_idx] = g_exp_fret;
                        PluckBatchCmd cmd;
                        memset(&cmd, 0, sizeof(cmd));
                        cmd.timbre = (int)g_timbre;
                        cmd.count  = 1;
                        cmd.notes[0] = (PluckSpec){ str_idx, g_exp_fret, k_tunings[g_tuning_idx].midi[str_idx] + g_exp_fret, 0 };
                        post_pluck_batch(&cmd);
                    }
                }
            } else if (pressed & BTN_L1) {
                if (g_mode == MODE_SCALES) {
                    g_caged_pos = (g_caged_pos + 6 - 1) % 6;
                } else if (g_mode == MODE_CHORDS) {
                    g_voicing_pos = (g_voicing_pos + 5 - 1) % 5;
                    audition_current_mode();
                } else {
                    g_exp_fret -= 3;
                    if (g_exp_fret < 0) g_exp_fret = 0;
                }
            } else if (pressed & BTN_R1) {
                if (g_mode == MODE_SCALES) {
                    g_caged_pos = (g_caged_pos + 1) % 6;
                } else if (g_mode == MODE_CHORDS) {
                    g_voicing_pos = (g_voicing_pos + 1) % 5;
                    audition_current_mode();
                } else {
                    g_exp_fret += 3;
                    if (g_exp_fret > MAX_FRET) g_exp_fret = MAX_FRET;
                }
            } else if (pressed & BTN_L2) {
                g_timbre = (GuitarTimbre)((g_timbre + 1) % 3);
                audition_current_mode();
            } else if (pressed & BTN_R2) {
                g_tuning_idx = (g_tuning_idx + 1) % NUM_TUNINGS;
                audition_current_mode();
            } else if (g_mode == MODE_SCALES) {
                if (pressed & BTN_UP)        g_root_pc = (g_root_pc + 1) % 12;
                else if (pressed & BTN_DOWN) g_root_pc = (g_root_pc + 11) % 12;
                else if (pressed & BTN_LEFT) g_scale_idx = (g_scale_idx + NUM_SCALES - 1) % NUM_SCALES;
                else if (pressed & BTN_RIGHT)g_scale_idx = (g_scale_idx + 1) % NUM_SCALES;
            } else if (g_mode == MODE_CHORDS) {
                if (pressed & BTN_UP)        { g_root_pc = (g_root_pc + 1) % 12; audition_current_mode(); }
                else if (pressed & BTN_DOWN) { g_root_pc = (g_root_pc + 11) % 12; audition_current_mode(); }
                else if (pressed & BTN_LEFT) { g_chord_idx = (g_chord_idx + NUM_CHORDS - 1) % NUM_CHORDS; audition_current_mode(); }
                else if (pressed & BTN_RIGHT){ g_chord_idx = (g_chord_idx + 1) % NUM_CHORDS; audition_current_mode(); }
            } else {
                if (pressed & BTN_UP)        g_exp_row = (g_exp_row + NUM_STRINGS - 1) % NUM_STRINGS;
                else if (pressed & BTN_DOWN) g_exp_row = (g_exp_row + 1) % NUM_STRINGS;
                else if (pressed & BTN_LEFT) g_exp_fret = (g_exp_fret + NUM_FRETS - 1) % NUM_FRETS;
                else if (pressed & BTN_RIGHT)g_exp_fret = (g_exp_fret + 1) % NUM_FRETS;
            }
        }

        g_anim_tick++;
        render_ui();
        platform_present();
        usleep(16000);
    }

    save_config();
    platform_shutdown();
    return 0;
}
