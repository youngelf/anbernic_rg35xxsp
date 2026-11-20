/*
 * licktrainer.c - Guitar Speed-Builder Practice Routine & Lick Coach
 *                 for Anbernic RG 35xxSP (640x480 IPS, Allwinner H700 aarch64)
 *                 & Linux x86_64 Desktop (X11)
 *
 * Features:
 *   - All 33 curated guitar exercises across 6 categories:
 *     1. PENTATONIC (#01-#05), 2. LEGATO/ARP (#06-#10), 3. ALT PICK (#11-#15),
 *     4. BLUES/ROCK (#16-#20), 5. SPIDER (#21-#25), 6. JAZZ 7THS (#26-#33).
 *   - Native 640x480 32-bit XRGB8888 UI with anti-aliased multi-size fonts:
 *     * Top header bar + Jazz 7ths 4-chord measure progression / Speed-Builder Ladder
 *     * Wide 6-string guitar tab staff with chord-tone degrees (1,3,5,7) or picking
 *       (D/U/H/P//), live playhead highlight, and left-hand fingerings (1-4)
 *     * Zoomed 8-fret rosewood fretboard with active 4-note chord arpeggio highlight
 *       + right-side live note / Roman numeral & chord readout card
 *   - 100% Background-Thread Audio & Sample-Accurate Sequencer:
 *     * Studio SoundFont 2.01 engine (GeneralUser-GS.sf2 via rg35xxsp_sf2.h)
 *       with 4 guitar timbres (Steel, Nylon, Clean Jazz, Overdrive) + woodblock
 *       & rimshot metronome clicks + Schroeder stereo room reverb.
 *     * Upgraded 44.1 kHz floating-point Extended Karplus-Strong (EKS) physical
 *       modeling fallback (all-pass fractional delay, legato articulation shaping,
 *       wooden body biquads, stereo room reverb) when no .sf2 is present.
 *   - Strict rg35xxsp_input.h compliance:
 *     * MENU / Esc    : Save settings & Quit (sole exit key)
 *     * Y             : Toggle English-only Help & Practice Guide modal
 *     * A / Space / Z : Play / Pause exercise loop (with 4-beat count-in)
 *     * B / Backspace : Cycle Audio Mix (TAB+CLK / CLK ONLY / TAB ONLY)
 *     * SELECT / Tab  : Cycle Speed-Builder BPM Ladder (OFF/+2/+5/+5/+10)
 *     * X             : Transpose Musical Key / Neck Fret Position (E..Eb)
 *     * L1 / R1       : Previous / Next Category (1..6)
 *     * LEFT / RIGHT  : Previous / Next Lick within Category
 *     * UP / DOWN     : Adjust BPM (40..240, continuous hold repeat)
 *     * L2 / 1        : Cycle Guitar Timbre (Steel / Nylon / Jazz / Overdrive)
 *     * R2 / 2        : Reset BPM to lick default & reset Speed Ladder
 *     * START / Enter : Reset playhead to Step 1 & base BPM (never quits)
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

#include "licktrainer_font.h"

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
#define COL_NECK_WOOD   RGB(42, 29, 22)
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
 * 1. GUITAR EXERCISE & LICK DATA MODEL (6 CATEGORIES, 33 TOTAL EXERCISES)
 * ============================================================================ */

#define NUM_CATEGORIES 6
#define TOTAL_LICKS    33
#define MAX_STEPS      16

typedef struct {
    int8_t str;      /* 0=6th(E) .. 5=1st(e), -1=rest */
    int8_t fret_off; /* Offset relative to anchor fret */
    int8_t finger;   /* Left-hand finger: 1=Index, 2=Middle, 3=Ring, 4=Pinky */
    char   pick;     /* 'D'=Down, 'U'=Up, 'H'=Hammer-on, 'P'=Pull-off, '/'=Slide */
} LickStep;

typedef struct {
    int         cat_idx;
    const char *title;
    const char *subtitle;
    int         num_steps;       /* 16 for 16th-note 4/4, 12 for triplets */
    int         steps_per_beat;  /* 4 for 16th notes, 3 for triplets */
    int         default_bpm;
    uint16_t    allowed_pc_mask; /* Allowed pitch-class bitmask relative to root (0..11) */
    int8_t      beat_deg[4];     /* Diatonic degree (1..7) per beat for Jazz 7ths (0 = standard) */
    LickStep    steps[MAX_STEPS];
} LickDef;

#define PC_MINOR_PENTA    ((1u<<0)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<10))
#define PC_NATURAL_MINOR  ((1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10))
#define PC_HARMONIC_MINOR ((1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<11))
#define PC_MAJOR_SCALE    ((1u<<0)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<11))
#define PC_MINOR_TRIAD    ((1u<<0)|(1u<<3)|(1u<<7))
#define PC_MIN7_ARP       ((1u<<0)|(1u<<3)|(1u<<7)|(1u<<10))
#define PC_DIM7_ARP       ((1u<<0)|(1u<<3)|(1u<<6)|(1u<<9))
#define PC_BLUES          ((1u<<0)|(1u<<3)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<10))
#define PC_HYBRID_BLUES   ((1u<<0)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<9)|(1u<<10))
#define PC_CHROMATIC      0x0FFFu

static const char *k_category_names[NUM_CATEGORIES] = {
    "PENTATONIC",
    "LEGATO/ARP",
    "ALT PICK",
    "BLUES/ROCK",
    "SPIDER",
    "JAZZ 7THS"
};

typedef struct {
    const char *roman;       /* "Imaj7", "iim7", "iiim7", "IVmaj7", "V7", "vim7", "viim7b5" */
    const char *roman_short; /* "I", "ii", "iii", "IV", "V7", "vi", "vii" */
    const char *qual_suffix; /* "maj7", "m7", "m7", "maj7", "7", "m7", "m7b5" */
    uint8_t     tones_pc[4]; /* Semitone offsets from Major key root for {1, 3, 5, 7} */
    const char *tone_lbl[4]; /* Interval labels for {1, 3, 5, 7} of this chord */
} Diatonic7thInfo;

static const Diatonic7thInfo k_diatonic_7ths[8] = {
    { "",        "",    "",     {0,  0, 0,  0}, {"",  "",   "",   ""  } },
    { "Imaj7",   "I",   "maj7", {0,  4, 7, 11}, {"1", "3",  "5",  "7" } },
    { "iim7",    "ii",  "m7",   {2,  5, 9,  0}, {"1", "b3", "5",  "b7"} },
    { "iiim7",   "iii", "m7",   {4,  7, 11, 2}, {"1", "b3", "5",  "b7"} },
    { "IVmaj7",  "IV",  "maj7", {5,  9, 0,  4}, {"1", "3",  "5",  "7" } },
    { "V7",      "V7",  "7",    {7, 11, 2,  5}, {"1", "3",  "5",  "b7"} },
    { "vim7",    "vi",  "m7",   {9,  0, 4,  7}, {"1", "b3", "5",  "b7"} },
    { "viim7b5", "vii", "m7b5", {11, 2, 5,  9}, {"1", "b3", "b5", "b7"} }
};

#define NUM_KEYS 12
typedef struct {
    const char *name;
    int         anchor_fret;
} KeyAnchor;

static const KeyAnchor k_keys[NUM_KEYS] = {
    { "E",  0  },
    { "F",  1  },
    { "F#", 2  },
    { "G",  3  },
    { "G#", 4  },
    { "A",  5  }, /* Default: Key of A at 5th fret */
    { "Bb", 6  },
    { "B",  7  },
    { "C",  8  },
    { "C#", 9  },
    { "D",  10 },
    { "Eb", 11 }
};

static const char *k_note_names[12] = {
    "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"
};

static const int  k_open_midi[6]  = { 40, 45, 50, 55, 59, 64 };
static const char k_str_labels[6] = { 'E', 'A', 'D', 'G', 'B', 'e' };

static const int k_cat_start[NUM_CATEGORIES] = { 0, 5, 10, 15, 20, 25 };
static const int k_cat_count[NUM_CATEGORIES] = { 5, 5,  5,  5,  5,  8 };

static const LickDef k_licks[TOTAL_LICKS] = {
    /* ==================== 1. PENTATONIC LICKS (#01..#05) ==================== */
    {
        0, "GILBERT 4-NOTE SEQ", "Paul Gilbert Box 1 Pentatonic Burner", 16, 4, 100, PC_MINOR_PENTA, {0,0,0,0},
        {
            {4,0,1,'D'}, {4,3,4,'U'}, {5,0,1,'D'}, {4,3,4,'U'},
            {4,0,1,'D'}, {3,2,3,'U'}, {4,0,1,'D'}, {4,3,4,'U'},
            {5,0,1,'D'}, {5,3,4,'U'}, {5,0,1,'P'}, {4,3,4,'D'},
            {5,0,1,'U'}, {4,3,4,'D'}, {4,0,1,'P'}, {3,2,3,'D'}
        }
    },
    {
        0, "ERIC JOHNSON 5S", "Cascading Pentatonic Waterfall", 16, 4, 96, PC_MINOR_PENTA, {0,0,0,0},
        {
            {5,3,4,'D'}, {5,0,1,'P'}, {4,3,4,'D'}, {4,0,1,'U'},
            {3,2,3,'D'}, {4,3,4,'U'}, {4,0,1,'P'}, {3,2,3,'D'},
            {3,0,1,'U'}, {2,2,3,'D'}, {3,2,3,'U'}, {3,0,1,'P'},
            {2,2,3,'D'}, {2,0,1,'U'}, {1,2,3,'D'}, {1,0,1,'U'}
        }
    },
    {
        0, "PENTATONIC THREES", "Ascending 12/8 Triplet Box 1 Sequence", 12, 3, 95, PC_MINOR_PENTA, {0,0,0,0},
        {
            {0,0,1,'D'}, {0,3,4,'U'}, {1,0,1,'D'},
            {0,3,4,'U'}, {1,0,1,'D'}, {1,2,3,'U'},
            {1,0,1,'D'}, {1,2,3,'U'}, {2,0,1,'D'},
            {1,2,3,'U'}, {2,0,1,'D'}, {2,2,3,'U'}
        }
    },
    {
        0, "WIDE STRING SKIP", "String-Skipping Minor Pentatonic Etude", 16, 4, 90, PC_MINOR_PENTA, {0,0,0,0},
        {
            {0,0,1,'D'}, {0,3,4,'U'}, {2,0,1,'D'}, {2,2,3,'U'},
            {1,0,1,'D'}, {1,2,3,'U'}, {3,0,1,'D'}, {3,2,3,'U'},
            {2,0,1,'D'}, {2,2,3,'U'}, {4,0,1,'D'}, {4,3,4,'U'},
            {3,0,1,'D'}, {3,2,3,'U'}, {5,0,1,'D'}, {5,3,4,'U'}
        }
    },
    {
        0, "BOX 1 TO 2 SLIDE", "Diagonal Pentatonic Position Shift", 16, 4, 100, PC_MINOR_PENTA, {0,0,0,0},
        {
            {0,0,1,'D'}, {0,3,3,'H'}, {1,0,1,'D'}, {1,2,3,'H'},
            {2,0,1,'D'}, {2,2,3,'H'}, {3,0,1,'D'}, {3,2,3,'H'},
            {3,4,3,'/'}, {4,3,1,'D'}, {4,5,3,'H'}, {5,3,1,'D'},
            {5,5,3,'H'}, {5,7,3,'/'}, {5,5,1,'D'}, {4,5,3,'U'}
        }
    },

    /* ==================== 2. LEGATO & ARPEGGIOS (#06..#10) ==================== */
    {
        1, "LEGATO 3-NPS ROLL", "Hammer-On & Pull-Off Stamina Builder", 16, 4, 95, PC_NATURAL_MINOR, {0,0,0,0},
        {
            {2,0,1,'D'}, {2,2,2,'H'}, {2,4,4,'H'}, {2,2,2,'P'},
            {3,0,1,'D'}, {3,2,2,'H'}, {3,4,4,'H'}, {3,2,2,'P'},
            {4,1,1,'D'}, {4,3,2,'H'}, {4,5,4,'H'}, {4,3,2,'P'},
            {5,2,1,'D'}, {5,3,2,'H'}, {5,5,4,'H'}, {5,3,2,'P'}
        }
    },
    {
        1, "MINOR TRIAD ARP", "3-String Minor Arpeggio Etude", 12, 3, 100, PC_MINOR_TRIAD, {0,0,0,0},
        {
            {2,2,3,'D'}, {3,0,1,'D'}, {4,0,1,'D'},
            {5,0,1,'D'}, {5,3,4,'H'}, {5,0,1,'P'},
            {4,0,1,'U'}, {3,0,1,'U'}, {2,2,3,'D'},
            {3,0,1,'D'}, {4,0,1,'D'}, {5,0,1,'U'}
        }
    },
    {
        1, "MIN7 SKIPPED ARP", "String-Skipped Minor 7th Arpeggio", 16, 4, 92, PC_MIN7_ARP, {0,0,0,0},
        {
            {0,0,1,'D'}, {0,3,4,'H'}, {2,0,1,'D'}, {2,2,3,'H'},
            {4,0,1,'D'}, {4,3,4,'H'}, {5,0,1,'D'}, {5,3,4,'H'},
            {5,0,1,'P'}, {4,3,4,'D'}, {4,0,1,'P'}, {2,2,3,'D'},
            {2,0,1,'P'}, {0,3,4,'D'}, {0,0,1,'P'}, {2,2,3,'D'}
        }
    },
    {
        1, "DIM7 SYMMETRY ARP", "Symmetrical Diminished 7th Sequence", 16, 4, 96, PC_DIM7_ARP, {0,0,0,0},
        {
            {3,0,1,'D'}, {3,3,4,'H'}, {4,2,2,'D'}, {5,0,1,'D'},
            {5,3,4,'H'}, {5,6,4,'/'}, {5,3,1,'P'}, {4,5,2,'D'},
            {3,6,3,'U'}, {3,3,1,'P'}, {4,5,2,'D'}, {5,3,1,'D'},
            {5,6,4,'H'}, {5,3,1,'P'}, {4,2,2,'D'}, {5,0,1,'U'}
        }
    },
    {
        1, "5-STR SWEEP SHAPE", "5th-Pos 5-String Minor Sweep Arpeggio", 16, 4, 88, PC_MINOR_TRIAD, {0,0,0,0},
        {
            {1,2,3,'D'}, {2,2,3,'D'}, {3,0,1,'D'}, {4,0,1,'D'},
            {5,0,1,'D'}, {5,3,4,'H'}, {5,0,1,'P'}, {4,0,1,'U'},
            {3,0,1,'U'}, {2,2,3,'U'}, {1,2,3,'U'}, {2,2,3,'D'},
            {3,0,1,'D'}, {4,0,1,'D'}, {5,0,1,'D'}, {5,3,4,'U'}
        }
    },

    /* ==================== 3. ALTERNATE PICKING (#11..#15) ==================== */
    {
        2, "GILBERT 3-NPS CELL", "Inside/Outside 6-Note Picking Etude", 12, 3, 105, PC_NATURAL_MINOR, {0,0,0,0},
        {
            {4,0,1,'D'}, {4,1,2,'U'}, {4,3,4,'D'},
            {5,0,1,'U'}, {4,3,4,'D'}, {4,1,2,'U'},
            {4,0,1,'D'}, {4,1,2,'U'}, {4,3,4,'D'},
            {5,0,1,'U'}, {5,2,3,'D'}, {5,3,4,'U'}
        }
    },
    {
        2, "NEOCLASSIC PEDAL", "Harmonic Minor High-e Pedal Point", 16, 4, 104, PC_HARMONIC_MINOR, {0,0,0,0},
        {
            {5,0,1,'D'}, {5,3,3,'U'}, {5,2,2,'D'}, {5,3,3,'U'},
            {4,4,4,'D'}, {5,3,3,'U'}, {5,0,1,'D'}, {5,3,3,'U'},
            {4,1,1,'D'}, {5,3,3,'U'}, {4,4,4,'D'}, {5,3,3,'U'},
            {5,0,1,'D'}, {5,3,3,'U'}, {5,2,2,'D'}, {5,0,1,'U'}
        }
    },
    {
        2, "DIATONIC 4-NOTE", "Strict Alternate Picking 3-NPS Climb", 16, 4, 100, PC_NATURAL_MINOR, {0,0,0,0},
        {
            {0,0,1,'D'}, {0,2,2,'U'}, {0,3,3,'D'}, {1,0,1,'U'},
            {0,2,2,'D'}, {0,3,3,'U'}, {1,0,1,'D'}, {1,2,2,'U'},
            {0,3,3,'D'}, {1,0,1,'U'}, {1,2,2,'D'}, {1,3,3,'U'},
            {1,0,1,'D'}, {1,2,2,'U'}, {1,3,3,'D'}, {2,0,1,'U'}
        }
    },
    {
        2, "DI MEOLA SYNCOPA", "Syncopated Cross-String Picking Etude", 16, 4, 98, PC_NATURAL_MINOR, {0,0,0,0},
        {
            {2,2,3,'D'}, {3,0,1,'U'}, {3,2,3,'D'}, {4,1,2,'U'},
            {3,2,3,'D'}, {3,0,1,'U'}, {2,2,3,'D'}, {3,0,1,'U'},
            {3,2,3,'D'}, {4,1,2,'U'}, {4,3,4,'D'}, {5,0,1,'U'},
            {4,3,4,'D'}, {4,1,2,'U'}, {3,2,3,'D'}, {3,0,1,'U'}
        }
    },
    {
        2, "CROSS-STRING JUMP", "Alternate Picking String-Crossing Drill", 16, 4, 94, PC_MINOR_PENTA, {0,0,0,0},
        {
            {2,0,1,'D'}, {3,2,3,'U'}, {2,2,3,'D'}, {4,0,1,'U'},
            {2,0,1,'D'}, {4,3,4,'U'}, {3,0,1,'D'}, {5,0,1,'U'},
            {3,2,3,'D'}, {5,3,4,'U'}, {4,0,1,'D'}, {5,0,1,'U'},
            {4,3,4,'D'}, {3,2,3,'U'}, {4,0,1,'D'}, {3,0,1,'U'}
        }
    },

    /* ==================== 4. BLUES & ROCK (#16..#20) ==================== */
    {
        3, "BB & ALBERT TURN", "Classic Major/Minor Blues Turnaround", 16, 4, 92, PC_HYBRID_BLUES, {0,0,0,0},
        {
            {3,2,2,'D'}, {3,4,2,'/'}, {4,3,1,'U'}, {5,3,1,'D'},
            {5,5,3,'U'}, {5,3,1,'P'}, {4,5,3,'D'}, {4,3,1,'U'},
            {3,2,3,'D'}, {3,0,1,'P'}, {3,1,2,'H'}, {2,2,3,'D'},
            {3,0,1,'U'}, {4,0,1,'D'}, {4,3,4,'U'}, {5,0,1,'D'}
        }
    },
    {
        3, "SRV TEXAS SHUFFLE", "Driving 12/8 Blues Box Groove", 12, 3, 108, PC_HYBRID_BLUES, {0,0,0,0},
        {
            {0,0,1,'D'}, {0,3,4,'U'}, {1,0,1,'D'},
            {1,1,2,'H'}, {1,2,3,'H'}, {2,0,1,'D'},
            {2,2,3,'U'}, {3,0,1,'D'}, {3,1,2,'H'},
            {4,0,1,'D'}, {4,3,4,'U'}, {2,2,3,'D'}
        }
    },
    {
        3, "DORIAN MIXO RUN", "Hybrid Minor/Major 3rd & 6th Lick", 16, 4, 98, PC_HYBRID_BLUES, {0,0,0,0},
        {
            {1,0,1,'D'}, {1,2,3,'H'}, {2,0,1,'D'}, {2,2,3,'H'},
            {3,0,1,'D'}, {3,1,2,'H'}, {4,0,1,'D'}, {4,2,3,'H'},
            {5,0,1,'D'}, {5,2,3,'U'}, {5,3,4,'D'}, {5,0,1,'P'},
            {4,3,4,'D'}, {4,2,3,'U'}, {3,0,1,'D'}, {3,1,2,'H'}
        }
    },
    {
        3, "HENDRIX CHORD EMBL", "Double-Stop Style Melodic Fill", 16, 4, 88, PC_MINOR_PENTA, {0,0,0,0},
        {
            {2,0,1,'D'}, {2,2,3,'H'}, {3,0,1,'D'}, {2,2,3,'U'},
            {3,0,1,'D'}, {3,2,3,'H'}, {3,0,1,'P'}, {2,2,3,'D'},
            {4,0,1,'D'}, {4,3,4,'H'}, {4,0,1,'P'}, {3,2,3,'D'},
            {3,0,1,'U'}, {2,2,3,'D'}, {2,0,1,'P'}, {1,2,3,'D'}
        }
    },
    {
        3, "BLUES b5 CASCADE", "Descending Blues Scale with Blue Note", 16, 4, 96, PC_BLUES, {0,0,0,0},
        {
            {5,3,4,'D'}, {5,0,1,'P'}, {4,3,4,'D'}, {4,0,1,'P'},
            {3,3,4,'D'}, {3,2,3,'P'}, {3,0,1,'P'}, {2,2,3,'D'},
            {3,2,3,'U'}, {3,0,1,'P'}, {2,2,3,'D'}, {2,0,1,'P'},
            {1,2,3,'D'}, {1,1,2,'P'}, {1,0,1,'P'}, {0,3,4,'D'}
        }
    },

    /* ==================== 5. WARMUP & SPIDER (#21..#25) ==================== */
    {
        4, "1-2-3-4 SPIDER", "Classic Chromatic 4-Finger Crawl", 16, 4, 90, PC_CHROMATIC, {0,0,0,0},
        {
            {0,0,1,'D'}, {0,1,2,'U'}, {0,2,3,'D'}, {0,3,4,'U'},
            {1,0,1,'D'}, {1,1,2,'U'}, {1,2,3,'D'}, {1,3,4,'U'},
            {2,0,1,'D'}, {2,1,2,'U'}, {2,2,3,'D'}, {2,3,4,'U'},
            {3,0,1,'D'}, {3,1,2,'U'}, {3,2,3,'D'}, {3,3,4,'U'}
        }
    },
    {
        4, "1-3-2-4 PERMUTE", "Alternating Finger Independence Drill", 16, 4, 88, PC_CHROMATIC, {0,0,0,0},
        {
            {2,0,1,'D'}, {2,2,3,'U'}, {2,1,2,'D'}, {2,3,4,'U'},
            {3,0,1,'D'}, {3,2,3,'U'}, {3,1,2,'D'}, {3,3,4,'U'},
            {4,0,1,'D'}, {4,2,3,'U'}, {4,1,2,'D'}, {4,3,4,'U'},
            {5,0,1,'D'}, {5,2,3,'U'}, {5,1,2,'D'}, {5,3,4,'U'}
        }
    },
    {
        4, "SKIPPING SPIDER", "String-Skipping Chromatic Spider (6-4-5-3)", 16, 4, 85, PC_CHROMATIC, {0,0,0,0},
        {
            {0,0,1,'D'}, {0,1,2,'U'}, {0,2,3,'D'}, {0,3,4,'U'},
            {2,0,1,'D'}, {2,1,2,'U'}, {2,2,3,'D'}, {2,3,4,'U'},
            {1,0,1,'D'}, {1,1,2,'U'}, {1,2,3,'D'}, {1,3,4,'U'},
            {3,0,1,'D'}, {3,1,2,'U'}, {3,2,3,'D'}, {3,3,4,'U'}
        }
    },
    {
        4, "DIAGONAL SPIDER", "Diagonal Chromatic Fretboard Shift", 16, 4, 90, PC_CHROMATIC, {0,0,0,0},
        {
            {0,0,1,'D'}, {1,1,2,'U'}, {2,2,3,'D'}, {3,3,4,'U'},
            {1,0,1,'D'}, {2,1,2,'U'}, {3,2,3,'D'}, {4,3,4,'U'},
            {2,0,1,'D'}, {3,1,2,'U'}, {4,2,3,'D'}, {5,3,4,'U'},
            {5,2,3,'D'}, {4,1,2,'U'}, {3,0,1,'D'}, {2,1,2,'U'}
        }
    },
    {
        4, "4-3-2-1 PINKY WORK", "Descending Pinky-First Finger Builder", 16, 4, 88, PC_CHROMATIC, {0,0,0,0},
        {
            {5,3,4,'D'}, {5,2,3,'U'}, {5,1,2,'D'}, {5,0,1,'U'},
            {4,3,4,'D'}, {4,2,3,'U'}, {4,1,2,'D'}, {4,0,1,'U'},
            {3,3,4,'D'}, {3,2,3,'U'}, {3,1,2,'D'}, {3,0,1,'U'},
            {2,3,4,'D'}, {2,2,3,'U'}, {2,1,2,'D'}, {2,0,1,'U'}
        }
    },

    /* ==================== 6. JAZZ 7TH ARPEGGIOS (#26..#33) ==================== */
    {
        5, "6TH-STR ASC I-IV", "Diatonic 1-3-5-7 Arps: Imaj7 iim7 iiim7 IVmaj7", 16, 4, 84, PC_MAJOR_SCALE, {1, 2, 3, 4},
        {
            {0,0,1,'D'}, {1,-1,1,'U'}, {1,2,4,'D'}, {2,1,2,'U'},
            {0,2,2,'D'}, {1,0,1,'U'},  {1,4,4,'D'}, {2,2,3,'U'},
            {1,-1,1,'D'}, {1,2,4,'U'}, {2,1,2,'D'}, {2,4,4,'U'},
            {1,0,1,'D'}, {1,4,4,'U'},  {2,2,2,'D'}, {3,1,1,'U'}
        }
    },
    {
        5, "6TH-STR ASC V-I", "Diatonic 1-3-5-7 Arps: V7 vim7 viim7b5 Imaj7", 16, 4, 84, PC_MAJOR_SCALE, {5, 6, 7, 1},
        {
            {1,2,2,'D'}, {2,1,1,'U'}, {2,4,4,'D'}, {3,2,2,'U'},
            {1,4,4,'D'}, {2,2,2,'U'}, {3,1,1,'D'}, {3,4,4,'U'},
            {2,1,1,'D'}, {2,4,4,'U'}, {3,2,2,'D'}, {4,2,3,'U'},
            {2,2,2,'D'}, {3,1,1,'U'}, {4,0,1,'D'}, {4,4,4,'U'}
        }
    },
    {
        5, "VOICE-LED ALT I-IV", "Up 1-3-5-7 / Down 7-5-3-1: I ii iii IV", 16, 4, 84, PC_MAJOR_SCALE, {1, 2, 3, 4},
        {
            {0,0,1,'D'}, {1,-1,1,'U'}, {1,2,4,'D'}, {2,1,2,'U'},
            {2,2,3,'D'}, {1,4,4,'U'},  {1,0,1,'D'}, {0,2,2,'U'},
            {1,-1,1,'D'}, {1,2,4,'U'}, {2,1,2,'D'}, {2,4,4,'U'},
            {3,1,1,'D'}, {2,2,2,'U'},  {1,4,4,'D'}, {1,0,1,'U'}
        }
    },
    {
        5, "VOICE-LED ALT V-I", "Up 1-3-5-7 / Down 7-5-3-1: V7 vi vii I", 16, 4, 84, PC_MAJOR_SCALE, {5, 6, 7, 1},
        {
            {1,2,2,'D'}, {2,1,1,'U'}, {2,4,4,'D'}, {3,2,2,'U'},
            {3,4,4,'D'}, {3,1,1,'U'}, {2,2,2,'D'}, {1,4,4,'U'},
            {2,1,1,'D'}, {2,4,4,'U'}, {3,2,2,'D'}, {4,2,3,'U'},
            {4,4,4,'D'}, {4,0,1,'U'}, {3,1,1,'D'}, {2,2,2,'U'}
        }
    },
    {
        5, "UPPER-REG 7THS I-IV", "Upper Register 1-3-5-7: Imaj7 iim7 iiim7 IVmaj7", 16, 4, 82, PC_MAJOR_SCALE, {1, 2, 3, 4},
        {
            {2,2,2,'D'}, {3,1,1,'U'}, {4,0,1,'D'}, {4,4,4,'U'},
            {2,4,4,'D'}, {3,2,2,'U'}, {4,2,3,'D'}, {5,0,1,'U'},
            {3,1,1,'D'}, {4,0,1,'U'}, {4,4,4,'D'}, {5,2,2,'U'},
            {3,2,2,'D'}, {4,2,3,'U'}, {5,0,1,'D'}, {5,4,4,'U'}
        }
    },
    {
        5, "UPPER-REG 7THS V-I", "Upper Register 1-3-5-7: V7 vim7 viim7b5 Imaj7", 16, 4, 82, PC_MAJOR_SCALE, {5, 6, 7, 1},
        {
            {3,4,3,'D'}, {4,4,4,'U'}, {5,2,1,'D'}, {5,5,4,'U'},
            {3,1,1,'D'}, {3,4,4,'U'}, {4,2,2,'D'}, {5,0,1,'U'},
            {2,1,1,'D'}, {2,4,4,'U'}, {3,2,2,'D'}, {4,2,3,'U'},
            {2,2,2,'D'}, {3,1,1,'U'}, {4,0,1,'D'}, {5,-1,1,'U'}
        }
    },
    {
        5, "CYCLE OF 4THS PT.1", "Jazz Cycle 1-3-5-7: Imaj7 IVmaj7 viim7b5 iiim7", 16, 4, 86, PC_MAJOR_SCALE, {1, 4, 7, 3},
        {
            {0,0,1,'D'}, {1,-1,1,'U'}, {1,2,4,'D'}, {2,1,2,'U'},
            {1,0,1,'D'}, {1,4,4,'U'},  {2,2,2,'D'}, {3,1,1,'U'},
            {2,1,1,'D'}, {2,4,4,'U'},  {3,2,2,'D'}, {4,2,3,'U'},
            {1,-1,1,'D'}, {1,2,4,'U'}, {2,1,2,'D'}, {2,4,4,'U'}
        }
    },
    {
        5, "CYCLE OF 4THS PT.2", "Jazz Turnaround 1-3-5-7: vim7 iim7 V7 Imaj7", 16, 4, 86, PC_MAJOR_SCALE, {6, 2, 5, 1},
        {
            {1,4,4,'D'}, {2,2,2,'U'}, {3,1,1,'D'}, {3,4,4,'U'},
            {0,2,2,'D'}, {1,0,1,'U'}, {1,4,4,'D'}, {2,2,3,'U'},
            {1,2,2,'D'}, {2,1,1,'U'}, {2,4,4,'D'}, {3,2,2,'U'},
            {2,2,2,'D'}, {3,1,1,'U'}, {4,0,1,'D'}, {4,4,4,'U'}
        }
    }
};

static bool verify_lick_library(bool verbose) {
    bool all_ok = true;
    const int anchor = 5; /* Key of A: root MIDI = 45 (A2) */
    const int root_midi = k_open_midi[0] + anchor;

    for (int i = 0; i < TOTAL_LICKS; i++) {
        const LickDef *l = &k_licks[i];
        for (int s = 0; s < l->num_steps; s++) {
            const LickStep *st = &l->steps[s];
            if (st->str < 0 || st->str > 5) {
                fprintf(stderr, "[VERIFY FAIL] Lick #%02d (%s) step %d: invalid string %d\n",
                        i + 1, l->title, s, st->str);
                all_ok = false;
                continue;
            }
            if (st->finger < 1 || st->finger > 4) {
                fprintf(stderr, "[VERIFY FAIL] Lick #%02d (%s) step %d: invalid finger %d\n",
                        i + 1, l->title, s, st->finger);
                all_ok = false;
            }
            int midi = k_open_midi[st->str] + anchor + st->fret_off;
            int pc = ((midi - root_midi) % 12 + 12) % 12;
            if (!(l->allowed_pc_mask & (1u << pc))) {
                fprintf(stderr,
                        "[VERIFY FAIL] Lick #%02d (%s) step %d: str=%c fret=%d note=%s (pc=%d) not in mask 0x%03x\n",
                        i + 1, l->title, s, k_str_labels[st->str], anchor + st->fret_off,
                        k_note_names[midi % 12], pc, l->allowed_pc_mask);
                all_ok = false;
            }

            int beat_idx = s / l->steps_per_beat;
            if (beat_idx >= 0 && beat_idx < 4 && l->beat_deg[beat_idx] >= 1 && l->beat_deg[beat_idx] <= 7) {
                int deg = l->beat_deg[beat_idx];
                const Diatonic7thInfo *ch = &k_diatonic_7ths[deg];
                bool in_chord = false;
                for (int t = 0; t < 4; t++) {
                    if (pc == ch->tones_pc[t]) { in_chord = true; break; }
                }
                if (!in_chord) {
                    fprintf(stderr,
                            "[VERIFY FAIL] Lick #%02d (%s) step %d (beat %d, %s): note %s not in {1,3,5,7}\n",
                            i + 1, l->title, s, beat_idx + 1, ch->roman, k_note_names[midi % 12]);
                    all_ok = false;
                }
                if ((s % 4) == 3 && l->steps_per_beat == 4) {
                    uint8_t seen_mask = 0;
                    for (int k = s - 3; k <= s; k++) {
                        int m_k = k_open_midi[l->steps[k].str] + anchor + l->steps[k].fret_off;
                        int pc_k = ((m_k - root_midi) % 12 + 12) % 12;
                        for (int t = 0; t < 4; t++) {
                            if (pc_k == ch->tones_pc[t]) seen_mask |= (1u << t);
                        }
                    }
                    if (seen_mask != 0x0F) {
                        fprintf(stderr,
                                "[VERIFY FAIL] Lick #%02d (%s) beat %d (%s): missing chord tones (mask=0x%x)\n",
                                i + 1, l->title, beat_idx + 1, ch->roman, seen_mask);
                        all_ok = false;
                    }
                }
            }

            if (s > 0) {
                const LickStep *prev = &l->steps[s - 1];
                int prev_midi = k_open_midi[prev->str] + anchor + prev->fret_off;
                if (st->pick == '/') {
                    if (st->str != prev->str || st->fret_off == prev->fret_off || st->finger != prev->finger) {
                        fprintf(stderr, "[VERIFY FAIL] Lick #%02d (%s) step %d: invalid slide '/'\n",
                                i + 1, l->title, s);
                        all_ok = false;
                    }
                } else if (st->pick == 'H') {
                    if (st->str != prev->str || st->fret_off <= prev->fret_off || st->finger <= prev->finger) {
                        fprintf(stderr, "[VERIFY FAIL] Lick #%02d (%s) step %d: invalid hammer-on 'H'\n",
                                i + 1, l->title, s);
                        all_ok = false;
                    }
                } else if (st->pick == 'P') {
                    if (st->str != prev->str || st->fret_off >= prev->fret_off || st->finger >= prev->finger) {
                        fprintf(stderr, "[VERIFY FAIL] Lick #%02d (%s) step %d: invalid pull-off 'P'\n",
                                i + 1, l->title, s);
                        all_ok = false;
                    }
                }
                if (l->allowed_pc_mask != PC_CHROMATIC && st->str != prev->str && midi == prev_midi) {
                    fprintf(stderr, "[VERIFY FAIL] Lick #%02d (%s) step %d: unison collision\n",
                            i + 1, l->title, s);
                    all_ok = false;
                }
            }
        }
        if (verbose) {
            printf("[OK] #%02d %-19s (%2d steps, mask=0x%03x)\n",
                   i + 1, l->title, l->num_steps, l->allowed_pc_mask);
        }
    }
    return all_ok;
}

/* ============================================================================
 * 2. SPEED-BUILDER LADDER PRESETS, AUDIO MIX & TIMBRES
 * ============================================================================ */

#define NUM_LADDERS 5
typedef struct {
    const char *label;
    int         bpm_step;
    int         bars_per_step;
    int         goal_offset;
} LadderPreset;

static const LadderPreset k_ladders[NUM_LADDERS] = {
    { "OFF (STATIC BPM)",   0, 0,  0 },
    { "+2 BPM / 2 BARS",    2, 2, 40 },
    { "+5 BPM / 4 BARS",    5, 4, 50 },
    { "+5 BPM / 8 BARS",    5, 8, 50 },
    { "+10 BPM / 4 BARS",  10, 4, 60 }
};

enum {
    MIX_TAB_AND_CLICK = 0,
    MIX_CLICK_ONLY    = 1,
    MIX_TAB_ONLY      = 2,
    MIX_COUNT         = 3
};

static const char *k_mix_names[MIX_COUNT] = {
    "TAB+CLK",
    "CLK ONLY",
    "TAB ONLY"
};

#define NUM_TIMBRES 4
typedef struct {
    const char *short_name;
    const char *full_name;
    int         gm_program;
} GuitarTimbreDef;

static const GuitarTimbreDef k_timbres[NUM_TIMBRES] = {
    { "STEEL",  "Steel Acoustic",     25 },
    { "NYLON",  "Nylon Classical",    24 },
    { "JAZZ",   "Clean Jazz Guitar",  26 },
    { "OVERD",  "Overdrive Electric", 29 }
};

#define MIN_BPM 40
#define MAX_BPM 240

/* UI-side settings state */
static int  g_cat_idx     = 0;
static int  g_lick_in_cat = 0;
static int  g_key_idx     = 5; /* Default: Key of A (5th fret) */
static int  g_base_bpm    = 100;
static int  g_ladder_idx  = 2; /* Default: +5 BPM / 4 BARS */
static int  g_mix_mode    = MIX_TAB_AND_CLICK;
static int  g_timbre_idx  = 0;
static bool g_show_help   = false;

static inline int current_lick_global_idx(void) {
    int c = g_cat_idx;
    if (c < 0 || c >= NUM_CATEGORIES) c = 0;
    int l = g_lick_in_cat;
    if (l < 0 || l >= k_cat_count[c]) l = 0;
    return k_cat_start[c] + l;
}

/* Telemetry shared from background audio thread -> UI thread */
static volatile int  g_ui_cur_step   = 0;
static volatile int  g_ui_cur_beat   = 0;
static volatile int  g_ui_cur_bar    = 0;
static volatile bool g_ui_in_countin = false;
static volatile int  g_ui_live_bpm   = 100;
static volatile bool g_ui_playing    = false;
static volatile bool g_sf2_active    = false;

/* Lock-free command ring: UI thread -> Audio thread */
typedef enum {
    ACMD_NONE = 0,
    ACMD_SET_PLAYING,
    ACMD_SET_LICK,
    ACMD_SET_KEY,
    ACMD_SET_BPM,
    ACMD_SET_LADDER,
    ACMD_SET_MIX,
    ACMD_SET_TIMBRE,
    ACMD_RESET_LOOP
} AudioCmdType;

typedef struct {
    AudioCmdType type;
    int          arg0;
    int          arg1;
} AudioCmd;

#define AUDIO_CMD_RING 32
static AudioCmd          g_acmd_ring[AUDIO_CMD_RING];
static volatile uint32_t g_acmd_write = 0;
static volatile uint32_t g_acmd_read  = 0;

static void post_audio_cmd(AudioCmdType type, int arg0, int arg1) {
    uint32_t w = g_acmd_write;
    uint32_t next_w = (w + 1) % AUDIO_CMD_RING;
    if (next_w == g_acmd_read) return;
    g_acmd_ring[w].type = type;
    g_acmd_ring[w].arg0 = arg0;
    g_acmd_ring[w].arg1 = arg1;
    __sync_synchronize();
    g_acmd_write = next_w;
}

/* ============================================================================
 * 3. BACKGROUND-THREAD SOUNDFONT 2.01 + ENHANCED EKS FALLBACK & SEQUENCER
 * ============================================================================ */

static Rg35Sf2Engine g_sf2;

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
static uint32_t  g_eks_rng = 0x51F2A83Du;

/* Audio-thread sequencer state */
static int    g_aud_lick_idx   = 0;
static int    g_aud_key_idx    = 5;
static int    g_aud_base_bpm   = 100;
static int    g_aud_bpm        = 100;
static int    g_aud_ladder_idx = 2;
static int    g_aud_mix_mode   = MIX_TAB_AND_CLICK;
static int    g_aud_timbre     = 0;
static bool   g_aud_playing    = false;
static bool   g_aud_in_countin = false;
static int    g_aud_cur_step   = 0;
static int    g_aud_cur_beat   = 0;
static int    g_aud_cur_bar    = 0;
static int    g_aud_last_midi  = -1;
static double g_aud_accum      = 0.0;

static void eks_trigger_note(int midi_note, int str_hint, char pick_type, int timbre_idx) {
    if (midi_note < 36) midi_note = 36;
    if (midi_note > 88) midi_note = 88;
    EksVoice *v = &g_eks_voices[g_eks_next_voice];
    g_eks_next_voice = (g_eks_next_voice + 1) % EKS_VOICES;

    float freq = 440.0f * powf(2.0f, (float)(midi_note - 69) / 12.0f);
    float exact_period = (float)SAMPLE_RATE / freq;
    if (exact_period < 16.0f) exact_period = 16.0f;
    if (exact_period > (float)(EKS_MAX_DELAY - 4)) exact_period = (float)(EKS_MAX_DELAY - 4);
    int int_len = (int)floorf(exact_period);

    bool is_legato = (pick_type == 'H' || pick_type == 'P' || pick_type == '/');

    v->buf_len      = int_len;
    v->exact_period = exact_period;
    v->idx          = 0;
    v->ap_prev_in   = 0.0f;
    v->ap_state     = 0.0f;
    v->samples_left = (int)(SAMPLE_RATE * 0.90f);

    if (timbre_idx == 1) {
        v->damping = 0.9946f; v->stretch = is_legato ? 0.62f : 0.54f;
    } else if (timbre_idx == 2) {
        v->damping = 0.9962f; v->stretch = is_legato ? 0.52f : 0.42f;
    } else if (timbre_idx == 3) {
        v->damping = 0.9972f; v->stretch = is_legato ? 0.34f : 0.24f;
    } else {
        v->damping = 0.9958f; v->stretch = is_legato ? 0.44f : 0.33f;
    }

    static const float k_pan_l[6] = { 0.72f, 0.66f, 0.58f, 0.50f, 0.44f, 0.38f };
    static const float k_pan_r[6] = { 0.38f, 0.44f, 0.50f, 0.58f, 0.66f, 0.72f };
    int s = (str_hint >= 0 && str_hint < 6) ? str_hint : 2;
    v->pan_l = k_pan_l[s];
    v->pan_r = k_pan_r[s];

    float raw[EKS_MAX_DELAY];
    int pick_comb = int_len / 7;
    if (pick_comb < 2) pick_comb = 2;
    float noise_amt = is_legato ? 0.22f : ((timbre_idx == 1) ? 0.28f : 0.50f);
    float amp_scale = is_legato ? 0.78f : 1.00f;

    for (int i = 0; i < int_len; i++) {
        g_eks_rng = g_eks_rng * 1664525u + 1013904223u;
        float nz = ((float)(int32_t)(g_eks_rng >> 16) - 32768.0f) / 32768.0f;
        float ph = (2.0f * (float)M_PI * (float)i) / (float)int_len;
        float body = 0.48f * sinf(ph) + 0.26f * sinf(2.0f * ph) + 0.12f * sinf(3.0f * ph);
        raw[i] = (body + noise_amt * nz) * amp_scale;
    }
    for (int i = 0; i < int_len; i++) {
        int prev_p = (i - pick_comb + int_len) % int_len;
        v->buf[i] = raw[i] - 0.42f * raw[prev_p];
    }
    int passes = is_legato ? 4 : 2;
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

static void eks_trigger_click(bool accent, bool countin) {
    g_eks_click.active = true;
    g_eks_click.pos    = 0;
    g_eks_click.len    = 1980; /* 45 ms crisp click */
    g_eks_click.freq   = accent ? 1720.0f : (countin ? 1280.0f : 1080.0f);
    g_eks_click.gain   = accent ? 0.50f : 0.34f;
}

static void audio_apply_timbre(int timbre_idx) {
    if (timbre_idx < 0 || timbre_idx >= NUM_TIMBRES) timbre_idx = 0;
    g_aud_timbre = timbre_idx;
    if (rg35_sf2_ready(&g_sf2)) {
        rg35_sf2_channel_notes_off(&g_sf2, 0);
        rg35_sf2_set_channel_program(&g_sf2, 0, 0, k_timbres[timbre_idx].gm_program);
        rg35_sf2_set_channel_pan_vol(&g_sf2, 0, 0.50f, 0.96f);
        rg35_sf2_set_channel_pan_vol(&g_sf2, 9, 0.50f, 0.78f);
        g_aud_last_midi = -1;
    }
}

static void audio_trigger_countin_click(bool accent) {
    if (g_aud_mix_mode == MIX_TAB_ONLY) return;
    if (rg35_sf2_ready(&g_sf2)) {
        int key = accent ? 76 : 37; /* GM Hi Wood Block (Beat 1) / Side Stick (Beats 2-4) */
        rg35_sf2_note_on(&g_sf2, 9, key, accent ? 0.88f : 0.70f);
    } else {
        eks_trigger_click(accent, true);
    }
}

static void audio_trigger_step(int step_idx, bool is_beat, bool is_accent) {
    const LickDef *lick = &k_licks[g_aud_lick_idx];
    if (g_aud_mix_mode != MIX_CLICK_ONLY && step_idx >= 0 && step_idx < lick->num_steps) {
        const LickStep *st = &lick->steps[step_idx];
        if (st->str >= 0 && st->str < 6) {
            int fret = k_keys[g_aud_key_idx].anchor_fret + st->fret_off;
            int midi = k_open_midi[st->str] + fret;
            bool is_legato = (st->pick == 'H' || st->pick == 'P' || st->pick == '/');
            if (rg35_sf2_ready(&g_sf2)) {
                if (g_aud_last_midi >= 0) {
                    rg35_sf2_note_off(&g_sf2, 0, g_aud_last_midi);
                }
                float vel = is_legato ? 0.72f : (is_accent ? 0.96f : 0.86f);
                rg35_sf2_note_on(&g_sf2, 0, midi, vel);
                g_aud_last_midi = midi;
            } else {
                eks_trigger_note(midi, st->str, st->pick, g_aud_timbre);
            }
        }
    }
    if (g_aud_mix_mode != MIX_TAB_ONLY && is_beat) {
        if (rg35_sf2_ready(&g_sf2)) {
            int key = is_accent ? 76 : 77; /* Hi / Lo Wood Block */
            rg35_sf2_note_on(&g_sf2, 9, key, is_accent ? 0.85f : 0.62f);
        } else {
            eks_trigger_click(is_accent, false);
        }
    }
}

static void audio_publish_ui_telemetry(void) {
    g_ui_cur_step   = g_aud_cur_step;
    g_ui_cur_beat   = g_aud_cur_beat;
    g_ui_cur_bar    = g_aud_cur_bar;
    g_ui_in_countin = g_aud_in_countin;
    g_ui_live_bpm   = g_aud_bpm;
    g_ui_playing    = g_aud_playing;
}

static void audio_process_commands(void) {
    while (g_acmd_read != g_acmd_write) {
        __sync_synchronize();
        AudioCmd cmd = g_acmd_ring[g_acmd_read];
        g_acmd_read = (g_acmd_read + 1) % AUDIO_CMD_RING;

        switch (cmd.type) {
            case ACMD_SET_PLAYING:
                g_aud_playing = (cmd.arg0 != 0);
                g_aud_accum   = 0.0;
                if (g_aud_playing) {
                    g_aud_bpm        = g_aud_base_bpm;
                    g_aud_cur_step   = 0;
                    g_aud_cur_beat   = 0;
                    g_aud_cur_bar    = 0;
                    g_aud_in_countin = true;
                    audio_trigger_countin_click(true);
                } else {
                    g_aud_in_countin = false;
                    if (rg35_sf2_ready(&g_sf2)) {
                        rg35_sf2_channel_notes_off(&g_sf2, 0);
                        g_aud_last_midi = -1;
                    }
                }
                audio_publish_ui_telemetry();
                break;

            case ACMD_SET_LICK:
                if (cmd.arg0 >= 0 && cmd.arg0 < TOTAL_LICKS) {
                    g_aud_lick_idx = cmd.arg0;
                    g_aud_cur_step = 0;
                    g_aud_cur_beat = 0;
                    g_aud_cur_bar  = 0;
                    g_aud_accum    = 0.0;
                    if (cmd.arg1 >= MIN_BPM && cmd.arg1 <= MAX_BPM) {
                        g_aud_base_bpm = g_aud_bpm = cmd.arg1;
                    }
                    if (rg35_sf2_ready(&g_sf2)) {
                        rg35_sf2_channel_notes_off(&g_sf2, 0);
                        g_aud_last_midi = -1;
                    }
                    audio_publish_ui_telemetry();
                }
                break;

            case ACMD_SET_KEY:
                if (cmd.arg0 >= 0 && cmd.arg0 < NUM_KEYS) {
                    g_aud_key_idx = cmd.arg0;
                }
                break;

            case ACMD_SET_BPM:
                if (cmd.arg0 >= MIN_BPM && cmd.arg0 <= MAX_BPM) {
                    g_aud_base_bpm = cmd.arg0;
                    g_aud_bpm      = cmd.arg0;
                    if (cmd.arg1) g_aud_cur_bar = 0;
                    audio_publish_ui_telemetry();
                }
                break;

            case ACMD_SET_LADDER:
                if (cmd.arg0 >= 0 && cmd.arg0 < NUM_LADDERS) {
                    g_aud_ladder_idx = cmd.arg0;
                    g_aud_cur_bar    = 0;
                    g_aud_bpm        = g_aud_base_bpm;
                    audio_publish_ui_telemetry();
                }
                break;

            case ACMD_SET_MIX:
                if (cmd.arg0 >= 0 && cmd.arg0 < MIX_COUNT) {
                    g_aud_mix_mode = cmd.arg0;
                    if (g_aud_mix_mode == MIX_CLICK_ONLY && rg35_sf2_ready(&g_sf2)) {
                        rg35_sf2_channel_notes_off(&g_sf2, 0);
                        g_aud_last_midi = -1;
                    }
                }
                break;

            case ACMD_SET_TIMBRE:
                audio_apply_timbre(cmd.arg0);
                break;

            case ACMD_RESET_LOOP:
                g_aud_cur_step   = 0;
                g_aud_cur_beat   = 0;
                g_aud_cur_bar    = 0;
                g_aud_bpm        = g_aud_base_bpm;
                g_aud_accum      = 0.0;
                g_aud_in_countin = false;
                if (rg35_sf2_ready(&g_sf2)) {
                    rg35_sf2_channel_notes_off(&g_sf2, 0);
                    g_aud_last_midi = -1;
                }
                if (g_aud_playing) {
                    audio_trigger_step(0, true, true);
                }
                audio_publish_ui_telemetry();
                break;

            default:
                break;
        }
    }
}

static void audio_advance_sequencer_tick(void) {
    if (!g_aud_playing) return;
    const LickDef *lick = &k_licks[g_aud_lick_idx];
    const LadderPreset *ladder = &k_ladders[g_aud_ladder_idx];

    if (g_aud_in_countin) {
        g_aud_cur_beat++;
        if (g_aud_cur_beat >= 4) {
            g_aud_in_countin = false;
            g_aud_cur_beat   = 0;
            g_aud_cur_step   = 0;
            g_aud_cur_bar    = 0;
            audio_trigger_step(0, true, true);
        } else {
            audio_trigger_countin_click(false);
        }
    } else {
        g_aud_cur_step++;
        if (g_aud_cur_step >= lick->num_steps) {
            g_aud_cur_step = 0;
            g_aud_cur_bar++;
            if (g_aud_ladder_idx > 0 && ladder->bars_per_step > 0 &&
                g_aud_cur_bar >= ladder->bars_per_step) {
                g_aud_cur_bar = 0;
                int goal_bpm = g_aud_base_bpm + ladder->goal_offset;
                if (goal_bpm > MAX_BPM) goal_bpm = MAX_BPM;
                if (g_aud_bpm < goal_bpm) {
                    g_aud_bpm += ladder->bpm_step;
                    if (g_aud_bpm > goal_bpm) g_aud_bpm = goal_bpm;
                }
            }
        }
        g_aud_cur_beat = g_aud_cur_step / lick->steps_per_beat;
        bool is_beat   = (g_aud_cur_step % lick->steps_per_beat) == 0;
        bool is_accent = (g_aud_cur_step == 0);
        audio_trigger_step(g_aud_cur_step, is_beat, is_accent);
    }
    audio_publish_ui_telemetry();
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

            float env = (v->samples_left < 2200) ? ((float)v->samples_left / 2200.0f) : 1.0f;
            v->samples_left--;
            if (v->samples_left <= 0) v->active = false;

            float s_val = cur * env;
            if (g_aud_timbre == 3) {
                s_val = tanhf(s_val * 2.9f) * 0.65f;
            }
            mix_l    += s_val * v->pan_l;
            mix_r    += s_val * v->pan_r;
            mono_sum += s_val;
        }

        float body_a = 0.032f * mono_sum + 1.968f * g_eks_body_z1a - 0.972f * g_eks_body_z2a;
        g_eks_body_z2a = g_eks_body_z1a;
        g_eks_body_z1a = body_a;

        float body_b = 0.022f * mono_sum + 1.942f * g_eks_body_z1b - 0.952f * g_eks_body_z2b;
        g_eks_body_z2b = g_eks_body_z1b;
        g_eks_body_z1b = body_b;

        float body_gain = (g_aud_timbre == 3) ? 0.08f : 0.22f;
        mix_l += (body_a + body_b) * body_gain;
        mix_r += (body_a + body_b) * body_gain;

        if (g_eks_click.active) {
            float p = (float)g_eks_click.pos / (float)g_eks_click.len;
            float env = expf(-p * 9.0f);
            float ph = (2.0f * (float)M_PI * g_eks_click.freq * (float)g_eks_click.pos) / (float)SAMPLE_RATE;
            float clk = (sinf(ph) + 0.35f * sinf(ph * 2.40f)) * env * g_eks_click.gain;
            mix_l += clk;
            mix_r += clk;
            if (++g_eks_click.pos >= g_eks_click.len) {
                g_eks_click.active = false;
            }
        }

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

        if (g_aud_playing) {
            const LickDef *lick = &k_licks[g_aud_lick_idx];
            double samples_per_beat = ((double)SAMPLE_RATE * 60.0) / (double)g_aud_bpm;
            double tick_samples = g_aud_in_countin
                                  ? samples_per_beat
                                  : (samples_per_beat / (double)lick->steps_per_beat);
            double until_tick = tick_samples - g_aud_accum;
            if (until_tick <= 0.5) {
                audio_advance_sequencer_tick();
                g_aud_accum -= tick_samples;
                if (g_aud_accum < 0.0) g_aud_accum = 0.0;
                samples_per_beat = ((double)SAMPLE_RATE * 60.0) / (double)g_aud_bpm;
                tick_samples = g_aud_in_countin
                               ? samples_per_beat
                               : (samples_per_beat / (double)lick->steps_per_beat);
                until_tick = tick_samples - g_aud_accum;
            }
            int to_next = (int)ceil(until_tick);
            if (to_next < 1) to_next = 1;
            if (chunk > to_next) chunk = to_next;
            g_aud_accum += (double)chunk;
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
    bool ok = rg35_sf2_init(&g_sf2, SAMPLE_RATE, 48, 0.20f, 1.38f);
    g_sf2_active = ok;
    if (ok) {
        audio_apply_timbre(g_timbre_idx);
        fprintf(stderr, "[licktrainer] SoundFont loaded: %s\n", g_sf2.loaded_path);
    } else {
        g_sf2.reverb_wet  = 0.20f;
        g_sf2.master_gain = 1.35f;
        fprintf(stderr, "[licktrainer] No SoundFont found; using Floating-Point EKS fallback\n");
    }
}

static void audio_free_soundfont(void) {
    rg35_sf2_free(&g_sf2);
    g_sf2_active = false;
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
            void *pa = fn_new(NULL, "RG35XXSP LickTrainer", 1, NULL, "Guitar Lick Coach",
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
    rg35xxsp_keepawake_init("licktrainer", g_sdl2_lib);
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
 * 5. NATIVE 640x480 ANTI-ALIASED DRAWING, TEXT & PNG SCREENSHOT WRITER
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

/* Self-contained zero-dependency RFC 2083 PNG writer */
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
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_write_chunk(f, "IHDR", ihdr, 13);

    size_t row_len = 1 + (size_t)NATIVE_W * 3;
    size_t raw_len = row_len * (size_t)NATIVE_H;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { fclose(f); return false; }

    uint32_t s1 = 1, s2 = 0;
    for (int y = 0; y < NATIVE_H; y++) {
        uint8_t *dst = raw + (size_t)y * row_len;
        dst[0] = 0;
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
    zbuf[zp++] = 0x78;
    zbuf[zp++] = 0x01;
    size_t rem = raw_len, off = 0;
    while (rem > 0) {
        size_t blen = (rem > 65535) ? 65535 : rem;
        uint8_t is_last = (rem <= 65535) ? 1 : 0;
        uint16_t len16  = (uint16_t)blen;
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
 * 6. PERSISTENT CONFIGURATION (.licktrainer_rg35xxsp.cfg)
 * ============================================================================ */

static char g_config_path[512] = "";

static void resolve_config_path(void) {
    const char *dirs[] = {
        "/userdata/roms/ports/licktrainer",
        "/userdata/system/configs",
        "/userdata",
        NULL
    };
    struct stat st;
    for (int i = 0; dirs[i]; i++) {
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && access(dirs[i], W_OK) == 0) {
            snprintf(g_config_path, sizeof(g_config_path), "%s/.licktrainer_rg35xxsp.cfg", dirs[i]);
            return;
        }
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "%s/.licktrainer_rg35xxsp.cfg", home);
    } else {
        snprintf(g_config_path, sizeof(g_config_path), "./.licktrainer_rg35xxsp.cfg");
    }
}

static bool load_config(void) {
    if (g_config_path[0] == '\0') return false;
    FILE *f = fopen(g_config_path, "r");
    if (!f) return false;
    bool loaded_bpm = false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int val = 0;
        if (sscanf(line, "cat_idx=%d", &val) == 1 && val >= 0 && val < NUM_CATEGORIES) {
            g_cat_idx = val;
        } else if (sscanf(line, "lick_in_cat=%d", &val) == 1 && val >= 0 && val < 16) {
            g_lick_in_cat = val;
        } else if (sscanf(line, "key_idx=%d", &val) == 1 && val >= 0 && val < NUM_KEYS) {
            g_key_idx = val;
        } else if (sscanf(line, "bpm=%d", &val) == 1 && val >= MIN_BPM && val <= MAX_BPM) {
            g_base_bpm = val;
            loaded_bpm = true;
        } else if (sscanf(line, "ladder_idx=%d", &val) == 1 && val >= 0 && val < NUM_LADDERS) {
            g_ladder_idx = val;
        } else if (sscanf(line, "mix_mode=%d", &val) == 1 && val >= 0 && val < MIX_COUNT) {
            g_mix_mode = val;
        } else if (sscanf(line, "timbre_idx=%d", &val) == 1 && val >= 0 && val < NUM_TIMBRES) {
            g_timbre_idx = val;
        }
    }
    if (g_lick_in_cat >= k_cat_count[g_cat_idx]) {
        g_lick_in_cat = 0;
    }
    fclose(f);
    return loaded_bpm;
}

static void save_config(void) {
    if (g_config_path[0] == '\0') return;
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "cat_idx=%d\n", g_cat_idx);
    fprintf(f, "lick_in_cat=%d\n", g_lick_in_cat);
    fprintf(f, "key_idx=%d\n", g_key_idx);
    fprintf(f, "bpm=%d\n", g_base_bpm);
    fprintf(f, "ladder_idx=%d\n", g_ladder_idx);
    fprintf(f, "mix_mode=%d\n", g_mix_mode);
    fprintf(f, "timbre_idx=%d\n", g_timbre_idx);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* ============================================================================
 * 7. NATIVE 640x480 RENDERING (TAB STAFF, 8-FRET NECK, JAZZ 7THS & HELP)
 * ============================================================================ */

static const char *pick_full_name(char pick) {
    switch (pick) {
        case 'D': return "Downstroke (D)";
        case 'U': return "Upstroke (U)";
        case 'H': return "Hammer-On (H)";
        case 'P': return "Pull-Off (P)";
        case '/': return "Slide (/)";
        default:  return "Pick";
    }
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
    draw_text(FONT_UI_20, mx + 98, my + 10, "LICK TRAINER - QUICK GUIDE", COL_GOLD);

    /* Quick Guide Intro */
    fill_rect(mx + 4, my + 44, mw - 8, 22, RGB(19, 29, 48));
    fill_rect(mx + 4, my + 66, mw - 8, 1, RGB(48, 66, 98));
    draw_text_centered(FONT_UI_13, mx + 8, my + 44, mw - 16, 22,
                       "33 guitar licks in 6 categories with 4-beat count-in & BPM speed ladder.",
                       RGB(186, 230, 253));

    static const struct {
        const char *btn;
        const char *desc;
        uint32_t    col;
    } k_rows[10] = {
        { "A  (Space/Z)",  "Play / Pause exercise loop (starts with 4-beat count-in)",  COL_EMERALD },
        { "B  (Bksp)",     "Cycle Audio Mix (TAB+CLK  /  CLK ONLY  /  TAB ONLY)",      COL_CYAN    },
        { "L1 / R1 (Q/E)", "Switch Category (Pentatonic, Legato, Alt, Blues, Spider, Jazz)", COL_GOLD },
        { "LEFT / RIGHT",  "Previous / Next Exercise within Category (#01 .. #33)",    COL_WHITE   },
        { "UP / DOWN",     "Adjust Tempo (40 - 240 BPM; hold for continuous repeat)",  COL_GOLD    },
        { "SELECT (Tab)",  "Cycle Speed-Builder BPM Ladder (+2/+5/+10 BPM per bars)",  COL_ORANGE  },
        { "X  (X Key)",    "Transpose Key / Guitar Neck Anchor Fret (E .. Eb)",        COL_PURPLE  },
        { "L2 / R2 (1/2)", "L2: Guitar Timbre (Steel/Nylon/Jazz/Overd) | R2: Reset BPM", COL_CYAN  },
        { "START (Enter)", "Restart loop from Beat 1 at base BPM (never exits)",       COL_MUTED   },
        { "Y / MENU (Esc)","Y: Close Help Overlay   |   MENU (Esc): Save & Quit",      COL_CORAL   }
    };

    int ky = my + 72;
    for (int i = 0; i < 10; i++) {
        int ry = ky + i * 35;
        draw_pill(mx + 14, ry, 142, 26, RGB(24, 38, 62), k_rows[i].col);
        draw_text_centered(FONT_UI_13, mx + 14, ry, 142, 26, k_rows[i].btn, k_rows[i].col);
        int df = (text_width(FONT_UI_13, k_rows[i].desc) > mw - 180) ? FONT_UI_11 : FONT_UI_13;
        draw_text(df, mx + 166, ry + (df == FONT_UI_11 ? 7 : 5), k_rows[i].desc, RGB(226, 232, 240));
    }

    /* Footer Bar */
    fill_rect(mx + 4, my + mh - 32, mw - 8, 28, RGB(12, 18, 30));
    fill_rect(mx + 4, my + mh - 33, mw - 8, 1, COL_BORDER);
    char foot[128];
    snprintf(foot, sizeof(foot),
             "PK: D=Dn U=Up H=Ham P=Pull /=Slide  |  %s (%s)  |  [Y]/[B] Close",
             g_sf2_active ? "GeneralUser-GS SF2" : "EKS Synth",
             k_timbres[g_timbre_idx].short_name);
    draw_text_centered(FONT_UI_11, mx + 8, my + mh - 31, mw - 16, 26, foot, RGB(186, 230, 253));
}

static void render_ui(void) {
    int lick_global_idx = current_lick_global_idx();
    const LickDef *lick = &k_licks[lick_global_idx];
    const KeyAnchor *key = &k_keys[g_key_idx];
    const LadderPreset *ladder = &k_ladders[g_ladder_idx];

    int cur_step         = g_ui_cur_step;
    if (cur_step < 0 || cur_step >= lick->num_steps) cur_step = 0;
    int cur_beat         = g_ui_cur_beat;
    if (cur_beat < 0 || cur_beat > 3) cur_beat = 0;
    int cur_bar_in_stage = g_ui_cur_bar;
    bool in_countin      = g_ui_in_countin;
    int live_bpm         = g_ui_live_bpm;
    bool is_playing      = g_ui_playing;

    int root_midi     = k_open_midi[0] + key->anchor_fret;
    bool is_jazz_7ths = (lick->beat_deg[0] >= 1 && lick->beat_deg[0] <= 7);

    fill_rect(0, 0, NATIVE_W, NATIVE_H, COL_BG);

    /* ================= 1. TOP HEADER BAR (y: 0..42) ================= */
    fill_rect(0, 0, NATIVE_W, 42, COL_HEADER);
    fill_rect(0, 40, NATIVE_W, 2, COL_BORDER);

    static const uint32_t k_cat_fills[NUM_CATEGORIES] = {
        RGB(14, 116, 144), RGB(180, 96, 14), RGB(161, 98, 7),
        RGB(109, 40, 217), RGB(14, 132, 92), RGB(185, 28, 60)
    };
    static const uint32_t k_cat_bdrs[NUM_CATEGORIES] = {
        COL_CYAN, COL_ORANGE, COL_GOLD, COL_PURPLE, COL_EMERALD, COL_CORAL
    };
    draw_pill(8, 6, 114, 30, k_cat_fills[g_cat_idx], k_cat_bdrs[g_cat_idx]);
    draw_text_centered(FONT_UI_13, 8, 6, 114, 30, k_category_names[g_cat_idx], COL_WHITE);

    char title_buf[64];
    snprintf(title_buf, sizeof(title_buf), "#%02d %s", lick_global_idx + 1, lick->title);
    int title_font = (text_width(FONT_UI_16, title_buf) > 250) ? FONT_UI_13 : FONT_UI_16;
    draw_text(title_font, 130, (title_font == FONT_UI_16) ? 11 : 13, title_buf, COL_WHITE);

    char key_buf[24];
    snprintf(key_buf, sizeof(key_buf), "%s (%dfr)", key->name, key->anchor_fret);
    draw_pill(388, 6, 78, 30, COL_PANEL_ALT, COL_GOLD);
    draw_text_centered(FONT_UI_13, 388, 6, 78, 30, key_buf, COL_GOLD);

    draw_pill(472, 6, 92, 30, RGB(18, 46, 58), g_sf2_active ? COL_EMERALD : COL_CYAN);
    char tone_str[24];
    snprintf(tone_str, sizeof(tone_str), "%s:%s",
             g_sf2_active ? "SF2" : "EKS", k_timbres[g_timbre_idx].short_name);
    draw_text_centered(FONT_UI_11, 472, 6, 92, 30, tone_str, RGB(209, 250, 229));

    draw_pill(570, 6, 62, 30, RGB(30, 58, 95), RGB(96, 165, 250));
    draw_text_centered(FONT_UI_13, 570, 6, 62, 30, "Y:Help", RGB(254, 240, 138));

    /* ================= 2. TEMPO, PROGRESSION / LADDER & BEAT STRIP (y: 48..120) ================= */
    /* Left Card: Tempo & Subdivision (x: 8..160) */
    fill_rect(8, 48, 152, 72, COL_PANEL);
    draw_border(8, 48, 152, 72, 2, COL_BORDER);
    draw_text(FONT_UI_11, 18, 55, "TEMPO", COL_MUTED);
    draw_pill(88, 52, 64, 19, RGB(18, 46, 64), COL_CYAN);
    draw_text_centered(FONT_UI_11, 88, 52, 64, 19,
                       (lick->steps_per_beat == 3) ? "TRIPLET" : "16TH", COL_CYAN);

    char bpm_buf[24];
    snprintf(bpm_buf, sizeof(bpm_buf), "%d BPM", live_bpm);
    draw_text_centered(FONT_UI_20, 10, 76, 148, 36, bpm_buf, COL_GOLD);

    /* Center Card: Jazz 7ths Progression OR Speed-Builder Ladder (x: 166..482, w=316) */
    fill_rect(166, 48, 316, 72, COL_PANEL);
    draw_border(166, 48, 316, 72, 2,
                is_jazz_7ths ? COL_GOLD : ((g_ladder_idx > 0) ? COL_ORANGE : COL_BORDER));

    int active_beat_idx = (cur_step / lick->steps_per_beat) & 3;

    if (is_jazz_7ths && !in_countin) {
        /* 4-chord progression pills across top of Center Card */
        for (int b = 0; b < 4; b++) {
            int cx = 172 + b * 76;
            bool is_ab = (b == active_beat_idx);
            const Diatonic7thInfo *bch = &k_diatonic_7ths[lick->beat_deg[b]];
            draw_pill(cx, 53, 73, 22,
                      is_ab ? RGB(202, 138, 4) : COL_PANEL_ALT,
                      is_ab ? RGB(254, 240, 138) : COL_BORDER);
            draw_text_centered(FONT_UI_11, cx, 53, 73, 22, bch->roman,
                               is_ab ? RGB(15, 23, 42) : COL_GOLD);
        }

        /* Spelled chord tones for active beat */
        int deg = lick->beat_deg[active_beat_idx];
        const Diatonic7thInfo *ch = &k_diatonic_7ths[deg];
        const char *ch_root_name = k_note_names[(root_midi + ch->tones_pc[0]) % 12];
        char sub_line[72];
        snprintf(sub_line, sizeof(sub_line), "%s = %s%s  (%s - %s - %s - %s)",
                 ch->roman, ch_root_name, ch->qual_suffix,
                 k_note_names[(root_midi + ch->tones_pc[0]) % 12],
                 k_note_names[(root_midi + ch->tones_pc[1]) % 12],
                 k_note_names[(root_midi + ch->tones_pc[2]) % 12],
                 k_note_names[(root_midi + ch->tones_pc[3]) % 12]);
        int sf = (text_width(FONT_UI_13, sub_line) > 304) ? FONT_UI_11 : FONT_UI_13;
        draw_text_centered(sf, 170, 79, 308, 19, sub_line, COL_WHITE);
    } else {
        char lad_hdr[48];
        snprintf(lad_hdr, sizeof(lad_hdr), "LADDER: %s", ladder->label);
        draw_text_centered(FONT_UI_13, 170, 53, 308, 20, lad_hdr,
                           (g_ladder_idx > 0) ? COL_ORANGE : COL_MUTED);

        int goal_bpm = g_base_bpm + ladder->goal_offset;
        if (goal_bpm > MAX_BPM) goal_bpm = MAX_BPM;

        char lad_sub[72];
        if (in_countin && is_playing) {
            snprintf(lad_sub, sizeof(lad_sub), "COUNT-IN: BEAT %d OF 4", cur_beat + 1);
        } else if (g_ladder_idx > 0) {
            snprintf(lad_sub, sizeof(lad_sub), "BAR %d/%d   |   %d -> %d BPM",
                     cur_bar_in_stage + 1, ladder->bars_per_step, g_base_bpm, goal_bpm);
        } else {
            snprintf(lad_sub, sizeof(lad_sub), "%s", lick->subtitle);
        }
        int sf = (text_width(FONT_UI_13, lad_sub) > 304) ? FONT_UI_11 : FONT_UI_13;
        draw_text_centered(sf, 170, 78, 308, 20, lad_sub,
                           (in_countin && is_playing) ? COL_CORAL : COL_WHITE);
    }

    /* Progress Bar inside Center Card (y: 104..112) */
    int pb_x = 176, pb_y = 104, pb_w = 296, pb_h = 8;
    fill_rect(pb_x, pb_y, pb_w, pb_h, COL_BG);
    draw_border(pb_x, pb_y, pb_w, pb_h, 1, COL_BORDER);
    if (in_countin && is_playing) {
        int fw = ((cur_beat + 1) * (pb_w - 2)) / 4;
        fill_rect(pb_x + 1, pb_y + 1, fw, pb_h - 2, COL_CORAL);
    } else if (g_ladder_idx > 0 && ladder->bars_per_step > 0) {
        int total_steps_in_stage = ladder->bars_per_step * lick->num_steps;
        int done_steps = cur_bar_in_stage * lick->num_steps + (is_playing ? (cur_step + 1) : 0);
        if (done_steps > total_steps_in_stage) done_steps = total_steps_in_stage;
        int fw = (done_steps * (pb_w - 2)) / total_steps_in_stage;
        fill_rect(pb_x + 1, pb_y + 1, fw, pb_h - 2, COL_EMERALD);
    } else if (is_jazz_7ths) {
        int fw = ((cur_step + 1) * (pb_w - 2)) / lick->num_steps;
        fill_rect(pb_x + 1, pb_y + 1, fw, pb_h - 2, COL_GOLD);
    }

    /* Right Card: Audio Mix & 4 Beat LEDs (x: 488..632, w=144) */
    fill_rect(488, 48, 144, 72, COL_PANEL);
    draw_border(488, 48, 144, 72, 2, COL_BORDER);
    char mix_lbl[24];
    snprintf(mix_lbl, sizeof(mix_lbl), "MIX: %s", k_mix_names[g_mix_mode]);
    draw_text_centered(FONT_UI_11, 492, 52, 136, 18, mix_lbl, COL_EMERALD);

    for (int b = 0; b < 4; b++) {
        int bx = 494 + b * 34;
        int by = 74;
        bool active_b = is_playing && (b == cur_beat);
        uint32_t b_fill = active_b ? ((b == 0) ? COL_CORAL : COL_GREEN) : COL_PANEL_ALT;
        uint32_t b_bdr  = active_b ? COL_WHITE : ((b == 0) ? COL_CORAL : COL_BORDER);
        draw_pill(bx, by, 30, 36, b_fill, b_bdr);
        char b_str[4] = { (char)('1' + b), '\0' };
        draw_text_centered(FONT_UI_16, bx, by, 30, 36, b_str,
                           active_b ? RGB(15, 23, 42) : ((b == 0) ? COL_CORAL : COL_WHITE));
    }

    /* ================= 3. 6-STRING GUITAR TAB STAFF PANEL (y: 126..308) ================= */
    fill_rect(8, 126, 624, 182, COL_PANEL);
    draw_border(8, 126, 624, 182, 2, COL_BORDER);

    int nsteps  = lick->num_steps;
    int col_w   = (nsteps == 12) ? 48 : 36; /* 576px exact grid width */
    int grid_w  = nsteps * col_w;
    int grid_x0 = 48;

    /* Left Labels: DG/PK, Strings e B G D A E, FG */
    draw_text_centered(FONT_UI_13, 12, 132, 30, 20,
                       is_jazz_7ths ? "DG" : "PK",
                       is_jazz_7ths ? COL_GOLD : COL_CYAN);
    for (int row = 0; row < 6; row++) {
        int str_idx = 5 - row;
        int sy = 166 + row * 20;
        char slbl[2] = { k_str_labels[str_idx], '\0' };
        draw_text_centered(FONT_UI_13, 14, sy - 9, 26, 18, slbl,
                           (str_idx == 0 || str_idx == 5) ? COL_GOLD : COL_MUTED);
        fill_rect(grid_x0 - 2, sy, grid_w + 4, (str_idx == 0) ? 2 : 1, COL_DIM);
    }
    draw_text_centered(FONT_UI_13, 12, 282, 30, 20, "FG", COL_GOLD);

    /* Closing right barline */
    fill_rect(grid_x0 + grid_w, 132, 2, 170, COL_BORDER);

    /* Columns: Beat dividers, Active Playhead, Degrees/Picking, Fret Pills, Fingerings */
    for (int s = 0; s < nsteps; s++) {
        int sx  = grid_x0 + s * col_w;
        int scx = sx + col_w / 2;

        if (s % lick->steps_per_beat == 0) {
            fill_rect(sx, 132, 2, 170, COL_BORDER);
        }

        bool is_cur = (!in_countin) && (s == cur_step);
        uint32_t hl_col = is_playing ? COL_CORAL : COL_GOLD;
        if (is_cur) {
            fill_rect_alpha(sx + 2, 132, col_w - 3, 170, hl_col, 48);
            draw_border(sx + 1, 131, col_w - 1, 172, 2, hl_col);
        }

        const LickStep *st = &lick->steps[s];

        /* Top Row: Chord-Tone Degree (1, 3, 5, 7) in Jazz 7ths, or Picking (D, U, H, P, /) */
        if (is_jazz_7ths) {
            int beat_i = s / lick->steps_per_beat;
            int deg = lick->beat_deg[beat_i];
            const Diatonic7thInfo *ch = &k_diatonic_7ths[deg];
            int s_midi = k_open_midi[st->str] + key->anchor_fret + st->fret_off;
            int s_pc = ((s_midi - root_midi) % 12 + 12) % 12;
            const char *deg_lbl = "1";
            uint32_t deg_col = COL_CYAN;
            if (s_pc == ch->tones_pc[0])      { deg_lbl = "1"; deg_col = COL_GOLD;    }
            else if (s_pc == ch->tones_pc[1]) { deg_lbl = "3"; deg_col = COL_EMERALD; }
            else if (s_pc == ch->tones_pc[2]) { deg_lbl = "5"; deg_col = COL_CYAN;    }
            else if (s_pc == ch->tones_pc[3]) { deg_lbl = "7"; deg_col = COL_ORANGE;  }
            draw_text_centered(FONT_UI_13, sx, 132, col_w, 20, deg_lbl,
                               is_cur ? COL_WHITE : deg_col);
        } else {
            char p_str[2] = { st->pick, '\0' };
            uint32_t p_col = COL_CYAN;
            if (st->pick == 'U') p_col = COL_EMERALD;
            else if (st->pick == 'H' || st->pick == 'P' || st->pick == '/') p_col = COL_ORANGE;
            draw_text_centered(FONT_UI_13, sx, 132, col_w, 20, p_str,
                               is_cur ? COL_WHITE : p_col);
        }

        /* Fret Number Pill on Active String */
        if (st->str >= 0 && st->str < 6) {
            int row = 5 - st->str;
            int sy  = 166 + row * 20;
            int actual_fret = key->anchor_fret + st->fret_off;
            char f_str[16];
            snprintf(f_str, sizeof(f_str), "%d", actual_fret);
            int pw = text_width(FONT_UI_13, f_str) + 10;
            if (pw < 22) pw = 22;
            if (pw > col_w - 4) pw = col_w - 4;

            if (is_cur) {
                draw_pill(scx - pw / 2, sy - 9, pw, 18, hl_col, COL_WHITE);
                draw_text_centered(FONT_UI_13, scx - pw / 2, sy - 9, pw, 18, f_str, RGB(15, 23, 42));
            } else {
                fill_rect(scx - pw / 2, sy - 8, pw, 17, COL_PANEL);
                draw_text_centered(FONT_UI_13, scx - pw / 2, sy - 9, pw, 18, f_str, COL_WHITE);
            }
        }

        /* Bottom Row: Left-Hand Finger Number (1..4) */
        if (st->finger >= 1 && st->finger <= 4) {
            char fg_str[4] = { (char)('0' + st->finger), '\0' };
            draw_text_centered(FONT_UI_13, sx, 282, col_w, 20, fg_str,
                               is_cur ? COL_GOLD : COL_MUTED);
        }
    }

    /* ================= 4. ZOOMED 8-FRET ROSEWOOD NECK + INFO CARD (y: 314..434) ================= */
    /* Left Card: Zoomed 8-Fret Rosewood Guitar Neck (x: 8..454, w=446, h=120) */
    fill_rect(8, 314, 446, 120, COL_PANEL);
    draw_border(8, 314, 446, 120, 2, COL_BORDER);

    int min_off = 99;
    for (int s = 0; s < nsteps; s++) {
        if (lick->steps[s].str >= 0 && lick->steps[s].fret_off < min_off) {
            min_off = lick->steps[s].fret_off;
        }
    }
    int start_fret = key->anchor_fret + min_off - 1;
    if (start_fret < 0) start_fret = 0;
    int num_frets = 8;

    int neck_x0 = 40;
    int fret_w  = 50; /* 8 * 50 = 400px (x: 40..440) */
    int neck_w  = fret_w * num_frets;
    int neck_y0 = 337;
    int neck_h  = 88;
    int str_sp  = 15; /* 6 strings: 344, 359, 374, 389, 404, 419 */

    fill_rect(neck_x0, neck_y0, neck_w, neck_h, COL_NECK_WOOD);

    /* Fret Numbers, Fret Wires & Pearloid Inlays */
    for (int f = 0; f <= num_frets; f++) {
        int fx = neck_x0 + f * fret_w;
        int fret_num = start_fret + f;
        bool is_nut = (fret_num == 0 && f == 0);
        if (is_nut) {
            fill_rect(fx - 3, neck_y0 - 1, 5, neck_h + 2, COL_BONE_NUT);
        } else {
            fill_rect(fx, neck_y0, 2, neck_h, COL_FRET_WIRE);
        }

        if (f < num_frets) {
            int fcx = fx + fret_w / 2;
            char fn_str[16];
            snprintf(fn_str, sizeof(fn_str), "%d", fret_num);
            draw_text_centered(FONT_UI_11, fcx - 12, 318, 24, 15, fn_str,
                               (fret_num == key->anchor_fret) ? COL_GOLD : COL_MUTED);

            if (fret_num == 3 || fret_num == 5 || fret_num == 7 ||
                fret_num == 9 || fret_num == 15 || fret_num == 17 || fret_num == 19) {
                draw_filled_circle_aa((float)fcx, (float)(neck_y0 + neck_h / 2), 4.2f,
                                      RGB(86, 78, 70), RGB(114, 105, 96));
            } else if (fret_num == 12) {
                draw_filled_circle_aa((float)fcx, (float)(neck_y0 + 22), 4.0f,
                                      RGB(86, 78, 70), RGB(114, 105, 96));
                draw_filled_circle_aa((float)fcx, (float)(neck_y0 + neck_h - 22), 4.0f,
                                      RGB(86, 78, 70), RGB(114, 105, 96));
            }
        }
    }

    /* 6 Horizontal Strings + Left Labels */
    const LickStep *active_st = &lick->steps[cur_step];
    for (int row = 0; row < 6; row++) {
        int str_idx = 5 - row;
        int sy = neck_y0 + 7 + row * str_sp;
        char slbl[2] = { k_str_labels[str_idx], '\0' };
        draw_text_centered(FONT_UI_11, 14, sy - 7, 18, 14, slbl,
                           (str_idx == active_st->str) ? COL_GOLD : COL_MUTED);

        uint32_t scol = (str_idx <= 2) ? RGB(196, 174, 142) : RGB(212, 222, 234);
        fill_rect(neck_x0 + 2, sy, neck_w - 2, (str_idx <= 1) ? 2 : 1, scol);
    }

    /* 1. Ghosted Lick Shape Dots */
    for (int s = 0; s < nsteps; s++) {
        const LickStep *st = &lick->steps[s];
        if (st->str < 0) continue;
        int actual_fret = key->anchor_fret + st->fret_off;
        int rel_f = actual_fret - start_fret;
        if (rel_f >= 0 && rel_f < num_frets) {
            int fcx = neck_x0 + rel_f * fret_w + fret_w / 2;
            int sy  = neck_y0 + 7 + (5 - st->str) * str_sp;
            if (is_jazz_7ths) {
                draw_filled_circle_aa((float)fcx, (float)sy, 4.4f, COL_PANEL_ALT, COL_DIM);
            } else {
                draw_filled_circle_aa((float)fcx, (float)sy, 6.2f, RGB(20, 44, 68), COL_CYAN);
            }
        }
    }

    /* 2. In Jazz 7ths Mode: Highlight Active Beat's 4-Note Chord Arpeggio (1-3-5-7) */
    if (is_jazz_7ths) {
        int b_start = active_beat_idx * lick->steps_per_beat;
        int b_end   = b_start + lick->steps_per_beat;
        int deg     = lick->beat_deg[active_beat_idx];
        const Diatonic7thInfo *ch = &k_diatonic_7ths[deg];
        for (int s = b_start; s < b_end && s < nsteps; s++) {
            const LickStep *st = &lick->steps[s];
            if (st->str < 0) continue;
            int actual_fret = key->anchor_fret + st->fret_off;
            int rel_f = actual_fret - start_fret;
            if (rel_f >= 0 && rel_f < num_frets) {
                int fcx = neck_x0 + rel_f * fret_w + fret_w / 2;
                int sy  = neck_y0 + 7 + (5 - st->str) * str_sp;
                int s_midi = k_open_midi[st->str] + actual_fret;
                int s_pc = ((s_midi - root_midi) % 12 + 12) % 12;
                bool is_chord_root = (s_pc == ch->tones_pc[0]);
                draw_filled_circle_aa((float)fcx, (float)sy, 7.6f,
                                      is_chord_root ? COL_GOLD : COL_CYAN, COL_WHITE);
            }
        }
    }

    /* 3. Active Step Highlight Circle on Neck + Right Readout Card */
    fill_rect(460, 314, 172, 120, COL_PANEL_ALT);
    draw_border(460, 314, 172, 120, 2, COL_GOLD);

    if (active_st->str >= 0) {
        int actual_fret = key->anchor_fret + active_st->fret_off;
        int rel_f = actual_fret - start_fret;
        int midi  = k_open_midi[active_st->str] + actual_fret;
        const char *note_name = k_note_names[midi % 12];
        static const char *k_fg_names[5] = {
            "", "Finger 1 (Index)", "Finger 2 (Middle)", "Finger 3 (Ring)", "Finger 4 (Pinky)"
        };
        int fg_i = (active_st->finger >= 1 && active_st->finger <= 4) ? active_st->finger : 1;

        if (rel_f >= 0 && rel_f < num_frets) {
            int fcx = neck_x0 + rel_f * fret_w + fret_w / 2;
            int sy  = neck_y0 + 7 + (5 - active_st->str) * str_sp;
            draw_filled_circle_aa((float)fcx, (float)sy, 9.5f, COL_CORAL, COL_WHITE);
            char fg_ch[2] = { (char)('0' + fg_i), '\0' };
            draw_text_centered(FONT_UI_11, fcx - 9, sy - 7, 18, 15, fg_ch, COL_WHITE);
        }

        if (is_jazz_7ths) {
            int deg = lick->beat_deg[active_beat_idx];
            const Diatonic7thInfo *ch = &k_diatonic_7ths[deg];
            const char *ch_root = k_note_names[(root_midi + ch->tones_pc[0]) % 12];
            char ch_hdr[32];
            snprintf(ch_hdr, sizeof(ch_hdr), "%s:  %s%s", ch->roman_short, ch_root, ch->qual_suffix);
            draw_pill(470, 322, 152, 24, RGB(34, 42, 66), COL_GOLD);
            draw_text_centered(FONT_UI_13, 470, 322, 152, 24, ch_hdr, COL_GOLD);

            int s_pc = ((midi - root_midi) % 12 + 12) % 12;
            const char *ivl_lbl = "1";
            for (int t = 0; t < 4; t++) {
                if (s_pc == ch->tones_pc[t]) { ivl_lbl = ch->tone_lbl[t]; break; }
            }
            char note_info[32];
            snprintf(note_info, sizeof(note_info), "%s  (%s)", note_name, ivl_lbl);
            draw_text_centered(FONT_UI_20, 464, 352, 164, 28, note_info, COL_WHITE);

            char str_info[32];
            snprintf(str_info, sizeof(str_info), "Str %d (%c)  |  Fret %d",
                     6 - active_st->str, k_str_labels[active_st->str], actual_fret);
            draw_text_centered(FONT_UI_11, 464, 386, 164, 18, str_info, COL_CYAN);
            draw_text_centered(FONT_UI_11, 464, 408, 164, 18, k_fg_names[fg_i], COL_EMERALD);
        } else {
            char step_hdr[32];
            snprintf(step_hdr, sizeof(step_hdr), "STEP %d / %d", cur_step + 1, nsteps);
            draw_text_centered(FONT_UI_11, 464, 322, 164, 18, step_hdr, COL_MUTED);

            char note_info[32];
            snprintf(note_info, sizeof(note_info), "%s  (Fr %d)", note_name, actual_fret);
            draw_text_centered(FONT_UI_20, 464, 344, 164, 28, note_info, COL_GOLD);

            draw_text_centered(FONT_UI_13, 464, 378, 164, 20, pick_full_name(active_st->pick), COL_CYAN);
            draw_text_centered(FONT_UI_11, 464, 404, 164, 18, k_fg_names[fg_i], COL_EMERALD);
        }
    }

    /* ================= 5. BOTTOM CONTROLS HINT BAR (y: 440..480) ================= */
    fill_rect(0, 440, NATIVE_W, 40, COL_HEADER);
    fill_rect(0, 440, NATIVE_W, 2, COL_BORDER);

    draw_pill(8, 446, 98, 28,
              is_playing ? RGB(180, 92, 12) : RGB(14, 132, 92),
              is_playing ? COL_GOLD : COL_EMERALD);
    draw_text_centered(FONT_UI_13, 8, 446, 98, 28,
                       is_playing ? "[A] PAUSE" : "[A] PLAY", COL_WHITE);

    draw_text_centered(FONT_UI_11, 110, 446, NATIVE_W - 116, 28,
                       "[L1/R1] Cat  [L/R] Lick  [U/D] BPM  [SEL] Ladder  [B] Mix  [X] Key  [L2] Tone  [R2] Rst",
                       RGB(214, 224, 238));

    if (g_show_help) {
        render_help_modal();
    }
}

/* ============================================================================
 * 8. SCREENSHOT MODE & MAIN ENTRY POINT
 * ============================================================================ */

static int run_screenshot_mode(const char *dir) {
    char path[512];
    audio_init_soundfont();

    /* 1. Pentatonic (#05 BOX 1 TO 2 SLIDE, Playing, Overdrive timbre) */
    g_cat_idx = 0; g_lick_in_cat = 4; g_key_idx = 5; g_ladder_idx = 2; g_mix_mode = 0; g_timbre_idx = 3;
    g_base_bpm = 100; g_ui_live_bpm = 105; g_ui_cur_step = 8; g_ui_cur_beat = 2; g_ui_cur_bar = 1;
    g_ui_in_countin = false; g_ui_playing = true; g_show_help = false;
    render_ui();
    snprintf(path, sizeof(path), "%s/licktrainer_pentatonic.png", dir);
    if (!save_png(path)) { fprintf(stderr, "Failed to write %s\n", path); return 1; }
    printf("Saved %s\n", path);

    /* 2. Legato/Arp (#06 LEGATO 3-NPS ROLL, Playing, Steel timbre) */
    g_cat_idx = 1; g_lick_in_cat = 0; g_key_idx = 5; g_ladder_idx = 1; g_mix_mode = 0; g_timbre_idx = 0;
    g_base_bpm = 95; g_ui_live_bpm = 97; g_ui_cur_step = 6; g_ui_cur_beat = 1; g_ui_cur_bar = 1;
    g_ui_in_countin = false; g_ui_playing = true; g_show_help = false;
    render_ui();
    snprintf(path, sizeof(path), "%s/licktrainer_legato.png", dir);
    if (!save_png(path)) { fprintf(stderr, "Failed to write %s\n", path); return 1; }
    printf("Saved %s\n", path);

    /* 3. Spider (#21 1-2-3-4 SPIDER, Playing, Nylon timbre) */
    g_cat_idx = 4; g_lick_in_cat = 0; g_key_idx = 5; g_ladder_idx = 3; g_mix_mode = 0; g_timbre_idx = 1;
    g_base_bpm = 90; g_ui_live_bpm = 95; g_ui_cur_step = 5; g_ui_cur_beat = 1; g_ui_cur_bar = 3;
    g_ui_in_countin = false; g_ui_playing = true; g_show_help = false;
    render_ui();
    snprintf(path, sizeof(path), "%s/licktrainer_spider.png", dir);
    if (!save_png(path)) { fprintf(stderr, "Failed to write %s\n", path); return 1; }
    printf("Saved %s\n", path);

    /* 4. Jazz 7ths Bar 1 (#26 6TH-STR ASC I-IV, ii: Bm7 highlighted, Clean Jazz timbre) */
    g_cat_idx = 5; g_lick_in_cat = 0; g_key_idx = 5; g_ladder_idx = 0; g_mix_mode = 0; g_timbre_idx = 2;
    g_base_bpm = 84; g_ui_live_bpm = 84; g_ui_cur_step = 5; g_ui_cur_beat = 1; g_ui_cur_bar = 0;
    g_ui_in_countin = false; g_ui_playing = true; g_show_help = false;
    render_ui();
    snprintf(path, sizeof(path), "%s/licktrainer_jazz_bar1.png", dir);
    if (!save_png(path)) { fprintf(stderr, "Failed to write %s\n", path); return 1; }
    printf("Saved %s\n", path);

    /* 5. English-only Help Modal */
    g_show_help = true;
    render_ui();
    snprintf(path, sizeof(path), "%s/licktrainer_help.png", dir);
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
            return verify_lick_library(true) ? 0 : 1;
        } else if ((strcmp(argv[i], "--screenshot") == 0 ||
                    strcmp(argv[i], "--screenshot-states") == 0) && i + 1 < argc) {
            if (!verify_lick_library(false)) {
                fprintf(stderr, "Fatal: Lick library verification failed.\n");
                return 1;
            }
            return run_screenshot_mode(argv[i + 1]);
        }
    }

    if (!verify_lick_library(false)) {
        fprintf(stderr, "Fatal: Lick library verification failed.\n");
        return 1;
    }

    resolve_config_path();
    bool loaded_bpm = load_config();
    if (!loaded_bpm) {
        g_base_bpm = k_licks[current_lick_global_idx()].default_bpm;
    }

    audio_init_soundfont();

    /* Initialize audio sequencer state before starting audio thread */
    g_aud_lick_idx   = current_lick_global_idx();
    g_aud_key_idx    = g_key_idx;
    g_aud_base_bpm   = g_base_bpm;
    g_aud_bpm        = g_base_bpm;
    g_aud_ladder_idx = g_ladder_idx;
    g_aud_mix_mode   = g_mix_mode;
    g_aud_timbre     = g_timbre_idx;
    g_aud_playing    = false;
    g_aud_in_countin = false;
    g_aud_cur_step   = 0;
    g_aud_cur_beat   = 0;
    g_aud_cur_bar    = 0;
    audio_publish_ui_telemetry();

    if (!platform_init("RG35XXSP LickTrainer (33 Guitar Studies)")) {
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
            /* A: Play / Pause (with 4-beat count-in) */
            if (p & BTN_A) {
                bool next_play = !g_ui_playing;
                post_audio_cmd(ACMD_SET_PLAYING, next_play ? 1 : 0, 0);
            }
            /* B: Cycle Audio Mix (TAB+CLK / CLK ONLY / TAB ONLY) */
            if (p & BTN_B) {
                g_mix_mode = (g_mix_mode + 1) % MIX_COUNT;
                post_audio_cmd(ACMD_SET_MIX, g_mix_mode, 0);
                save_config();
            }
            /* SELECT: Cycle Speed-Builder BPM Ladder */
            if (p & BTN_SELECT) {
                g_ladder_idx = (g_ladder_idx + 1) % NUM_LADDERS;
                post_audio_cmd(ACMD_SET_LADDER, g_ladder_idx, 0);
                save_config();
            }
            /* X: Transpose Key / Neck Fret Position */
            if (p & BTN_X) {
                g_key_idx = (g_key_idx + 1) % NUM_KEYS;
                post_audio_cmd(ACMD_SET_KEY, g_key_idx, 0);
                save_config();
            }
            /* L1 / R1: Prev / Next Category (1..6) */
            if (p & BTN_L1) {
                g_cat_idx = (g_cat_idx + NUM_CATEGORIES - 1) % NUM_CATEGORIES;
                if (g_lick_in_cat >= k_cat_count[g_cat_idx]) g_lick_in_cat = 0;
                int idx = current_lick_global_idx();
                g_base_bpm = k_licks[idx].default_bpm;
                post_audio_cmd(ACMD_SET_LICK, idx, g_base_bpm);
                save_config();
            }
            if (p & BTN_R1) {
                g_cat_idx = (g_cat_idx + 1) % NUM_CATEGORIES;
                if (g_lick_in_cat >= k_cat_count[g_cat_idx]) g_lick_in_cat = 0;
                int idx = current_lick_global_idx();
                g_base_bpm = k_licks[idx].default_bpm;
                post_audio_cmd(ACMD_SET_LICK, idx, g_base_bpm);
                save_config();
            }
            /* LEFT / RIGHT: Prev / Next Lick within Category */
            if (p & BTN_LEFT) {
                int cnt = k_cat_count[g_cat_idx];
                g_lick_in_cat = (g_lick_in_cat + cnt - 1) % cnt;
                int idx = current_lick_global_idx();
                g_base_bpm = k_licks[idx].default_bpm;
                post_audio_cmd(ACMD_SET_LICK, idx, g_base_bpm);
                save_config();
            }
            if (p & BTN_RIGHT) {
                int cnt = k_cat_count[g_cat_idx];
                g_lick_in_cat = (g_lick_in_cat + 1) % cnt;
                int idx = current_lick_global_idx();
                g_base_bpm = k_licks[idx].default_bpm;
                post_audio_cmd(ACMD_SET_LICK, idx, g_base_bpm);
                save_config();
            }
            /* L2: Cycle Guitar Timbre (Steel / Nylon / Jazz / Overdrive) */
            if (p & BTN_L2) {
                g_timbre_idx = (g_timbre_idx + 1) % NUM_TIMBRES;
                post_audio_cmd(ACMD_SET_TIMBRE, g_timbre_idx, 0);
                save_config();
            }
            /* R2: Reset BPM to lick's default_bpm & reset Speed Ladder stage */
            if (p & BTN_R2) {
                g_base_bpm = k_licks[current_lick_global_idx()].default_bpm;
                post_audio_cmd(ACMD_SET_BPM, g_base_bpm, 1);
                save_config();
            }
            /* START: Restart loop from Step 0 at base BPM (NEVER quits!) */
            if (p & BTN_START) {
                post_audio_cmd(ACMD_RESET_LOOP, 0, 0);
            }
            /* UP / DOWN: Adjust BPM with smooth continuous hold repeat */
            if (p & BTN_UP) {
                if (g_base_bpm < MAX_BPM) {
                    g_base_bpm++;
                    post_audio_cmd(ACMD_SET_BPM, g_base_bpm, 0);
                    save_config();
                }
                hold_start_ms = last_repeat_ms = now_ms;
            }
            if (p & BTN_DOWN) {
                if (g_base_bpm > MIN_BPM) {
                    g_base_bpm--;
                    post_audio_cmd(ACMD_SET_BPM, g_base_bpm, 0);
                    save_config();
                }
                hold_start_ms = last_repeat_ms = now_ms;
            }
            if ((input.held & (BTN_UP | BTN_DOWN)) && (now_ms >= hold_start_ms + 320)) {
                if (now_ms >= last_repeat_ms + 55) {
                    bool changed = false;
                    if ((input.held & BTN_UP) && g_base_bpm < MAX_BPM) { g_base_bpm++; changed = true; }
                    if ((input.held & BTN_DOWN) && g_base_bpm > MIN_BPM) { g_base_bpm--; changed = true; }
                    if (changed) {
                        post_audio_cmd(ACMD_SET_BPM, g_base_bpm, 0);
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
