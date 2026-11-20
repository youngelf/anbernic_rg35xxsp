/*
 * chordwheel.c - Interactive Circle of Fifths, Guitar Voicing Visualizer &
 *                Floating-Point Extended Karplus-Strong (EKS) Guitar Synthesizer
 *                for Anbernic RG 35xxSP (640x480 IPS, Allwinner H700 aarch64) & Linux X11
 *
 * Improvements for Anbernic RG 35xxSP:
 *   1. Studio-Grade Floating-Point Extended Karplus-Strong (EKS) Audio Engine (44.1 kHz Stereo):
 *      - Fractional-delay Lagrange/all-pass interpolated waveguides for exact fret intonation
 *      - Pick-position comb filtering, velocity-sensitive low-pass damping, and dual-biquad
 *        acoustic guitar body resonance (105 Hz Helmholtz + 215 Hz top-plate modes)
 *      - 3 selectable guitar timbres (L2 / 'T'): Acoustic Dreadnought, Electric Chorus, Nylon Arp
 *      - Stereo string panning across the 6 strings + subtle stereo chorus/room reflection
 *   2. 100% Lock-Free Background Audio Synthesis:
 *      - UI thread posts non-blocking strum commands via an atomic sequence ring; all string
 *        excitation, pick-stagger scheduling, and waveguide synthesis run in the background
 *        audio thread while exporting live string envelopes & waveform samples for 60 FPS UI.
 *   3. Exact Anbernic RG 35xxSP Hardware Evdev & Desktop X11 Key Mappings:
 *      - A (304 / BTN_SOUTH) : Strum selected 6-string guitar chord
 *      - B (305 / BTN_EAST)  : Toggle Triads (C, Am, G) <-> 7th Chords (Cmaj7, Am7, G7)
 *      - X (307 / BTN_NORTH) : Add current chord to 4-bar Progression
 *      - Y (306 / BTN_C)     : Open / Close Help, Progression Looper & Presets Modal
 *      - L1 / R1 (308 / 309) : Cycle Guitar Voicing (1: Open/CAGED, 2: E-Barre, 3: A-Barre)
 *      - L2 / R2 (314 / 315) : L2: Cycle Guitar Timbre | R2: Start/Stop 4-Bar Auto-Looper
 *      - SELECT (310)        : Toggle Key-Rotate Mode vs Chord-Explore (Locked Key) Mode
 *      - MENU (312 / Esc)    : Save state & Exit
 *   4. Native 640x480 IPS High-Resolution UI:
 *      - 2x2 supersampled Circle of Fifths wheel with Roman numeral badges inside diatonic sectors
 *      - Rosewood 6-string guitar fretboard with curved barre indicators, interval labels inside
 *        finger dots (R, 3, b3, 5, b5, 7, b7), live vibrating strings, and real-time waveform scope
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

#include "chordwheel_font.h"

#define NATIVE_W       640
#define NATIVE_H       480
#define SAMPLE_RATE    44100
#define AUDIO_CHANNELS 2

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * 1. LOGICAL BUTTON BITMASK & INPUT STATE
 * ============================================================================ */

#include "../rg35xxsp_input.h"

#define RG35XXSP_SF2_IMPLEMENTATION
#include "../rg35xxsp_sf2.h"

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

/* ============================================================================
 * 2. CIRCLE OF FIFTHS & GUITAR MUSIC THEORY DATA
 * ============================================================================ */

typedef enum {
    QUAL_MAJ  = 0,
    QUAL_MIN  = 1,
    QUAL_DIM  = 2,
    QUAL_MAJ7 = 3,
    QUAL_MIN7 = 4,
    QUAL_DOM7 = 5,
    QUAL_M7B5 = 6
} ChordQuality;

typedef struct {
    const char *maj_name;
    const char *min_name;
    const char *dim_root_name;
    const char *key_sig;
    int         maj_pc;
    int         min_pc;
    int         dim_pc;
    bool        use_flats;
} WheelSector;

static const WheelSector k_wheel[12] = {
    { "C",  "Am",  "B",  "0 #/b",  0,  9, 11, false }, /* 0  (12:00) */
    { "G",  "Em",  "F#", "1 #",    7,  4,  6, false }, /* 1  ( 1:00) */
    { "D",  "Bm",  "C#", "2 #",    2, 11,  1, false }, /* 2  ( 2:00) */
    { "A",  "F#m", "G#", "3 #",    9,  6,  8, false }, /* 3  ( 3:00) */
    { "E",  "C#m", "D#", "4 #",    4,  1,  3, false }, /* 4  ( 4:00) */
    { "B",  "G#m", "A#", "5 #",   11,  8, 10, false }, /* 5  ( 5:00) */
    { "F#", "D#m", "F",  "6 #",    6,  3,  5, false }, /* 6  ( 6:00) */
    { "Db", "Bbm", "C",  "5 b",    1, 10,  0, true  }, /* 7  ( 7:00) */
    { "Ab", "Fm",  "G",  "4 b",    8,  5,  7, true  }, /* 8  ( 8:00) */
    { "Eb", "Cm",  "D",  "3 b",    3,  0,  2, true  }, /* 9  ( 9:00) */
    { "Bb", "Gm",  "A",  "2 b",   10,  7,  9, true  }, /* 10 (10:00) */
    { "F",  "Dm",  "E",  "1 b",    5,  2,  4, true  }  /* 11 (11:00) */
};

static const char *k_note_sharp[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static const char *k_note_flat[12] = {
    "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
};

/* Standard guitar open string MIDI pitches: E2(40), A2(45), D3(50), G3(55), B3(59), E4(64) */
static const int k_string_midi[6] = { 40, 45, 50, 55, 59, 64 };
static const char *k_string_names[6] = { "E", "A", "D", "G", "B", "e" };

typedef struct {
    int8_t      frets[6];   /* -1 = muted (X), 0 = open (O), 1..16 = fret number */
    const char *label;      /* "1/3 Open/CAGED", "2/3 E-Barre (6str)", "3/3 A-Barre (5str)" */
} GuitarVoicing;

static const int8_t k_open_maj[12][6] = {
    { -1,  3,  2,  0,  1,  0 }, /* C  */
    { -1,  4,  3,  1,  2,  1 }, /* Db */
    { -1, -1,  0,  2,  3,  2 }, /* D  */
    { -1,  6,  5,  3,  4,  3 }, /* Eb */
    {  0,  2,  2,  1,  0,  0 }, /* E  */
    {  1,  3,  3,  2,  1,  1 }, /* F  */
    {  2,  4,  4,  3,  2,  2 }, /* F# */
    {  3,  2,  0,  0,  0,  3 }, /* G  */
    {  4,  6,  6,  5,  4,  4 }, /* Ab */
    { -1,  0,  2,  2,  2,  0 }, /* A  */
    { -1,  1,  3,  3,  3,  1 }, /* Bb */
    { -1,  2,  4,  4,  4,  2 }  /* B  */
};

static const int8_t k_open_min[12][6] = {
    { -1,  3,  5,  5,  4,  3 }, /* Cm  */
    { -1,  4,  6,  6,  5,  4 }, /* C#m */
    { -1, -1,  0,  2,  3,  1 }, /* Dm  */
    { -1,  6,  8,  8,  7,  6 }, /* D#m */
    {  0,  2,  2,  0,  0,  0 }, /* Em  */
    {  1,  3,  3,  1,  1,  1 }, /* Fm  */
    {  2,  4,  4,  2,  2,  2 }, /* F#m */
    {  3,  5,  5,  3,  3,  3 }, /* Gm  */
    {  4,  6,  6,  4,  4,  4 }, /* G#m */
    { -1,  0,  2,  2,  1,  0 }, /* Am  */
    { -1,  1,  3,  3,  2,  1 }, /* Bbm */
    { -1,  2,  4,  4,  3,  2 }  /* Bm  */
};

static const int8_t k_open_maj7[12][6] = {
    { -1,  3,  2,  0,  0,  0 }, /* Cmaj7  */
    { -1,  4,  3,  1,  1,  1 }, /* Dbmaj7 */
    { -1, -1,  0,  2,  2,  2 }, /* Dmaj7  */
    { -1,  6,  5,  3,  3,  3 }, /* Ebmaj7 */
    {  0,  2,  1,  1,  0,  0 }, /* Emaj7  */
    { -1, -1,  3,  2,  1,  0 }, /* Fmaj7  */
    {  2, -1,  3,  3,  2, -1 }, /* F#maj7 */
    {  3,  2,  0,  0,  0,  2 }, /* Gmaj7  */
    {  4, -1,  5,  5,  4, -1 }, /* Abmaj7 */
    { -1,  0,  2,  1,  2,  0 }, /* Amaj7  */
    { -1,  1,  3,  2,  3,  1 }, /* Bbmaj7 */
    { -1,  2,  4,  3,  4,  2 }  /* Bmaj7  */
};

static const int8_t k_open_min7[12][6] = {
    { -1,  3,  5,  3,  4,  3 }, /* Cm7  */
    { -1,  4,  6,  4,  5,  4 }, /* C#m7 */
    { -1, -1,  0,  2,  1,  1 }, /* Dm7  */
    { -1,  6,  8,  6,  7,  6 }, /* D#m7 */
    {  0,  2,  0,  0,  0,  0 }, /* Em7  */
    {  1,  3,  1,  1,  1,  1 }, /* Fm7  */
    {  2,  4,  2,  2,  2,  2 }, /* F#m7 */
    {  3,  5,  3,  3,  3,  3 }, /* Gm7  */
    {  4,  6,  4,  4,  4,  4 }, /* G#m7 */
    { -1,  0,  2,  0,  1,  0 }, /* Am7  */
    { -1,  1,  3,  1,  2,  1 }, /* Bbm7 */
    { -1,  2,  0,  2,  0,  2 }  /* Bm7  */
};

static const int8_t k_open_dom7[12][6] = {
    { -1,  3,  2,  3,  1,  0 }, /* C7  */
    { -1,  4,  3,  4,  2, -1 }, /* Db7 */
    { -1, -1,  0,  2,  1,  2 }, /* D7  */
    { -1,  6,  5,  6,  4, -1 }, /* Eb7 */
    {  0,  2,  0,  1,  0,  0 }, /* E7  */
    {  1,  3,  1,  2,  1,  1 }, /* F7  */
    {  2,  4,  2,  3,  2,  2 }, /* F#7 */
    {  3,  2,  0,  0,  0,  1 }, /* G7  */
    {  4,  6,  4,  5,  4,  4 }, /* Ab7 */
    { -1,  0,  2,  0,  2,  0 }, /* A7  */
    { -1,  1,  3,  1,  3,  1 }, /* Bb7 */
    { -1,  2,  1,  2,  0,  2 }  /* B7  */
};

static void build_guitar_voicing(int root_pc, ChordQuality qual, int voicing_idx, GuitarVoicing *out) {
    root_pc = ((root_pc % 12) + 12) % 12;
    voicing_idx = ((voicing_idx % 3) + 3) % 3;

    int e_fret = (root_pc - 4 + 12) % 12;
    if (e_fret == 0) e_fret = 12;
    int a_fret = (root_pc - 9 + 12) % 12;
    if (a_fret == 0) a_fret = 12;

    if (voicing_idx == 0) {
        out->label = "1/3 Open / CAGED";
        if (qual == QUAL_MAJ) {
            memcpy(out->frets, k_open_maj[root_pc], 6);
            return;
        } else if (qual == QUAL_MIN) {
            memcpy(out->frets, k_open_min[root_pc], 6);
            return;
        } else if (qual == QUAL_MAJ7) {
            memcpy(out->frets, k_open_maj7[root_pc], 6);
            return;
        } else if (qual == QUAL_MIN7) {
            memcpy(out->frets, k_open_min7[root_pc], 6);
            return;
        } else if (qual == QUAL_DOM7) {
            memcpy(out->frets, k_open_dom7[root_pc], 6);
            return;
        } else {
            int d_fret = (root_pc - 2 + 12) % 12;
            out->frets[0] = -1;
            out->frets[1] = -1;
            out->frets[2] = (int8_t)d_fret;
            out->frets[3] = (int8_t)(d_fret + 1);
            out->frets[4] = (int8_t)d_fret;
            out->frets[5] = (qual == QUAL_M7B5) ? (int8_t)(d_fret + 1) : -1;
            return;
        }
    } else if (voicing_idx == 1) {
        out->label = "2/3 E-Barre (6str)";
        int f = e_fret;
        switch (qual) {
            case QUAL_MAJ:
                out->frets[0] = f; out->frets[1] = f + 2; out->frets[2] = f + 2;
                out->frets[3] = f + 1; out->frets[4] = f; out->frets[5] = f;
                break;
            case QUAL_MIN:
                out->frets[0] = f; out->frets[1] = f + 2; out->frets[2] = f + 2;
                out->frets[3] = f; out->frets[4] = f; out->frets[5] = f;
                break;
            case QUAL_MAJ7:
                out->frets[0] = f; out->frets[1] = -1; out->frets[2] = f + 1;
                out->frets[3] = f + 1; out->frets[4] = f; out->frets[5] = -1;
                break;
            case QUAL_MIN7:
                out->frets[0] = f; out->frets[1] = f + 2; out->frets[2] = f;
                out->frets[3] = f; out->frets[4] = f; out->frets[5] = f;
                break;
            case QUAL_DOM7:
                out->frets[0] = f; out->frets[1] = f + 2; out->frets[2] = f;
                out->frets[3] = f + 1; out->frets[4] = f; out->frets[5] = f;
                break;
            case QUAL_DIM:
            case QUAL_M7B5:
                out->frets[0] = e_fret; out->frets[1] = -1; out->frets[2] = e_fret;
                out->frets[3] = e_fret; out->frets[4] = e_fret - 1; out->frets[5] = -1;
                break;
        }
    } else {
        out->label = "3/3 A-Barre (5str)";
        int f = a_fret;
        switch (qual) {
            case QUAL_MAJ:
                out->frets[0] = -1; out->frets[1] = f; out->frets[2] = f + 2;
                out->frets[3] = f + 2; out->frets[4] = f + 2; out->frets[5] = f;
                break;
            case QUAL_MIN:
                out->frets[0] = -1; out->frets[1] = f; out->frets[2] = f + 2;
                out->frets[3] = f + 2; out->frets[4] = f + 1; out->frets[5] = f;
                break;
            case QUAL_MAJ7:
                out->frets[0] = -1; out->frets[1] = f; out->frets[2] = f + 2;
                out->frets[3] = f + 1; out->frets[4] = f + 2; out->frets[5] = f;
                break;
            case QUAL_MIN7:
                out->frets[0] = -1; out->frets[1] = f; out->frets[2] = f + 2;
                out->frets[3] = f; out->frets[4] = f + 1; out->frets[5] = f;
                break;
            case QUAL_DOM7:
                out->frets[0] = -1; out->frets[1] = f; out->frets[2] = f + 2;
                out->frets[3] = f; out->frets[4] = f + 2; out->frets[5] = f;
                break;
            case QUAL_DIM:
            case QUAL_M7B5:
                out->frets[0] = -1; out->frets[1] = a_fret; out->frets[2] = a_fret + 1;
                out->frets[3] = (qual == QUAL_M7B5) ? a_fret : (a_fret + 2);
                out->frets[4] = a_fret + 1; out->frets[5] = -1;
                break;
        }
    }
}

/* ============================================================================
 * 3. STUDIO-GRADE BACKGROUND SOUNDFONT (SF2) + EKS GUITAR SYNTHESIZER
 *    - Primary: TinySoundFont (GeneralUser-GS.sf2 / pocketband.sf2) on 6
 *      constant-power stereo-panned channels + stereo chorus + Freeverb reverb
 *    - Fallback: Extended Karplus-Strong with 1st-order all-pass dispersion,
 *      zero-mean excitation, 6 ms anti-click crossfade, 350 ms raised-cosine
 *      release, 2-band body resonator, 20 Hz DC blocker, & stereo room reverb
 * ============================================================================ */

typedef enum {
    TIMBRE_ACOUSTIC = 0,
    TIMBRE_CHORUS   = 1,
    TIMBRE_NYLON    = 2
} GuitarTimbre;

static const char *k_timbre_names[3] = {
    "ACOUSTIC",
    "CHORUS EL",
    "NYLON ARP"
};

#define EKS_MAX_DELAY          1024
#define EKS_RING_SAMPLES       (SAMPLE_RATE * 3)
#define EKS_RELEASE_SAMPLES    ((SAMPLE_RATE * 350) / 1000)

typedef struct {
    float buf[EKS_MAX_DELAY];
    int   buf_len;
    int   pos;
    float frac_delay;
    float damping;
    float brightness;
    float disp_coeff;
    float disp_prev_in;
    float disp_prev_out;
    float pan_l;
    float pan_r;
    float last_out;
    float declick_sample;
    int   start_delay_samples;
    int   samples_left;
    bool  active;
} EksString;

/* Per-string SF2 strum scheduler (zero malloc on audio thread) */
typedef struct {
    bool  pending;
    int   delay_samples;
    int   midi_note;
    float vel;
    bool  sounding;
    int   active_midi;
    int   gate_samples;
} Sf2StringSlot;

/* Lock-free strum request posted from UI thread -> Background Audio thread */
typedef struct {
    int8_t       frets[6];
    GuitarTimbre timbre;
} StrumCommand;

static volatile uint32_t g_cmd_write_seq = 0;
static uint32_t          g_cmd_read_seq  = 0;
static StrumCommand      g_pending_cmd;

/* Audio thread internal state */
static Rg35Sf2Engine  g_sf2;
static bool           g_sf2_init_attempted = false;
static int            g_sf2_active_timbre  = -1;
static Sf2StringSlot  g_sf2_slots[6];
static GuitarTimbre   g_audio_timbre = TIMBRE_ACOUSTIC;

static EksString g_eks[6];
static bool      g_eks_pan_ready = false;
static float     g_body_air_b0, g_body_air_a1, g_body_air_a2;
static float     g_body_wood_b0, g_body_wood_a1, g_body_wood_a2;
static float     g_body_air_y1 = 0.0f, g_body_air_y2 = 0.0f;
static float     g_body_wood_y1 = 0.0f, g_body_wood_y2 = 0.0f;
static float     g_chorus_buf_l[2048];
static float     g_chorus_buf_r[2048];
static int       g_chorus_pos = 0;
static float     g_chorus_phase = 0.0f;
static uint32_t  g_audio_rng = 0x9E3779B9u;

static void compute_resonator_coeffs(double freq_hz, double q,
                                     float *b0, float *a1, float *a2) {
    double w0 = (2.0 * M_PI * freq_hz) / (double)SAMPLE_RATE;
    double alpha = sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;
    *b0 = (float)(alpha / a0);
    *a1 = (float)((-2.0 * cos(w0)) / a0);
    *a2 = (float)((1.0 - alpha) / a0);
}

static void init_eks_tables_if_needed(void) {
    if (g_eks_pan_ready) return;
    for (int s = 0; s < 6; s++) {
        double norm = ((double)s / 5.0) * 2.0 - 1.0; /* -1 (low E) to +1 (high e) */
        double angle = (M_PI * 0.25) + norm * 0.34;
        g_eks[s].pan_l = (float)cos(angle);
        g_eks[s].pan_r = (float)sin(angle);
    }
    compute_resonator_coeffs(105.0, 4.5, &g_body_air_b0, &g_body_air_a1, &g_body_air_a2);
    compute_resonator_coeffs(235.0, 5.5, &g_body_wood_b0, &g_body_wood_a1, &g_body_wood_a2);
    g_eks_pan_ready = true;
}

static void sf2_configure_guitar_timbre(GuitarTimbre timbre) {
    if (!rg35_sf2_ready(&g_sf2)) return;
    int pref1 = 25, pref2 = 24;
    if (timbre == TIMBRE_ACOUSTIC) {
        pref1 = 25; pref2 = 24; /* Steel Acoustic Guitar */
    } else if (timbre == TIMBRE_CHORUS) {
        pref1 = 27; pref2 = 26; /* Clean / Jazz Electric Guitar */
    } else {
        pref1 = 24; pref2 = 25; /* Nylon Classical Guitar */
    }
    int idx = tsf_get_presetindex(g_sf2.synth, 0, pref1);
    if (idx < 0) idx = tsf_get_presetindex(g_sf2.synth, 0, pref2);
    if (idx < 0) idx = 0;

    /* Constant-power stereo string spread: 6th string E2 left -> 1st string E4 right */
    static const float k_sf2_pan[6] = { 0.26f, 0.35f, 0.45f, 0.55f, 0.65f, 0.74f };
    for (int s = 0; s < 6; s++) {
        tsf_channel_set_presetindex(g_sf2.synth, s, idx);
        rg35_sf2_set_channel_pan_vol(&g_sf2, s, k_sf2_pan[s], 0.90f);
    }
    g_sf2_active_timbre = (int)timbre;
}

static void init_sf2_in_audio_thread(void) {
    if (g_sf2_init_attempted) return;
    g_sf2_init_attempted = true;
    init_eks_tables_if_needed();
    if (rg35_sf2_init(&g_sf2, SAMPLE_RATE, 48, 0.22f, 1.30f)) {
        g_sf2_active_timbre = -1;
        sf2_configure_guitar_timbre(g_audio_timbre);
    }
}

static void post_strum_command(const GuitarVoicing *v, GuitarTimbre timbre) {
    StrumCommand cmd;
    memcpy(cmd.frets, v->frets, 6);
    cmd.timbre = timbre;
    g_pending_cmd = cmd;
    __sync_synchronize();
    g_cmd_write_seq++;
}

static void init_strum_in_audio_thread(const StrumCommand *cmd) {
    init_eks_tables_if_needed();
    g_audio_timbre = cmd->timbre;
    int stagger_ms = (cmd->timbre == TIMBRE_NYLON) ? 58 :
                     (cmd->timbre == TIMBRE_CHORUS) ? 18 : 22;

    if (rg35_sf2_ready(&g_sf2)) {
        if (g_sf2_active_timbre != (int)cmd->timbre) {
            sf2_configure_guitar_timbre(cmd->timbre);
        }
        int active_order = 0;
        for (int s = 0; s < 6; s++) {
            Sf2StringSlot *sl = &g_sf2_slots[s];
            if (sl->sounding) {
                rg35_sf2_note_off(&g_sf2, s, sl->active_midi);
                sl->sounding = false;
            }
            sl->pending = false;
            if (cmd->frets[s] < 0) continue;

            sl->midi_note     = k_string_midi[s] + cmd->frets[s];
            sl->delay_samples = active_order * ((SAMPLE_RATE * stagger_ms) / 1000);
            sl->vel           = 0.86f - (float)s * 0.025f;
            sl->gate_samples  = (int)(SAMPLE_RATE * 2.8f);
            sl->pending       = true;
            active_order++;
        }
        return;
    }

    /* Upgraded Extended Karplus-Strong fallback with 1st-order all-pass dispersion,
     * zero-mean excitation, and 6 ms declick crossfade */
    int active_order = 0;
    for (int s = 0; s < 6; s++) {
        EksString *st = &g_eks[s];
        if (cmd->frets[s] < 0) {
            if (st->active) st->declick_sample += st->last_out;
            st->active = false;
            st->samples_left = 0;
            st->last_out = 0.0f;
            continue;
        }
        if (st->active) {
            st->declick_sample += st->last_out;
            st->last_out = 0.0f;
        }

        int midi_note = k_string_midi[s] + cmd->frets[s];
        double freq = 440.0 * pow(2.0, (double)(midi_note - 69) / 12.0);
        double exact_period = (double)SAMPLE_RATE / freq;

        float brightness = (cmd->timbre == TIMBRE_NYLON) ? (0.56f - (float)s * 0.025f) :
                           (cmd->timbre == TIMBRE_CHORUS) ? (0.32f - (float)s * 0.020f) :
                                                            (0.43f - (float)s * 0.034f);
        if (brightness < 0.22f) brightness = 0.22f;
        st->brightness = brightness;

        /* 1st-order all-pass inharmonicity dispersion filter */
        float disp_a = (cmd->timbre == TIMBRE_NYLON) ? (-0.12f + (float)s * 0.01f)
                                                     : (-0.25f + (float)s * 0.02f);
        st->disp_coeff    = disp_a;
        st->disp_prev_in  = 0.0f;
        st->disp_prev_out = 0.0f;
        double disp_delay = (1.0 + (double)disp_a) / (1.0 - (double)disp_a);

        double comp_delay = exact_period - (double)brightness - disp_delay;
        int period = (int)floor(comp_delay);
        if (period < 20) period = 20;
        if (period >= EKS_MAX_DELAY - 3) period = EKS_MAX_DELAY - 3;
        st->buf_len    = period;
        st->frac_delay = (float)(comp_delay - (double)period);
        if (st->frac_delay < 0.0f) st->frac_delay = 0.0f;
        if (st->frac_delay > 0.999f) st->frac_delay = 0.999f;

        double target_t60 = (cmd->timbre == TIMBRE_CHORUS ? 3.8 : 3.2) - (double)s * 0.18 - (double)cmd->frets[s] * 0.025;
        if (target_t60 < 1.6) target_t60 = 1.6;
        st->damping = (float)pow(0.001, exact_period / (target_t60 * (double)SAMPLE_RATE));
        if (st->damping > 0.9992f) st->damping = 0.9992f;

        st->pos                 = 0;
        st->start_delay_samples = active_order * ((SAMPLE_RATE * stagger_ms) / 1000);
        st->samples_left        = EKS_RING_SAMPLES;
        active_order++;

        double inv_period = (2.0 * M_PI) / (double)period;
        float noise_lvl = (cmd->timbre == TIMBRE_NYLON) ? 0.18f : (0.26f + 0.025f * (float)s);
        for (int i = 0; i < period; i++) {
            g_audio_rng = g_audio_rng * 1664525u + 1013904223u;
            float white = (float)((int32_t)(g_audio_rng >> 8) - 8388608) * (1.0f / 8388608.0f);
            double ph = (double)i * inv_period;
            double harm = 0.50 * sin(ph) + 0.27 * sin(2.0 * ph + 0.35) +
                          0.14 * sin(3.0 * ph + 0.90) + 0.07 * sin(4.0 * ph + 1.45);
            st->buf[i] = white * noise_lvl + (float)harm * 0.62f;
        }

        /* Pluck-position comb filter (~15% from bridge) */
        int pick_off = (int)(period * (0.14 + 0.012 * (double)s));
        if (pick_off < 2) pick_off = 2;
        float prev_pick = st->buf[period - 1];
        for (int i = period - 1; i >= 0; i--) {
            int di = (i - pick_off + period) % period;
            st->buf[i] = 0.65f * st->buf[i] - 0.35f * st->buf[di];
        }
        st->buf[0] = 0.5f * (st->buf[0] + prev_pick);

        /* Warm circular smoothing */
        int smooth_passes = (cmd->timbre == TIMBRE_NYLON) ? 3 : ((s <= 2) ? 2 : 1);
        for (int p = 0; p < smooth_passes; p++) {
            float prev = st->buf[period - 1];
            for (int i = 0; i < period; i++) {
                float cur = st->buf[i];
                st->buf[i] = 0.62f * cur + 0.38f * prev;
                prev = cur;
            }
        }

        /* Explicit zero-mean DC-offset removal */
        double sum = 0.0;
        for (int i = 0; i < period; i++) sum += st->buf[i];
        float mean = (float)(sum / (double)period);
        float s_gain = 0.92f - 0.03f * (float)s;
        for (int i = 0; i < period; i++) {
            st->buf[i] = (st->buf[i] - mean) * s_gain;
        }
        st->active = true;
    }
}

static void audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int frames = len / (2 * AUDIO_CHANNELS);
    if (frames <= 0) return;

    init_sf2_in_audio_thread();

    /* Check if the UI thread queued a new strum command */
    uint32_t w_seq = g_cmd_write_seq;
    if (w_seq != g_cmd_read_seq) {
        __sync_synchronize();
        StrumCommand cmd = g_pending_cmd;
        g_cmd_read_seq = w_seq;
        init_strum_in_audio_thread(&cmd);
    }

    GuitarTimbre cur_timbre = g_audio_timbre;

    if (rg35_sf2_ready(&g_sf2)) {
        /* Sub-block sample-accurate SF2 rendering + optional stereo chorus + room reverb */
        float raw_stereo[1024 * 2];
        int pos = 0;
        while (pos < frames) {
            int rem = frames - pos;
            if (rem > 1024) rem = 1024;

            /* Find earliest pending strum or gate expiration */
            int next_ev = rem;
            for (int s = 0; s < 6; s++) {
                if (g_sf2_slots[s].pending && g_sf2_slots[s].delay_samples < next_ev) {
                    next_ev = g_sf2_slots[s].delay_samples;
                }
                if (g_sf2_slots[s].sounding && g_sf2_slots[s].gate_samples < next_ev) {
                    next_ev = g_sf2_slots[s].gate_samples;
                }
            }
            if (next_ev <= 0) {
                for (int s = 0; s < 6; s++) {
                    if (g_sf2_slots[s].pending && g_sf2_slots[s].delay_samples <= 0) {
                        g_sf2_slots[s].pending = false;
                        g_sf2_slots[s].sounding = true;
                        g_sf2_slots[s].active_midi = g_sf2_slots[s].midi_note;
                        rg35_sf2_note_on(&g_sf2, s, g_sf2_slots[s].midi_note, g_sf2_slots[s].vel);
                    }
                    if (g_sf2_slots[s].sounding && g_sf2_slots[s].gate_samples <= 0) {
                        g_sf2_slots[s].sounding = false;
                        rg35_sf2_note_off(&g_sf2, s, g_sf2_slots[s].active_midi);
                    }
                }
                next_ev = 1;
            }
            int chunk = (next_ev < rem) ? next_ev : rem;
            for (int s = 0; s < 6; s++) {
                if (g_sf2_slots[s].pending)  g_sf2_slots[s].delay_samples -= chunk;
                if (g_sf2_slots[s].sounding) g_sf2_slots[s].gate_samples  -= chunk;
            }

            tsf_render_float(g_sf2.synth, raw_stereo, chunk, 0);
            for (int i = 0; i < chunk; i++) {
                float in_l = raw_stereo[i * 2];
                float in_r = raw_stereo[i * 2 + 1];

                /* Lush stereo chorus LFO modulation for CHORUS EL timbre */
                if (cur_timbre == TIMBRE_CHORUS) {
                    g_chorus_buf_l[g_chorus_pos] = in_l;
                    g_chorus_buf_r[g_chorus_pos] = in_r;
                    g_chorus_phase += (2.0f * (float)M_PI * 0.78f) / (float)SAMPLE_RATE;
                    if (g_chorus_phase > 2.0f * (float)M_PI) g_chorus_phase -= 2.0f * (float)M_PI;
                    int d_l = (int)(320.0f + 95.0f * sinf(g_chorus_phase));
                    int d_r = (int)(350.0f + 95.0f * cosf(g_chorus_phase));
                    in_l = in_l * 0.76f + 0.34f * g_chorus_buf_l[(g_chorus_pos - d_l + 2048) & 2047];
                    in_r = in_r * 0.76f + 0.34f * g_chorus_buf_r[(g_chorus_pos - d_r + 2048) & 2047];
                    g_chorus_pos = (g_chorus_pos + 1) & 2047;
                }

                float ol, or_;
                rg35_sf2_fx_sample(&g_sf2, in_l, in_r, &ol, &or_);
                int32_t sl = (int32_t)(ol * 31500.0f);
                int32_t sr = (int32_t)(or_ * 31500.0f);
                if (sl >  32767) sl =  32767; else if (sl < -32767) sl = -32767;
                if (sr >  32767) sr =  32767; else if (sr < -32767) sr = -32767;
                out[(pos + i) * 2 + 0] = (int16_t)sl;
                out[(pos + i) * 2 + 1] = (int16_t)sr;
            }
            pos += chunk;
        }
        return;
    }

    /* Fallback: Upgraded EKS Physical Model + Stereo Room Reverb + DC Blocker */
    for (int f = 0; f < frames; f++) {
        float dry_l = 0.0f;
        float dry_r = 0.0f;
        float mono_sum = 0.0f;

        for (int s = 0; s < 6; s++) {
            EksString *st = &g_eks[s];

            /* Drain 6 ms anti-click crossfade residual */
            if (st->declick_sample != 0.0f) {
                float dc = st->declick_sample;
                dry_l += dc * st->pan_l;
                dry_r += dc * st->pan_r;
                mono_sum += dc;
                st->declick_sample = dc * 0.985f;
                if (fabsf(st->declick_sample) < 1e-6f) st->declick_sample = 0.0f;
            }

            if (!st->active || st->samples_left <= 0) continue;

            if (st->start_delay_samples > 0) {
                st->start_delay_samples--;
                continue;
            }

            int len = st->buf_len;
            int p0  = st->pos;
            int p1  = (p0 + 1 < len) ? (p0 + 1) : 0;
            int p2  = (p1 + 1 < len) ? (p1 + 1) : 0;

            float s0 = st->buf[p0];
            float s1 = st->buf[p1];
            float s2 = st->buf[p2];

            float frac = st->frac_delay;
            float cur_i = s0 + frac * (s1 - s0);
            float nxt_i = s1 + frac * (s2 - s1);
            float lp_out = ((1.0f - st->brightness) * cur_i + st->brightness * nxt_i) * st->damping;

            /* 1st-order all-pass inharmonicity dispersion filter */
            float a = st->disp_coeff;
            float disp_out = a * lp_out + st->disp_prev_in - a * st->disp_prev_out;
            if (fabsf(disp_out) < 1e-9f) disp_out = 0.0f;
            st->disp_prev_in  = lp_out;
            st->disp_prev_out = disp_out;

            st->buf[p0] = disp_out;
            st->pos = p1;

            /* 350 ms raised-cosine tail release */
            float env = 1.0f;
            int left = st->samples_left;
            if (left < EKS_RELEASE_SAMPLES) {
                double prog = (double)left / (double)EKS_RELEASE_SAMPLES;
                env = (float)(0.5 * (1.0 - cos(M_PI * prog)));
            }

            float sample = s0 * env;
            st->last_out = sample;
            dry_l += sample * st->pan_l;
            dry_r += sample * st->pan_r;
            mono_sum += sample;

            st->samples_left = left - 1;
            if (st->samples_left <= 0) {
                st->active = false;
                st->last_out = 0.0f;
            }
        }

        /* 2-band acoustic guitar wooden soundboard resonator (105 Hz + 235 Hz) */
        float air_out = g_body_air_b0 * mono_sum - g_body_air_a1 * g_body_air_y1 - g_body_air_a2 * g_body_air_y2;
        g_body_air_y2 = g_body_air_y1;
        g_body_air_y1 = (fabsf(air_out) < 1e-9f) ? 0.0f : air_out;

        float wood_out = g_body_wood_b0 * mono_sum - g_body_wood_a1 * g_body_wood_y1 - g_body_wood_a2 * g_body_wood_y2;
        g_body_wood_y2 = g_body_wood_y1;
        g_body_wood_y1 = (fabsf(wood_out) < 1e-9f) ? 0.0f : wood_out;

        float body_res = (air_out * 0.13f + wood_out * 0.15f) * (cur_timbre == TIMBRE_CHORUS ? 0.45f : 1.0f);
        float mix_l = dry_l * 0.56f + body_res;
        float mix_r = dry_r * 0.56f + body_res;

        /* Stereo chorus LFO for CHORUS EL timbre */
        if (cur_timbre == TIMBRE_CHORUS) {
            g_chorus_buf_l[g_chorus_pos] = mix_l;
            g_chorus_buf_r[g_chorus_pos] = mix_r;
            g_chorus_phase += (2.0f * (float)M_PI * 0.85f) / (float)SAMPLE_RATE;
            if (g_chorus_phase > 2.0f * (float)M_PI) g_chorus_phase -= 2.0f * (float)M_PI;
            int d_l = (int)(340.0f + 100.0f * sinf(g_chorus_phase));
            int d_r = (int)(380.0f + 100.0f * cosf(g_chorus_phase));
            mix_l += 0.34f * g_chorus_buf_l[(g_chorus_pos - d_l + 2048) & 2047];
            mix_r += 0.34f * g_chorus_buf_r[(g_chorus_pos - d_r + 2048) & 2047];
            g_chorus_pos = (g_chorus_pos + 1) & 2047;
        }

        /* Stereo Schroeder/Freeverb room reverb + 20 Hz DC blocker + soft limiter */
        float ol, or_;
        rg35_sf2_fx_sample(&g_sf2, mix_l, mix_r, &ol, &or_);
        int32_t sl = (int32_t)(ol * 31000.0f);
        int32_t sr = (int32_t)(or_ * 31000.0f);
        if (sl >  32767) sl =  32767; else if (sl < -32767) sl = -32767;
        if (sr >  32767) sr =  32767; else if (sr < -32767) sr = -32767;

        out[f * 2 + 0] = (int16_t)sl;
        out[f * 2 + 1] = (int16_t)sr;
    }
}

/* ============================================================================
 * 4. PLATFORM BACKEND: HOST_X11 vs TARGET_RG35XXSP (SDL2 + FBDEV + EVDEV + ALSA)
 * ============================================================================ */

static uint32_t g_prev_buttons = 0;
static uint32_t g_cur_buttons  = 0;

#if defined(HOST_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

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
            pa_ss_t ss = { 3 /* PA_SAMPLE_S16LE */, SAMPLE_RATE, AUDIO_CHANNELS };
            int err = 0;
            void *pa = fn_new(NULL, "RG35XXSP ChordWheel", 1, NULL, "EKS Guitar", &ss, NULL, NULL, &err);
            if (pa) {
                while (g_audio_running) {
                    audio_callback(NULL, (uint8_t *)buf, chunk_bytes);
                    fn_write(pa, buf, (size_t)chunk_bytes, &err);
                }
                fn_free(pa);
                dlclose(pa_lib);
                return NULL;
            }
        }
        dlclose(pa_lib);
    }

    while (g_audio_running) {
        audio_callback(NULL, (uint8_t *)buf, chunk_bytes);
        usleep((512 * 1000000ULL) / SAMPLE_RATE);
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
        DefaultDepth(g_x11_dpy, scr), ZPixmap, 0,
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

static uint32_t translate_x11_keysym(KeySym ks) {
    return rg35xxsp_map_x11_keysym(ks);
}

static uint32_t xkey_to_btn(XKeyEvent *xkey) {
    char buf[8] = {0};
    KeySym sym = NoSymbol;
    XLookupString(xkey, buf, sizeof(buf) - 1, &sym, NULL);
    if (sym == NoSymbol) sym = XLookupKeysym(xkey, 0);
    uint32_t mask = translate_x11_keysym(sym);
    if (!mask) {
        KeySym base = XLookupKeysym(xkey, 0);
        if (base != NoSymbol) mask = translate_x11_keysym(base);
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
    rg35_sf2_free(&g_sf2);
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

#else /* TARGET_RG35XXSP (Allwinner H700 aarch64: SDL2 dlopen + /dev/fb0 + evdev + ALSA) */

#include <linux/fb.h>
#include <linux/input.h>
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
                fn_params(pcm, 2 /* S16_LE */, 3 /* RW_INTERLEAVED */, AUDIO_CHANNELS, SAMPLE_RATE, 1, 25000);
            }
        }
    }

    while (g_alsa_running) {
        audio_callback(NULL, (uint8_t *)buf, (int)sizeof(buf));
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

static bool platform_init(const char *title) {
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
                    desired.freq     = SAMPLE_RATE;
                    desired.format   = SDL2_AUDIO_S16LSB;
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

    if (!g_use_sdl2_video && !init_fbdev_fallback()) return false;
    if (!g_use_sdl2_audio) {
        g_alsa_running = true;
        pthread_create(&g_alsa_thread, NULL, alsa_audio_worker, NULL);
    }
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

static uint32_t translate_rg35xxsp_evkey(uint16_t code) {
    return rg35xxsp_map_evkey((int)code);
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
    if (g_alsa_running) {
        g_alsa_running = false;
        pthread_join(g_alsa_thread, NULL);
    }
    for (int i = 0; i < g_num_evdevs; i++) {
        if (g_evdevs[i].fd >= 0) close(g_evdevs[i].fd);
    }
    g_num_evdevs = 0;

    if (g_use_sdl2_audio && fn_SDL_CloseAudio) {
        fn_SDL_CloseAudio();
        g_use_sdl2_audio = false;
    }
    rg35_sf2_free(&g_sf2);
    if (g_use_sdl2_video) {
        if (g_sdl2_tex && fn_SDL_DestroyTexture)   fn_SDL_DestroyTexture(g_sdl2_tex);
        if (g_sdl2_ren && fn_SDL_DestroyRenderer)  fn_SDL_DestroyRenderer(g_sdl2_ren);
        if (g_sdl2_win && fn_SDL_DestroyWindow)    fn_SDL_DestroyWindow(g_sdl2_win);
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
 * 5. NATIVE 640x480 ANTI-ALIASED DRAWING & TEXT PRIMITIVES
 * ============================================================================ */

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

static void draw_border(int x, int y, int w, int h, int thickness, uint32_t color) {
    fill_rect(x, y, w, thickness, color);
    fill_rect(x, y + h - thickness, w, thickness, color);
    fill_rect(x, y, thickness, h, color);
    fill_rect(x + w - thickness, y, thickness, h, color);
}

static void draw_pill(int x, int y, int w, int h, uint32_t fill_col, uint32_t border_col) {
    if (w < 6 || h < 6) return;
    fill_rect(x + 2, y + 2, w - 4, h - 4, fill_col);
    fill_rect(x + 2, y, w - 4, 2, border_col);
    fill_rect(x + 2, y + h - 2, w - 4, 2, border_col);
    fill_rect(x, y + 2, 2, h - 4, border_col);
    fill_rect(x + w - 2, y + 2, 2, h - 4, border_col);
}

static void draw_filled_circle_aa(float cx, float cy, float radius, uint32_t fill_col, uint32_t border_col) {
    int x0 = (int)(cx - radius - 2.0f);
    int x1 = (int)(cx + radius + 2.0f);
    int y0 = (int)(cy - radius - 2.0f);
    int y1 = (int)(cy + radius + 2.0f);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float d  = sqrtf(dx * dx + dy * dy);
            if (d <= radius - 1.4f) {
                blend_pixel(x, y, fill_col, 255);
            } else if (d <= radius) {
                float t = (radius - d) / 1.4f;
                blend_pixel(x, y, fill_col, (uint8_t)(t * 255.0f));
                blend_pixel(x, y, border_col, (uint8_t)((1.0f - t * 0.4f) * 235.0f));
            } else if (d <= radius + 1.2f) {
                float a = (radius + 1.2f - d) / 1.2f;
                blend_pixel(x, y, border_col, (uint8_t)(a * 215.0f));
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
 * 6. APPLICATION STATE, CHORD RESOLUTION, & PERSISTENT CONFIG
 * ============================================================================ */

typedef struct {
    int  sector;
    int  ring;
    bool use_7th;
} ProgSlot;

static int          g_key_sector    = 0;
static int          g_cur_sector    = 0;
static int          g_cur_ring      = 0;
static bool         g_key_locked    = false;
static bool         g_seventh_mode  = false;
static int          g_voicing_idx   = 0;
static GuitarTimbre g_timbre        = TIMBRE_ACOUSTIC;
static bool         g_show_help     = false;

static ProgSlot     g_prog[4];
static int          g_prog_write_idx = 0;
static bool         g_loop_playing   = false;
static int          g_loop_step      = 0;
static uint32_t     g_last_loop_ms   = 0;
static int          g_preset_idx     = 0;

static char         g_config_path[512] = "";

typedef struct {
    const char *name;
    int         sector_offsets[4];
    int         rings[4];
} ProgPreset;

static const ProgPreset k_presets[5] = {
    { "I - V - vi - IV (Pop/Rock)",      {  0,  1,  0, 11 }, { 0, 0, 1, 0 } },
    { "I - IV - V - I (Blues/Folk)",     {  0, 11,  1,  0 }, { 0, 0, 0, 0 } },
    { "ii - V - I - vi (Jazz Turn)",     { 11,  1,  0,  0 }, { 1, 0, 0, 1 } },
    { "I - vi - IV - V (50s Classic)",   {  0,  0, 11,  1 }, { 0, 1, 0, 0 } },
    { "vi - IV - I - V (Minor Anthem)",  {  0, 11,  0,  1 }, { 1, 0, 0, 0 } }
};

static void apply_progression_preset(int preset_id) {
    preset_id = ((preset_id % 5) + 5) % 5;
    g_preset_idx = preset_id;
    for (int i = 0; i < 4; i++) {
        g_prog[i].sector  = (g_key_sector + k_presets[preset_id].sector_offsets[i]) % 12;
        g_prog[i].ring    = k_presets[preset_id].rings[i];
        g_prog[i].use_7th = g_seventh_mode;
    }
    g_prog_write_idx = 0;
}

typedef struct {
    int          root_pc;
    ChordQuality qual;
    char         chord_name[24];
    char         roman_num[16];
    char         func_desc[32];
    char         notes_str[40];
    int          diatonic_family;
} ResolvedChord;

static void resolve_chord_at(int key_sec, int sec, int ring, bool seventh, ResolvedChord *out) {
    key_sec = ((key_sec % 12) + 12) % 12;
    sec     = ((sec % 12) + 12) % 12;
    int rel = (sec - key_sec + 12) % 12;
    bool flats = k_wheel[key_sec].use_flats;
    const char **note_tbl = flats ? k_note_flat : k_note_sharp;

    memset(out, 0, sizeof(*out));

    if (ring == 2) {
        out->root_pc = k_wheel[key_sec].dim_pc;
        out->qual    = seventh ? QUAL_M7B5 : QUAL_DIM;
        snprintf(out->chord_name, sizeof(out->chord_name), "%s%s",
                 k_wheel[key_sec].dim_root_name, seventh ? "m7b5" : "dim");
        snprintf(out->roman_num, sizeof(out->roman_num), "%s", seventh ? "viim7b5" : "viidim");
        snprintf(out->func_desc, sizeof(out->func_desc), "Leading-Tone Diminished");
        out->diatonic_family = 3;
    } else if (ring == 0) {
        out->root_pc = k_wheel[sec].maj_pc;
        if (rel == 0) {
            out->qual = seventh ? QUAL_MAJ7 : QUAL_MAJ;
            snprintf(out->roman_num, sizeof(out->roman_num), "%s", seventh ? "Imaj7" : "I");
            snprintf(out->func_desc, sizeof(out->func_desc), "Tonic Major Center");
            out->diatonic_family = 1;
        } else if (rel == 11) {
            out->qual = seventh ? QUAL_MAJ7 : QUAL_MAJ;
            snprintf(out->roman_num, sizeof(out->roman_num), "%s", seventh ? "IVmaj7" : "IV");
            snprintf(out->func_desc, sizeof(out->func_desc), "Subdominant Major");
            out->diatonic_family = 2;
        } else if (rel == 1) {
            out->qual = seventh ? QUAL_DOM7 : QUAL_MAJ;
            snprintf(out->roman_num, sizeof(out->roman_num), "%s", seventh ? "V7" : "V");
            snprintf(out->func_desc, sizeof(out->func_desc), "Dominant Resolution");
            out->diatonic_family = 3;
        } else {
            out->qual = seventh ? QUAL_DOM7 : QUAL_MAJ;
            out->diatonic_family = 0;
            static const char *k_maj_romans[12] = {
                "I", "V", "V/V", "V/vi", "V/ii", "V/iii", "bV", "bII", "bVI", "bIII", "bVII", "IV"
            };
            static const char *k_maj_funcs[12] = {
                "Tonic", "Dominant", "Secondary Dom (II)", "Secondary Dom (VI)", "Secondary Dom (III)",
                "Secondary Dom (VII)", "Tritone Sub", "Neapolitan bII", "Borrowed bVI",
                "Borrowed bIII", "Borrowed bVII", "Subdominant"
            };
            snprintf(out->roman_num, sizeof(out->roman_num), "%s%s",
                      k_maj_romans[rel], seventh ? "7" : "");
            snprintf(out->func_desc, sizeof(out->func_desc), "%s", k_maj_funcs[rel]);
        }
        const char *suffix = (out->qual == QUAL_MAJ7) ? "maj7" :
                             (out->qual == QUAL_DOM7) ? "7"    : "";
        snprintf(out->chord_name, sizeof(out->chord_name), "%s%s", k_wheel[sec].maj_name, suffix);
    } else {
        out->root_pc = k_wheel[sec].min_pc;
        out->qual    = seventh ? QUAL_MIN7 : QUAL_MIN;
        if (rel == 0) {
            snprintf(out->roman_num, sizeof(out->roman_num), "%s", seventh ? "vi7" : "vi");
            snprintf(out->func_desc, sizeof(out->func_desc), "Relative Minor Tonic");
            out->diatonic_family = 1;
        } else if (rel == 11) {
            snprintf(out->roman_num, sizeof(out->roman_num), "%s", seventh ? "ii7" : "ii");
            snprintf(out->func_desc, sizeof(out->func_desc), "Supertonic Pre-Dom");
            out->diatonic_family = 2;
        } else if (rel == 1) {
            snprintf(out->roman_num, sizeof(out->roman_num), "%s", seventh ? "iii7" : "iii");
            snprintf(out->func_desc, sizeof(out->func_desc), "Mediant Minor");
            out->diatonic_family = 3;
        } else {
            out->diatonic_family = 0;
            static const char *k_min_romans[12] = {
                "vi", "iii", "vii", "#iv", "#i", "#v", "biii", "bvii", "iv", "i", "v", "ii"
            };
            static const char *k_min_funcs[12] = {
                "Relative Min", "Mediant Min", "Min v/III", "Chromatic Min", "Chromatic Min",
                "Chromatic Min", "Chromatic Min", "Borrowed bvii", "Borrowed iv",
                "Parallel Min (i)", "Modal Min (v)", "Supertonic"
            };
            snprintf(out->roman_num, sizeof(out->roman_num), "%s%s",
                      k_min_romans[rel], seventh ? "7" : "");
            snprintf(out->func_desc, sizeof(out->func_desc), "%s", k_min_funcs[rel]);
        }
        snprintf(out->chord_name, sizeof(out->chord_name), "%s%s",
                 k_wheel[sec].min_name, seventh ? "7" : "");
    }

    int i3 = (out->qual == QUAL_MAJ || out->qual == QUAL_MAJ7 || out->qual == QUAL_DOM7) ? 4 : 3;
    int i5 = (out->qual == QUAL_DIM || out->qual == QUAL_M7B5) ? 6 : 7;
    int n1 = out->root_pc;
    int n2 = (out->root_pc + i3) % 12;
    int n3 = (out->root_pc + i5) % 12;

    if (out->qual == QUAL_MAJ7 || out->qual == QUAL_MIN7 || out->qual == QUAL_DOM7 || out->qual == QUAL_M7B5) {
        int i7 = (out->qual == QUAL_MAJ7) ? 11 : 10;
        int n4 = (out->root_pc + i7) % 12;
        snprintf(out->notes_str, sizeof(out->notes_str), "%s - %s - %s - %s",
                 note_tbl[n1], note_tbl[n2], note_tbl[n3], note_tbl[n4]);
    } else {
        snprintf(out->notes_str, sizeof(out->notes_str), "%s - %s - %s",
                 note_tbl[n1], note_tbl[n2], note_tbl[n3]);
    }
}

static void strum_current_selection(void) {
    ResolvedChord rc;
    resolve_chord_at(g_key_sector, g_cur_sector, g_cur_ring, g_seventh_mode, &rc);
    GuitarVoicing gv;
    build_guitar_voicing(rc.root_pc, rc.qual, g_voicing_idx, &gv);
    post_strum_command(&gv, g_timbre);
}

static void resolve_config_path(void) {
    const char *dirs[] = {
        "/userdata/roms/ports/chordwheel",
        "/userdata/system/configs",
        "/userdata",
        NULL
    };
    struct stat st;
    for (int i = 0; dirs[i]; i++) {
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && access(dirs[i], W_OK) == 0) {
            snprintf(g_config_path, sizeof(g_config_path), "%s/.chordwheel.cfg", dirs[i]);
            return;
        }
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "%s/.chordwheel.cfg", home);
    } else {
        snprintf(g_config_path, sizeof(g_config_path), "./.chordwheel.cfg");
    }
}

static void save_config(void) {
    if (g_config_path[0] == '\0') return;
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "key_sector=%d\n", g_key_sector);
    fprintf(f, "cur_sector=%d\n", g_cur_sector);
    fprintf(f, "cur_ring=%d\n", g_cur_ring);
    fprintf(f, "key_locked=%d\n", g_key_locked ? 1 : 0);
    fprintf(f, "seventh_mode=%d\n", g_seventh_mode ? 1 : 0);
    fprintf(f, "voicing_idx=%d\n", g_voicing_idx);
    fprintf(f, "timbre=%d\n", (int)g_timbre);
    fprintf(f, "preset_idx=%d\n", g_preset_idx);
    for (int i = 0; i < 4; i++) {
        fprintf(f, "prog%d=%d,%d,%d\n", i,
                g_prog[i].sector, g_prog[i].ring, g_prog[i].use_7th ? 1 : 0);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void load_config(void) {
    apply_progression_preset(0);
    if (g_config_path[0] == '\0') return;
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v = 0, s = 0, r = 0, u7 = 0, idx = 0;
        if (sscanf(line, "key_sector=%d", &v) == 1 && v >= 0 && v < 12) {
            g_key_sector = v;
        } else if (sscanf(line, "cur_sector=%d", &v) == 1 && v >= 0 && v < 12) {
            g_cur_sector = v;
        } else if (sscanf(line, "cur_ring=%d", &v) == 1 && v >= 0 && v <= 2) {
            g_cur_ring = v;
        } else if (sscanf(line, "key_locked=%d", &v) == 1) {
            g_key_locked = (v != 0);
        } else if (sscanf(line, "seventh_mode=%d", &v) == 1) {
            g_seventh_mode = (v != 0);
        } else if (sscanf(line, "voicing_idx=%d", &v) == 1 && v >= 0 && v < 3) {
            g_voicing_idx = v;
        } else if (sscanf(line, "timbre=%d", &v) == 1 && v >= 0 && v < 3) {
            g_timbre = (GuitarTimbre)v;
        } else if (sscanf(line, "preset_idx=%d", &v) == 1 && v >= 0 && v < 5) {
            g_preset_idx = v;
        } else if (sscanf(line, "prog%d=%d,%d,%d", &idx, &s, &r, &u7) == 4 &&
                   idx >= 0 && idx < 4 && s >= 0 && s < 12 && r >= 0 && r <= 2) {
            g_prog[idx].sector  = s;
            g_prog[idx].ring    = r;
            g_prog[idx].use_7th = (u7 != 0);
        }
    }
    fclose(f);
}

/* ============================================================================
 * 7. NATIVE 640x480 RENDERING (WHEEL, ROSEWOOD FRETBOARD, SCOPE & LOOPER)
 * ============================================================================ */

#define WHEEL_CX    174.0f
#define WHEEL_CY    224.0f
#define R_OUTER_MAX 166.0f
#define R_OUTER_MIN 112.0f
#define R_INNER_MAX 109.0f
#define R_INNER_MIN  63.0f
#define R_HUB_MAX    60.0f

#define WHEEL_LUT_X0 6
#define WHEEL_LUT_Y0 56
#define WHEEL_LUT_W  338
#define WHEEL_LUT_H  338

/* Precomputed 2x2 supersampled wheel LUT (eliminates ~450k trig calls/frame) */
static uint8_t g_wheel_lut[WHEEL_LUT_H][WHEEL_LUT_W][4];
static bool    g_wheel_lut_ready = false;

static void init_wheel_lut(void) {
    if (g_wheel_lut_ready) return;
    for (int ly = 0; ly < WHEEL_LUT_H; ly++) {
        int py = WHEEL_LUT_Y0 + ly;
        for (int lx = 0; lx < WHEEL_LUT_W; lx++) {
            int px = WHEEL_LUT_X0 + lx;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    float fx = (float)px + (sx ? 0.75f : 0.25f) - WHEEL_CX;
                    float fy = (float)py + (sy ? 0.75f : 0.25f) - WHEEL_CY;
                    float r  = sqrtf(fx * fx + fy * fy);
                    uint8_t zone = 0; /* 0 = bg */
                    uint8_t sec  = 0;
                    if (r <= R_OUTER_MAX) {
                        float deg = atan2f(fy, fx) * (180.0f / (float)M_PI) + 90.0f + 15.0f;
                        while (deg < 0.0f)    deg += 360.0f;
                        while (deg >= 360.0f) deg -= 360.0f;
                        sec = (uint8_t)(((int)(deg / 30.0f)) % 12);
                        float sec_pos = fmodf(deg, 30.0f);

                        if (r >= R_OUTER_MIN && r <= R_OUTER_MAX) {
                            bool on_radial = (sec_pos < 1.2f || sec_pos > 28.8f);
                            bool on_ring   = (r < R_OUTER_MIN + 2.4f || r > R_OUTER_MAX - 2.4f);
                            if (sec_pos < 0.6f || sec_pos > 29.4f) {
                                zone = 4; /* outer radial sep (sel_bdr if sel, else sep_col) */
                            } else if (on_radial || on_ring) {
                                zone = 5; /* outer sel border (sel_bdr if sel, else outer_fill) */
                            } else {
                                zone = 3; /* outer fill */
                            }
                        } else if (r >= R_INNER_MIN && r <= R_INNER_MAX) {
                            bool on_radial = (sec_pos < 1.6f || sec_pos > 28.4f);
                            bool on_ring   = (r < R_INNER_MIN + 2.4f || r > R_INNER_MAX - 2.4f);
                            if (sec_pos < 0.8f || sec_pos > 29.2f) {
                                zone = 7; /* inner radial sep */
                            } else if (on_radial || on_ring) {
                                zone = 8; /* inner sel border */
                            } else {
                                zone = 6; /* inner fill */
                            }
                        } else if (r <= R_HUB_MAX) {
                            zone = (r > R_HUB_MAX - 2.6f) ? 9 : 2; /* 9 = hub rim, 2 = hub fill */
                        } else {
                            zone = 1; /* ring separator */
                        }
                    }
                    g_wheel_lut[ly][lx][sy * 2 + sx] = (uint8_t)((zone << 4) | (sec & 0x0Fu));
                }
            }
        }
    }
    g_wheel_lut_ready = true;
}

static void render_circle_of_fifths_wheel(void) {
    init_wheel_lut();
    uint32_t outer_fill[12], inner_fill[12];
    for (int s = 0; s < 12; s++) {
        int rel = (s - g_key_sector + 12) % 12;
        bool sel_outer = (g_cur_ring == 0 && g_cur_sector == s);
        bool sel_inner = (g_cur_ring == 1 && g_cur_sector == s);

        if (rel == 0) {
            outer_fill[s] = sel_outer ? RGB(245, 158, 11) : RGB(180, 112, 12);
        } else if (rel == 11) {
            outer_fill[s] = sel_outer ? RGB(37, 99, 235)  : RGB(29, 78, 178);
        } else if (rel == 1) {
            outer_fill[s] = sel_outer ? RGB(16, 185, 129) : RGB(14, 132, 92);
        } else {
            outer_fill[s] = sel_outer ? RGB(71, 85, 105)  : RGB(26, 35, 52);
        }

        if (rel == 0) {
            inner_fill[s] = sel_inner ? RGB(217, 119, 6)  : RGB(138, 78, 14);
        } else if (rel == 11) {
            inner_fill[s] = sel_inner ? RGB(59, 130, 246) : RGB(30, 64, 142);
        } else if (rel == 1) {
            inner_fill[s] = sel_inner ? RGB(20, 184, 166) : RGB(15, 110, 96);
        } else {
            inner_fill[s] = sel_inner ? RGB(71, 85, 105)  : RGB(21, 29, 44);
        }
    }

    uint32_t hub_fill = (g_cur_ring == 2) ? RGB(190, 24, 93) : RGB(30, 41, 59);
    uint32_t hub_rim  = (g_cur_ring == 2) ? RGB(254, 240, 138) : RGB(96, 165, 250);
    uint32_t bg_col   = RGB(13, 18, 28);
    uint32_t sep_col  = RGB(11, 15, 24);
    uint32_t sel_bdr  = RGB(254, 240, 138);

    for (int ly = 0; ly < WHEEL_LUT_H; ly++) {
        int py = WHEEL_LUT_Y0 + ly;
        if (py < 0 || py >= NATIVE_H) continue;
        uint32_t *fb_row = &g_fb[py * NATIVE_W];
        for (int lx = 0; lx < WHEEL_LUT_W; lx++) {
            int px = WHEEL_LUT_X0 + lx;
            if (px < 0 || px >= 346) continue;

            const uint8_t *cell = g_wheel_lut[ly][lx];
            uint32_t r_acc = 0, g_acc = 0, b_acc = 0;
            for (int k = 0; k < 4; k++) {
                uint8_t code = cell[k];
                uint8_t z    = code >> 4;
                uint8_t sec  = code & 0x0Fu;
                uint32_t sc  = bg_col;
                switch (z) {
                    case 0: sc = bg_col; break;
                    case 1: sc = sep_col; break;
                    case 2: sc = hub_fill; break;
                    case 9: sc = hub_rim; break;
                    case 3: sc = outer_fill[sec]; break;
                    case 4: sc = (g_cur_ring == 0 && g_cur_sector == sec) ? sel_bdr : sep_col; break;
                    case 5: sc = (g_cur_ring == 0 && g_cur_sector == sec) ? sel_bdr : outer_fill[sec]; break;
                    case 6: sc = inner_fill[sec]; break;
                    case 7: sc = (g_cur_ring == 1 && g_cur_sector == sec) ? sel_bdr : sep_col; break;
                    case 8: sc = (g_cur_ring == 1 && g_cur_sector == sec) ? sel_bdr : inner_fill[sec]; break;
                }
                r_acc += (sc >> 16) & 0xFF;
                g_acc += (sc >> 8)  & 0xFF;
                b_acc += sc & 0xFF;
            }
            fb_row[px] = RGB(r_acc >> 2, g_acc >> 2, b_acc >> 2);
        }
    }

    /* Draw chord labels + diatonic Roman numeral sub-labels on all 12 sectors */
    float r_outer_lbl = (R_OUTER_MIN + R_OUTER_MAX) * 0.5f;
    float r_inner_lbl = (R_INNER_MIN + R_INNER_MAX) * 0.5f;

    for (int s = 0; s < 12; s++) {
        float ang   = ((float)s * 30.0f - 90.0f) * ((float)M_PI / 180.0f);
        float cos_a = cosf(ang);
        float sin_a = sinf(ang);
        int rel     = (s - g_key_sector + 12) % 12;
        bool in_key = (rel == 0 || rel == 1 || rel == 11);

        int ox = (int)(WHEEL_CX + cos_a * r_outer_lbl + 0.5f);
        int oy = (int)(WHEEL_CY + sin_a * r_outer_lbl + 0.5f);
        bool sel_o = (g_cur_ring == 0 && g_cur_sector == s);
        uint32_t o_col = sel_o ? RGB(255, 255, 255) :
                         (in_key ? RGB(255, 251, 235) : RGB(148, 163, 184));

        if (in_key) {
            const char *rom = (rel == 0) ? "I" : (rel == 11) ? "IV" : "V";
            draw_text_centered(FONT_UI_16, ox - 24, oy - 16, 48, 18, k_wheel[s].maj_name, o_col);
            draw_text_centered(FONT_UI_11, ox - 20, oy + 2,  40, 12, rom, RGB(254, 240, 138));
        } else {
            draw_text_centered(FONT_UI_16, ox - 24, oy - 10, 48, 20, k_wheel[s].maj_name, o_col);
        }

        int ix = (int)(WHEEL_CX + cos_a * r_inner_lbl + 0.5f);
        int iy = (int)(WHEEL_CY + sin_a * r_inner_lbl + 0.5f);
        bool sel_i = (g_cur_ring == 1 && g_cur_sector == s);
        uint32_t i_col = sel_i ? RGB(255, 255, 255) :
                         (in_key ? RGB(241, 245, 249) : RGB(120, 136, 158));

        if (in_key) {
            const char *mrom = (rel == 0) ? "vi" : (rel == 11) ? "ii" : "iii";
            draw_text_centered(FONT_UI_13, ix - 22, iy - 13, 44, 15, k_wheel[s].min_name, i_col);
            draw_text_centered(FONT_UI_11, ix - 18, iy + 2,  36, 12, mrom, RGB(186, 230, 253));
        } else {
            draw_text_centered(FONT_UI_13, ix - 22, iy - 8,  44, 16, k_wheel[s].min_name, i_col);
        }
    }

    /* Center Hub Text: Key Name + Key Signature + vii° Diminished Chord */
    char key_hub[24];
    snprintf(key_hub, sizeof(key_hub), "KEY %s", k_wheel[g_key_sector].maj_name);
    draw_text_centered(FONT_UI_16, (int)WHEEL_CX - 48, (int)WHEEL_CY - 34, 96, 20,
                       key_hub, RGB(250, 204, 21));
    draw_text_centered(FONT_UI_13, (int)WHEEL_CX - 48, (int)WHEEL_CY - 10, 96, 16,
                       k_wheel[g_key_sector].key_sig, RGB(186, 230, 253));

    char dim_lbl[24];
    snprintf(dim_lbl, sizeof(dim_lbl), "vii: %s%s",
             k_wheel[g_key_sector].dim_root_name, g_seventh_mode ? "m7b5" : "dim");
    draw_text_centered(FONT_UI_13, (int)WHEEL_CX - 48, (int)WHEEL_CY + 12, 96, 16,
                       dim_lbl, (g_cur_ring == 2) ? RGB(254, 240, 138) : RGB(244, 114, 182));

    /* Top-left Header Badges */
    draw_pill(8, 8, 196, 30, RGB(23, 33, 52), RGB(56, 189, 248));
    draw_text_centered(FONT_UI_13, 8, 8, 196, 30, "CIRCLE OF FIFTHS", RGB(186, 230, 253));

    uint32_t lock_fill = g_key_locked ? RGB(124, 45, 18) : RGB(20, 83, 45);
    uint32_t lock_bdr  = g_key_locked ? RGB(251, 146, 60) : RGB(74, 222, 128);
    draw_pill(210, 8, 132, 30, lock_fill, lock_bdr);
    draw_text_centered(FONT_UI_13, 210, 8, 132, 30,
                       g_key_locked ? "SEL: LOCKED" : "SEL: ROTATE",
                       RGB(255, 255, 255));

    /* Diatonic Color Legend beneath the wheel (y = 400..428) */
    draw_pill(8, 400, 106, 28, RGB(146, 92, 12), RGB(250, 204, 21));
    draw_text_centered(FONT_UI_13, 8, 400, 106, 28, "I / vi (Tonic)", RGB(255, 255, 255));

    draw_pill(120, 400, 108, 28, RGB(29, 78, 178), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, 120, 400, 108, 28, "IV / ii (Sub)", RGB(255, 255, 255));

    draw_pill(234, 400, 108, 28, RGB(14, 132, 92), RGB(52, 211, 153));
    draw_text_centered(FONT_UI_13, 234, 400, 108, 28, "V / iii (Dom)", RGB(255, 255, 255));
}

static const char *interval_label_for_semitones(int semi) {
    static const char *k_int_lbl[12] = {
        "R", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7"
    };
    return k_int_lbl[((semi % 12) + 12) % 12];
}

static void render_guitar_inspector_pane(void) {
    int px = 350, pw = NATIVE_W - px - 6; /* width = 284 */

    ResolvedChord rc;
    resolve_chord_at(g_key_sector, g_cur_sector, g_cur_ring, g_seventh_mode, &rc);

    GuitarVoicing gv;
    build_guitar_voicing(rc.root_pc, rc.qual, g_voicing_idx, &gv);

    bool flats = k_wheel[g_key_sector].use_flats;
    const char **note_tbl = flats ? k_note_flat : k_note_sharp;

    /* 1. Selected Chord & Roman Function Card (y = 8..108) */
    fill_rect(px, 8, pw, 100, RGB(21, 29, 45));
    draw_border(px, 8, pw, 100, 2, RGB(51, 68, 98));

    draw_text(FONT_TITLE_28, px + 12, 14, rc.chord_name, RGB(250, 204, 21));

    int cname_w = text_width(FONT_TITLE_28, rc.chord_name);
    uint32_t fam_fill = (rc.diatonic_family == 1) ? RGB(146, 92, 12) :
                        (rc.diatonic_family == 2) ? RGB(29, 78, 178) :
                        (rc.diatonic_family == 3) ? RGB(14, 132, 92) : RGB(68, 64, 92);
    uint32_t fam_bdr  = (rc.diatonic_family == 1) ? RGB(250, 204, 21) :
                        (rc.diatonic_family == 2) ? RGB(96, 165, 250) :
                        (rc.diatonic_family == 3) ? RGB(52, 211, 153) : RGB(167, 139, 250);
    int rpill_w = text_width(FONT_UI_13, rc.roman_num) + 18;
    draw_pill(px + 18 + cname_w, 18, rpill_w, 26, fam_fill, fam_bdr);
    draw_text_centered(FONT_UI_13, px + 18 + cname_w, 18, rpill_w, 26, rc.roman_num, RGB(255, 255, 255));

    draw_pill(px + pw - 74, 14, 66, 26,
              g_seventh_mode ? RGB(109, 40, 217) : RGB(30, 58, 95),
              g_seventh_mode ? RGB(192, 132, 252) : RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, px + pw - 74, 14, 66, 26,
                       g_seventh_mode ? "B: 7TH" : "B: TRI", RGB(255, 255, 255));

    draw_text(FONT_UI_13, px + 12, 52, rc.func_desc, RGB(148, 163, 184));
    draw_text(FONT_UI_16, px + 12, 76, rc.notes_str, RGB(226, 232, 240));

    /* Timbre pill on right side of chord card */
    draw_pill(px + pw - 104, 72, 96, 26, RGB(24, 52, 68), RGB(56, 189, 248));
    draw_text_centered(FONT_UI_11, px + pw - 104, 72, 96, 26,
                       k_timbre_names[g_timbre], RGB(186, 230, 253));

    /* 2. 6-String Rosewood Fretboard Card (y = 114..340) */
    int fy = 114, fh = 226;
    fill_rect(px, fy, pw, fh, RGB(21, 29, 45));
    draw_border(px, fy, pw, fh, 2, RGB(51, 68, 98));

    draw_text(FONT_UI_13, px + 12, fy + 8, gv.label, RGB(125, 211, 252));
    draw_text_right(FONT_UI_13, px + pw - 10, fy + 8, "L1/R1: Voicing", RGB(250, 204, 21));

    int min_fret = 99, max_fret = 0;
    for (int s = 0; s < 6; s++) {
        if (gv.frets[s] > 0) {
            if (gv.frets[s] < min_fret) min_fret = gv.frets[s];
            if (gv.frets[s] > max_fret) max_fret = gv.frets[s];
        }
    }
    int base_fret = 1;
    if (max_fret > 4) {
        base_fret = (min_fret < 99 && min_fret > 1) ? min_fret : (max_fret - 3);
    }

    int str_dx  = 40;            /* 5 string spaces * 40 = 200 px */
    int fret_dy = 34;            /* 4 fret spaces * 34 = 136 px */
    int grid_w  = str_dx * 5;
    int grid_h  = fret_dy * 4;
    int grid_x  = px + (pw - grid_w) / 2 + 6;
    int grid_y  = fy + 50;

    /* Rosewood fretboard background + side binding */
    fill_rect(grid_x - 6, grid_y, grid_w + 13, grid_h + 2, RGB(54, 34, 24));
    fill_rect(grid_x, grid_y, grid_w + 1, grid_h + 1, RGB(38, 25, 20));

    /* Pearl fret inlay dots on frets 3, 5, 7, 9, 12 */
    for (int f = 1; f <= 4; f++) {
        int actual_fret = base_fret + f - 1;
        int cy = grid_y + (f - 1) * fret_dy + fret_dy / 2;
        if (actual_fret == 3 || actual_fret == 5 || actual_fret == 7 || actual_fret == 9) {
            draw_filled_circle_aa((float)(grid_x + grid_w / 2), (float)cy, 4.5f,
                                  RGB(75, 68, 62), RGB(95, 88, 82));
        } else if (actual_fret == 12) {
            draw_filled_circle_aa((float)(grid_x + str_dx * 1.5f), (float)cy, 4.0f,
                                  RGB(75, 68, 62), RGB(95, 88, 82));
            draw_filled_circle_aa((float)(grid_x + str_dx * 3.5f), (float)cy, 4.0f,
                                  RGB(75, 68, 62), RGB(95, 88, 82));
        }
    }

    /* Nut bar or base fret indicator */
    if (base_fret == 1) {
        fill_rect(grid_x - 3, grid_y - 5, grid_w + 7, 6, RGB(241, 245, 249));
    } else {
        char fr_str[12];
        snprintf(fr_str, sizeof(fr_str), "%dfr", base_fret);
        draw_text_right(FONT_UI_13, grid_x - 15, grid_y + 8, fr_str, RGB(250, 204, 21));
        fill_rect(grid_x, grid_y, grid_w + 1, 2, RGB(148, 163, 184));
    }

    /* Nickel fret wires */
    for (int f = 1; f <= 4; f++) {
        fill_rect(grid_x, grid_y + f * fret_dy, grid_w + 1, 2, RGB(130, 142, 160));
    }

    /* 6 guitar strings */
    for (int s = 0; s < 6; s++) {
        int sx = grid_x + s * str_dx;
        uint32_t s_col = (s < 3) ? RGB(203, 213, 225) : RGB(155, 170, 190);
        fill_rect(sx, grid_y, (s < 2) ? 2 : 1, grid_h + 1, s_col);

        if (gv.frets[s] < 0) {
            draw_text_centered(FONT_UI_13, sx - 8, grid_y - 24, 16, 16, "X", RGB(248, 113, 113));
        } else if (gv.frets[s] == 0) {
            draw_text_centered(FONT_UI_13, sx - 8, grid_y - 24, 16, 16, "O", RGB(74, 222, 128));
        }

        int sy_lbl = grid_y + grid_h + 8;
        if (gv.frets[s] >= 0) {
            int pc = (k_string_midi[s] + gv.frets[s]) % 12;
            bool is_root = (pc == rc.root_pc);
            draw_text_centered(FONT_UI_13, sx - 12, sy_lbl, 24, 16,
                               note_tbl[pc], is_root ? RGB(250, 204, 21) : RGB(226, 232, 240));
        } else {
            draw_text_centered(FONT_UI_13, sx - 10, sy_lbl, 20, 16,
                               k_string_names[s], RGB(100, 116, 139));
        }
    }

    /* Check if this voicing uses an index-finger barre across multiple strings at min_fret */
    if (min_fret >= base_fret && min_fret <= base_fret + 3) {
        int first_s = -1, last_s = -1, count_s = 0;
        for (int s = 0; s < 6; s++) {
            if (gv.frets[s] == min_fret) {
                if (first_s < 0) first_s = s;
                last_s = s;
                count_s++;
            }
        }
        if (count_s >= 3 && last_s > first_s) {
            int rel_f = min_fret - base_fret + 1;
            int by = grid_y + (rel_f - 1) * fret_dy + fret_dy / 2 - 5;
            int bx0 = grid_x + first_s * str_dx;
            int bw_bar = (last_s - first_s) * str_dx;
            draw_pill(bx0, by, bw_bar, 10, RGB(14, 116, 170), RGB(125, 211, 252));
        }
    }

    /* Finger dots with interval labels (R, 3, 5, b7, 7) inside each dot */
    for (int s = 0; s < 6; s++) {
        int f = gv.frets[s];
        if (f > 0) {
            int rel_f = f - base_fret + 1;
            if (rel_f >= 1 && rel_f <= 4) {
                float cx = (float)(grid_x + s * str_dx);
                float cy = (float)(grid_y + (rel_f - 1) * fret_dy + fret_dy / 2);
                int pc = (k_string_midi[s] + f) % 12;
                int semi = (pc - rc.root_pc + 12) % 12;
                bool is_root = (semi == 0);
                uint32_t dot_fill = is_root ? RGB(245, 158, 11) : RGB(14, 165, 233);
                uint32_t dot_bdr  = is_root ? RGB(254, 240, 138) : RGB(224, 242, 254);
                draw_filled_circle_aa(cx, cy, 11.5f, dot_fill, dot_bdr);
                draw_text_centered(FONT_UI_11, (int)cx - 10, (int)cy - 7, 20, 14,
                                   interval_label_for_semitones(semi), RGB(15, 23, 42));
            }
        }
    }

    /* 3. Diatonic Chords in Active Key Card (y = 346..430) */
    int dy = 346, dh = 84;
    fill_rect(px, dy, pw, dh, RGB(21, 29, 45));
    draw_border(px, dy, pw, dh, 2, RGB(51, 68, 98));

    char d_hdr[48];
    snprintf(d_hdr, sizeof(d_hdr), "DIATONIC (%s MAJOR)", k_wheel[g_key_sector].maj_name);
    draw_text(FONT_UI_13, px + 10, dy + 6, d_hdr, RGB(148, 163, 184));
    draw_text_right(FONT_UI_13, px + pw - 10, dy + 6, "A: Strum", RGB(74, 222, 128));

    static const int k_deg_off[7]  = {  0, 11,  1, 11,  1,  0,  0 };
    static const int k_deg_ring[7] = {  0,  1,  1,  0,  0,  1,  2 };
    static const char *k_deg_rom[7] = { "I", "ii", "iii", "IV", "V", "vi", "vii" };
    int cell_w = (pw - 14) / 7;
    for (int d = 0; d < 7; d++) {
        int d_sec = (g_key_sector + k_deg_off[d]) % 12;
        int d_rng = k_deg_ring[d];
        bool is_sel = (d_rng == 2) ? (g_cur_ring == 2) :
                      (g_cur_ring == d_rng && g_cur_sector == d_sec);
        int cx = px + 8 + d * cell_w;
        int cy = dy + 28;
        if (is_sel) {
            fill_rect(cx, cy, cell_w - 2, 48, RGB(37, 99, 235));
            draw_border(cx, cy, cell_w - 2, 48, 2, RGB(250, 204, 21));
        } else {
            fill_rect(cx, cy, cell_w - 2, 48, RGB(15, 22, 34));
            draw_border(cx, cy, cell_w - 2, 48, 1, RGB(38, 52, 76));
        }
        draw_text_centered(FONT_UI_11, cx, cy + 4, cell_w - 2, 14, k_deg_rom[d],
                           is_sel ? RGB(254, 240, 138) : RGB(148, 163, 184));
        const char *ch_lbl = (d_rng == 0) ? k_wheel[d_sec].maj_name :
                             (d_rng == 1) ? k_wheel[d_sec].min_name :
                                            k_wheel[g_key_sector].dim_root_name;
        draw_text_centered(FONT_UI_13, cx, cy + 22, cell_w - 2, 20, ch_lbl,
                           is_sel ? RGB(255, 255, 255) : RGB(226, 232, 240));
    }
}

static void render_bottom_progression_bar(void) {
    int by = 436, bh = 44;
    fill_rect(0, by, NATIVE_W, bh, RGB(17, 24, 39));
    fill_rect(0, by, NATIVE_W, 2, RGB(51, 65, 85));

    draw_pill(8, by + 7, 102, 30,
              g_loop_playing ? RGB(16, 185, 129) : RGB(30, 41, 59),
              g_loop_playing ? RGB(110, 231, 183) : RGB(71, 85, 105));
    draw_text_centered(FONT_UI_13, 8, by + 7, 102, 30,
                       g_loop_playing ? "LOOP: ON" : "X: +PROG",
                       RGB(255, 255, 255));

    int sx = 118;
    int slot_w = 100;
    for (int i = 0; i < 4; i++) {
        ResolvedChord prc;
        resolve_chord_at(g_key_sector, g_prog[i].sector, g_prog[i].ring, g_prog[i].use_7th, &prc);
        bool active_step = (g_loop_playing && g_loop_step == i);
        bool next_write  = (!g_loop_playing && g_prog_write_idx == i);

        uint32_t s_fill = active_step ? RGB(217, 119, 6) : RGB(26, 36, 56);
        uint32_t s_bdr  = active_step ? RGB(254, 240, 138) :
                          (next_write ? RGB(56, 189, 248) : RGB(64, 82, 115));
        draw_pill(sx, by + 7, slot_w, 30, s_fill, s_bdr);

        char slot_lbl[64];
        snprintf(slot_lbl, sizeof(slot_lbl), "%s (%s)", prc.chord_name, prc.roman_num);
        int s_font = (text_width(FONT_UI_13, slot_lbl) > slot_w - 8)
                     ? FONT_UI_11 : FONT_UI_13;
        draw_text_centered(s_font, sx, by + 7, slot_w, 30, slot_lbl,
                           active_step ? RGB(255, 255, 255) : RGB(226, 232, 240));
        sx += slot_w + 6;
    }

    draw_pill(NATIVE_W - 92, by + 7, 84, 30, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, NATIVE_W - 92, by + 7, 84, 30, "Y: Help", RGB(254, 240, 138));
}

static void render_help_modal(void) {
    int mx = 20, my = 14, mw = NATIVE_W - 40, mh = NATIVE_H - 28;
    fill_rect(mx, my, mw, mh, RGB(15, 23, 38));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    fill_rect(mx + 4, my + 4, mw - 8, 40, RGB(28, 39, 60));
    fill_rect(mx + 4, my + 44, mw - 8, 2, RGB(64, 82, 115));
    draw_pill(mx + 12, my + 8, 76, 28, RGB(37, 99, 235), RGB(147, 197, 253));
    draw_text_centered(FONT_UI_13, mx + 12, my + 8, 76, 28, "HELP", RGB(255, 255, 255));
    draw_text(FONT_UI_20, mx + 100, my + 10, "CHORDWHEEL - QUICK GUIDE & PRESETS", RGB(248, 250, 252));

    fill_rect(mx + 4, my + 46, mw - 8, 22, RGB(19, 29, 48));
    fill_rect(mx + 4, my + 68, mw - 8, 1, RGB(48, 66, 98));
    draw_text_centered(FONT_UI_13, mx + 8, my + 46, mw - 16, 22,
                       "Explore Circle of Fifths harmony, 3 guitar voicings & 4-bar chord loops.",
                       RGB(186, 230, 253));

    int py = my + 74;
    fill_rect(mx + 14, py, mw - 28, 72, RGB(23, 33, 52));
    draw_border(mx + 14, py, mw - 28, 72, 2, RGB(56, 189, 248));

    char p_str[96];
    snprintf(p_str, sizeof(p_str), "PRESET (LEFT/RIGHT): %s", k_presets[g_preset_idx].name);
    draw_text_centered(FONT_UI_16, mx + 14, py + 10, mw - 28, 24, p_str, RGB(250, 204, 21));
    draw_text_centered(FONT_UI_13, mx + 14, py + 40, mw - 28, 20,
                       "[A] / [R2]: Start/Stop 4-Bar Looper  |  [L2]: Cycle Guitar Timbre",
                       RGB(125, 211, 252));

    int ky = py + 82;
    static const struct {
        const char *btn;
        const char *desc;
    } k_help_rows[7] = {
        { "LEFT / RIGHT", "Rotate Wheel (4ths / 5ths around Circle)" },
        { "UP / DOWN",    "Switch Outer Major / Inner Minor / Center vii Ring" },
        { "A Button",     "Strum 6-String Guitar Voicing (SF2 / EKS Synth)" },
        { "B Button",     "Toggle Triads (C, Am, G) <-> 7ths (Cmaj7, Am7, G7)" },
        { "L1 / R1",      "Cycle Guitar Voicing (Open/CAGED, E-Barre, A-Barre)" },
        { "L2 / R2",      "L2: Timbre (Acoustic/Chorus/Nylon) | R2: Toggle Looper" },
        { "SELECT / X",   "SELECT: Lock Key Center | X: Queue Chord | MENU: Quit" }
    };

    for (int i = 0; i < 7; i++) {
        int ry = ky + i * 36;
        draw_pill(mx + 16, ry, 136, 28, RGB(30, 58, 95), RGB(71, 98, 145));
        draw_text_centered(FONT_UI_13, mx + 16, ry, 136, 28, k_help_rows[i].btn, RGB(254, 240, 138));
        draw_text(FONT_UI_13, mx + 166, ry + 6, k_help_rows[i].desc, RGB(226, 232, 240));
    }

    fill_rect(mx + 4, my + mh - 34, mw - 8, 30, RGB(12, 18, 30));
    fill_rect(mx + 4, my + mh - 34, mw - 8, 1, RGB(51, 65, 85));
    draw_text_centered(FONT_UI_13, mx, my + mh - 32, mw, 26,
                       "Press [Y] or [B] to return to the Circle of Fifths",
                       RGB(186, 230, 253));
}

static void render_ui(void) {
    fill_rect(0, 0, NATIVE_W, NATIVE_H, RGB(13, 18, 28));
    render_circle_of_fifths_wheel();
    render_guitar_inspector_pane();
    render_bottom_progression_bar();
    if (g_show_help) {
        render_help_modal();
    }
}

/* ============================================================================
 * 8. MAIN ENTRY POINT & EVENT LOOP
 * ============================================================================ */

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    resolve_config_path();
    load_config();

    /* Automated Audio Self-Test (--audio-test) */
    if (argc >= 2 && strcmp(argv[1], "--audio-test") == 0) {
        strum_current_selection();
        int16_t pcm[512 * AUDIO_CHANNELS];
        double energy = 0.0;
        int peak = 0;
        int total_frames = SAMPLE_RATE / 2;
        for (int done = 0; done < total_frames; done += 512) {
            audio_callback(NULL, (uint8_t *)pcm, (int)sizeof(pcm));
            for (int i = 0; i < 512 * AUDIO_CHANNELS; i++) {
                int v = pcm[i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
                energy += (double)pcm[i] * (double)pcm[i];
            }
        }
        double rms = sqrt(energy / (double)(total_frames * AUDIO_CHANNELS));
        printf("chordwheel audio-test: sf2=%s path=%s peak=%d rms=%.1f\n",
               rg35_sf2_ready(&g_sf2) ? "YES" : "NO",
               rg35_sf2_ready(&g_sf2) ? g_sf2.loaded_path : "(fallback-eks)",
               peak, rms);
        rg35_sf2_free(&g_sf2);
        return (peak > 100) ? 0 : 1;
    }

    /* Automated Screenshot Mode (--screenshot-states <dir>) */
    if (argc >= 3 && strcmp(argv[1], "--screenshot-states") == 0) {
        const char *dir = argv[2];
        char path[512];

        /* 1. Key of C Major — Triads, Open C Major Voicing */
        g_key_sector = 0; g_cur_sector = 0; g_cur_ring = 0;
        g_key_locked = false; g_seventh_mode = false; g_voicing_idx = 0;
        g_timbre = TIMBRE_ACOUSTIC;
        g_show_help = false;
        apply_progression_preset(0);
        render_ui();
        snprintf(path, sizeof(path), "%s/chordwheel_main.bmp", dir);
        save_bmp(path);

        /* 2. Key of G Major (Locked) — Exploring ii7 Am7 (7th Chords + 5fr E-Barre Voicing) */
        g_key_sector = 1; g_cur_sector = 0; g_cur_ring = 1;
        g_key_locked = true; g_seventh_mode = true; g_voicing_idx = 1;
        g_timbre = TIMBRE_CHORUS;
        g_show_help = false;
        apply_progression_preset(2);
        render_ui();
        snprintf(path, sizeof(path), "%s/chordwheel_7ths.bmp", dir);
        save_bmp(path);

        /* 3. Key of F Major — 4-Bar Auto-Loop Active on Bb (IV) with A-Shape Barre Voicing */
        g_key_sector = 11; g_cur_sector = 10; g_cur_ring = 0;
        g_key_locked = true; g_seventh_mode = false; g_voicing_idx = 2;
        g_timbre = TIMBRE_NYLON;
        g_show_help = false;
        apply_progression_preset(3);
        g_loop_playing = true; g_loop_step = 2;
        render_ui();
        snprintf(path, sizeof(path), "%s/chordwheel_loop.bmp", dir);
        save_bmp(path);
        g_loop_playing = false;

        /* 4. Help & Progression Looper Modal */
        g_show_help = true;
        render_ui();
        snprintf(path, sizeof(path), "%s/chordwheel_help.bmp", dir);
        save_bmp(path);
        return 0;
    }

    if (!platform_init("RG35XXSP Circle of Fifths (chordwheel)")) {
        fprintf(stderr, "Failed to initialize platform backend\n");
        return 1;
    }

    strum_current_selection();

    input_state_t input;
    while (!g_sig_quit && platform_poll_input(&input)) {
        uint32_t now_ms = get_ticks_ms();
        uint32_t p = input.pressed;

        if (p & BTN_MENU) {
            break;
        }

        if (p & BTN_Y) {
            g_show_help = !g_show_help;
            p &= ~BTN_Y;
        }

        if (p & BTN_L2) {
            g_timbre = (GuitarTimbre)((g_timbre + 1) % 3);
            strum_current_selection();
        }
        if (p & BTN_R2) {
            g_loop_playing = !g_loop_playing;
            g_loop_step    = 0;
            g_last_loop_ms = now_ms;
            if (g_loop_playing) {
                g_cur_sector = g_prog[0].sector;
                g_cur_ring   = g_prog[0].ring;
                strum_current_selection();
            }
        }

        if (g_show_help) {
            if (p & BTN_LEFT) {
                apply_progression_preset(g_preset_idx - 1);
            } else if (p & BTN_RIGHT) {
                apply_progression_preset(g_preset_idx + 1);
            } else if (p & BTN_A) {
                g_loop_playing = !g_loop_playing;
                g_loop_step    = 0;
                g_last_loop_ms = now_ms;
                if (g_loop_playing) {
                    g_cur_sector = g_prog[0].sector;
                    g_cur_ring   = g_prog[0].ring;
                    strum_current_selection();
                }
            } else if (p & (BTN_B | BTN_SELECT)) {
                g_show_help = false;
            }
        } else {
            if (p & BTN_RIGHT) {
                g_loop_playing = false;
                if (!g_key_locked) {
                    g_key_sector = (g_key_sector + 1) % 12;
                    g_cur_sector = (g_cur_sector + 1) % 12;
                    for (int i = 0; i < 4; i++) {
                        g_prog[i].sector = (g_prog[i].sector + 1) % 12;
                    }
                } else {
                    g_cur_sector = (g_cur_sector + 1) % 12;
                }
                strum_current_selection();
            } else if (p & BTN_LEFT) {
                g_loop_playing = false;
                if (!g_key_locked) {
                    g_key_sector = (g_key_sector + 11) % 12;
                    g_cur_sector = (g_cur_sector + 11) % 12;
                    for (int i = 0; i < 4; i++) {
                        g_prog[i].sector = (g_prog[i].sector + 11) % 12;
                    }
                } else {
                    g_cur_sector = (g_cur_sector + 11) % 12;
                }
                strum_current_selection();
            } else if (p & BTN_DOWN) {
                g_loop_playing = false;
                g_cur_ring = (g_cur_ring < 2) ? (g_cur_ring + 1) : 0;
                strum_current_selection();
            } else if (p & BTN_UP) {
                g_loop_playing = false;
                g_cur_ring = (g_cur_ring > 0) ? (g_cur_ring - 1) : 2;
                strum_current_selection();
            } else if (p & BTN_A) {
                strum_current_selection();
            } else if (p & BTN_B) {
                g_seventh_mode = !g_seventh_mode;
                for (int i = 0; i < 4; i++) {
                    g_prog[i].use_7th = g_seventh_mode;
                }
                strum_current_selection();
            } else if (p & BTN_R1) {
                g_voicing_idx = (g_voicing_idx + 1) % 3;
                strum_current_selection();
            } else if (p & BTN_L1) {
                g_voicing_idx = (g_voicing_idx + 2) % 3;
                strum_current_selection();
            } else if (p & BTN_SELECT) {
                g_key_locked = !g_key_locked;
                if (!g_key_locked) g_cur_sector = g_key_sector;
            } else if (p & BTN_X) {
                g_prog[g_prog_write_idx].sector  = g_cur_sector;
                g_prog[g_prog_write_idx].ring    = g_cur_ring;
                g_prog[g_prog_write_idx].use_7th = g_seventh_mode;
                g_prog_write_idx = (g_prog_write_idx + 1) % 4;
                strum_current_selection();
            }
        }

        if (g_loop_playing && (now_ms - g_last_loop_ms >= 1400)) {
            g_last_loop_ms = now_ms;
            g_loop_step    = (g_loop_step + 1) % 4;
            g_cur_sector   = g_prog[g_loop_step].sector;
            g_cur_ring     = g_prog[g_loop_step].ring;
            g_seventh_mode = g_prog[g_loop_step].use_7th;
            strum_current_selection();
        }

        render_ui();
        platform_present();
        delay_ms(16);
    }

    save_config();
    platform_shutdown();
    return 0;
}
