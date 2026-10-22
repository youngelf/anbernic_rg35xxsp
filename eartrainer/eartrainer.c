/*
 * Anbernic RG 35xxSP Guitar Interval & Chord Quality Ear Trainer (eartrainer.c)
 *
 * Native 640x480 single-screen edition engineered for the Anbernic RG 35xxSP
 * (Allwinner H700 quad-core 64-bit ARM Cortex-A53) and x86_64 Linux (X11).
 *
 * Key Upgrades for RG 35xxSP:
 *   1. High-Fidelity 64-Bit Floating-Point Extended Karplus-Strong (EKS) Audio:
 *      - 6-voice polyphonic physical string modeling with first-order Thiran
 *        all-pass fractional delay interpolation, pick-position comb filtering,
 *        dual-biquad acoustic guitar body resonance (~102 Hz Helmholtz air +
 *        ~215 Hz spruce top plate), and stereo modulated chorus.
 *      - 3 selectable ear-training timbres (L2 / '1'): Acoustic Dreadnought,
 *        Chorus Electric, and Pure Harmonic Bell.
 *   2. Dedicated Background Audio Synthesis Thread:
 *      - All note scheduling, waveguide string excitation, body filtering, and
 *        stereo chorus run asynchronously in the background audio thread via a
 *        lock-free atomic command queue so the 640x480 UI never stutters.
 *   3. Hardware-Accurate RG 35xxSP Controls & Linux X11 Keyboard Support:
 *      - Full evdev button mapping (304=A, 305=B, 306=X, 307=Y, 308=L1, 309=R1,
 *        310/314=SELECT, 311/315=START, 312=L2, 313=R2) plus desktop keys.
 *   4. Rich 640x480 Visual Layout:
 *      - 12-fret 6-string rosewood guitar fretboard with interval degree
 *        badges (R, 3, 5, 7), inspector card, and 12 detailed answer cards.
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

#include "eartrainer_font.h"

/* ============================================================================
 * 1. CONSTANTS & DISPLAY DIMENSIONS (640x480 NATIVE)
 * ============================================================================ */

#define NATIVE_W        640
#define NATIVE_H        480
#define SAMPLE_RATE     44100
#define AUDIO_CHANNELS  2
#define NUM_ITEMS       12
#define NUM_STRINGS     6

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

#include "../rg35xxsp_input.h"
#undef BTN_QUIT
#define BTN_QUIT    (1u << 20)

/* ============================================================================
 * 3. MUSIC THEORY TABLES: INTERVALS, CHORD QUALITIES & GUITAR VOICINGS
 * ============================================================================ */

/* Standard 6-string guitar open string MIDI notes: E2(40), A2(45), D3(50), G3(55), B3(59), E4(64) */
static const int k_open_string_midi[6] = { 40, 45, 50, 55, 59, 64 };
static const char *k_string_names[6]   = { "E", "A", "D", "G", "B", "e" };
static const char *k_note_names[12]    = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

typedef struct {
    const char *short_name;
    const char *deg_badge;
    const char *sub_label;
    const char *full_name;
    const char *mnemonic;
    int semitones;
    int min_tier; /* 0 = Easy, 1 = Medium (Diatonic), 2 = All 12 (Chromatic) */
} IntervalDef;

static const IntervalDef k_intervals[NUM_ITEMS] = {
    { "m2", "b2", "1 semitone",   "Minor 2nd",   "\"Jaws\" Theme",         1, 2 },
    { "M2", "2",  "2 semitones",  "Major 2nd",   "\"Happy Birthday\"",     2, 1 },
    { "m3", "b3", "3 semitones",  "Minor 3rd",   "\"Smoke on the Water\"", 3, 0 },
    { "M3", "3",  "4 semitones",  "Major 3rd",   "\"When the Saints\"",    4, 0 },
    { "P4", "4",  "5 semitones",  "Perfect 4th", "\"Here Comes Bride\"",   5, 0 },
    { "TT", "b5", "6 semitones",  "Tritone",     "\"The Simpsons\"",       6, 2 },
    { "P5", "5",  "7 semitones",  "Perfect 5th", "\"Star Wars\" Theme",    7, 0 },
    { "m6", "b6", "8 semitones",  "Minor 6th",   "\"The Entertainer\"",    8, 2 },
    { "M6", "6",  "9 semitones",  "Major 6th",   "\"My Bonnie\"",          9, 1 },
    { "m7", "b7", "10 semitones", "Minor 7th",   "\"Star Trek\" Theme",   10, 2 },
    { "M7", "7",  "11 semitones", "Major 7th",   "\"Take On Me\"",        11, 1 },
    { "P8", "8",  "12 semitones", "Octave",      "\"Over the Rainbow\"",  12, 0 }
};

typedef struct {
    const char *short_name;
    const char *formula;
    const char *full_name;
    int num_notes;
    int semitones[4];
    const char *deg_badges[4];
    int str_off[4];
    int fret_off[4];
    int min_tier; /* 0 = Easy, 1 = Medium, 2 = All 12 */
} ChordDef;

static const ChordDef k_chords[NUM_ITEMS] = {
    { "Major", "1 - 3 - 5",       "Major Triad",       4, {0, 4, 7, 12},  {"R","5","R","3"},   {0, 1, 2, 3}, {0, 2, 2, 2}, 0 },
    { "Minor", "1 - b3 - 5",      "Minor Triad",       4, {0, 3, 7, 12},  {"R","5","R","b3"},  {0, 1, 2, 3}, {0, 2, 2, 1}, 0 },
    { "dim",   "1 - b3 - b5",     "Diminished Triad",  3, {0, 3, 6, 0},   {"R","b5","R",""},   {0, 1, 2, 0}, {0, 1, 2, 0}, 1 },
    { "aug",   "1 - 3 - #5",      "Augmented Triad",   3, {0, 4, 8, 0},   {"R","3","#5",""},   {0, 1, 2, 0}, {0, -1, 1, 0}, 1 },
    { "sus2",  "1 - 2 - 5",       "Suspended 2nd",     4, {0, 2, 7, 12},  {"R","5","R","2"},   {0, 1, 2, 3}, {0, 2, 2, 0}, 0 },
    { "sus4",  "1 - 4 - 5",       "Suspended 4th",     4, {0, 5, 7, 12},  {"R","5","R","4"},   {0, 1, 2, 3}, {0, 2, 2, 3}, 0 },
    { "Dom7",  "1 - 3 - 5 - b7",  "Dominant 7th (7)",  4, {0, 4, 7, 10},  {"R","5","b7","3"},  {0, 1, 2, 3}, {0, 2, 0, 2}, 1 },
    { "Maj7",  "1 - 3 - 5 - 7",   "Major 7th",         4, {0, 4, 7, 11},  {"R","5","7","3"},   {0, 1, 2, 3}, {0, 2, 1, 2}, 1 },
    { "m7",    "1 - b3 - 5 - b7", "Minor 7th",         4, {0, 3, 7, 10},  {"R","5","b7","b3"}, {0, 1, 2, 3}, {0, 2, 0, 1}, 1 },
    { "m7b5",  "1 - b3 - b5 - b7","Half-Diminished",   4, {0, 3, 6, 10},  {"R","b5","b7","b3"},{0, 1, 2, 3}, {0, 1, 0, 1}, 2 },
    { "dim7",  "1 - b3 - b5 - 6", "Full Diminished 7", 4, {0, 3, 6, 9},   {"R","b5","6","b3"}, {0, 1, 2, 3}, {0, 1, -1, 1}, 2 },
    { "7sus4", "1 - 4 - 5 - b7",  "Dominant 7sus4",    4, {0, 5, 7, 10},  {"R","5","b7","4"},  {0, 1, 2, 3}, {0, 2, 0, 3}, 2 }
};

static void format_midi_note(int midi, char *buf, size_t sz) {
    int note_idx = ((midi % 12) + 12) % 12;
    int octave = (midi / 12) - 1;
    snprintf(buf, sz, "%s%d", k_note_names[note_idx], octave);
}

static void find_guitar_interval_pos(int root_str, int root_fret, int semitones,
                                     int *out_str, int *out_fret) {
    int target_midi = k_open_string_midi[root_str] + root_fret + semitones;
    int best_str = root_str;
    int best_fret = root_fret + semitones;
    int best_cost = 9999;

    for (int s = root_str; s < 6; s++) {
        int f = target_midi - k_open_string_midi[s];
        if (f < 0 || f > 12) continue;
        int fret_dist = abs(f - root_fret);
        int str_dist = s - root_str;
        int cost = fret_dist * 3 + str_dist * 2;
        if (s == root_str && semitones > 4) cost += 10;
        if (cost < best_cost) {
            best_cost = cost;
            best_str = s;
            best_fret = f;
        }
    }
    *out_str = best_str;
    *out_fret = best_fret;
}

/* ============================================================================
 * 4. BACKGROUND FLOATING-POINT EKS GUITAR & BELL SYNTHESIZER (44.1 kHz STEREO)
 * ============================================================================ */

typedef enum {
    TIMBRE_ACOUSTIC  = 0, /* Warm Steel-String Acoustic Dreadnought */
    TIMBRE_CHORUS_EL = 1, /* Clean Jazz Electric + Stereo LFO Chorus */
    TIMBRE_PURE_BELL = 2  /* Harmonic Bell / Electric Piano Reference */
} SynthTimbre;

static const char *k_timbre_names[3] = {
    "ACOUSTIC", "CHORUS EL", "PURE BELL"
};

#define MAX_VOICES      6
#define MAX_DELAY_LEN   1024
#define MAX_SCHED_NOTES 12
#define CMD_RING_SIZE   16

typedef struct {
    int midi_note;
    int string_hint; /* 0..5 for string vibration animation */
    int delay_ms;
    int duration_ms;
} PluckNoteSpec;

typedef struct {
    int count;
    int timbre;
    PluckNoteSpec notes[MAX_SCHED_NOTES];
} AudioBatchCmd;

static AudioBatchCmd     g_cmd_ring[CMD_RING_SIZE];
static volatile uint32_t g_cmd_write_idx = 0;
static volatile uint32_t g_cmd_read_idx  = 0;

typedef struct {
    bool  active;
    float delay_line[MAX_DELAY_LEN];
    int   delay_len;
    int   pos;
    int   samples_left;
    int   total_samples;
    int   string_idx;
    float damping;
    float stretch;
    float ap_coeff;
    float ap_x1;
    float ap_y1;
    float lp_prev;
    float pan_l;
    float pan_r;
    float phase;
    float phase_inc;
    bool  bell_mode;
} EksVoice;

typedef struct {
    bool pending;
    int  delay_samples;
    int  midi_note;
    int  string_hint;
    int  duration_ms;
    int  timbre;
} ScheduledPluck;

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} BiquadFilter;

static EksVoice       g_voices[MAX_VOICES];
static ScheduledPluck g_sched[MAX_SCHED_NOTES];
static int            g_next_voice = 0;
static uint32_t       g_prng_state = 0x9E3779B9u;
static BiquadFilter   g_body_air;
static BiquadFilter   g_body_top;
static bool           g_dsp_inited = false;

#define CHORUS_BUF_LEN 2048
static float          g_chorus_buf[CHORUS_BUF_LEN];
static int            g_chorus_pos = 0;
static float          g_chorus_lfo = 0.0f;

/* Shared atomic visual telemetry for 60 FPS UI */
static volatile float    g_string_env[NUM_STRINGS];

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

static void excite_voice_in_audio_thread(int midi_note, int string_hint, int duration_ms, int timbre) {
    float freq = 440.0f * powf(2.0f, (float)(midi_note - 69) / 12.0f);
    float exact_period = (float)SAMPLE_RATE / freq;
    int dlen = (int)floorf(exact_period - 0.5f);
    if (dlen < 12) dlen = 12;
    if (dlen >= MAX_DELAY_LEN - 4) dlen = MAX_DELAY_LEN - 4;

    float frac = exact_period - 0.5f - (float)dlen;
    if (frac < 0.05f) frac = 0.05f;
    if (frac > 0.95f) frac = 0.95f;

    EksVoice *v = &g_voices[g_next_voice];
    g_next_voice = (g_next_voice + 1) % MAX_VOICES;

    v->active        = false;
    v->delay_len     = dlen;
    v->pos           = 0;
    v->total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    v->samples_left  = v->total_samples;
    v->string_idx    = (string_hint >= 0 && string_hint < 6) ? string_hint : (midi_note % 6);
    v->ap_coeff      = (1.0f - frac) / (1.0f + frac);
    v->ap_x1         = 0.0f;
    v->ap_y1         = 0.0f;
    v->lp_prev       = 0.0f;
    v->phase         = 0.0f;
    v->phase_inc     = 2.0f * (float)M_PI * freq / (float)SAMPLE_RATE;
    v->bell_mode     = (timbre == TIMBRE_PURE_BELL);

    float pan = ((float)v->string_idx / 5.0f) * 0.46f - 0.23f;
    v->pan_l  = 0.707f * (1.0f - pan);
    v->pan_r  = 0.707f * (1.0f + pan);

    if (timbre == TIMBRE_PURE_BELL) {
        v->damping = 0.9965f;
        v->stretch = 0.65f;
    } else if (timbre == TIMBRE_CHORUS_EL) {
        v->damping = (midi_note < 52) ? 0.9965f : 0.9950f;
        v->stretch = 0.58f;
    } else {
        v->damping = (midi_note < 52) ? 0.9955f : 0.9935f;
        v->stretch = 0.48f;
    }

    int pick_offset = (dlen * 22) / 100;
    if (pick_offset < 2) pick_offset = 2;

    float raw[MAX_DELAY_LEN];
    float lp = 0.0f;
    float alpha = (timbre == TIMBRE_CHORUS_EL) ? 0.52f : 0.36f;

    for (int i = 0; i < dlen; i++) {
        float ph = (2.0f * (float)M_PI * (float)i) / (float)dlen;
        float body = 0.44f * sinf(ph)
                   + 0.22f * sinf(2.0f * ph)
                   + 0.10f * sinf(3.0f * ph);
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

    g_string_env[v->string_idx] = 1.0f;
    v->active = true;
}

static void post_audio_batch(const AudioBatchCmd *cmd) {
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
    int active_timbre = TIMBRE_ACOUSTIC;

    /* Drain lock-free command queue from UI thread */
    while (g_cmd_read_idx != g_cmd_write_idx) {
        AudioBatchCmd cmd = g_cmd_ring[g_cmd_read_idx & (CMD_RING_SIZE - 1)];
        __sync_synchronize();
        g_cmd_read_idx++;

        for (int i = 0; i < MAX_SCHED_NOTES; i++) g_sched[i].pending = false;
        for (int i = 0; i < MAX_VOICES; i++)      g_voices[i].active = false;
        for (int s = 0; s < NUM_STRINGS; s++)     g_string_env[s]    = 0.0f;

        for (int n = 0; n < cmd.count && n < MAX_SCHED_NOTES; n++) {
            g_sched[n].pending       = true;
            g_sched[n].delay_samples = (SAMPLE_RATE * cmd.notes[n].delay_ms) / 1000;
            g_sched[n].midi_note     = cmd.notes[n].midi_note;
            g_sched[n].string_hint   = cmd.notes[n].string_hint;
            g_sched[n].duration_ms   = cmd.notes[n].duration_ms;
            g_sched[n].timbre        = cmd.timbre;
        }
    }

    for (int i = 0; i < frames; i++) {
        for (int s = 0; s < MAX_SCHED_NOTES; s++) {
            if (g_sched[s].pending) {
                if (g_sched[s].delay_samples <= 0) {
                    g_sched[s].pending = false;
                    excite_voice_in_audio_thread(g_sched[s].midi_note,
                                                 g_sched[s].string_hint,
                                                 g_sched[s].duration_ms,
                                                 g_sched[s].timbre);
                } else {
                    g_sched[s].delay_samples--;
                }
            }
        }

        float mix_l = 0.0f;
        float mix_r = 0.0f;
        float mono_sum = 0.0f;

        for (int v = 0; v < MAX_VOICES; v++) {
            EksVoice *ev = &g_voices[v];
            if (!ev->active) continue;
            if (ev->samples_left <= 0) {
                ev->active = false;
                continue;
            }
            ev->samples_left--;
            if (ev->bell_mode) active_timbre = TIMBRE_PURE_BELL;

            float sig = 0.0f;
            if (ev->bell_mode) {
                float env = (float)ev->samples_left / (float)ev->total_samples;
                env = env * env;
                sig = (0.58f * sinf(ev->phase)
                     + 0.25f * sinf(2.0f * ev->phase)
                     + 0.11f * sinf(3.0f * ev->phase)
                     + 0.06f * sinf(4.0f * ev->phase)) * env * 0.65f;
                ev->phase += ev->phase_inc;
                if (ev->phase > 2.0f * (float)M_PI) ev->phase -= 2.0f * (float)M_PI;
            } else {
                int p0 = ev->pos;
                int p1 = (p0 + 1 < ev->delay_len) ? (p0 + 1) : 0;
                float s0 = ev->delay_line[p0];
                float s1 = ev->delay_line[p1];
                sig = s0;

                float avg = (1.0f - ev->stretch) * s0 + ev->stretch * s1;
                float damped = (0.86f * avg + 0.14f * ev->lp_prev) * ev->damping;
                ev->lp_prev = damped;

                float ap_out = ev->ap_coeff * damped + ev->ap_x1 - ev->ap_coeff * ev->ap_y1;
                ev->ap_x1 = damped;
                ev->ap_y1 = ap_out;

                ev->delay_line[p0] = ap_out;
                ev->pos = p1;
            }

            mix_l    += sig * ev->pan_l;
            mix_r    += sig * ev->pan_r;
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

        /* Subtle stereo room / chorus width */
        g_chorus_buf[g_chorus_pos] = mono_sum * 0.45f;
        g_chorus_lfo += 2.0f * (float)M_PI * 1.15f / (float)SAMPLE_RATE;
        if (g_chorus_lfo > 2.0f * (float)M_PI) g_chorus_lfo -= 2.0f * (float)M_PI;

        float d_l = 240.0f + 50.0f * sinf(g_chorus_lfo);
        float d_r = 280.0f + 50.0f * cosf(g_chorus_lfo);
        int idx_l = (g_chorus_pos + CHORUS_BUF_LEN - (int)d_l) & (CHORUS_BUF_LEN - 1);
        int idx_r = (g_chorus_pos + CHORUS_BUF_LEN - (int)d_r) & (CHORUS_BUF_LEN - 1);
        float wet = (active_timbre == TIMBRE_PURE_BELL) ? 0.18f : 0.24f;
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
            pa = fn_new(NULL, "RG35xxSP EarTrainer", PA_STREAM_PLAYBACK, NULL,
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
    XStoreName(g_x11_dpy, g_x11_win, "Anbernic RG 35xxSP EarTrainer (640x480)");

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
    return rg35xxsp_map_x11_keysym(ks);
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
static volatile bool g_alsa_running   = false;
static bool          g_use_sdl2_audio = false;

typedef struct _snd_pcm snd_pcm_t;
#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_FORMAT_S16_LE   2

static void *alsa_audio_worker(void *arg) {
    (void)arg;
    int16_t buf[512 * AUDIO_CHANNELS];
    void *asound = dlopen("libasound.so.2", RTLD_LAZY);
    snd_pcm_t *pcm = NULL;
    long (*snd_writei)(snd_pcm_t*, const void*, unsigned long) = NULL;
    int (*snd_recover)(snd_pcm_t*, int, int) = NULL;
    int (*snd_close)(snd_pcm_t*) = NULL;

    if (asound) {
        int (*snd_open)(snd_pcm_t**, const char*, int, int) = dlsym(asound, "snd_pcm_open");
        int (*snd_set_params)(snd_pcm_t*, int, int, unsigned int, unsigned int, int, unsigned int) = dlsym(asound, "snd_pcm_set_params");
        snd_writei  = dlsym(asound, "snd_pcm_writei");
        snd_recover = dlsym(asound, "snd_pcm_recover");
        snd_close   = dlsym(asound, "snd_pcm_close");

        if (snd_open && snd_set_params && snd_writei && snd_close) {
            if (snd_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0 &&
                snd_open(&pcm, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0 &&
                snd_open(&pcm, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
                pcm = NULL;
            }
            if (pcm) {
                snd_set_params(pcm, SND_PCM_FORMAT_S16_LE, 3, AUDIO_CHANNELS, SAMPLE_RATE, 1, 40000);
            }
        }
    }

    while (g_alsa_running) {
        audio_callback(NULL, (uint8_t *)buf, sizeof(buf));
        if (pcm && snd_writei) {
            long r = snd_writei(pcm, buf, 512);
            if (r < 0 && snd_recover) snd_recover(pcm, (int)r, 1);
        } else {
            usleep(11600);
        }
    }

    if (pcm && snd_close) snd_close(pcm);
    if (asound) dlclose(asound);
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
            if (fn_SDL_Init(SDL2_INIT_VIDEO | SDL2_INIT_AUDIO) == 0 ||
                fn_SDL_Init(SDL2_INIT_VIDEO) == 0) {
                if (fn_SDL_ShowCursor) fn_SDL_ShowCursor(0);
                g_sdl_win = fn_SDL_CreateWindow("EarTrainer",
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
                        g_use_sdl2_audio = true;
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
    }
    if (!g_use_sdl2_audio) {
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
                uint32_t bit = rg35xxsp_map_evkey(ev.code);
                if (bit) {
                    if (ev.value > 0) g_held_mask |= bit;
                    else              g_held_mask &= ~bit;
                }
            } else if (ev.type == EV_ABS) {
                g_held_mask = rg35xxsp_update_evabs(g_held_mask, ev.code, ev.value, 0, 0);
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
 * 7. APPLICATION STATE, QUESTION ENGINE & PERSISTENT ACCURACY STATS
 * ============================================================================ */

typedef enum {
    MODE_INTERVALS = 0,
    MODE_CHORDS    = 1
} TrainerMode;

typedef enum {
    INT_STYLE_ASC  = 0,
    INT_STYLE_DESC = 1,
    INT_STYLE_HARM = 2,
    INT_STYLE_MIX  = 3
} IntervalStyle;

typedef enum {
    CHD_STYLE_ARP_STRUM = 0,
    CHD_STYLE_STRUM     = 1,
    CHD_STYLE_ARP       = 2
} ChordStyle;

static const char *k_int_style_names[4] = { "ASC", "DESC", "HARM", "MIX" };
static const char *k_chd_style_names[3] = { "ARP+STRUM", "STRUM", "ARPEGGIO" };
static const char *k_tier_names[3]      = { "EASY", "MEDIUM", "ALL 12" };

static TrainerMode   g_mode          = MODE_INTERVALS;
static IntervalStyle g_int_style     = INT_STYLE_ASC;
static ChordStyle    g_chd_style     = CHD_STYLE_ARP_STRUM;
static SynthTimbre   g_timbre        = TIMBRE_ACOUSTIC;
static int           g_tier          = 2; /* Default: ALL 12 */
static bool          g_show_help     = false;

/* Current question state */
static int           g_root_str      = 0;
static int           g_root_fret     = 5;
static int           g_root_midi     = 45;
static int           g_correct_idx   = 6;
static bool          g_cur_is_desc   = false;
static int           g_guesses_left  = 3;
static bool          g_wrong_guessed[NUM_ITEMS];
static bool          g_revealed      = false;
static bool          g_solved        = false;
static int           g_audition_idx  = 6;
static int           g_cursor        = 0;
static int           g_anim_tick     = 0;

/* Session & Lifetime Stats */
static int           g_streak        = 0;
static int           g_best_streak   = 0;
static int           g_session_ok    = 0;
static int           g_session_tot   = 0;
static uint32_t      g_int_ok[NUM_ITEMS];
static uint32_t      g_int_tot[NUM_ITEMS];
static uint32_t      g_chd_ok[NUM_ITEMS];
static uint32_t      g_chd_tot[NUM_ITEMS];
static char          g_cfg_path[512];

static void resolve_cfg_path(void) {
    struct stat st;
    if (stat("/userdata/system", &st) == 0 && S_ISDIR(st.st_mode) && access("/userdata/system", W_OK) == 0) {
        snprintf(g_cfg_path, sizeof(g_cfg_path), "/userdata/system/.eartrainer.cfg");
        return;
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_cfg_path, sizeof(g_cfg_path), "%s/.eartrainer.cfg", home);
    } else {
        snprintf(g_cfg_path, sizeof(g_cfg_path), "./.eartrainer.cfg");
    }
}

static void save_stats(void) {
    if (g_cfg_path[0] == '\0') return;
    FILE *f = fopen(g_cfg_path, "w");
    if (!f) return;
    fprintf(f, "best_streak=%d\n", g_best_streak);
    fprintf(f, "mode=%d\n", (int)g_mode);
    fprintf(f, "tier=%d\n", g_tier);
    fprintf(f, "timbre=%d\n", (int)g_timbre);
    for (int i = 0; i < NUM_ITEMS; i++) {
        fprintf(f, "int_%d=%u,%u\n", i, g_int_ok[i], g_int_tot[i]);
        fprintf(f, "chd_%d=%u,%u\n", i, g_chd_ok[i], g_chd_tot[i]);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void load_stats(void) {
    if (g_cfg_path[0] == '\0') return;
    FILE *f = fopen(g_cfg_path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int idx = 0, val = 0;
        unsigned int ok = 0, tot = 0;
        if (sscanf(line, "best_streak=%d", &val) == 1 && val >= 0) {
            g_best_streak = val;
        } else if (sscanf(line, "tier=%d", &val) == 1 && val >= 0 && val <= 2) {
            g_tier = val;
        } else if (sscanf(line, "timbre=%d", &val) == 1 && val >= 0 && val <= 2) {
            g_timbre = (SynthTimbre)val;
        } else if (sscanf(line, "int_%d=%u,%u", &idx, &ok, &tot) == 3 && idx >= 0 && idx < NUM_ITEMS) {
            g_int_ok[idx] = ok;
            g_int_tot[idx] = tot;
        } else if (sscanf(line, "chd_%d=%u,%u", &idx, &ok, &tot) == 3 && idx >= 0 && idx < NUM_ITEMS) {
            g_chd_ok[idx] = ok;
            g_chd_tot[idx] = tot;
        }
    }
    fclose(f);
}

static bool is_item_enabled(int idx) {
    if (idx < 0 || idx >= NUM_ITEMS) return false;
    return (g_mode == MODE_INTERVALS) ? (k_intervals[idx].min_tier <= g_tier)
                                      : (k_chords[idx].min_tier <= g_tier);
}

static void play_root_only(void) {
    AudioBatchCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.timbre = (int)g_timbre;
    cmd.count  = 1;
    cmd.notes[0].midi_note   = g_root_midi;
    cmd.notes[0].string_hint = g_root_str;
    cmd.notes[0].delay_ms    = 0;
    cmd.notes[0].duration_ms = 1400;
    post_audio_batch(&cmd);
}

static void play_item_audio(int item_idx) {
    if (item_idx < 0 || item_idx >= NUM_ITEMS) return;
    AudioBatchCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.timbre = (int)g_timbre;

    if (g_mode == MODE_INTERVALS) {
        int semitones = k_intervals[item_idx].semitones;
        int t_str = 0, t_fret = 0;
        find_guitar_interval_pos(g_root_str, g_root_fret, semitones, &t_str, &t_fret);
        int note1 = g_root_midi;
        int note2 = g_root_midi + semitones;

        cmd.count = 2;
        if (g_int_style == INT_STYLE_HARM) {
            cmd.notes[0] = (PluckNoteSpec){ note1, g_root_str, 0,   1600 };
            cmd.notes[1] = (PluckNoteSpec){ note2, t_str,      24,  1600 };
        } else if (g_cur_is_desc) {
            cmd.notes[0] = (PluckNoteSpec){ note2, t_str,      0,   1150 };
            cmd.notes[1] = (PluckNoteSpec){ note1, g_root_str, 560, 1450 };
        } else {
            cmd.notes[0] = (PluckNoteSpec){ note1, g_root_str, 0,   1150 };
            cmd.notes[1] = (PluckNoteSpec){ note2, t_str,      560, 1450 };
        }
    } else {
        const ChordDef *cd = &k_chords[item_idx];
        int k = 0;
        if (g_chd_style == CHD_STYLE_STRUM) {
            for (int n = 0; n < cd->num_notes; n++) {
                cmd.notes[k++] = (PluckNoteSpec){
                    g_root_midi + cd->semitones[n],
                    g_root_str + cd->str_off[n],
                    n * 34, 1700
                };
            }
        } else if (g_chd_style == CHD_STYLE_ARP) {
            for (int n = 0; n < cd->num_notes; n++) {
                cmd.notes[k++] = (PluckNoteSpec){
                    g_root_midi + cd->semitones[n],
                    g_root_str + cd->str_off[n],
                    n * 220, 1500
                };
            }
        } else {
            for (int n = 0; n < cd->num_notes; n++) {
                cmd.notes[k++] = (PluckNoteSpec){
                    g_root_midi + cd->semitones[n],
                    g_root_str + cd->str_off[n],
                    n * 195, 1100
                };
            }
            int strum_start = cd->num_notes * 195 + 210;
            for (int n = 0; n < cd->num_notes && k < MAX_SCHED_NOTES; n++) {
                cmd.notes[k++] = (PluckNoteSpec){
                    g_root_midi + cd->semitones[n],
                    g_root_str + cd->str_off[n],
                    strum_start + n * 32, 1600
                };
            }
        }
        cmd.count = k;
    }
    post_audio_batch(&cmd);
}

static void generate_new_question(bool play_sound) {
    if (g_mode == MODE_INTERVALS) {
        g_root_str  = rand() % 3;
        g_root_fret = 1 + (rand() % 7);
    } else {
        g_root_str  = 1; /* 5th string (A) root for 4-string moveable guitar voicings */
        g_root_fret = 2 + (rand() % 6);
    }
    g_root_midi = k_open_string_midi[g_root_str] + g_root_fret;

    int candidates[NUM_ITEMS];
    int count = 0;
    for (int i = 0; i < NUM_ITEMS; i++) {
        if (is_item_enabled(i)) candidates[count++] = i;
    }
    if (count == 0) {
        g_correct_idx = 0;
    } else {
        int pick = candidates[rand() % count];
        if (count > 1 && pick == g_correct_idx) pick = candidates[rand() % count];
        g_correct_idx = pick;
    }

    if (g_int_style == INT_STYLE_DESC)      g_cur_is_desc = true;
    else if (g_int_style == INT_STYLE_MIX)  g_cur_is_desc = (rand() % 2) != 0;
    else                                    g_cur_is_desc = false;

    g_guesses_left = 3;
    memset(g_wrong_guessed, 0, sizeof(g_wrong_guessed));
    g_revealed     = false;
    g_solved       = false;
    g_audition_idx = g_correct_idx;

    if (!is_item_enabled(g_cursor)) {
        for (int i = 0; i < NUM_ITEMS; i++) {
            if (is_item_enabled(i)) { g_cursor = i; break; }
        }
    }

    if (play_sound) play_item_audio(g_correct_idx);
}

static void submit_guess(int idx) {
    if (g_revealed) {
        generate_new_question(true);
        return;
    }
    if (!is_item_enabled(idx) || g_wrong_guessed[idx]) return;

    if (idx == g_correct_idx) {
        g_solved       = true;
        g_revealed     = true;
        g_audition_idx = g_correct_idx;
        g_streak++;
        if (g_streak > g_best_streak) g_best_streak = g_streak;
        g_session_ok++;
        g_session_tot++;
        if (g_mode == MODE_INTERVALS) {
            g_int_ok[g_correct_idx]++;
            g_int_tot[g_correct_idx]++;
        } else {
            g_chd_ok[g_correct_idx]++;
            g_chd_tot[g_correct_idx]++;
        }
        save_stats();
        play_item_audio(g_correct_idx);
    } else {
        g_wrong_guessed[idx] = true;
        g_guesses_left--;
        if (g_guesses_left <= 0) {
            g_solved       = false;
            g_revealed     = true;
            g_audition_idx = g_correct_idx;
            g_streak       = 0;
            g_session_tot++;
            if (g_mode == MODE_INTERVALS) g_int_tot[g_correct_idx]++;
            else                          g_chd_tot[g_correct_idx]++;
            save_stats();
            play_item_audio(g_correct_idx);
        } else {
            play_item_audio(g_correct_idx);
        }
    }
}

/* ============================================================================
 * 8. 640x480 UI RENDERING (TOP BAR, 12-FRET GUITAR NECK + SCOPE, 4x3 KEYPAD)
 * ============================================================================ */

static void render_top_status_bar(void) {
    fill_rect(0, 0, NATIVE_W, 40, RGB(19, 27, 44));
    fill_rect(0, 40, NATIVE_W, 2, RGB(51, 68, 98));

    /* 1. Mode Pill (SELECT) */
    uint32_t m_fill = (g_mode == MODE_INTERVALS) ? RGB(37, 99, 235) : RGB(124, 58, 237);
    uint32_t m_bdr  = (g_mode == MODE_INTERVALS) ? RGB(147, 197, 253) : RGB(216, 180, 254);
    const char *m_txt = (g_mode == MODE_INTERVALS) ? "SEL: INTERVALS" : "SEL: CHORDS";
    draw_pill(8, 6, 128, 28, m_fill, m_bdr);
    draw_text_centered(FONT_UI_13, 8, 6, 128, 28, m_txt, RGB(255, 255, 255));

    /* 2. Playback Style Pill (L1) */
    char st_txt[32];
    snprintf(st_txt, sizeof(st_txt), "L1: %s",
             (g_mode == MODE_INTERVALS) ? k_int_style_names[g_int_style] : k_chd_style_names[g_chd_style]);
    int sw = text_width(FONT_UI_11, st_txt) + 14;
    draw_pill(142, 6, sw, 28, RGB(26, 39, 64), RGB(71, 98, 142));
    draw_text_centered(FONT_UI_11, 142, 6, sw, 28, st_txt, RGB(186, 230, 253));

    /* 3. Difficulty Tier Pill (R1) */
    char tr_txt[32];
    snprintf(tr_txt, sizeof(tr_txt), "R1: %s", k_tier_names[g_tier]);
    int tw = text_width(FONT_UI_11, tr_txt) + 14;
    draw_pill(147 + sw, 6, tw, 28, RGB(26, 39, 64), RGB(71, 98, 142));
    draw_text_centered(FONT_UI_11, 147 + sw, 6, tw, 28, tr_txt, RGB(254, 240, 138));

    /* 4. Synth Timbre Pill (L2) */
    char tm_txt[32];
    snprintf(tm_txt, sizeof(tm_txt), "L2: %s", k_timbre_names[g_timbre]);
    int mw = text_width(FONT_UI_11, tm_txt) + 14;
    draw_pill(152 + sw + tw, 6, mw, 28, RGB(18, 52, 72), RGB(56, 189, 248));
    draw_text_centered(FONT_UI_11, 152 + sw + tw, 6, mw, 28, tm_txt, RGB(125, 211, 252));

    /* 5. Streak & Session Score (Right-Aligned) */
    char score_str[64];
    int pct = (g_session_tot > 0) ? ((g_session_ok * 100) / g_session_tot) : 100;
    snprintf(score_str, sizeof(score_str), "Streak:%d  %d/%d (%d%%)",
             g_streak, g_session_ok, g_session_tot, pct);
    draw_text_right(FONT_UI_13, NATIVE_W - 8, 12, score_str, RGB(226, 232, 240));
}

static void render_fretboard_and_inspector(void) {
    /* Middle Section: y = 44..216 (height = 172px) */
    int py = 44, ph = 172;
    fill_rect(0, py, NATIVE_W, ph, RGB(14, 20, 32));
    fill_rect(0, py + ph - 1, NATIVE_W, 1, RGB(45, 58, 82));

    /* Left Card: 12-Fret 6-String Rosewood Fretboard (x = 8..400, y = 48..212) */
    int fx_card = 8, fy_card = 48, fw_card = 392, fh_card = 164;
    fill_rect(fx_card, fy_card, fw_card, fh_card, RGB(20, 28, 44));
    draw_border(fx_card, fy_card, fw_card, fh_card, 2, RGB(48, 65, 95));

    int fb_x   = fx_card + 28;
    int fb_y   = fy_card + 16;
    int fret_w = 29;              /* 12 frets * 29px = 348px */
    int str_sp = 21;              /* 5 string gaps * 21px = 105px */
    int fb_w   = fret_w * 12;
    int fb_h   = str_sp * 5;

    /* Rosewood fingerboard + bone nut */
    fill_rect(fb_x, fb_y - 6, fb_w, fb_h + 12, RGB(44, 28, 20));
    draw_border(fb_x, fb_y - 6, fb_w, fb_h + 12, 2, RGB(96, 68, 48));
    fill_rect(fb_x - 5, fb_y - 6, 6, fb_h + 12, RGB(236, 228, 212));

    /* Fret wires & Subtle Pearloid Inlay Dots (3, 5, 7, 9, double dot at 12) */
    for (int f = 1; f <= 12; f++) {
        int fx = fb_x + f * fret_w;
        fill_rect(fx, fb_y - 4, 2, fb_h + 8, RGB(148, 163, 184));
        int mid_x = fx - fret_w / 2;
        if (f == 3 || f == 5 || f == 7 || f == 9) {
            draw_circle_aa(mid_x, fb_y + fb_h / 2, 4.0f, 1.0f, RGB(88, 78, 70), RGB(120, 110, 100));
        } else if (f == 12) {
            draw_circle_aa(mid_x, fb_y + str_sp * 1 + str_sp / 2, 4.0f, 1.0f, RGB(88, 78, 70), RGB(120, 110, 100));
            draw_circle_aa(mid_x, fb_y + str_sp * 3 + str_sp / 2, 4.0f, 1.0f, RGB(88, 78, 70), RGB(120, 110, 100));
        }
    }

    /* Fret numbers below neck (1..12) */
    for (int f = 1; f <= 12; f++) {
        bool marker = (f == 3 || f == 5 || f == 7 || f == 9 || f == 12 || f == g_root_fret);
        char fstr[8];
        snprintf(fstr, sizeof(fstr), "%d", f);
        int cx = fb_x + f * fret_w - fret_w / 2;
        draw_text_centered(FONT_UI_11, cx - 10, fb_y + fb_h + 9, 20, 14, fstr,
                           (f == g_root_fret) ? RGB(250, 204, 21) :
                           (marker ? RGB(186, 200, 220) : RGB(90, 105, 128)));
    }

    /* 6 Guitar Strings with real-time vibration displacement */
    for (int s = 0; s < 6; s++) {
        int row = 5 - s;
        int sy = fb_y + row * str_sp;
        draw_text(FONT_UI_13, fx_card + 8, sy - 9, k_string_names[s],
                  (s == g_root_str) ? RGB(250, 204, 21) : RGB(148, 163, 184));

        float env = g_string_env[s];
        uint32_t scol = (env > 0.05f) ? RGB(250, 204, 21) :
                        ((s < 3) ? RGB(205, 170, 120) : RGB(212, 220, 232));
        int thick = (s < 2) ? 2 : 1;
        fill_rect(fb_x, sy, fb_w, thick, scol);
    }

    int shown_idx = g_revealed ? g_audition_idx : g_correct_idx;
    int root_row  = 5 - g_root_str;
    int root_cx   = (g_root_fret == 0) ? (fb_x - 2) : (fb_x + g_root_fret * fret_w - fret_w / 2);
    int root_cy   = fb_y + root_row * str_sp;

    /* While awaiting an interval guess, show a subtle '?' target preview for the cursor's interval */
    if (!g_revealed && g_mode == MODE_INTERVALS && is_item_enabled(g_cursor)) {
        int p_str = 0, p_fret = 0;
        find_guitar_interval_pos(g_root_str, g_root_fret, k_intervals[g_cursor].semitones, &p_str, &p_fret);
        int p_row = 5 - p_str;
        int p_cx  = (p_fret == 0) ? (fb_x - 2) : (fb_x + p_fret * fret_w - fret_w / 2);
        int p_cy  = fb_y + p_row * str_sp;
        draw_circle_aa(p_cx, p_cy, 10.0f, 2.0f, RGB(30, 58, 95), RGB(96, 165, 250));
        draw_text_centered(FONT_UI_11, p_cx - 10, p_cy - 10, 20, 20, "?", RGB(186, 230, 253));
    }

    /* If revealed, draw Target Interval or Moveable 4-String Chord Voicing */
    if (g_revealed) {
        if (g_mode == MODE_INTERVALS) {
            int t_str = 0, t_fret = 0;
            find_guitar_interval_pos(g_root_str, g_root_fret, k_intervals[shown_idx].semitones, &t_str, &t_fret);
            int t_row = 5 - t_str;
            int t_cx  = (t_fret == 0) ? (fb_x - 2) : (fb_x + t_fret * fret_w - fret_w / 2);
            int t_cy  = fb_y + t_row * str_sp;
            uint32_t t_fill = (shown_idx == g_correct_idx) ? RGB(16, 185, 129) : RGB(37, 99, 235);
            uint32_t t_bdr  = (shown_idx == g_correct_idx) ? RGB(167, 243, 208) : RGB(191, 219, 254);
            draw_circle_aa(t_cx, t_cy, 10.5f, 2.0f, t_fill, t_bdr);
            draw_text_centered(FONT_UI_11, t_cx - 10, t_cy - 10, 20, 20,
                               k_intervals[shown_idx].deg_badge, RGB(255, 255, 255));
        } else {
            const ChordDef *cd = &k_chords[shown_idx];
            uint32_t c_fill = (shown_idx == g_correct_idx) ? RGB(16, 185, 129) : RGB(37, 99, 235);
            uint32_t c_bdr  = (shown_idx == g_correct_idx) ? RGB(167, 243, 208) : RGB(191, 219, 254);
            for (int n = 1; n < cd->num_notes; n++) {
                int ns = g_root_str + cd->str_off[n];
                int nf = g_root_fret + cd->fret_off[n];
                if (ns >= 0 && ns < 6 && nf >= 0 && nf <= 12) {
                    int nrow = 5 - ns;
                    int ncx  = (nf == 0) ? (fb_x - 2) : (fb_x + nf * fret_w - fret_w / 2);
                    int ncy  = fb_y + nrow * str_sp;
                    draw_circle_aa(ncx, ncy, 10.0f, 2.0f, c_fill, c_bdr);
                    draw_text_centered(FONT_UI_11, ncx - 10, ncy - 10, 20, 20,
                                       cd->deg_badges[n], RGB(255, 255, 255));
                }
            }
        }
    }

    /* Draw Root Note badge ('R') in Amber Gold */
    draw_circle_aa(root_cx, root_cy, 10.5f, 2.0f, RGB(234, 158, 12), RGB(254, 240, 138));
    draw_text_centered(FONT_UI_11, root_cx - 10, root_cy - 10, 20, 20, "R", RGB(20, 20, 20));

    /* Right Inspector Card (x = 406..632, y = 48..212) */
    int ix = 406, iy = 48, iw = 226, ih = 164;
    fill_rect(ix, iy, iw, ih, RGB(21, 30, 48));
    uint32_t ibdr = g_revealed ? (g_solved ? RGB(16, 185, 129) : RGB(239, 68, 68)) : RGB(56, 78, 112);
    draw_border(ix, iy, iw, ih, 2, ibdr);

    char root_name[16];
    format_midi_note(g_root_midi, root_name, sizeof(root_name));

    if (!g_revealed) {
        char rline[48];
        snprintf(rline, sizeof(rline), "Root: %s  (Str %s, Fret %d)",
                 root_name, k_string_names[g_root_str], g_root_fret);
        draw_text(FONT_UI_13, ix + 10, iy + 14, rline, RGB(250, 204, 21));

        const char *q_prompt = (g_mode == MODE_INTERVALS)
                                   ? (g_cur_is_desc ? "Identify Descending Int:" : "Identify Interval:")
                                   : "Identify Guitar Chord:";
        draw_text(FONT_UI_16, ix + 10, iy + 44, q_prompt, RGB(248, 250, 252));

        /* 3-Guess Remaining Pips */
        draw_text(FONT_UI_13, ix + 10, iy + 86, "Tries Left:", RGB(148, 163, 184));
        for (int g = 0; g < 3; g++) {
            bool avail = (g < g_guesses_left);
            uint32_t pfill = avail ? RGB(16, 185, 129) : RGB(185, 28, 28);
            uint32_t pbdr  = avail ? RGB(110, 231, 183) : RGB(252, 165, 165);
            draw_pill(ix + 96 + g * 38, iy + 84, 32, 22, pfill, pbdr);
        }

        draw_text(FONT_UI_11, ix + 10, iy + 128, "[B] Replay   |   [X] Hear Root", RGB(186, 230, 253));
    } else {
        const char *hdr = (shown_idx == g_correct_idx)
                              ? (g_solved ? "CORRECT! (+1 STREAK)" : "ANSWER REVEALED:")
                              : "AUDITIONING (X):";
        uint32_t hcol = (shown_idx == g_correct_idx)
                            ? (g_solved ? RGB(52, 211, 153) : RGB(248, 113, 113))
                            : RGB(96, 165, 250);
        draw_text(FONT_UI_13, ix + 10, iy + 10, hdr, hcol);

        if (g_mode == MODE_INTERVALS) {
            const IntervalDef *id = &k_intervals[shown_idx];
            draw_text(FONT_UI_20, ix + 10, iy + 34, id->full_name, RGB(255, 255, 255));

            char t_name[16];
            format_midi_note(g_root_midi + id->semitones, t_name, sizeof(t_name));
            char nline[64];
            snprintf(nline, sizeof(nline), "%s -> %s", root_name, t_name);
            draw_text(FONT_UI_16, ix + 10, iy + 72, nline, RGB(250, 204, 21));
        } else {
            const ChordDef *cd = &k_chords[shown_idx];
            char cname[48];
            snprintf(cname, sizeof(cname), "%s %s", k_note_names[g_root_midi % 12], cd->short_name);
            draw_text(FONT_UI_20, ix + 10, iy + 34, cname, RGB(255, 255, 255));
            draw_text(FONT_UI_13, ix + 10, iy + 68, cd->full_name, RGB(250, 204, 21));
            char fline[48];
            snprintf(fline, sizeof(fline), "Formula: %s", cd->formula);
            draw_text(FONT_UI_13, ix + 10, iy + 96, fline, RGB(147, 197, 253));
        }
        draw_text(FONT_UI_11, ix + 10, iy + 130, "[A] Next Question  |  [X] Compare", RGB(167, 243, 208));
    }
}

static void render_keypad_grid(void) {
    /* 4x3 Answer Keypad Grid: y = 220..440 (height = 220px) */
    int gx = 8, gy = 220;
    int cols = 4, rows = 3;
    int gap_x = 8, gap_y = 6;
    int bw = (NATIVE_W - 16 - (cols - 1) * gap_x) / cols; /* 150px */
    int bh = (216 - (rows - 1) * gap_y) / rows;           /* 68px  */

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            int bx = gx + c * (bw + gap_x);
            int by = gy + r * (bh + gap_y);

            bool enabled = is_item_enabled(idx);
            bool focused = (idx == g_cursor);
            bool wrong   = g_wrong_guessed[idx];
            bool is_ans  = g_revealed && (idx == g_correct_idx);
            bool is_aud  = g_revealed && (idx == g_audition_idx) && (idx != g_correct_idx);

            uint32_t fill_col = RGB(26, 36, 56);
            uint32_t bdr_col  = RGB(56, 74, 106);
            uint32_t txt_col  = RGB(248, 250, 252);

            if (!enabled) {
                fill_col = RGB(14, 19, 30);
                bdr_col  = RGB(28, 36, 52);
                txt_col  = RGB(58, 70, 90);
            } else if (is_ans) {
                fill_col = RGB(6, 118, 82);
                bdr_col  = RGB(52, 211, 153);
                txt_col  = RGB(255, 255, 255);
            } else if (wrong) {
                fill_col = RGB(122, 26, 26);
                bdr_col  = RGB(248, 113, 113);
                txt_col  = RGB(254, 202, 202);
            } else if (is_aud) {
                fill_col = RGB(29, 78, 216);
                bdr_col  = RGB(96, 165, 250);
                txt_col  = RGB(255, 255, 255);
            }

            fill_rect(bx, by, bw, bh, fill_col);
            draw_border(bx, by, bw, bh, focused ? 3 : 2, focused ? RGB(250, 204, 21) : bdr_col);

            const char *main_lbl = (g_mode == MODE_INTERVALS) ? k_intervals[idx].short_name
                                                              : k_chords[idx].short_name;

            /* Top-right accuracy badge if practiced */
            uint32_t ok  = (g_mode == MODE_INTERVALS) ? g_int_ok[idx]  : g_chd_ok[idx];
            uint32_t tot = (g_mode == MODE_INTERVALS) ? g_int_tot[idx] : g_chd_tot[idx];
            if (tot > 0 && enabled) {
                char acc[16];
                snprintf(acc, sizeof(acc), "%u%%", (ok * 100) / tot);
                draw_text_right(FONT_UI_11, bx + bw - 6, by + 4, acc,
                                is_ans ? RGB(254, 240, 138) : RGB(148, 163, 184));
            }

            draw_text_centered(FONT_TITLE_28, bx, by, bw, bh, main_lbl, txt_col);
        }
    }
}

static void render_bottom_footer(void) {
    int fy = 442, fh = 38;
    fill_rect(0, fy, NATIVE_W, fh, RGB(17, 24, 39));
    fill_rect(0, fy, NATIVE_W, 2, RGB(51, 65, 85));

    const char *msg = g_revealed
        ? "A: Next   |   B: Replay   |   X: Audition & Compare   |   Y: Help & Stats   |   MENU: Exit"
        : "A: Guess (3 Tries)   |   B: Replay   |   X: Root Note   |   Y: Help & Stats   |   MENU: Exit";
    draw_text_centered(FONT_UI_13, 0, fy + 4, NATIVE_W, fh - 4, msg, RGB(186, 230, 253));
}

static void render_help_stats_modal(void) {
    int mx = 20, my = 18, mw = NATIVE_W - 40, mh = NATIVE_H - 36;
    fill_rect(mx, my, mw, mh, RGB(15, 23, 38));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    fill_rect(mx + 4, my + 4, mw - 8, 40, RGB(28, 39, 60));
    fill_rect(mx + 4, my + 44, mw - 8, 2, RGB(64, 82, 115));
    draw_pill(mx + 12, my + 8, 84, 30, RGB(37, 99, 235), RGB(147, 197, 253));
    draw_text_centered(FONT_UI_13, mx + 12, my + 8, 84, 30, "STATS", RGB(255, 255, 255));

    char hdr_buf[96];
    snprintf(hdr_buf, sizeof(hdr_buf), "LIFETIME ACCURACY: %s  (Best Streak: %d)",
             (g_mode == MODE_INTERVALS) ? "INTERVALS" : "CHORDS", g_best_streak);
    draw_text(FONT_UI_16, mx + 108, my + 12, hdr_buf, RGB(248, 250, 252));

    /* Per-item accuracy grid (4x3) */
    int gx = mx + 14, gy = my + 54;
    int bw = (mw - 28 - 18) / 4; /* 138px */
    int bh = 48;
    for (int i = 0; i < NUM_ITEMS; i++) {
        int r = i / 4, c = i % 4;
        int bx = gx + c * (bw + 6);
        int by = gy + r * (bh + 6);
        fill_rect(bx, by, bw, bh, RGB(23, 33, 52));
        draw_border(bx, by, bw, bh, 1, RGB(56, 74, 106));

        const char *name = (g_mode == MODE_INTERVALS) ? k_intervals[i].short_name : k_chords[i].short_name;
        const char *full = (g_mode == MODE_INTERVALS) ? k_intervals[i].full_name  : k_chords[i].formula;
        uint32_t ok  = (g_mode == MODE_INTERVALS) ? g_int_ok[i]  : g_chd_ok[i];
        uint32_t tot = (g_mode == MODE_INTERVALS) ? g_int_tot[i] : g_chd_tot[i];

        char stat_str[32];
        if (tot > 0) snprintf(stat_str, sizeof(stat_str), "%u/%u (%u%%)", ok, tot, (ok * 100) / tot);
        else         snprintf(stat_str, sizeof(stat_str), "No attempts");

        draw_text(FONT_UI_13, bx + 8, by + 4, name, RGB(250, 204, 21));
        draw_text_right(FONT_UI_11, bx + bw - 6, by + 6, full, RGB(148, 163, 184));
        draw_text(FONT_UI_13, bx + 8, by + 25, stat_str,
                  (tot > 0) ? RGB(110, 231, 183) : RGB(100, 116, 139));
    }

    /* Controls Reference Table */
    int ky = my + 222;
    fill_rect(mx + 14, ky, mw - 28, 2, RGB(51, 65, 85));
    ky += 8;

    static const struct {
        const char *btn;
        const char *desc;
    } k_help_rows[6] = {
        { "A / B Buttons", "A: Submit Guess (3 Tries) / Next  |  B: Replay Audio" },
        { "X Button",      "Play Root (Guessing)  |  Audition & Compare (Revealed)" },
        { "SELECT",        "Switch Mode: INTERVALS (m2..P8) <-> CHORDS (12 Types)" },
        { "L1 / R1",       "L1: Play Style (ASC/DESC/HARM/MIX)  |  R1: Difficulty" },
        { "L2 Trigger",    "L2: Cycle Timbre (Acoustic / Chorus El / Pure Bell)" },
        { "MENU / Y",      "MENU: Save Stats & Exit  |  Y: Toggle Help & Stats" }
    };

    for (int i = 0; i < 6; i++) {
        int ry = ky + i * 31;
        draw_pill(mx + 16, ry, 132, 25, RGB(30, 58, 95), RGB(71, 98, 145));
        draw_text_centered(FONT_UI_13, mx + 16, ry, 132, 25, k_help_rows[i].btn, RGB(254, 240, 138));
        draw_text(FONT_UI_13, mx + 160, ry + 4, k_help_rows[i].desc, RGB(226, 232, 240));
    }
}

static void render_ui(void) {
    fill_rect(0, 0, NATIVE_W, NATIVE_H, RGB(11, 16, 26));
    render_top_status_bar();
    render_fretboard_and_inspector();
    render_keypad_grid();
    render_bottom_footer();
    if (g_show_help) {
        render_help_stats_modal();
    }
}

/* ============================================================================
 * 9. MAIN ENTRY POINT & 60 FPS EVENT LOOP
 * ============================================================================ */

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    resolve_cfg_path();
    load_stats();

    /* Automated Screenshot Mode (--screenshot-states <dir>) */
    if (argc >= 3 && strcmp(argv[1], "--screenshot-states") == 0) {
        const char *dir = argv[2];
        char path[512];
        uint8_t dummy_pcm[2048];

        /* 1. Interval Guessing State (1 wrong guess made, 2 tries left) */
        g_mode        = MODE_INTERVALS;
        g_int_style   = INT_STYLE_ASC;
        g_timbre      = TIMBRE_ACOUSTIC;
        g_tier        = 2;
        g_root_str    = 0; /* Low E string */
        g_root_fret   = 5; /* A2 */
        g_root_midi   = 45;
        g_correct_idx = 6; /* P5 */
        g_guesses_left = 2;
        memset(g_wrong_guessed, 0, sizeof(g_wrong_guessed));
        g_wrong_guessed[4] = true; /* P4 tried */
        g_cursor      = 6;
        g_revealed    = false;
        g_show_help   = false;
        g_streak      = 4;
        g_session_ok  = 9;
        g_session_tot = 10;
        for (int i = 0; i < NUM_ITEMS; i++) {
            g_int_tot[i] = 10;
            g_int_ok[i]  = 8 + (i % 3);
            g_chd_tot[i] = 8;
            g_chd_ok[i]  = 6 + (i % 3);
        }
        play_item_audio(g_correct_idx);
        for (int i = 0; i < 16; i++) audio_callback(NULL, dummy_pcm, sizeof(dummy_pcm));
        g_anim_tick = 5;
        render_ui();
        snprintf(path, sizeof(path), "%s/eartrainer_intervals.bmp", dir);
        save_bmp(path);

        /* 2. Interval Revealed State (showing Root + Target on fretboard) */
        g_revealed     = true;
        g_solved       = true;
        g_audition_idx = 6;
        g_streak       = 5;
        g_session_ok   = 10;
        g_session_tot  = 11;
        play_item_audio(g_correct_idx);
        for (int i = 0; i < 24; i++) audio_callback(NULL, dummy_pcm, sizeof(dummy_pcm));
        g_anim_tick = 11;
        render_ui();
        snprintf(path, sizeof(path), "%s/eartrainer_revealed.bmp", dir);
        save_bmp(path);

        /* 3. Chord Quality Mode (Revealed C Maj7 chord voicing on fretboard) */
        g_mode         = MODE_CHORDS;
        g_chd_style    = CHD_STYLE_ARP_STRUM;
        g_timbre       = TIMBRE_CHORUS_EL;
        g_root_str     = 1; /* A string */
        g_root_fret    = 3; /* C3 */
        g_root_midi    = 48;
        g_correct_idx  = 7; /* Maj7 */
        g_audition_idx = 7;
        g_cursor       = 7;
        g_revealed     = true;
        g_solved       = true;
        play_item_audio(g_correct_idx);
        for (int i = 0; i < 18; i++) audio_callback(NULL, dummy_pcm, sizeof(dummy_pcm));
        g_anim_tick = 17;
        render_ui();
        snprintf(path, sizeof(path), "%s/eartrainer_chords.bmp", dir);
        save_bmp(path);

        /* 4. Lifetime Accuracy Stats & Controls Modal */
        g_mode        = MODE_INTERVALS;
        g_best_streak = 14;
        g_show_help   = true;
        render_ui();
        snprintf(path, sizeof(path), "%s/eartrainer_help.bmp", dir);
        save_bmp(path);
        return 0;
    }

    if (platform_init() != 0) return 1;

    generate_new_question(true);

    while (g_running) {
        uint32_t pressed = platform_poll_pressed();
        if (pressed & (BTN_QUIT | BTN_MENU)) {
            g_running = 0;
            break;
        }

        if (g_show_help) {
            if (pressed & (BTN_Y | BTN_A | BTN_B)) {
                g_show_help = false;
            } else if (pressed & BTN_SELECT) {
                g_mode = (g_mode == MODE_INTERVALS) ? MODE_CHORDS : MODE_INTERVALS;
            }
        } else {
            if (pressed & BTN_LEFT) {
                g_cursor = (g_cursor % 4 == 0) ? (g_cursor + 3) : (g_cursor - 1);
            } else if (pressed & BTN_RIGHT) {
                g_cursor = (g_cursor % 4 == 3) ? (g_cursor - 3) : (g_cursor + 1);
            } else if (pressed & BTN_UP) {
                g_cursor = (g_cursor < 4) ? (g_cursor + 8) : (g_cursor - 4);
            } else if (pressed & BTN_DOWN) {
                g_cursor = (g_cursor >= 8) ? (g_cursor - 8) : (g_cursor + 4);
            } else if (pressed & BTN_A) {
                submit_guess(g_cursor);
            } else if (pressed & BTN_B) {
                g_audition_idx = g_correct_idx;
                play_item_audio(g_correct_idx);
            } else if (pressed & BTN_X) {
                if (g_revealed) {
                    g_audition_idx = g_cursor;
                    play_item_audio(g_cursor);
                } else {
                    play_root_only();
                }
            } else if (pressed & BTN_Y) {
                g_show_help = true;
            } else if (pressed & BTN_SELECT) {
                g_mode = (g_mode == MODE_INTERVALS) ? MODE_CHORDS : MODE_INTERVALS;
                save_stats();
                generate_new_question(true);
            } else if (pressed & BTN_L1) {
                if (g_mode == MODE_INTERVALS) {
                    g_int_style = (IntervalStyle)((g_int_style + 1) % 4);
                    if (g_int_style == INT_STYLE_DESC)     g_cur_is_desc = true;
                    else if (g_int_style == INT_STYLE_ASC) g_cur_is_desc = false;
                } else {
                    g_chd_style = (ChordStyle)((g_chd_style + 1) % 3);
                }
                play_item_audio(g_correct_idx);
            } else if (pressed & BTN_R1) {
                g_tier = (g_tier + 1) % 3;
                save_stats();
                generate_new_question(true);
            } else if (pressed & BTN_L2) {
                g_timbre = (SynthTimbre)((g_timbre + 1) % 3);
                save_stats();
                play_item_audio(g_revealed ? g_audition_idx : g_correct_idx);
            }
        }

        g_anim_tick++;
        render_ui();
        platform_present();
        usleep(16000);
    }

    save_stats();
    platform_shutdown();
    return 0;
}
