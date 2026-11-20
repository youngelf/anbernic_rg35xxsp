/*
 * sightread.c - Guitar Staff-to-Fretboard Sight-Reading Trainer
 *               for Anbernic RG 35xxSP (640x480 IPS, Allwinner H700 aarch64)
 *               & Linux x86_64 Desktop (X11)
 *
 * Features:
 *   - Native 640x480 32-bit XRGB8888 UI with anti-aliased multi-size typography
 *     and FreeSerif-engraved Treble Clef (8vb), Sharp, and Flat symbols.
 *   - Deterministic progressive exercise engine (EX #01..#99 across LVL1..LVL4),
 *     7 key signatures (C, G 1#, D 2#, A 3#, F 1b, Bb 2b, Eb 3b), and
 *     5 neck positions (OPEN 0-4fr, POS 2, POS 5, POS 7, POS 9).
 *   - 100% Background-Thread Audio & Sample-Accurate Playback Sequencer:
 *     * Studio SoundFont 2.01 engine (GeneralUser-GS.sf2 via rg35xxsp_sf2.h)
 *       with 4 guitar timbres (Steel, Nylon, Clean Jazz, Overdrive) + woodblock
 *       metronome clicks + Schroeder stereo room reverb.
 *     * Upgraded 44.1 kHz floating-point Extended Karplus-Strong (EKS) physical
 *       modeling fallback (all-pass fractional delay, pick comb filter, wooden
 *       body biquads, stereo room reverb) when no .sf2 is present.
 *   - Full rg35xxsp_input.h compliance:
 *     * MENU / Esc    : Save settings & Quit (sole exit key)
 *     * Y             : Toggle English-only Help & Guide modal
 *     * A / Space / Z : Play / Pause entire musical piece
 *     * B / Backspace : Play single highlighted note on staff
 *     * LEFT / RIGHT  : Move cursor across staff notes
 *     * UP / DOWN     : Adjust BPM (40..180, continuous hold repeat)
 *     * L1 / R1       : Previous / Next Exercise (EX #01..#99)
 *     * SELECT / Tab  : Cycle Neck Position (OPEN..POS 9)
 *     * X             : Cycle 7 Key Signatures
 *     * L2 / 1        : Cycle Guitar Timbre (Steel / Nylon / Jazz / Overdrive)
 *     * R2 / 2        : Jump Difficulty Tier (LVL1..LVL4)
 *     * START / Enter : Reset BPM to 84 and cursor to Note 1
 */

#include "../rg35xxsp_input.h"
#include "../rg35xxsp_keepawake.h"

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

#define RG35XXSP_SF2_IMPLEMENTATION
#include "../rg35xxsp_sf2.h"

#include "sightread_font.h"

#define NATIVE_W        640
#define NATIVE_H        480
#define SAMPLE_RATE     44100
#define AUDIO_CHANNELS  2

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RGB(r, g, b) \
    (0xFF000000u | (((uint32_t)(r) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu))

/* Palette matching the RG35xxSP guitar suite */
#define COL_BG          RGB(13, 18, 28)
#define COL_HEADER      RGB(19, 27, 42)
#define COL_PANEL       RGB(20, 28, 44)
#define COL_PANEL_ALT   RGB(26, 36, 56)
#define COL_BORDER      RGB(51, 68, 98)
#define COL_STAFF_LINE  RGB(156, 174, 202)
#define COL_BAR_LINE    RGB(108, 128, 160)
#define COL_WHITE       RGB(246, 250, 255)
#define COL_MUTED       RGB(148, 163, 184)
#define COL_DIM         RGB(82, 98, 124)
#define COL_CORAL       RGB(244, 92, 92)
#define COL_GOLD        RGB(250, 204, 21)
#define COL_AMBER       RGB(245, 158, 11)
#define COL_CYAN        RGB(56, 189, 248)
#define COL_EMERALD     RGB(52, 211, 153)
#define COL_GREEN       RGB(16, 185, 129)
#define COL_ORANGE      RGB(251, 146, 60)
#define COL_PURPLE      RGB(167, 139, 250)
#define COL_NECK_WOOD   RGB(40, 27, 20)
#define COL_NECK_POS    RGB(66, 46, 35)
#define COL_FRET_WIRE   RGB(148, 163, 184)
#define COL_BONE_NUT    RGB(238, 230, 214)

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

/* ============================================================================
 * 1. GUITAR POSITIONS, 7 KEY SIGNATURES & PROGRESSIVE PIECE GENERATOR
 * ============================================================================ */

/* Standard guitar open string MIDI pitches: E2(40), A2(45), D3(50), G3(55), B3(59), E4(64) */
static const int  k_open_midi[6]  = { 40, 45, 50, 55, 59, 64 };
static const char k_str_labels[6] = { 'E', 'A', 'D', 'G', 'B', 'e' };

#define NUM_POSITIONS 5
typedef struct {
    const char *short_name;
    const char *full_name;
    int         min_fret;
    int         max_fret;
    int         base_fret; /* Fret where index finger (1) rests */
} NeckPosition;

static const NeckPosition k_positions[NUM_POSITIONS] = {
    { "OPEN (0-4fr)",  "Open Position (Frets 0-4)", 0,  4, 1 },
    { "POS 2 (2-5fr)", "2nd Position (Frets 2-5)",  2,  5, 2 },
    { "POS 5 (5-8fr)", "5th Position (Frets 5-8)",  5,  8, 5 },
    { "POS 7 (7-10fr)","7th Position (Frets 7-10)", 7, 10, 7 },
    { "POS 9 (9-12fr)","9th Position (Frets 9-12)", 9, 12, 9 }
};

#define NUM_KEYS 7
typedef struct {
    const char *name;
    const char *badge;
    int         tonic_pc;
    int         sharps_or_flats; /* >0 = sharps, <0 = flats, 0 = C Major */
    int         deg_pc[7];       /* Pitch class (0..11) for letter C, D, E, F, G, A, B */
    const char *deg_name[7];     /* Note name for letter C, D, E, F, G, A, B */
} KeySigDef;

static const KeySigDef k_keys[NUM_KEYS] = {
    { "C Major",  "C Maj",    0,  0, { 0, 2, 4, 5, 7, 9, 11 }, { "C",  "D",  "E",  "F",  "G",  "A",  "B"  } },
    { "G Major",  "G (1#)",   7,  1, { 0, 2, 4, 6, 7, 9, 11 }, { "C",  "D",  "E",  "F#", "G",  "A",  "B"  } },
    { "D Major",  "D (2#)",   2,  2, { 1, 2, 4, 6, 7, 9, 11 }, { "C#", "D",  "E",  "F#", "G",  "A",  "B"  } },
    { "A Major",  "A (3#)",   9,  3, { 1, 2, 4, 6, 8, 9, 11 }, { "C#", "D",  "E",  "F#", "G#", "A",  "B"  } },
    { "F Major",  "F (1b)",   5, -1, { 0, 2, 4, 5, 7, 9, 10 }, { "C",  "D",  "E",  "F",  "G",  "A",  "Bb" } },
    { "Bb Major", "Bb (2b)", 10, -2, { 0, 2, 3, 5, 7, 9, 10 }, { "C",  "D",  "Eb", "F",  "G",  "A",  "Bb" } },
    { "Eb Major", "Eb (3b)",  3, -3, { 0, 2, 3, 5, 7, 8, 10 }, { "C",  "D",  "Eb", "F",  "G",  "Ab", "Bb" } }
};

/* Standard Treble Clef Key Signature step placements (C3=0, E3=2, G3=4, B3=6, D4=8, F4=10):
 *   Sharps order: F(10), C(7), G(11)
 *   Flats order : B(6),  E(9), A(5)
 */
static const int k_sharp_staff_steps[3] = { 10, 7, 11 };
static const int k_flat_staff_steps[3]  = {  6, 9,  5 };

#define MAX_PIECE_NOTES 16
#define MAX_SCALE_POOL  32

typedef struct {
    int8_t  str;            /* Guitar string 0 (6th E) .. 5 (1st e) */
    int8_t  fret;           /* Exact fret strictly within active NeckPosition */
    int8_t  finger;         /* 0=Open, 1=Index, 2=Middle, 3=Ring, 4=Pinky */
    int8_t  midi;           /* Sounding MIDI pitch (40=E2 .. 76=E5) */
    int8_t  diatonic_step;  /* Staff step relative to C3=0 (-5=E2 .. +16=E5) */
    int8_t  dur_eighths;    /* 2 = Quarter Note, 1 = Eighth Note */
    int8_t  bar_idx;        /* Measure index (0..1) */
    int8_t  eighth_in_bar;  /* Eighth-note offset within measure (0..7) */
    bool    beam_with_next; /* True if beamed to next eighth note on same beat */
    bool    beam_from_prev; /* True if beamed from previous eighth note */
    char    label[16];      /* e.g. "C4", "F#3", "Eb4" */
} StaffNote;

typedef struct {
    int       exercise_num;  /* 1..99 */
    int       level;         /* 1..4 progressive complexity */
    int       num_bars;      /* 2 measures */
    int       beats_per_bar; /* 4 (4/4 time) */
    int       num_notes;     /* Total notes in piece (8..16) */
    StaffNote notes[MAX_PIECE_NOTES];
} SightPiece;

static int build_position_scale_pool(int pos_idx, int key_idx, StaffNote *pool) {
    const NeckPosition *pos = &k_positions[pos_idx];
    const KeySigDef *key = &k_keys[key_idx];
    int count = 0;
    int last_midi = -1;

    for (int s = 0; s < 6; s++) {
        int next_str_min_midi = (s < 5) ? (k_open_midi[s + 1] + pos->min_fret) : 999;
        for (int f = pos->min_fret; f <= pos->max_fret; f++) {
            int midi = k_open_midi[s] + f;
            if (midi >= next_str_min_midi && s < 5) break;
            if (midi <= last_midi) continue;

            int pc = midi % 12;
            int found_deg = -1;
            for (int d = 0; d < 7; d++) {
                if (key->deg_pc[d] == pc) {
                    found_deg = d;
                    break;
                }
            }
            if (found_deg < 0) continue;

            int c_base_midi = midi - key->deg_pc[found_deg];
            int octave_from_c3 = (c_base_midi - 48) / 12;
            if (c_base_midi < 48) octave_from_c3 = -((48 - c_base_midi + 11) / 12);
            int diatonic_step = octave_from_c3 * 7 + found_deg;

            int finger = 0;
            if (f > 0) {
                finger = f - pos->base_fret + 1;
                if (finger < 1) finger = 1;
                if (finger > 4) finger = 4;
            }

            StaffNote *n = &pool[count++];
            memset(n, 0, sizeof(*n));
            n->str           = (int8_t)s;
            n->fret          = (int8_t)f;
            n->finger        = (int8_t)finger;
            n->midi          = (int8_t)midi;
            n->diatonic_step = (int8_t)diatonic_step;
            int oct_num = (c_base_midi / 12) - 1;
            snprintf(n->label, sizeof(n->label), "%s%d", key->deg_name[found_deg], oct_num);

            last_midi = midi;
            if (count >= MAX_SCALE_POOL) return count;
        }
    }
    return count;
}

static void generate_sight_piece(SightPiece *piece, int exercise_num, int pos_idx, int key_idx) {
    memset(piece, 0, sizeof(*piece));
    if (exercise_num < 1) exercise_num = 1;
    if (exercise_num > 99) exercise_num = 99;
    piece->exercise_num  = exercise_num;
    piece->num_bars      = 2;
    piece->beats_per_bar = 4;

    if (exercise_num <= 3)       piece->level = 1;
    else if (exercise_num <= 8)  piece->level = 2;
    else if (exercise_num <= 15) piece->level = 3;
    else                         piece->level = 4;

    StaffNote pool[MAX_SCALE_POOL];
    int pool_len = build_position_scale_pool(pos_idx, key_idx, pool);
    if (pool_len <= 0) return;

    const KeySigDef *key = &k_keys[key_idx];

    int tonic_idx = pool_len / 2;
    int best_dist = 999;
    for (int i = 0; i < pool_len; i++) {
        if ((pool[i].midi % 12) == key->tonic_pc) {
            int d = abs(i - pool_len / 2);
            if (d < best_dist) {
                best_dist = d;
                tonic_idx = i;
            }
        }
    }

    uint32_t rng = (uint32_t)(exercise_num * 747796405u + pos_idx * 2891336453u +
                              key_idx * 1013904223u + 12345u);

    bool beat_is_eighth[8];
    int eighth_pair_target = 1;
    if (piece->level == 1)      eighth_pair_target = (exercise_num == 1) ? 1 : 2;
    else if (piece->level == 2) eighth_pair_target = 3;
    else if (piece->level == 3) eighth_pair_target = 4;
    else                        eighth_pair_target = 5;

    memset(beat_is_eighth, 0, sizeof(beat_is_eighth));
    int pairs_placed = 0;
    for (int attempt = 0; attempt < 24 && pairs_placed < eighth_pair_target; attempt++) {
        rng = rng * 1664525u + 1013904223u;
        int b = (int)((rng >> 16) % 7);
        if (!beat_is_eighth[b]) {
            beat_is_eighth[b] = true;
            pairs_placed++;
        }
    }

    int cur_pool_idx = tonic_idx;
    int dir = ((exercise_num & 1) == 0) ? 1 : -1;
    int n_idx = 0;

    for (int beat = 0; beat < 8 && n_idx < MAX_PIECE_NOTES; beat++) {
        int bar = beat / 4;
        int beat_in_bar = beat % 4;
        bool is_pair = beat_is_eighth[beat] && (n_idx + 1 < MAX_PIECE_NOTES);
        int sub_notes = is_pair ? 2 : 1;

        for (int sub = 0; sub < sub_notes; sub++) {
            if (beat == 0 && sub == 0) {
                cur_pool_idx = tonic_idx;
            } else if (beat == 7 && sub == 0) {
                int nearest_t = tonic_idx;
                int min_d = 999;
                for (int i = 0; i < pool_len; i++) {
                    if ((pool[i].midi % 12) == key->tonic_pc) {
                        int d = abs(i - cur_pool_idx);
                        if (d < min_d) { min_d = d; nearest_t = i; }
                    }
                }
                cur_pool_idx = nearest_t;
            } else {
                rng = rng * 1664525u + 1013904223u;
                int r = (int)((rng >> 16) % 100);
                int step_delta = dir;
                if (piece->level == 1) {
                    if (r < 25) dir = -dir;
                    step_delta = dir;
                } else if (piece->level == 2) {
                    if (r < 30) dir = -dir;
                    step_delta = (r > 80 && sub == 0) ? (dir * 2) : dir;
                } else {
                    if (r < 35) dir = -dir;
                    if (sub == 1) step_delta = dir;
                    else if (r > 85) step_delta = dir * 3;
                    else if (r > 65) step_delta = dir * 2;
                    else step_delta = dir;
                }

                cur_pool_idx += step_delta;
                if (cur_pool_idx >= pool_len - 1) {
                    cur_pool_idx = pool_len - 1;
                    dir = -1;
                } else if (cur_pool_idx <= 0) {
                    cur_pool_idx = 0;
                    dir = 1;
                }
            }

            StaffNote *dst = &piece->notes[n_idx];
            *dst = pool[cur_pool_idx];
            dst->dur_eighths    = is_pair ? 1 : 2;
            dst->bar_idx        = (int8_t)bar;
            dst->eighth_in_bar  = (int8_t)(beat_in_bar * 2 + sub);
            dst->beam_with_next = (is_pair && sub == 0);
            dst->beam_from_prev = (is_pair && sub == 1);
            n_idx++;
        }
    }
    piece->num_notes = n_idx;
}

static bool verify_sightread_generator(bool verbose) {
    bool all_ok = true;
    for (int pos = 0; pos < NUM_POSITIONS; pos++) {
        const NeckPosition *np = &k_positions[pos];
        for (int key = 0; key < NUM_KEYS; key++) {
            for (int ex = 1; ex <= 20; ex++) {
                SightPiece sp;
                generate_sight_piece(&sp, ex, pos, key);
                if (sp.num_notes < 8 || sp.num_notes > MAX_PIECE_NOTES) {
                    fprintf(stderr, "[FAIL] Ex #%d pos=%d key=%d: invalid num_notes=%d\n",
                            ex, pos, key, sp.num_notes);
                    all_ok = false;
                }
                int total_eighths = 0;
                for (int i = 0; i < sp.num_notes; i++) {
                    const StaffNote *n = &sp.notes[i];
                    if (n->fret < np->min_fret || n->fret > np->max_fret) {
                        fprintf(stderr,
                                "[FAIL] Ex #%d pos=%s note %d: fret %d outside [%d..%d]\n",
                                ex, np->short_name, i, n->fret, np->min_fret, np->max_fret);
                        all_ok = false;
                    }
                    total_eighths += n->dur_eighths;
                }
                if (total_eighths != 16) {
                    fprintf(stderr, "[FAIL] Ex #%d pos=%d key=%d: total_eighths=%d (expected 16)\n",
                            ex, pos, key, total_eighths);
                    all_ok = false;
                }
            }
        }
        if (verbose) {
            printf("[OK] Position %-14s verified across all %d keys x 20 exercises\n",
                   np->short_name, NUM_KEYS);
        }
    }
    return all_ok;
}

/* ============================================================================
 * 2. BACKGROUND-THREAD AUDIO ENGINE: SOUNDFONT 2.01 (rg35xxsp_sf2.h) +
 *    ENHANCED FLOATING-POINT EKS PHYSICAL MODELING FALLBACK & SEQUENCER
 * ============================================================================ */

#define NUM_TIMBRES 4
typedef struct {
    const char *short_name;
    const char *full_name;
    int         gm_program; /* GM Bank 0 preset */
} GuitarTimbreDef;

static const GuitarTimbreDef k_timbres[NUM_TIMBRES] = {
    { "STEEL",  "Steel Acoustic",     25 },
    { "NYLON",  "Nylon Classical",    24 },
    { "JAZZ",   "Clean Jazz Guitar",  26 },
    { "OVERD",  "Overdrive Electric", 29 }
};

#define MIN_BPM 40
#define MAX_BPM 180

/* UI / App State */
static SightPiece    g_piece;
static int           g_exercise_num = 1;
static int           g_pos_idx      = 0;
static int           g_key_idx      = 0;
static int           g_bpm          = 84;
static int           g_timbre_idx   = 0;
static bool          g_show_help    = false;

/* Shared atomic telemetry written by audio thread, read by UI thread */
static volatile int  g_ui_cur_note_idx = 0;
static volatile bool g_ui_playing      = false;
static volatile bool g_sf2_active      = false;

/* Lock-free command ring: UI thread -> Audio thread */
typedef enum {
    ACMD_NONE = 0,
    ACMD_LOAD_PIECE,
    ACMD_PLAY_SINGLE,
    ACMD_SET_PLAYING,
    ACMD_SET_BPM,
    ACMD_SET_TIMBRE,
    ACMD_SET_CURSOR
} AudioCmdType;

typedef struct {
    AudioCmdType type;
    int          arg0;
    int          arg1;
    SightPiece   piece;
} AudioCmd;

#define AUDIO_CMD_RING 16
static AudioCmd          g_acmd_ring[AUDIO_CMD_RING];
static volatile uint32_t g_acmd_write = 0;
static volatile uint32_t g_acmd_read  = 0;

static void post_audio_cmd(const AudioCmd *cmd) {
    uint32_t w = g_acmd_write;
    uint32_t next_w = (w + 1) % AUDIO_CMD_RING;
    if (next_w == g_acmd_read) {
        return; /* Ring full; drop safely rather than blocking UI */
    }
    g_acmd_ring[w] = *cmd;
    __sync_synchronize();
    g_acmd_write = next_w;
}

static void post_simple_cmd(AudioCmdType type, int arg0, int arg1) {
    AudioCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.arg0 = arg0;
    cmd.arg1 = arg1;
    post_audio_cmd(&cmd);
}

/* SoundFont engine instance (audio thread owned after init) */
static Rg35Sf2Engine g_sf2;

/* Enhanced Floating-Point EKS Fallback Engine (44.1 kHz Stereo + Room Reverb) */
#define EKS_VOICES    6
#define EKS_MAX_DELAY 1200

typedef struct {
    bool  active;
    float buf[EKS_MAX_DELAY];
    int   buf_len;
    int   idx;
    float exact_period;
    float damping;
    float stretch;
    float ap_prev_in;
    float ap_state;
    float pan_l;
    float pan_r;
    int   samples_left;
    int   total_samples;
} EksVoice;

typedef struct {
    bool  active;
    int   pos;
    int   len;
    float freq;
    float gain;
} EksClick;

static EksVoice  g_eks_voices[EKS_VOICES];
static int       g_eks_next_voice = 0;
static EksClick  g_eks_click;
static float     g_eks_body_z1a = 0.0f, g_eks_body_z2a = 0.0f;
static float     g_eks_body_z1b = 0.0f, g_eks_body_z2b = 0.0f;
static uint32_t  g_eks_rng = 0xA511E9B3u;

/* Audio-thread sequencer state */
static SightPiece g_aud_piece;
static bool       g_aud_playing       = false;
static int        g_aud_bpm           = 84;
static int        g_aud_timbre        = 0;
static int        g_aud_cur_note      = 0;
static int        g_aud_eighth_rem    = 2;
static int        g_aud_last_midi     = -1;
static double     g_aud_sample_accum  = 0.0;

static void eks_trigger_note(int midi_note, int str_hint, int timbre_idx) {
    if (midi_note < 36) midi_note = 36;
    if (midi_note > 88) midi_note = 88;
    EksVoice *v = &g_eks_voices[g_eks_next_voice];
    g_eks_next_voice = (g_eks_next_voice + 1) % EKS_VOICES;

    float freq = 440.0f * powf(2.0f, (float)(midi_note - 69) / 12.0f);
    float exact_period = (float)SAMPLE_RATE / freq;
    if (exact_period < 16.0f) exact_period = 16.0f;
    if (exact_period > (float)(EKS_MAX_DELAY - 4)) exact_period = (float)(EKS_MAX_DELAY - 4);
    int int_len = (int)floorf(exact_period);

    v->buf_len       = int_len;
    v->exact_period  = exact_period;
    v->idx           = 0;
    v->ap_prev_in    = 0.0f;
    v->ap_state      = 0.0f;
    v->total_samples = (int)(SAMPLE_RATE * 1.45f);
    v->samples_left  = v->total_samples;

    if (timbre_idx == 1) { /* Nylon */
        v->damping = 0.9948f;
        v->stretch = 0.56f;
    } else if (timbre_idx == 2) { /* Jazz */
        v->damping = 0.9964f;
        v->stretch = 0.44f;
    } else if (timbre_idx == 3) { /* Overdrive */
        v->damping = 0.9974f;
        v->stretch = 0.24f;
    } else { /* Steel */
        v->damping = 0.9960f;
        v->stretch = 0.34f;
    }

    static const float k_pan_l[6] = { 0.72f, 0.66f, 0.58f, 0.50f, 0.44f, 0.38f };
    static const float k_pan_r[6] = { 0.38f, 0.44f, 0.50f, 0.58f, 0.66f, 0.72f };
    int s = (str_hint >= 0 && str_hint < 6) ? str_hint : 2;
    v->pan_l = k_pan_l[s];
    v->pan_r = k_pan_r[s];

    float raw[EKS_MAX_DELAY];
    int pick_comb = int_len / 7;
    if (pick_comb < 2) pick_comb = 2;
    float noise_amt = (timbre_idx == 1) ? 0.28f : (timbre_idx == 2 ? 0.34f : 0.52f);

    for (int i = 0; i < int_len; i++) {
        g_eks_rng = g_eks_rng * 1664525u + 1013904223u;
        float nz = ((float)(int32_t)(g_eks_rng >> 16) - 32768.0f) / 32768.0f;
        float ph = (2.0f * (float)M_PI * (float)i) / (float)int_len;
        float body = 0.48f * sinf(ph) + 0.26f * sinf(2.0f * ph) + 0.12f * sinf(3.0f * ph);
        raw[i] = body + noise_amt * nz;
    }
    for (int i = 0; i < int_len; i++) {
        int prev_p = (i - pick_comb + int_len) % int_len;
        v->buf[i] = raw[i] - 0.44f * raw[prev_p];
    }
    int passes = (timbre_idx == 1) ? 3 : 2;
    for (int p = 0; p < passes; p++) {
        float prev = v->buf[int_len - 1];
        for (int i = 0; i < int_len; i++) {
            float cur = v->buf[i];
            v->buf[i] = 0.56f * cur + 0.44f * prev;
            prev = cur;
        }
    }
    v->active = true;
}

static void eks_trigger_click(bool accent) {
    g_eks_click.active = true;
    g_eks_click.pos    = 0;
    g_eks_click.len    = 1764; /* 40 ms crisp woodblock */
    g_eks_click.freq   = accent ? 1620.0f : 1120.0f;
    g_eks_click.gain   = accent ? 0.44f : 0.30f;
}

static void audio_trigger_guitar_note(int midi_note, int str_hint, bool with_click, bool accent_click) {
    if (rg35_sf2_ready(&g_sf2)) {
        if (g_aud_last_midi >= 0) {
            rg35_sf2_note_off(&g_sf2, 0, g_aud_last_midi);
        }
        rg35_sf2_note_on(&g_sf2, 0, midi_note, 0.88f);
        g_aud_last_midi = midi_note;
        if (with_click) {
            int wood_key = accent_click ? 76 : 77; /* GM Hi / Lo Wood Block on Drum Ch 9 */
            rg35_sf2_note_on(&g_sf2, 9, wood_key, accent_click ? 0.82f : 0.58f);
        }
    } else {
        eks_trigger_note(midi_note, str_hint, g_aud_timbre);
        if (with_click) {
            eks_trigger_click(accent_click);
        }
    }
}

static void audio_trigger_metronome_only(bool accent_click) {
    if (rg35_sf2_ready(&g_sf2)) {
        int wood_key = accent_click ? 76 : 77;
        rg35_sf2_note_on(&g_sf2, 9, wood_key, accent_click ? 0.82f : 0.58f);
    } else {
        eks_trigger_click(accent_click);
    }
}

static void audio_apply_timbre(int timbre_idx) {
    if (timbre_idx < 0 || timbre_idx >= NUM_TIMBRES) timbre_idx = 0;
    g_aud_timbre = timbre_idx;
    if (rg35_sf2_ready(&g_sf2)) {
        rg35_sf2_channel_notes_off(&g_sf2, 0);
        rg35_sf2_set_channel_program(&g_sf2, 0, 0, k_timbres[timbre_idx].gm_program);
        rg35_sf2_set_channel_pan_vol(&g_sf2, 0, 0.50f, 0.96f);
        rg35_sf2_set_channel_pan_vol(&g_sf2, 9, 0.50f, 0.72f);
        g_aud_last_midi = -1;
    }
}

static void audio_process_commands(void) {
    while (g_acmd_read != g_acmd_write) {
        __sync_synchronize();
        AudioCmd cmd = g_acmd_ring[g_acmd_read];
        g_acmd_read = (g_acmd_read + 1) % AUDIO_CMD_RING;

        switch (cmd.type) {
            case ACMD_LOAD_PIECE:
                g_aud_piece = cmd.piece;
                g_aud_playing = false;
                g_ui_playing  = false;
                g_aud_cur_note = (cmd.arg0 >= 0 && cmd.arg0 < g_aud_piece.num_notes) ? cmd.arg0 : 0;
                g_ui_cur_note_idx = g_aud_cur_note;
                g_aud_eighth_rem = (g_aud_piece.num_notes > 0) ? g_aud_piece.notes[g_aud_cur_note].dur_eighths : 2;
                g_aud_sample_accum = 0.0;
                if (rg35_sf2_ready(&g_sf2)) {
                    rg35_sf2_channel_notes_off(&g_sf2, 0);
                    g_aud_last_midi = -1;
                }
                break;

            case ACMD_PLAY_SINGLE: {
                g_aud_playing = false;
                g_ui_playing  = false;
                int idx = cmd.arg0;
                if (idx >= 0 && idx < g_aud_piece.num_notes) {
                    g_aud_cur_note = idx;
                    g_ui_cur_note_idx = idx;
                    g_aud_eighth_rem = g_aud_piece.notes[idx].dur_eighths;
                    audio_trigger_guitar_note(g_aud_piece.notes[idx].midi,
                                              g_aud_piece.notes[idx].str, false, false);
                }
                break;
            }

            case ACMD_SET_PLAYING:
                g_aud_playing = (cmd.arg0 != 0);
                g_ui_playing  = g_aud_playing;
                g_aud_sample_accum = 0.0;
                if (g_aud_playing && g_aud_piece.num_notes > 0) {
                    if (g_aud_cur_note < 0 || g_aud_cur_note >= g_aud_piece.num_notes) {
                        g_aud_cur_note = 0;
                    }
                    const StaffNote *n = &g_aud_piece.notes[g_aud_cur_note];
                    g_aud_eighth_rem = n->dur_eighths;
                    bool on_beat = ((n->eighth_in_bar % 2) == 0);
                    bool accent  = (n->eighth_in_bar == 0);
                    audio_trigger_guitar_note(n->midi, n->str, on_beat, accent);
                } else {
                    if (rg35_sf2_ready(&g_sf2)) {
                        rg35_sf2_channel_notes_off(&g_sf2, 0);
                        g_aud_last_midi = -1;
                    }
                }
                break;

            case ACMD_SET_BPM:
                if (cmd.arg0 >= MIN_BPM && cmd.arg0 <= MAX_BPM) {
                    g_aud_bpm = cmd.arg0;
                }
                break;

            case ACMD_SET_TIMBRE:
                audio_apply_timbre(cmd.arg0);
                if (cmd.arg1 && g_aud_piece.num_notes > 0 && !g_aud_playing) {
                    const StaffNote *n = &g_aud_piece.notes[g_aud_cur_note];
                    audio_trigger_guitar_note(n->midi, n->str, false, false);
                }
                break;

            case ACMD_SET_CURSOR:
                if (cmd.arg0 >= 0 && cmd.arg0 < g_aud_piece.num_notes) {
                    g_aud_cur_note = cmd.arg0;
                    g_ui_cur_note_idx = cmd.arg0;
                    g_aud_eighth_rem = g_aud_piece.notes[cmd.arg0].dur_eighths;
                }
                break;

            default:
                break;
        }
    }
}

static void audio_advance_eighth_tick(void) {
    if (!g_aud_playing || g_aud_piece.num_notes <= 0) return;

    g_aud_eighth_rem--;
    if (g_aud_eighth_rem <= 0) {
        g_aud_cur_note = (g_aud_cur_note + 1) % g_aud_piece.num_notes;
        g_ui_cur_note_idx = g_aud_cur_note;
        const StaffNote *nn = &g_aud_piece.notes[g_aud_cur_note];
        g_aud_eighth_rem = nn->dur_eighths;
        bool on_beat = ((nn->eighth_in_bar % 2) == 0);
        bool accent  = (nn->eighth_in_bar == 0);
        audio_trigger_guitar_note(nn->midi, nn->str, on_beat, accent);
    } else {
        /* Inside a multi-eighth note: only click if this eighth lands on a beat boundary */
        const StaffNote *cur = &g_aud_piece.notes[g_aud_cur_note];
        int cur_eighth = cur->eighth_in_bar + (cur->dur_eighths - g_aud_eighth_rem);
        if ((cur_eighth % 2) == 0) {
            audio_trigger_metronome_only((cur_eighth % 8) == 0);
        }
    }
}

static void eks_render_i16(int16_t *dst_i16, int frames) {
    for (int f = 0; f < frames; f++) {
        float mix_l = 0.0f, mix_r = 0.0f, mono_sum = 0.0f;

        for (int i = 0; i < EKS_VOICES; i++) {
            EksVoice *v = &g_eks_voices[i];
            if (!v->active || v->samples_left <= 0) continue;

            int idx0 = v->idx;
            int idx1 = (idx0 + 1 < v->buf_len) ? (idx0 + 1) : 0;
            float cur = v->buf[idx0];
            float nxt = v->buf[idx1];

            float filtered = (1.0f - v->stretch) * cur + v->stretch * nxt;
            float frac = v->exact_period - (float)v->buf_len;
            float c_ap = (1.0f - frac) / (1.0f + frac + 0.001f);
            float ap_out = c_ap * filtered + v->ap_prev_in - c_ap * v->ap_state;
            v->ap_prev_in = filtered;
            v->ap_state   = ap_out;

            v->buf[idx0] = ap_out * v->damping;
            v->idx = idx1;

            float env = 1.0f;
            if (v->samples_left < 3000) {
                env = (float)v->samples_left / 3000.0f;
            }
            v->samples_left--;
            if (v->samples_left <= 0) v->active = false;

            float s_val = cur * env;
            if (g_aud_timbre == 3) {
                /* Warm tube overdrive waveshaping */
                s_val = tanhf(s_val * 2.8f) * 0.65f;
            }
            mix_l    += s_val * v->pan_l;
            mix_r    += s_val * v->pan_r;
            mono_sum += s_val;
        }

        /* Acoustic Guitar Wooden Body Resonators (105 Hz Helmholtz + 215 Hz Soundboard) */
        float body_a = 0.032f * mono_sum + 1.968f * g_eks_body_z1a - 0.972f * g_eks_body_z2a;
        g_eks_body_z2a = g_eks_body_z1a;
        g_eks_body_z1a = body_a;

        float body_b = 0.022f * mono_sum + 1.942f * g_eks_body_z1b - 0.952f * g_eks_body_z2b;
        g_eks_body_z2b = g_eks_body_z1b;
        g_eks_body_z1b = body_b;

        float body_gain = (g_aud_timbre == 3) ? 0.08f : 0.24f;
        mix_l += (body_a + body_b) * body_gain;
        mix_r += (body_a + body_b) * body_gain;

        /* Crisp woodblock metronome click */
        if (g_eks_click.active) {
            float p = (float)g_eks_click.pos / (float)g_eks_click.len;
            float env = expf(-p * 9.5f);
            float ph = (2.0f * (float)M_PI * g_eks_click.freq * (float)g_eks_click.pos) / (float)SAMPLE_RATE;
            float clk = (sinf(ph) + 0.35f * sinf(ph * 2.42f)) * env * g_eks_click.gain;
            mix_l += clk;
            mix_r += clk;
            if (++g_eks_click.pos >= g_eks_click.len) {
                g_eks_click.active = false;
            }
        }

        /* Route through shared Schroeder stereo room reverb + DC block + limiter */
        float ol, or_;
        rg35_sf2_fx_sample(&g_sf2, mix_l * 0.55f, mix_r * 0.55f, &ol, &or_);
        int sl = (int)(ol * 31500.0f);
        int sr = (int)(or_ * 31500.0f);
        if (sl >  32767) sl =  32767; else if (sl < -32768) sl = -32768;
        if (sr >  32767) sr =  32767; else if (sr < -32768) sr = -32768;
        dst_i16[f * 2 + 0] = (int16_t)sl;
        dst_i16[f * 2 + 1] = (int16_t)sr;
    }
}

static void audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int total_frames = len / (2 * AUDIO_CHANNELS);
    if (total_frames <= 0) return;

    audio_process_commands();

    int offset = 0;
    while (offset < total_frames) {
        int chunk = total_frames - offset;

        if (g_aud_playing && g_aud_piece.num_notes > 0) {
            double samples_per_eighth = ((double)SAMPLE_RATE * 30.0) / (double)g_aud_bpm;
            double until_tick = samples_per_eighth - g_aud_sample_accum;
            if (until_tick <= 0.5) {
                audio_advance_eighth_tick();
                g_aud_sample_accum -= samples_per_eighth;
                if (g_aud_sample_accum < 0.0) g_aud_sample_accum = 0.0;
                until_tick = samples_per_eighth - g_aud_sample_accum;
            }
            int to_next = (int)ceil(until_tick);
            if (to_next < 1) to_next = 1;
            if (chunk > to_next) chunk = to_next;
            g_aud_sample_accum += (double)chunk;
        }

        if (rg35_sf2_ready(&g_sf2)) {
            rg35_sf2_render_i16(&g_sf2, out + offset * 2, chunk);
        } else {
            eks_render_i16(out + offset * 2, chunk);
        }
        offset += chunk;
    }
}

static void audio_init_soundfont(void) {
    bool ok = rg35_sf2_init(&g_sf2, SAMPLE_RATE, 48, 0.22f, 1.38f);
    g_sf2_active = ok;
    if (ok) {
        audio_apply_timbre(g_timbre_idx);
        fprintf(stderr, "[sightread] SoundFont loaded: %s\n", g_sf2.loaded_path);
    } else {
        /* Still keep reverb parameters active for EKS fallback */
        g_sf2.reverb_wet  = 0.22f;
        g_sf2.master_gain = 1.35f;
        fprintf(stderr, "[sightread] No SoundFont found; using Floating-Point EKS fallback\n");
    }
}

static void audio_free_soundfont(void) {
    rg35_sf2_free(&g_sf2);
    g_sf2_active = false;
}

/* ============================================================================
 * 3. PLATFORM BACKEND: HOST_X11 vs TARGET_RG35XXSP (SDL2 + FBDEV + EVDEV + ALSA)
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
            void *pa = fn_new(NULL, "RG35XXSP SightRead", 1, NULL, "Guitar SightRead",
                              &ss, NULL, NULL, &err);
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

#else /* TARGET_RG35XXSP (Allwinner H700 aarch64: SDL2 dlopen + /dev/fb0 + evdev + ALSA) */

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
                fn_params(pcm, 2 /* S16_LE */, 3 /* RW_INTERLEAVED */,
                          AUDIO_CHANNELS, SAMPLE_RATE, 1, 25000);
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
    rg35xxsp_keepawake_init("sightread", g_sdl2_lib);
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
 * 4. NATIVE 640x480 ANTI-ALIASED DRAWING, MUSIC ENGRAVING & PNG SCREENSHOT
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

static void draw_filled_circle_aa(float cx, float cy, float radius,
                                  uint32_t fill_col, uint32_t border_col) {
    int x0 = (int)(cx - radius - 2.0f);
    int x1 = (int)(cx + radius + 2.0f);
    int y0 = (int)(cy - radius - 2.0f);
    int y1 = (int)(cy + radius + 2.0f);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float d  = sqrtf(dx * dx + dy * dy);
            if (d <= radius - 1.5f) {
                blend_pixel(x, y, fill_col, 255);
            } else if (d <= radius) {
                float t = (radius - d) / 1.5f;
                blend_pixel(x, y, fill_col, (uint8_t)(t * 255.0f));
                blend_pixel(x, y, border_col, (uint8_t)((1.0f - t * 0.35f) * 240.0f));
            } else if (d <= radius + 1.3f) {
                float a = (radius + 1.3f - d) / 1.3f;
                blend_pixel(x, y, border_col, (uint8_t)(a * 225.0f));
            }
        }
    }
}

/* Anti-aliased tilted musical oval notehead (concert engraving ~22 deg counter-clockwise tilt) */
static void draw_notehead_aa(float cx, float cy, uint32_t col) {
    const float rx = 9.2f;
    const float ry = 6.7f;
    const float c = 0.927f;  /* cos(-22 deg) */
    const float s = -0.375f; /* sin(-22 deg) */
    int x0 = (int)(cx - 12.0f), x1 = (int)(cx + 12.0f);
    int y0 = (int)(cy - 10.0f), y1 = (int)(cy + 10.0f);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float u = (dx * c - dy * s) / rx;
            float v = (dx * s + dy * c) / ry;
            float d = sqrtf(u * u + v * v);
            if (d <= 0.88f) {
                blend_pixel(x, y, col, 255);
            } else if (d <= 1.06f) {
                float a = (1.06f - d) / 0.18f;
                blend_pixel(x, y, col, (uint8_t)(a * 255.0f));
            }
        }
    }
}

/* Smooth anti-aliased slanted eighth-note beam */
static void draw_beam_aa(int x0, float y0, int x1, float y1, float thickness, uint32_t col) {
    if (x1 <= x0) return;
    int iy_min = (int)floorf((y0 < y1 ? y0 : y1) - 2.0f);
    int iy_max = (int)ceilf((y0 > y1 ? y0 : y1) + thickness + 2.0f);
    for (int x = x0; x <= x1; x++) {
        float t = (float)(x - x0) / (float)(x1 - x0);
        float yt = y0 + (y1 - y0) * t;
        float yb = yt + thickness;
        for (int y = iy_min; y <= iy_max; y++) {
            float fy = (float)y;
            float cov = 1.0f;
            if (fy < yt) {
                cov = 1.0f - (yt - fy);
            } else if (fy > yb) {
                cov = 1.0f - (fy - yb);
            }
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                blend_pixel(x, y, col, (uint8_t)(cov * 255.0f));
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

static void draw_text_centered(int font_id, int bx, int by, int bw, int bh,
                               const char *s, uint32_t color) {
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

/* Engraved Treble G-Clef (8vb), Sharp, and Flat drawing from FreeSerif bitmaps */
static void draw_engraved_clef(int x, int g_line_y, uint32_t col) {
    /* Row 84 of g_engraved_clef centers on the G-Line (Line 2) */
    int y0 = g_line_y - 84;
    for (int r = 0; r < ENGRAVED_CLEF_H; r++) {
        for (int c = 0; c < ENGRAVED_CLEF_W; c++) {
            uint8_t a = g_engraved_clef[r][c];
            if (a > 0) blend_pixel(x + c, y0 + r, col, a);
        }
    }
    /* Crisp '8' beneath clef curl indicating guitar 8vb octave transposition */
    draw_text_centered(FONT_UI_11, x + 4, y0 + ENGRAVED_CLEF_H - 1, ENGRAVED_CLEF_W - 8, 14,
                       "8", col);
}

static void draw_engraved_sharp(int cx, int cy, uint32_t col) {
    int x0 = cx - ENGRAVED_SHARP_W / 2;
    int y0 = cy - 16; /* Center opening of sharp */
    for (int r = 0; r < ENGRAVED_SHARP_H; r++) {
        for (int c = 0; c < ENGRAVED_SHARP_W; c++) {
            uint8_t a = g_engraved_sharp[r][c];
            if (a > 0) blend_pixel(x0 + c, y0 + r, col, a);
        }
    }
}

static void draw_engraved_flat(int cx, int cy, uint32_t col) {
    int x0 = cx - ENGRAVED_FLAT_W / 2;
    int y0 = cy - 22; /* Center of flat's lower loop */
    for (int r = 0; r < ENGRAVED_FLAT_H; r++) {
        for (int c = 0; c < ENGRAVED_FLAT_W; c++) {
            uint8_t a = g_engraved_flat[r][c];
            if (a > 0) blend_pixel(x0 + c, y0 + r, col, a);
        }
    }
}

/* Self-contained zero-dependency RFC 2083 PNG writer (RGB 24-bit, stored zlib blocks) */
static uint32_t png_crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= buf[i];
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

static void png_write_be32(FILE *f, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)(v)
    };
    fwrite(b, 1, 4, f);
}

static void png_write_chunk(FILE *f, const char type[4], const uint8_t *data, size_t len) {
    png_write_be32(f, (uint32_t)len);
    fwrite(type, 1, 4, f);
    if (len > 0) fwrite(data, 1, len, f);
    uint32_t crc = png_crc32_update(0, (const uint8_t *)type, 4);
    if (len > 0) crc = png_crc32_update(crc, data, len);
    png_write_be32(f, crc);
}

static bool save_png(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    ihdr[0] = (NATIVE_W >> 24) & 0xFF; ihdr[1] = (NATIVE_W >> 16) & 0xFF;
    ihdr[2] = (NATIVE_W >>  8) & 0xFF; ihdr[3] =  NATIVE_W        & 0xFF;
    ihdr[4] = (NATIVE_H >> 24) & 0xFF; ihdr[5] = (NATIVE_H >> 16) & 0xFF;
    ihdr[6] = (NATIVE_H >>  8) & 0xFF; ihdr[7] =  NATIVE_H        & 0xFF;
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 2; /* RGB */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_write_chunk(f, "IHDR", ihdr, 13);

    size_t row_len = 1 + (size_t)NATIVE_W * 3;
    size_t raw_len = row_len * (size_t)NATIVE_H;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { fclose(f); return false; }

    uint32_t s1 = 1, s2 = 0;
    for (int y = 0; y < NATIVE_H; y++) {
        uint8_t *dst = raw + (size_t)y * row_len;
        dst[0] = 0; /* filter: None */
        s1 = (s1 + 0) % 65521u;
        s2 = (s2 + s1) % 65521u;
        for (int x = 0; x < NATIVE_W; x++) {
            uint32_t c = g_fb[y * NATIVE_W + x];
            uint8_t r = (uint8_t)((c >> 16) & 0xFF);
            uint8_t g = (uint8_t)((c >>  8) & 0xFF);
            uint8_t b = (uint8_t)( c        & 0xFF);
            dst[1 + x * 3 + 0] = r;
            dst[1 + x * 3 + 1] = g;
            dst[1 + x * 3 + 2] = b;
            s1 = (s1 + r) % 65521u; s2 = (s2 + s1) % 65521u;
            s1 = (s1 + g) % 65521u; s2 = (s2 + s1) % 65521u;
            s1 = (s1 + b) % 65521u; s2 = (s2 + s1) % 65521u;
        }
    }

    size_t num_blocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + num_blocks * 5 + 4;
    uint8_t *zbuf = (uint8_t *)malloc(zlen);
    if (!zbuf) { free(raw); fclose(f); return false; }

    size_t zp = 0;
    zbuf[zp++] = 0x78; /* zlib CMF */
    zbuf[zp++] = 0x01; /* zlib FLG (no dict, fastest) */
    size_t rem = raw_len, off = 0;
    while (rem > 0) {
        size_t blen = (rem > 65535) ? 65535 : rem;
        uint8_t is_last = (rem <= 65535) ? 1 : 0;
        uint16_t len16 = (uint16_t)blen;
        uint16_t nlen16 = (uint16_t)(~len16);
        zbuf[zp++] = is_last;
        zbuf[zp++] = (uint8_t)(len16 & 0xFF);
        zbuf[zp++] = (uint8_t)(len16 >> 8);
        zbuf[zp++] = (uint8_t)(nlen16 & 0xFF);
        zbuf[zp++] = (uint8_t)(nlen16 >> 8);
        memcpy(zbuf + zp, raw + off, blen);
        zp  += blen;
        off += blen;
        rem -= blen;
    }
    uint32_t adler = (s2 << 16) | s1;
    zbuf[zp++] = (uint8_t)(adler >> 24);
    zbuf[zp++] = (uint8_t)(adler >> 16);
    zbuf[zp++] = (uint8_t)(adler >>  8);
    zbuf[zp++] = (uint8_t)(adler);

    png_write_chunk(f, "IDAT", zbuf, zp);
    png_write_chunk(f, "IEND", NULL, 0);

    free(zbuf);
    free(raw);
    fclose(f);
    return true;
}

/* ============================================================================
 * 5. PERSISTENT CONFIGURATION (.sightread_rg35xxsp.cfg)
 * ============================================================================ */

static char g_config_path[512] = "";

static void resolve_config_path(void) {
    const char *dirs[] = {
        "/userdata/roms/ports/sightread",
        "/userdata/system/configs",
        "/userdata",
        NULL
    };
    struct stat st;
    for (int i = 0; dirs[i]; i++) {
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && access(dirs[i], W_OK) == 0) {
            snprintf(g_config_path, sizeof(g_config_path), "%s/.sightread_rg35xxsp.cfg", dirs[i]);
            return;
        }
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "%s/.sightread_rg35xxsp.cfg", home);
    } else {
        snprintf(g_config_path, sizeof(g_config_path), "./.sightread_rg35xxsp.cfg");
    }
}

static void load_config(void) {
    if (g_config_path[0] == '\0') return;
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int val = 0;
        if (sscanf(line, "exercise_num=%d", &val) == 1 && val >= 1 && val <= 99) {
            g_exercise_num = val;
        } else if (sscanf(line, "pos_idx=%d", &val) == 1 && val >= 0 && val < NUM_POSITIONS) {
            g_pos_idx = val;
        } else if (sscanf(line, "key_idx=%d", &val) == 1 && val >= 0 && val < NUM_KEYS) {
            g_key_idx = val;
        } else if (sscanf(line, "bpm=%d", &val) == 1 && val >= MIN_BPM && val <= MAX_BPM) {
            g_bpm = val;
        } else if (sscanf(line, "timbre_idx=%d", &val) == 1 && val >= 0 && val < NUM_TIMBRES) {
            g_timbre_idx = val;
        }
    }
    fclose(f);
}

static void save_config(void) {
    if (g_config_path[0] == '\0') return;
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "exercise_num=%d\n", g_exercise_num);
    fprintf(f, "pos_idx=%d\n", g_pos_idx);
    fprintf(f, "key_idx=%d\n", g_key_idx);
    fprintf(f, "bpm=%d\n", g_bpm);
    fprintf(f, "timbre_idx=%d\n", g_timbre_idx);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void sync_piece_to_audio(int reset_note_idx) {
    generate_sight_piece(&g_piece, g_exercise_num, g_pos_idx, g_key_idx);
    if (reset_note_idx < 0 || reset_note_idx >= g_piece.num_notes) reset_note_idx = 0;
    g_ui_cur_note_idx = reset_note_idx;
    g_ui_playing = false;

    AudioCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type  = ACMD_LOAD_PIECE;
    cmd.arg0  = reset_note_idx;
    cmd.piece = g_piece;
    post_audio_cmd(&cmd);
}

/* ============================================================================
 * 6. NATIVE 640x480 RENDERING (TREBLE STAFF + 12-FRET ROSEWOOD NECK + HELP)
 * ============================================================================ */

/*
 * Treble Staff vertical geometry inside Prominent Staff Card (y: 48..322):
 *   16 px line spacing (8 px per diatonic step):
 *     Line 5 (F4 sounding / F5 written): y = 144 (step = 10)
 *     Line 4 (D4 sounding / D5 written): y = 160 (step = 8)
 *     Line 3 (B3 sounding / B4 written): y = 176 (step = 6)
 *     Line 2 (G3 sounding / G4 written): y = 192 (step = 4)
 *     Line 1 (E3 sounding / E4 written): y = 208 (step = 2)
 *   1st Ledger Line below staff (C3 guitar / Middle C): y = 224 (step = 0)
 *   Range: E2 (step -5 -> y = 264) through E5 (step +16 -> y = 96)
 */
static inline int staff_y_for_step(int diatonic_step) {
    return 224 - diatonic_step * 8;
}

static void render_help_modal(void) {
    fill_rect_alpha(0, 0, NATIVE_W, NATIVE_H, COL_BG, 218);

    int mx = 22, my = 10, mw = NATIVE_W - 44, mh = NATIVE_H - 20;
    fill_rect(mx, my, mw, mh, RGB(15, 23, 38));
    draw_border(mx, my, mw, mh, 2, RGB(96, 165, 250));
    draw_border(mx + 3, my + 3, mw - 6, mh - 6, 1, RGB(37, 99, 235));

    /* Header Bar */
    fill_rect(mx + 4, my + 4, mw - 8, 38, RGB(26, 36, 56));
    fill_rect(mx + 4, my + 42, mw - 8, 2, RGB(64, 82, 115));
    draw_pill(mx + 14, my + 8, 72, 28, RGB(37, 99, 235), RGB(147, 197, 253));
    draw_text_centered(FONT_UI_13, mx + 14, my + 8, 72, 28, "HELP", COL_WHITE);
    draw_text(FONT_UI_20, mx + 98, my + 10, "SIGHTREAD - QUICK GUIDE", RGB(250, 204, 21));

    /* Quick Guide Intro */
    fill_rect(mx + 4, my + 44, mw - 8, 22, RGB(19, 29, 48));
    fill_rect(mx + 4, my + 66, mw - 8, 1, RGB(48, 66, 98));
    draw_text_centered(FONT_UI_13, mx + 8, my + 44, mw - 16, 22,
                       "99 progressive 8vb guitar staff etudes across 7 keys & 5 neck positions.",
                       RGB(186, 230, 253));

    static const struct {
        const char *btn;
        const char *desc;
        uint32_t    col;
    } k_rows[10] = {
        { "A  (Space/Z)",  "Play / Pause entire 2-bar musical piece with metronome", COL_EMERALD },
        { "B  (Bksp)",     "Play single highlighted note on guitar & highlight neck", COL_CYAN    },
        { "LEFT / RIGHT",  "Move staff highlight cursor prev / next note & audition", COL_WHITE   },
        { "UP / DOWN",     "Adjust tempo (40 - 180 BPM; hold for fast repeat)",       COL_GOLD    },
        { "L1 / R1 (Q/E)", "Previous / Next progressive exercise (EX #01 .. #99)",    COL_GOLD    },
        { "SELECT (Tab)",  "Cycle 5 Neck Positions (OPEN, POS 2, POS 5, POS 7, 9)",   COL_ORANGE  },
        { "X  (X Key)",    "Cycle 7 Key Signatures (C, G, D, A, F, Bb, Eb)",          COL_PURPLE  },
        { "L2 / R2 (1/2)", "L2: Guitar Timbre (Steel/Nylon/Jazz/Overd) | R2: Level",  COL_CYAN    },
        { "START (Enter)", "Reset tempo to 84 BPM and return cursor to Note 1",       COL_MUTED   },
        { "Y / MENU (Esc)","Y: Close Help Overlay   |   MENU (Esc): Save & Quit",     COL_CORAL   }
    };

    int ky = my + 72;
    for (int i = 0; i < 10; i++) {
        int ry = ky + i * 35;
        draw_pill(mx + 14, ry, 144, 26, RGB(24, 38, 62), k_rows[i].col);
        draw_text_centered(FONT_UI_13, mx + 14, ry, 144, 26, k_rows[i].btn, k_rows[i].col);
        draw_text(FONT_UI_13, mx + 168, ry + 5, k_rows[i].desc, RGB(226, 232, 240));
    }

    /* Footer Bar */
    fill_rect(mx + 4, my + mh - 32, mw - 8, 28, RGB(12, 18, 30));
    fill_rect(mx + 4, my + mh - 33, mw - 8, 1, COL_BORDER);
    char foot[128];
    snprintf(foot, sizeof(foot),
             "8vb Guitar Staff  |  Audio: %s (%s)  |  [Y] or [B] Close",
             g_sf2_active ? "GeneralUser-GS SF2" : "EKS Synth",
             k_timbres[g_timbre_idx].full_name);
    draw_text_centered(FONT_UI_11, mx + 8, my + mh - 31, mw - 16, 26, foot, RGB(186, 230, 253));
}

static void render_ui(void) {
    const NeckPosition *pos = &k_positions[g_pos_idx];
    const KeySigDef *key    = &k_keys[g_key_idx];
    int cur_idx = g_ui_cur_note_idx;
    if (cur_idx < 0 || cur_idx >= g_piece.num_notes) cur_idx = 0;
    const StaffNote *active_note = &g_piece.notes[cur_idx];
    bool is_playing = g_ui_playing;

    fill_rect(0, 0, NATIVE_W, NATIVE_H, COL_BG);

    /* ================= 1. TOP HEADER BAR (y: 0..42) ================= */
    fill_rect(0, 0, NATIVE_W, 42, COL_HEADER);
    fill_rect(0, 40, NATIVE_W, 2, COL_BORDER);

    /* Exercise # & Progressive Level Pill */
    uint32_t lvl_fills[5] = {
        RGB(14, 116, 144), RGB(14, 116, 144), RGB(14, 132, 92), RGB(180, 96, 14), RGB(109, 40, 217)
    };
    uint32_t lvl_bdrs[5] = {
        COL_CYAN, COL_CYAN, COL_EMERALD, COL_ORANGE, COL_PURPLE
    };
    int lvl = (g_piece.level >= 1 && g_piece.level <= 4) ? g_piece.level : 1;
    draw_pill(8, 6, 122, 30, lvl_fills[lvl], lvl_bdrs[lvl]);
    char ex_str[32];
    snprintf(ex_str, sizeof(ex_str), "EX #%02d  LVL%d", g_piece.exercise_num, lvl);
    draw_text_centered(FONT_UI_13, 8, 6, 122, 30, ex_str, COL_WHITE);

    /* Neck Position Pill */
    draw_pill(136, 6, 134, 30, COL_PANEL_ALT, COL_CYAN);
    draw_text_centered(FONT_UI_13, 136, 6, 134, 30, pos->short_name, RGB(186, 230, 253));

    /* Key Signature Pill */
    draw_pill(276, 6, 82, 30, COL_PANEL_ALT, COL_GOLD);
    draw_text_centered(FONT_UI_13, 276, 6, 82, 30, key->badge, COL_GOLD);

    /* BPM Pill */
    draw_pill(364, 6, 86, 30, COL_PANEL_ALT, COL_BORDER);
    char bpm_str[24];
    snprintf(bpm_str, sizeof(bpm_str), "%d BPM", g_bpm);
    draw_text_centered(FONT_UI_13, 364, 6, 86, 30, bpm_str, COL_WHITE);

    /* Audio Engine & Timbre Pill */
    draw_pill(456, 6, 104, 30, RGB(18, 46, 58), g_sf2_active ? COL_EMERALD : COL_CYAN);
    char tone_str[24];
    snprintf(tone_str, sizeof(tone_str), "%s:%s",
             g_sf2_active ? "SF2" : "EKS", k_timbres[g_timbre_idx].short_name);
    draw_text_centered(FONT_UI_11, 456, 6, 104, 30, tone_str, RGB(209, 250, 229));

    /* Y: Help Pill */
    draw_pill(566, 6, 66, 30, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, 566, 6, 66, 30, "Y:Help", RGB(254, 240, 138));

    /* ================= 2. PROMINENT TREBLE CLEF STAFF CARD (y: 48..322) ================= */
    fill_rect(8, 48, 624, 274, COL_PANEL);
    draw_border(8, 48, 624, 274, 2, COL_BORDER);

    /* Top Info Strip inside Staff Card (y: 50..80) */
    fill_rect(10, 50, 620, 30, COL_PANEL_ALT);
    fill_rect(10, 80, 620, 1, COL_BORDER);

    char info_left[64];
    snprintf(info_left, sizeof(info_left), "NOTE %d/%d: %s (%s)",
             cur_idx + 1, g_piece.num_notes,
             active_note->label,
             (active_note->dur_eighths == 1) ? "1/8 Eighth" : "1/4 Quarter");
    draw_text(FONT_UI_13, 18, 57, info_left, COL_GOLD);

    char info_mid[48];
    snprintf(info_mid, sizeof(info_mid), "%s  |  4/4  |  %s",
             key->name, is_playing ? "PLAYING" : "PAUSED");
    draw_text_centered(FONT_UI_13, 252, 50, 186, 30, info_mid,
                       is_playing ? COL_EMERALD : COL_MUTED);

    char info_right[48];
    if (active_note->fret == 0) {
        snprintf(info_right, sizeof(info_right), "Str %d (%c) Open [0]",
                 6 - active_note->str, k_str_labels[active_note->str]);
    } else {
        snprintf(info_right, sizeof(info_right), "Str %d (%c) Fr %d [Fg %d]",
                 6 - active_note->str, k_str_labels[active_note->str],
                 active_note->fret, active_note->finger);
    }
    draw_text_right(FONT_UI_13, 622, 57, info_right, COL_CYAN);

    /* Draw the 5 Prominent Treble Staff Lines (y = 144, 160, 176, 192, 208) */
    int staff_x0 = 22;
    int staff_x1 = 618;
    for (int l = 0; l < 5; l++) {
        int ly = 144 + l * 16;
        fill_rect(staff_x0, ly, staff_x1 - staff_x0, 2, COL_STAFF_LINE);
    }

    /* Left Staff Barline & Engraved Treble Clef (8vb) */
    fill_rect(staff_x0, 144, 3, 66, COL_STAFF_LINE);
    draw_engraved_clef(staff_x0 + 8, 192, COL_GOLD);

    /* Key Signature Sharps / Flats (engraved FreeSerif symbols) */
    int ksig_x = staff_x0 + 62;
    if (key->sharps_or_flats > 0) {
        for (int s = 0; s < key->sharps_or_flats; s++) {
            int sy = staff_y_for_step(k_sharp_staff_steps[s]);
            draw_engraved_sharp(ksig_x + s * 15, sy, COL_GOLD);
        }
        ksig_x += key->sharps_or_flats * 15 + 8;
    } else if (key->sharps_or_flats < 0) {
        int nflats = -key->sharps_or_flats;
        for (int b = 0; b < nflats; b++) {
            int sy = staff_y_for_step(k_flat_staff_steps[b]);
            draw_engraved_flat(ksig_x + b * 14, sy, COL_GOLD);
        }
        ksig_x += nflats * 14 + 8;
    } else {
        ksig_x += 6;
    }

    /* Bold 4/4 Time Signature aligned to top and bottom halves of staff */
    draw_text_centered(FONT_TITLE_28, ksig_x - 2, 145, 28, 32, "4", COL_WHITE);
    draw_text_centered(FONT_TITLE_28, ksig_x - 2, 177, 28, 32, "4", COL_WHITE);

    /* Calculate Horizontal X Positions across 2 Measures (16 eighth-note slots) */
    int notes_x0 = ksig_x + 34;
    int notes_x1 = 606;
    int bar_w    = (notes_x1 - notes_x0) / 2;

    /* Middle Measure Barline & Double Final Barline */
    int mid_bar_x = notes_x0 + bar_w;
    fill_rect(mid_bar_x, 144, 2, 66, COL_BAR_LINE);
    fill_rect(staff_x1 - 7, 144, 2, 66, COL_STAFF_LINE);
    fill_rect(staff_x1 - 3, 144, 4, 66, COL_STAFF_LINE);

    /* Subtle bar numbers ("1", "2") above the staff */
    draw_text(FONT_UI_11, notes_x0 + 4, 90, "Bar 1", COL_DIM);
    draw_text(FONT_UI_11, mid_bar_x + 8, 90, "Bar 2", COL_DIM);

    int note_cx[MAX_PIECE_NOTES];
    int note_cy[MAX_PIECE_NOTES];

    for (int i = 0; i < g_piece.num_notes; i++) {
        const StaffNote *n = &g_piece.notes[i];
        int bar_start_x  = notes_x0 + n->bar_idx * bar_w + 18;
        int usable_bar_w = bar_w - 26;
        note_cx[i] = bar_start_x + (n->eighth_in_bar * usable_bar_w) / 8 + 8;
        note_cy[i] = staff_y_for_step(n->diatonic_step);
    }

    /* Interactive Highlight Column for Selected / Playing Note */
    int sel_x = note_cx[cur_idx];
    uint32_t sel_col = is_playing ? COL_EMERALD : COL_GOLD;
    fill_rect_alpha(sel_x - 18, 86, 37, 228, sel_col, 38);
    draw_border(sel_x - 19, 85, 39, 230, 2, sel_col);

    /* Draw Ledger Lines, Anti-Aliased Noteheads, Stems, Beams, and Note Labels */
    for (int i = 0; i < g_piece.num_notes; i++) {
        const StaffNote *n = &g_piece.notes[i];
        int cx = note_cx[i];
        int cy = note_cy[i];
        bool is_sel = (i == cur_idx);

        /* 1. Ledger lines below bottom staff line (step <= 0: C3, A2, F2) */
        if (n->diatonic_step <= 0) {
            for (int st = 0; st >= n->diatonic_step; st -= 2) {
                int ly = staff_y_for_step(st);
                fill_rect(cx - 14, ly, 29, 2, COL_STAFF_LINE);
            }
        }
        /* 2. Ledger lines above top staff line (step >= 12: A4, C5, E5) */
        if (n->diatonic_step >= 12) {
            for (int st = 12; st <= n->diatonic_step; st += 2) {
                int ly = staff_y_for_step(st);
                fill_rect(cx - 14, ly, 29, 2, COL_STAFF_LINE);
            }
        }

        /* 3. Stem direction (beamed pairs share average height direction) */
        int ref_step = n->diatonic_step;
        if (n->beam_with_next && i + 1 < g_piece.num_notes) {
            ref_step = (n->diatonic_step + g_piece.notes[i + 1].diatonic_step) / 2;
        } else if (n->beam_from_prev && i > 0) {
            ref_step = (g_piece.notes[i - 1].diatonic_step + n->diatonic_step) / 2;
        }
        bool stem_up = (ref_step < 6);

        uint32_t ncol = is_sel ? sel_col : ((n->dur_eighths == 1) ? COL_CYAN : COL_WHITE);
        draw_notehead_aa((float)cx, (float)cy, ncol);

        int stem_x     = stem_up ? (cx + 7) : (cx - 9);
        int stem_y_end = stem_up ? (cy - 44) : (cy + 44);
        int sy0        = (cy < stem_y_end) ? cy : stem_y_end;
        int sh         = abs(stem_y_end - cy);
        fill_rect(stem_x, sy0, 3, sh, ncol);

        /* 4. Connecting slanted beam for eighth-note pairs */
        if (n->beam_with_next && i + 1 < g_piece.num_notes) {
            int cx2 = note_cx[i + 1];
            int cy2 = note_cy[i + 1];
            int stem_x2     = stem_up ? (cx2 + 7) : (cx2 - 9);
            int stem_y2_end = stem_up ? (cy2 - 44) : (cy2 + 44);
            bool next_sel   = (i + 1 == cur_idx);
            uint32_t bcol   = (is_sel || next_sel) ? sel_col : COL_CYAN;
            float by0 = stem_up ? (float)stem_y_end : (float)(stem_y_end - 6);
            float by1 = stem_up ? (float)stem_y2_end : (float)(stem_y2_end - 6);
            draw_beam_aa(stem_x, by0, stem_x2 + 2, by1, 6.0f, bcol);
        }

        /* 5. Note name along bottom of Staff Card (avoid crowding closely paired 8ths or cursor box) */
        bool show_lbl = is_sel;
        if (!show_lbl && abs(cx - sel_x) >= 36) {
            if (!n->beam_with_next && !n->beam_from_prev) {
                show_lbl = true;
            } else if (n->beam_with_next) {
                show_lbl = true;
            } else if (n->beam_from_prev) {
                if (i > 0 && (cx - note_cx[i - 1]) >= 30) show_lbl = true;
            }
        }
        if (show_lbl) {
            draw_text_centered(FONT_UI_11, cx - 18, 294, 36, 18, n->label,
                               is_sel ? sel_col : COL_MUTED);
        }
    }

    /* ================= 3. 12-FRET ROSEWOOD GUITAR NECK MAP (y: 328..434) ================= */
    fill_rect(8, 328, 624, 106, COL_PANEL);
    draw_border(8, 328, 624, 106, 2, COL_BORDER);

    int neck_x0 = 60;
    int fret_w  = 46; /* 12 frets * 46px = 552px (x: 60..612) */
    int neck_w  = fret_w * 12;
    int neck_y0 = 349;
    int neck_h  = 78;
    int str_sp  = 13; /* 6 strings at y = 355, 368, 381, 394, 407, 420 */

    /* Rosewood fingerboard + outer binding */
    fill_rect(neck_x0, neck_y0, neck_w, neck_h, COL_NECK_WOOD);

    /* Highlight active Neck Position fret span */
    int pos_f0 = (pos->min_fret == 0) ? 1 : pos->min_fret;
    int pos_f1 = pos->max_fret;
    int pos_x  = neck_x0 + (pos_f0 - 1) * fret_w;
    int pos_w  = (pos_f1 - pos_f0 + 1) * fret_w;
    fill_rect(pos_x, neck_y0, pos_w, neck_h, COL_NECK_POS);
    fill_rect(pos_x, neck_y0, pos_w, 2, COL_CYAN);
    fill_rect(pos_x, neck_y0 + neck_h - 2, pos_w, 2, COL_CYAN);

    /* Open string ('0') header label */
    draw_text_centered(FONT_UI_11, 32, 331, 24, 15, "0",
                       (pos->min_fret == 0) ? COL_CYAN : COL_DIM);

    /* Fret Numbers (1..12), Bone Nut, Nickel Fret Wires & Pearloid Inlays */
    for (int f = 0; f <= 12; f++) {
        int fx = neck_x0 + f * fret_w;
        if (f == 0) {
            fill_rect(fx - 3, neck_y0 - 1, 5, neck_h + 2, COL_BONE_NUT);
        } else {
            fill_rect(fx, neck_y0, 2, neck_h, COL_FRET_WIRE);
            int fcx = fx - fret_w / 2;
            bool in_pos = (f >= pos->min_fret && f <= pos->max_fret);
            char fn_str[8];
            snprintf(fn_str, sizeof(fn_str), "%d", f);
            draw_text_centered(FONT_UI_11, fcx - 12, 331, 24, 15, fn_str,
                               in_pos ? COL_CYAN : COL_DIM);

            if (f == 3 || f == 5 || f == 7 || f == 9) {
                draw_filled_circle_aa((float)fcx, (float)(neck_y0 + neck_h / 2), 3.8f,
                                      RGB(86, 78, 70), RGB(114, 105, 96));
            } else if (f == 12) {
                draw_filled_circle_aa((float)fcx, (float)(neck_y0 + 21), 3.6f,
                                      RGB(86, 78, 70), RGB(114, 105, 96));
                draw_filled_circle_aa((float)fcx, (float)(neck_y0 + neck_h - 21), 3.6f,
                                      RGB(86, 78, 70), RGB(114, 105, 96));
            }
        }
    }

    /* 6 Horizontal Guitar Strings + Left String Names ('e B G D A E') */
    for (int row = 0; row < 6; row++) {
        int str_idx = 5 - row;
        int sy = neck_y0 + 6 + row * str_sp;
        char slbl[2] = { k_str_labels[str_idx], '\0' };
        draw_text_centered(FONT_UI_13, 14, sy - 8, 18, 16, slbl,
                           (str_idx == active_note->str) ? COL_GOLD : COL_MUTED);

        uint32_t scol = (str_idx <= 2) ? RGB(196, 174, 142) : RGB(212, 222, 234);
        fill_rect(neck_x0 + 2, sy, neck_w - 2, (str_idx <= 1) ? 2 : 1, scol);
    }

    /* 1. Draw all notes used in this exercise on the fretboard (clean cyan-rimmed dots) */
    for (int i = 0; i < g_piece.num_notes; i++) {
        const StaffNote *n = &g_piece.notes[i];
        if (n->str == active_note->str && n->fret == active_note->fret) continue;
        int sy  = neck_y0 + 6 + (5 - n->str) * str_sp;
        int fcx = (n->fret == 0) ? 44 : (neck_x0 + n->fret * fret_w - fret_w / 2);
        draw_filled_circle_aa((float)fcx, (float)sy, 6.2f, RGB(20, 44, 68), COL_CYAN);
    }

    /* 2. Brightly highlight the currently selected/playing note + finger number (0..4) */
    {
        int sy  = neck_y0 + 6 + (5 - active_note->str) * str_sp;
        int fcx = (active_note->fret == 0) ? 44 : (neck_x0 + active_note->fret * fret_w - fret_w / 2);
        draw_filled_circle_aa((float)fcx, (float)sy, 9.2f, sel_col, COL_WHITE);
        char fg_str[8];
        snprintf(fg_str, sizeof(fg_str), "%d", active_note->finger);
        draw_text_centered(FONT_UI_11, fcx - 9, sy - 7, 18, 15, fg_str, RGB(15, 23, 42));
    }

    /* ================= 4. BOTTOM CONTROLS HINT BAR (y: 440..480) ================= */
    fill_rect(0, 440, NATIVE_W, 40, COL_HEADER);
    fill_rect(0, 440, NATIVE_W, 2, COL_BORDER);

    draw_pill(8, 446, 98, 28,
              is_playing ? RGB(180, 92, 12) : RGB(14, 132, 92),
              is_playing ? COL_GOLD : COL_EMERALD);
    draw_text_centered(FONT_UI_13, 8, 446, 98, 28,
                       is_playing ? "[A] PAUSE" : "[A] PLAY", COL_WHITE);

    draw_text_centered(FONT_UI_11, 110, 446, NATIVE_W - 116, 28,
                       "[B] 1-Note  [L/R] Note  [U/D] BPM  [L1/R1] Ex  [SEL] Pos  [X] Key  [L2] Tone  [R2] Lvl",
                       RGB(214, 224, 238));

    if (g_show_help) {
        render_help_modal();
    }
}

/* ============================================================================
 * 7. SCREENSHOT GENERATOR & MAIN ENTRY POINT
 * ============================================================================ */

static int run_screenshot_mode(const char *dir) {
    char path[512];
    audio_init_soundfont();

    /* 1. Open Position (EX #01 LVL1, C Maj, Note 3 highlighted, Paused) */
    g_exercise_num = 1; g_pos_idx = 0; g_key_idx = 0; g_bpm = 84; g_timbre_idx = 0;
    g_show_help = false;
    generate_sight_piece(&g_piece, g_exercise_num, g_pos_idx, g_key_idx);
    g_ui_cur_note_idx = 2;
    g_ui_playing = false;
    render_ui();
    snprintf(path, sizeof(path), "%s/sightread_open.png", dir);
    if (!save_png(path)) { fprintf(stderr, "Failed to write %s\n", path); return 1; }
    printf("Saved %s\n", path);

    /* 2. 5th Position (EX #07 LVL2, G (1#), Playing, Nylon timbre) */
    g_exercise_num = 7; g_pos_idx = 2; g_key_idx = 1; g_bpm = 96; g_timbre_idx = 1;
    g_show_help = false;
    generate_sight_piece(&g_piece, g_exercise_num, g_pos_idx, g_key_idx);
    g_ui_cur_note_idx = 5;
    g_ui_playing = true;
    render_ui();
    snprintf(path, sizeof(path), "%s/sightread_pos5.png", dir);
    if (!save_png(path)) { fprintf(stderr, "Failed to write %s\n", path); return 1; }
    printf("Saved %s\n", path);

    /* 3. Advanced 7th Position (EX #16 LVL4, Eb (3b), Clean Jazz timbre) */
    g_exercise_num = 16; g_pos_idx = 3; g_key_idx = 6; g_bpm = 112; g_timbre_idx = 2;
    g_show_help = false;
    generate_sight_piece(&g_piece, g_exercise_num, g_pos_idx, g_key_idx);
    g_ui_cur_note_idx = 4;
    g_ui_playing = false;
    render_ui();
    snprintf(path, sizeof(path), "%s/sightread_advanced.png", dir);
    if (!save_png(path)) { fprintf(stderr, "Failed to write %s\n", path); return 1; }
    printf("Saved %s\n", path);

    /* 4. English-only Help Modal */
    g_show_help = true;
    render_ui();
    snprintf(path, sizeof(path), "%s/sightread_help.png", dir);
    if (!save_png(path)) { fprintf(stderr, "Failed to write %s\n", path); return 1; }
    printf("Saved %s\n", path);

    audio_free_soundfont();
    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0) {
            return verify_sightread_generator(true) ? 0 : 1;
        } else if ((strcmp(argv[i], "--screenshot") == 0 ||
                    strcmp(argv[i], "--screenshot-states") == 0) && i + 1 < argc) {
            if (!verify_sightread_generator(false)) {
                fprintf(stderr, "Fatal: Sight-reading generator verification failed.\n");
                return 1;
            }
            return run_screenshot_mode(argv[i + 1]);
        }
    }

    if (!verify_sightread_generator(false)) {
        fprintf(stderr, "Fatal: Sight-reading generator verification failed.\n");
        return 1;
    }

    resolve_config_path();
    load_config();
    audio_init_soundfont();

    generate_sight_piece(&g_piece, g_exercise_num, g_pos_idx, g_key_idx);
    g_aud_piece    = g_piece;
    g_aud_bpm      = g_bpm;
    g_aud_timbre   = g_timbre_idx;
    g_aud_cur_note = 0;
    g_ui_cur_note_idx = 0;
    g_ui_playing   = false;

    if (!platform_init("RG35XXSP SightRead (Guitar Staff Trainer)")) {
        fprintf(stderr, "Failed to initialize platform backend\n");
        audio_free_soundfont();
        return 1;
    }

    uint32_t hold_start_ms = 0, last_repeat_ms = 0;
    input_state_t input;

    while (!g_sig_quit && platform_poll_input(&input)) {
        uint32_t now_ms = get_ticks_ms();
        uint32_t p = input.pressed;

        /* MENU / Esc: SOLE Quit Key */
        if (p & BTN_MENU) {
            break;
        }

        /* Y: Toggle Help Overlay */
        if (p & BTN_Y) {
            g_show_help = !g_show_help;
            p &= ~BTN_Y;
        }

        if (g_show_help) {
            if (p & (BTN_B | BTN_A | BTN_SELECT)) {
                g_show_help = false;
            }
        } else {
            /* A: Play / Pause entire piece */
            if (p & BTN_A) {
                bool next_play = !g_ui_playing;
                post_simple_cmd(ACMD_SET_PLAYING, next_play ? 1 : 0, 0);
            }
            /* B: Play single highlighted note */
            if (p & BTN_B) {
                post_simple_cmd(ACMD_PLAY_SINGLE, g_ui_cur_note_idx, 0);
            }
            /* LEFT / RIGHT: Move note highlight cursor & audition */
            if (p & BTN_LEFT) {
                int idx = (g_ui_cur_note_idx + g_piece.num_notes - 1) % g_piece.num_notes;
                post_simple_cmd(ACMD_PLAY_SINGLE, idx, 0);
            }
            if (p & BTN_RIGHT) {
                int idx = (g_ui_cur_note_idx + 1) % g_piece.num_notes;
                post_simple_cmd(ACMD_PLAY_SINGLE, idx, 0);
            }
            /* L1 / R1: Prev / Next Exercise (#01..#99) */
            if (p & BTN_L1) {
                g_exercise_num = (g_exercise_num > 1) ? (g_exercise_num - 1) : 99;
                sync_piece_to_audio(0);
                save_config();
            }
            if (p & BTN_R1) {
                g_exercise_num = (g_exercise_num < 99) ? (g_exercise_num + 1) : 1;
                sync_piece_to_audio(0);
                save_config();
            }
            /* SELECT: Cycle 5 Neck Positions */
            if (p & BTN_SELECT) {
                g_pos_idx = (g_pos_idx + 1) % NUM_POSITIONS;
                sync_piece_to_audio(0);
                save_config();
            }
            /* X: Cycle 7 Key Signatures */
            if (p & BTN_X) {
                g_key_idx = (g_key_idx + 1) % NUM_KEYS;
                sync_piece_to_audio(0);
                save_config();
            }
            /* L2: Cycle Guitar Timbre (Steel / Nylon / Jazz / Overdrive) */
            if (p & BTN_L2) {
                g_timbre_idx = (g_timbre_idx + 1) % NUM_TIMBRES;
                post_simple_cmd(ACMD_SET_TIMBRE, g_timbre_idx, 1);
                save_config();
            }
            /* R2: Quick Difficulty Tier Jump (LVL1 #01 -> LVL2 #04 -> LVL3 #09 -> LVL4 #16) */
            if (p & BTN_R2) {
                if (g_exercise_num <= 3)       g_exercise_num = 4;
                else if (g_exercise_num <= 8)  g_exercise_num = 9;
                else if (g_exercise_num <= 15) g_exercise_num = 16;
                else                           g_exercise_num = 1;
                sync_piece_to_audio(0);
                save_config();
            }
            /* START: Reset BPM to 84 and cursor to Note 1 (NEVER quits!) */
            if (p & BTN_START) {
                g_bpm = 84;
                post_simple_cmd(ACMD_SET_BPM, g_bpm, 0);
                post_simple_cmd(ACMD_SET_CURSOR, 0, 0);
                save_config();
            }
            /* UP / DOWN: BPM adjustment with smooth continuous hold repeat */
            if (p & BTN_UP) {
                if (g_bpm < MAX_BPM) {
                    g_bpm++;
                    post_simple_cmd(ACMD_SET_BPM, g_bpm, 0);
                    save_config();
                }
                hold_start_ms = last_repeat_ms = now_ms;
            }
            if (p & BTN_DOWN) {
                if (g_bpm > MIN_BPM) {
                    g_bpm--;
                    post_simple_cmd(ACMD_SET_BPM, g_bpm, 0);
                    save_config();
                }
                hold_start_ms = last_repeat_ms = now_ms;
            }
            if ((input.held & (BTN_UP | BTN_DOWN)) && (now_ms >= hold_start_ms + 320)) {
                if (now_ms >= last_repeat_ms + 55) {
                    bool changed = false;
                    if ((input.held & BTN_UP) && g_bpm < MAX_BPM) { g_bpm++; changed = true; }
                    if ((input.held & BTN_DOWN) && g_bpm > MIN_BPM) { g_bpm--; changed = true; }
                    if (changed) {
                        post_simple_cmd(ACMD_SET_BPM, g_bpm, 0);
                    }
                    last_repeat_ms = now_ms;
                }
            }
        }

        render_ui();
        platform_present();
        delay_ms(16);
    }

    save_config();
    platform_shutdown();
    audio_free_soundfont();
    return 0;
}
